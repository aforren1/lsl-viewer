#!/usr/bin/env python3
# Conformance test: record the mock LSL streams with BOTH our recorder (the headless
# `xdf_record` CLI, same Recorder/xdf_writer as the viewer) and LabRecorder (LabRecorderCLI),
# at the same time, then compare. Because both recorders subscribe to the same outlets, the
# two XDFs must agree — and this test checks that agreement at several levels:
#
#   * data:        identical sample VALUES and TIMESTAMPS over several windows of the run
#   * metadata:    identical stream header (type/rate/format/source_id) and per-channel
#                  description (label/type/unit/location) for every shared stream
#   * structure:   identical XDF chunk layout — both files carry a FileHeader, and every
#                  stream has a StreamHeader, a StreamFooter, ClockOffset (time-correction)
#                  chunks, and Sample chunks (parsed straight from the binary, below pyxdf)
#   * validity:    our recording is internally sound — timestamps non-decreasing, no NaN/Inf
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
from pylsl import StreamInfo, StreamOutlet, local_clock

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
def compare_window(a, b, t0, t1, is_regular):
    tsa = np.asarray(a["time_stamps"], dtype=float)
    tsb = np.asarray(b["time_stamps"], dtype=float)
    ma = (tsa >= t0) & (tsa <= t1)
    mb = (tsb >= t0) & (tsb <= t1)
    na, nb = int(ma.sum()), int(mb.sum())
    if na != nb:
        return False, f"sample count differs: ours={na} lr={nb}"
    if na == 0:
        if is_regular:
            return False, "no samples in window (regular-rate stream should have data)"
        return True, "no samples (both)"

    da, db = a["time_series"], b["time_series"]
    is_str = isinstance(da, list) or (hasattr(da, "dtype") and da.dtype.kind in "OUS")
    if is_str:
        va = [da[i] for i in np.nonzero(ma)[0]]
        vb = [db[i] for i in np.nonzero(mb)[0]]
        vals_ok = va == vb
    else:
        vals_ok = np.array_equal(np.asarray(da)[ma], np.asarray(db)[mb])
    if not vals_ok:
        return False, f"sample VALUES differ ({na} samples)"
    if not np.allclose(tsa[ma], tsb[mb], atol=1e-6, rtol=0):
        d = float(np.max(np.abs(tsa[ma] - tsb[mb])))
        return False, f"timestamps differ (max {d:.2e}s, values OK)"
    return True, f"{na} samples identical"


def data_span(streams):
    """[first, last] of the regular-rate streams' timestamps (where both recorders overlap)."""
    los, his = [], []
    for s in streams.values():
        if float(s["info"]["nominal_srate"][0]) <= 0:
            continue
        ts = np.asarray(s["time_stamps"], dtype=float)
        if ts.size > 1:
            los.append(ts[0]); his.append(ts[-1])
    return (max(los), min(his)) if los else (None, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--streams",
                    default="eeg,sine,chirp,markers,evoked,drift,accel,audio,highdensity,mouse,fastmarkers")
    ap.add_argument("--window", type=float, default=4.0, help="seconds per comparison window")
    ap.add_argument("--cycles", type=int, default=2, help="number of start/stop windows to compare")
    ap.add_argument("--gap", type=float, default=1.0, help="seconds between windows")
    ap.add_argument("--keep", action="store_true", help="keep the temp XDFs")
    args = ap.parse_args()

    lrcli = shutil.which("LabRecorderCLI")
    if not lrcli:
        sys.exit("LabRecorderCLI not found on PATH (this test needs the real LabRecorder)")
    xdf_record = find_xdf_record()

    tmp = Path(tempfile.mkdtemp(prefix="lrcmp_"))
    ours_xdf, lr_xdf = str(tmp / "ours.xdf"), str(tmp / "lr.xdf")
    connect_s, post_s = 5.0, 1.5     # generous so both recorders subscribe before the first 'start'
    record_s = connect_s + args.cycles * args.window + (args.cycles - 1) * args.gap + post_s

    procs = []
    def spawn(cmd, **kw):
        p = subprocess.Popen(cmd, **kw); procs.append(p); return p

    print(f"streams: {args.streams}\ncycles : {args.cycles} x {args.window:g}s windows\n")
    mock = spawn([sys.executable, str(REPO / "tools" / "lsl_test_streams.py"),
                  "--streams", args.streams])
    time.sleep(3.0)   # let the outlets come up

    ctl = StreamOutlet(StreamInfo("CompareControl", "Markers", 1, 0, "string", "cmp-ctl"))
    time.sleep(0.5)

    print("starting both recorders…")
    rec_ours = spawn([xdf_record, ours_xdf, "--seconds", str(record_s + 1.0), "--resolve-wait", "2.5"])
    rec_lr = spawn([lrcli, lr_xdf, "*"], stdin=subprocess.PIPE)   # stops on a newline
    time.sleep(connect_s)            # both subscribe

    # The windows are defined by the harness's local_clock at each start/stop push. All
    # recorded timestamps live in that same clock domain, so both files window identically.
    windows = []
    for c in range(args.cycles):
        ctl.push_sample(["start"]); w0 = local_clock()
        time.sleep(args.window)
        ctl.push_sample(["stop"]); w1 = local_clock()
        windows.append((w0, w1))
        print(f"  window {c + 1}: [{w0:.3f}, {w1:.3f}]")
        if c < args.cycles - 1:
            time.sleep(args.gap)
    time.sleep(post_s)

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
    del ctl

    ours, lr = load(ours_xdf), load(lr_xdf)
    print(f"\nours: {sorted(ours)}\nlr  : {sorted(lr)}")
    names = sorted((set(ours) & set(lr)) - {"CompareControl"})
    only = (set(ours) ^ set(lr)) - {"CompareControl"}
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

    # 4) sample-level data parity over each window (clamped to where both recorders overlap).
    olo, ohi = data_span(ours); llo, lhi = data_span(lr)
    if None in (olo, llo):
        sys.exit("one recording has no regular-rate data")
    lo, hi = max(olo, llo) + 0.05, min(ohi, lhi) - 0.05
    print(f"\n-- data (overlap [{lo:.3f}, {hi:.3f}], {hi - lo:.2f}s) --")
    for n in names:
        is_regular = float(ours[n]["info"]["nominal_srate"][0]) > 0
        results = []
        for (w0, w1) in windows:
            t0, t1 = max(w0, lo), min(w1, hi)
            if t1 <= t0:
                results.append((False, "window outside overlap")); continue
            results.append(compare_window(ours[n], lr[n], t0, t1, is_regular))
        ok = all(r[0] for r in results)
        detail = " | ".join(m for _, m in results)
        print(f"  [{'PASS' if ok else 'FAIL'}] data {n:<18} {detail}")
        fails += not ok

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'OK' if fails == 0 else 'FAILED'}: {fails} check(s) failed across "
          f"{len(names)} streams (structure + metadata + validity + {args.cycles} data windows)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
