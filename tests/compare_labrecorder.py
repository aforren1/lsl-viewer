#!/usr/bin/env python3
# Conformance test: record the mock LSL streams with BOTH our recorder (the headless
# `xdf_record` CLI, same Recorder/xdf_writer as the viewer) and LabRecorder (LabRecorderCLI),
# at the same time, then compare. Because both recorders subscribe to the same outlets, the
# two XDFs must agree — and this test checks that agreement at several levels:
#
# Worst case: every stream is put on its OWN apparent clock — a distinct constant OFFSET plus a
# distinct rate DRIFT (--clock-skew / --clock-drift), as if each came from a separate device — so
# the recorder has to capture arbitrary, non-uniform per-stream timebases.
#
#   * data:        bit-IDENTICAL sample values AND timestamps over several windows of the run,
#                  matched on identical timestamps — both recorders stamp the same LSL samples, so
#                  there is NO tolerance (each stream is compared against itself across recorders,
#                  which is offset/drift-agnostic)
#   * skew:        the recorded per-stream start times span more than the offset, i.e. the recorder
#                  preserved each stream's clock and never normalized them together
#   * metadata:    identical stream header (type/rate/format/source_id) and per-channel
#                  description (label/type/unit/location) for every shared stream
#   * structure:   identical XDF chunk layout — both files carry a FileHeader, and every
#                  stream has a StreamHeader, a StreamFooter, ClockOffset (time-correction)
#                  chunks, and Sample chunks (parsed straight from the binary, below pyxdf)
#   * dropouts:    a `flaky` stream disconnects/reconnects mid-run; both recorders must recover
#                  it (recover=True) and still agree on the data either side of the gap
#   * validity:    our recording is internally sound — timestamps non-decreasing, no NaN/Inf
#
# (The offset/drift only shift the recorded timestamp VALUES — on one host the real clocks ARE the
# same, so time_correction is ~0 and cross-machine SYNC can't be exercised. This proves the
# recorder captures arbitrary timebases faithfully and writes LabRecorder-shaped ClockOffset
# chunks, not that it resolves real drift.)
#
# Requires LabRecorderCLI on PATH and a built `xdf_record` (override with $XDF_RECORD). It is
# an integration test, not part of the C++ suite or the normal build CI (those have no
# LabRecorder). Run it from the repo root after building:
#
#   uv run tests/compare_labrecorder.py
#   uv run tests/compare_labrecorder.py --streams eeg,markers --cycles 3 --window 5
#
# /// script
# requires-python = ">=3.9"
# dependencies = ["pylsl", "numpy", "pyxdf"]
# ///

import argparse, collections, os, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path

import numpy as np
import pyxdf
# (pylsl is in the deps below so the mock-streams subprocess can import it — this script no
#  longer creates LSL outlets itself; each stream is compared against itself across recorders.)

REPO = Path(__file__).resolve().parent.parent

# XDF chunk tags (see the XDF spec / our xdf_writer.hpp).
TAGS = {1: "FileHeader", 2: "StreamHeader", 3: "Samples", 4: "ClockOffset",
        5: "Boundary", 6: "StreamFooter"}
TAGGED_WITH_STREAMID = {2, 3, 4, 6}   # these chunks begin their content with a 4-byte stream id


def find_xdf_record() -> str:
    if os.environ.get("XDF_RECORD"):
        return os.environ["XDF_RECORD"]
    for c in [REPO / "build" / "xdf_record", *REPO.glob("build*/xdf_record")]:
        if c.exists():
            return str(c)
    sys.exit("xdf_record not found; build it or set $XDF_RECORD")


def load(path: str) -> dict:
    # Raw, un-synced, un-dejittered: compare exactly what each recorder wrote.
    streams, _ = pyxdf.load_xdf(path, synchronize_clocks=False, dejitter_timestamps=False)
    return {s["info"]["name"][0]: s for s in streams}


# ---- raw chunk parser: count chunk types per stream, straight from the binary -------------
def xdf_chunks(path: str):
    """Return (per_stream_id -> Counter(tag->n), global Counter(tag->n)). Raises on bad magic."""
    per = collections.defaultdict(collections.Counter)
    glob = collections.Counter()
    with open(path, "rb") as f:
        if f.read(4) != b"XDF:":
            raise ValueError(f"{path}: missing 'XDF:' magic")
        while True:
            nb = f.read(1)
            if not nb:
                break
            length = int.from_bytes(f.read(nb[0]), "little")        # bytes from the tag onward
            tag = int.from_bytes(f.read(2), "little")
            content = length - 2
            if tag in TAGGED_WITH_STREAMID:
                sid = int.from_bytes(f.read(4), "little")
                per[sid][tag] += 1
                f.seek(content - 4, 1)
            else:
                glob[tag] += 1
                f.seek(content, 1)
    return per, glob


def check_structure(label: str, path: str, n_streams: int) -> int:
    """Assert the file's chunk layout is well-formed; return a failure count and print a line."""
    per, glob = xdf_chunks(path)
    fails = 0
    problems = []
    if glob.get(1, 0) != 1:
        fails += 1; problems.append(f"FileHeader={glob.get(1,0)} (want 1)")
    if len(per) != n_streams:
        fails += 1; problems.append(f"{len(per)} streams in chunks vs {n_streams} parsed")
    for sid, c in sorted(per.items()):
        if c.get(2, 0) != 1: fails += 1; problems.append(f"sid {sid}: StreamHeader={c.get(2,0)}")
        if c.get(6, 0) != 1: fails += 1; problems.append(f"sid {sid}: StreamFooter={c.get(6,0)}")
        if c.get(4, 0) < 1:  fails += 1; problems.append(f"sid {sid}: no ClockOffset chunks")
        if c.get(3, 0) < 1:  fails += 1; problems.append(f"sid {sid}: no Sample chunks")
    totals = collections.Counter()
    for c in per.values():
        totals.update(c)
    totals.update(glob)
    summary = " ".join(f"{TAGS[t]}={totals[t]}" for t in sorted(TAGS) if totals[t])
    print(f"  [{'PASS' if not fails else 'FAIL'}] structure {label:<5} {summary}")
    for p in problems:
        print(f"           - {p}")
    return fails


# ---- metadata extraction (from pyxdf's parsed StreamHeader XML) ----------------------------
def header_summary(s) -> dict:
    i = s["info"]
    one = lambda k: (i.get(k) or [None])[0]
    return {
        "type": one("type"),
        "source_id": one("source_id"),
        "channel_count": one("channel_count"),
        "channel_format": one("channel_format"),
        "nominal_srate": float(one("nominal_srate") or 0.0),
    }


def channel_meta(s):
    """List of (label, type, unit, (X,Y,Z)) per channel, from the stream description."""
    desc = (s["info"].get("desc") or [None])[0]
    if not isinstance(desc, dict):
        return []
    chs = (desc.get("channels") or [None])[0]
    if not isinstance(chs, dict):
        return []
    out = []
    for c in (chs.get("channel") or []):
        one = lambda k: (c.get(k) or [None])[0]
        loc = (c.get("location") or [None])[0]
        locv = None
        if isinstance(loc, dict):
            locv = tuple((loc.get(k) or [None])[0] for k in ("X", "Y", "Z"))
        out.append((one("label"), one("type"), one("unit"), locv))
    return out


def check_validity(name: str, s) -> int:
    """Internal soundness of OUR recording: timestamps non-decreasing, values finite."""
    ts = np.asarray(s["time_stamps"], dtype=float)
    fails = 0
    if ts.size and np.any(np.diff(ts) < 0):
        n = int(np.sum(np.diff(ts) < 0))
        print(f"  [FAIL] valid {name:<18} {n} backward timestamp step(s)"); fails += 1
    data = s["time_series"]
    is_str = isinstance(data, list) or (hasattr(data, "dtype") and data.dtype.kind in "OUS")
    if not is_str:
        arr = np.asarray(data, dtype=float)
        if arr.size and not np.all(np.isfinite(arr)):
            print(f"  [FAIL] valid {name:<18} non-finite (NaN/Inf) sample values"); fails += 1
    return fails


# ---- data comparison over a window --------------------------------------------------------
def compare_window(a, b, t0, t1, is_regular, allow_edge=0):
    # Both recorders subscribe to the SAME outlet, so LSL stamps each sample once and both write
    # that exact timestamp — there is no clock to "sync" on a single host (time_correction ~= 0),
    # the times are bit-identical. So we match on identical timestamps and require identical
    # values there: no tolerance. `allow_edge` permits a few unmatched boundary samples ONLY for a
    # flaky (disconnect/reconnect) stream, where one recorder may catch a sample at a reconnect the
    # other misses; for every steady stream it's 0 (strict).
    tsa = np.asarray(a["time_stamps"], dtype=float)
    tsb = np.asarray(b["time_stamps"], dtype=float)
    ia = np.nonzero((tsa >= t0) & (tsa <= t1))[0]
    ib = np.nonzero((tsb >= t0) & (tsb <= t1))[0]
    if len(ia) == 0 and len(ib) == 0:
        return (False, "no samples (regular-rate stream should have data)") if is_regular \
               else (True, "no samples (both)")

    common, ca, cb = np.intersect1d(tsa[ia], tsb[ib], return_indices=True)
    unmatched = (len(ia) - len(common)) + (len(ib) - len(common))
    if unmatched > allow_edge:
        return False, f"timestamps differ: {unmatched} unmatched (ours={len(ia)} lr={len(ib)} shared={len(common)})"
    if len(common) == 0:
        return False, "no shared timestamps"

    ai, bi = ia[ca], ib[cb]                                   # original indices of the shared samples
    da, db = a["time_series"], b["time_series"]
    is_str = isinstance(da, list) or (hasattr(da, "dtype") and da.dtype.kind in "OUS")
    if is_str:
        vals_ok = [da[i] for i in ai] == [db[i] for i in bi]
    else:
        vals_ok = np.array_equal(np.asarray(da)[ai], np.asarray(db)[bi])
    if not vals_ok:
        return False, f"sample VALUES differ ({len(common)} shared samples)"
    return True, f"{len(common)} samples identical" + (f" (+{unmatched} edge)" if unmatched else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--streams",
                    default="eeg,sine,chirp,markers,evoked,drift,accel,audio,highdensity,mouse,fastmarkers,flaky")
    ap.add_argument("--window", type=float, default=4.0, help="seconds per comparison window")
    ap.add_argument("--cycles", type=int, default=2, help="number of windows compared per stream")
    ap.add_argument("--clock-skew", type=float, default=2.0,
                    help="per-stream constant timestamp OFFSET spread (s); the recorder must preserve it")
    ap.add_argument("--clock-drift", type=float, default=1e-3,
                    help="per-stream timestamp DRIFT/rate spread (fraction) — each clock ticks fast/slow")
    ap.add_argument("--keep", action="store_true", help="keep the temp XDFs")
    args = ap.parse_args()

    lrcli = shutil.which("LabRecorderCLI")
    if not lrcli:
        sys.exit("LabRecorderCLI not found on PATH (this test needs the real LabRecorder)")
    xdf_record = find_xdf_record()

    tmp = Path(tempfile.mkdtemp(prefix="lrcmp_"))
    ours_xdf, lr_xdf = str(tmp / "ours.xdf"), str(tmp / "lr.xdf")
    connect_s, post_s = 5.0, 1.5     # generous so both recorders subscribe before recording matters
    # long enough to also span a full flaky cycle (~8 s up / 3 s down) so the dropout is exercised
    active_s = max(14.0, args.cycles * args.window + 2.0)
    record_s = connect_s + active_s + post_s

    procs = []
    def spawn(cmd, **kw):
        p = subprocess.Popen(cmd, **kw); procs.append(p); return p

    print(f"streams: {args.streams}\nclock  : offset {args.clock_skew:g}s + drift {args.clock_drift:g}/s per stream"
          f"   compare: {args.cycles} x {args.window:g}s/stream\n")
    mock = spawn([sys.executable, str(REPO / "tools" / "lsl_test_streams.py"),
                  "--streams", args.streams,
                  "--clock-skew", str(args.clock_skew), "--clock-drift", str(args.clock_drift)])
    time.sleep(3.5)   # let the outlets come up

    print("starting both recorders…")
    rec_ours = spawn([xdf_record, ours_xdf, "--seconds", str(record_s + 1.0), "--resolve-wait", "2.5"])
    rec_lr = spawn([lrcli, lr_xdf, "*"], stdin=subprocess.PIPE)   # stops on a newline
    # Each stream is compared against itself across the two recorders, so the comparison is
    # skew-agnostic and needs no shared clock window — just let it record (incl. a flaky cycle).
    time.sleep(connect_s + active_s + post_s)

    try:                             # LabRecorderCLI stops cleanly on a newline
        rec_lr.stdin.write(b"\n"); rec_lr.stdin.flush(); rec_lr.stdin.close()
    except OSError:
        rec_lr.send_signal(signal.SIGINT)
    try: rec_lr.wait(timeout=10)
    except subprocess.TimeoutExpired: rec_lr.kill()
    try: rec_ours.wait(timeout=10)   # ours stops itself at --seconds
    except subprocess.TimeoutExpired: rec_ours.kill()
    mock.send_signal(signal.SIGINT)
    try: mock.wait(timeout=5)
    except subprocess.TimeoutExpired: mock.kill()

    ours, lr = load(ours_xdf), load(lr_xdf)
    print(f"\nours: {sorted(ours)}\nlr  : {sorted(lr)}")
    names = sorted(set(ours) & set(lr))
    only = set(ours) ^ set(lr)
    if only:
        print(f"note: streams in only one recording: {sorted(only)}")
    if not names:
        sys.exit("no streams in common between the two recordings")

    fails = 0

    # 1) structural parity (FileHeader, per-stream header/footer/clock-offset/samples).
    print("\n-- structure --")
    fails += check_structure("ours", ours_xdf, len(ours))
    fails += check_structure("lr", lr_xdf, len(lr))

    # 2) header + per-channel metadata parity (both copy the outlet's XML — must match).
    print("\n-- metadata --")
    for n in names:
        hd_ok = header_summary(ours[n]) == header_summary(lr[n])
        ch_ok = channel_meta(ours[n]) == channel_meta(lr[n])
        ok = hd_ok and ch_ok
        why = "" if ok else ("header differs" if not hd_ok else "channel desc differs")
        nch = len(channel_meta(ours[n]))
        print(f"  [{'PASS' if ok else 'FAIL'}] meta {n:<18} {nch} ch desc; {why}".rstrip())
        fails += not ok

    # 3) internal validity of our recording.
    print("\n-- validity (ours) --")
    vfails = sum(check_validity(n, ours[n]) for n in names)
    if not vfails:
        print("  [PASS] all streams: timestamps non-decreasing, values finite")
    fails += vfails

    # 4) skew preservation: with a skew set, the streams must land on distinct apparent clocks
    #    (the recorder writes raw timestamps and never normalizes them across streams).
    if args.clock_skew > 0:
        firsts = [np.asarray(ours[n]["time_stamps"], float)[0]
                  for n in names if np.asarray(ours[n]["time_stamps"], float).size]
        spread = (max(firsts) - min(firsts)) if firsts else 0.0
        ok = spread > args.clock_skew
        print(f"\n-- skew (start-time spread across streams = {spread:.2f}s vs skew {args.clock_skew:g}s) --")
        print(f"  [{'PASS' if ok else 'FAIL'}] per-stream clock offsets preserved")
        fails += not ok

    # 5) sample-level data parity. Each stream is compared against ITSELF across the two recorders
    #    over its own [first, last] overlap (skew-agnostic), split into `cycles` windows.
    print(f"\n-- data ({args.cycles} x {args.window:g}s window(s)/stream, exact match) --")
    for n in names:
        flaky = "Flaky" in n                                  # disconnect/reconnect: gaps + boundary slack
        is_regular = float(ours[n]["info"]["nominal_srate"][0]) > 0 and not flaky
        ta = np.asarray(ours[n]["time_stamps"], float); tb = np.asarray(lr[n]["time_stamps"], float)
        if ta.size == 0 or tb.size == 0:
            print(f"  [FAIL] data {n:<18} empty in one recording"); fails += 1; continue
        lo, hi = max(ta[0], tb[0]) + 0.05, min(ta[-1], tb[-1]) - 0.05
        edge = 4 if flaky else 0
        results = []
        if hi - lo < args.window:                             # short / irregular: one window over the overlap
            results.append(compare_window(ours[n], lr[n], min(ta[0], tb[0]), max(ta[-1], tb[-1]), is_regular, edge))
        else:
            step = (hi - lo) / args.cycles
            for c in range(args.cycles):
                w0 = lo + c * step
                results.append(compare_window(ours[n], lr[n], w0, min(w0 + args.window, lo + (c + 1) * step), is_regular, edge))
        ok = all(r[0] for r in results)
        detail = " | ".join(m for _, m in results)
        print(f"  [{'PASS' if ok else 'FAIL'}] data {n:<18} {detail}")
        fails += not ok

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'OK' if fails == 0 else 'FAILED'}: {fails} check(s) failed across {len(names)} streams "
          f"(structure + metadata + validity + skew + dropout + {args.cycles} data windows/stream)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
