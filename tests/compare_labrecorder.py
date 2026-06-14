#!/usr/bin/env python3
# Conformance test: record the mock LSL streams with BOTH our recorder (the headless
# `xdf_record` CLI, same Recorder/xdf_writer as the viewer) and LabRecorder (LabRecorderCLI),
# at the same time, then compare. Between a `start` and a `stop` control marker the two XDFs
# must contain identical data for every stream — same sample values and timestamps — because
# both recorders subscribe to the same outlets.
#
# Requires LabRecorderCLI on PATH and a built `xdf_record` (override with $XDF_RECORD). It is
# an integration test, not part of the C++ suite or the normal build CI (those have no
# LabRecorder). Run it from the repo root after building:
#
#   uv run tests/compare_labrecorder.py
#   uv run tests/compare_labrecorder.py --streams eeg,sine,chirp,markers,evoked,drift,accel,audio,highdensity
#
# /// script
# requires-python = ">=3.9"
# dependencies = ["pylsl", "numpy", "pyxdf"]
# ///

import argparse, os, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path

import numpy as np
import pyxdf
from pylsl import StreamInfo, StreamOutlet, local_clock

REPO = Path(__file__).resolve().parent.parent


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


def compare_stream(a, b, t0, t1):
    """Window both recordings to [t0, t1] (LSL local_clock) and compare."""
    tsa = np.asarray(a["time_stamps"], dtype=float)
    tsb = np.asarray(b["time_stamps"], dtype=float)
    ma = (tsa >= t0) & (tsa <= t1)
    mb = (tsb >= t0) & (tsb <= t1)
    na, nb = int(ma.sum()), int(mb.sum())
    da, db = a["time_series"], b["time_series"]
    is_str = isinstance(da, list) or (hasattr(da, "dtype") and da.dtype.kind in "OUS")

    if na != nb:
        return False, f"sample count differs: ours={na} lr={nb}"
    if na == 0:
        return True, "no samples in window"

    if is_str:
        va = [da[i] for i in np.nonzero(ma)[0]]
        vb = [db[i] for i in np.nonzero(mb)[0]]
        vals_ok = va == vb
    else:
        va = np.asarray(da)[ma]
        vb = np.asarray(db)[mb]
        vals_ok = np.array_equal(va, vb)

    ts_close = np.allclose(tsa[ma], tsb[mb], atol=1e-6, rtol=0)
    if not vals_ok:
        return False, f"sample VALUES differ ({na} samples)"
    if not ts_close:
        d = float(np.max(np.abs(tsa[ma] - tsb[mb])))
        return False, f"timestamps differ (max {d:.2e}s, values OK)"
    return True, f"{na} samples identical"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--streams", default="eeg,sine,chirp,markers,evoked,drift,accel,audio,highdensity")
    ap.add_argument("--window", type=float, default=4.0, help="seconds between start/stop markers")
    ap.add_argument("--keep", action="store_true", help="keep the temp XDFs")
    args = ap.parse_args()

    lrcli = shutil.which("LabRecorderCLI")
    if not lrcli:
        sys.exit("LabRecorderCLI not found on PATH (this test needs the real LabRecorder)")
    xdf_record = find_xdf_record()

    tmp = Path(tempfile.mkdtemp(prefix="lrcmp_"))
    ours_xdf, lr_xdf = str(tmp / "ours.xdf"), str(tmp / "lr.xdf")
    connect_s, post_s = 5.0, 1.5   # generous so both recorders subscribe before 'start'
    total_s = connect_s + args.window + post_s

    procs = []
    def spawn(cmd, **kw):
        p = subprocess.Popen(cmd, **kw); procs.append(p); return p

    print(f"streams: {args.streams}")
    mock = spawn([sys.executable, str(REPO / "tools" / "lsl_test_streams.py"),
                  "--streams", args.streams])
    time.sleep(3.0)   # let the outlets come up

    # Control marker stream, recorded by both; delimits the comparison window.
    ctl = StreamOutlet(StreamInfo("CompareControl", "Markers", 1, 0, "string", "cmp-ctl"))
    time.sleep(0.5)

    print("starting both recorders…")
    rec_ours = spawn([xdf_record, ours_xdf, "--seconds", str(total_s + 1.0), "--resolve-wait", "2.5"])
    rec_lr = spawn([lrcli, lr_xdf, "*"], stdin=subprocess.PIPE)   # stops on a newline
    time.sleep(connect_s)                      # both subscribe

    # The window is defined by the harness's local_clock at the start/stop pushes. All
    # recorded timestamps live in that same clock domain, so both files window identically —
    # no dependence on either recorder having captured the (irregular) marker samples.
    ctl.push_sample(["start"]); w0 = local_clock(); print("  pushed 'start'")
    time.sleep(args.window)
    ctl.push_sample(["stop"]); w1 = local_clock(); print("  pushed 'stop'")
    time.sleep(post_s)

    try:                                       # LabRecorderCLI stops cleanly on a newline
        rec_lr.stdin.write(b"\n"); rec_lr.stdin.flush(); rec_lr.stdin.close()
    except OSError:
        rec_lr.send_signal(signal.SIGINT)
    try: rec_lr.wait(timeout=10)
    except subprocess.TimeoutExpired: rec_lr.kill()
    try: rec_ours.wait(timeout=10)             # ours stops itself at --seconds
    except subprocess.TimeoutExpired: rec_ours.kill()
    mock.send_signal(signal.SIGINT);
    try: mock.wait(timeout=5)
    except subprocess.TimeoutExpired: mock.kill()
    del ctl

    ours, lr = load(ours_xdf), load(lr_xdf)
    print(f"\nours: {sorted(ours)}\nlr  : {sorted(lr)}")

    # Clamp the marker window to where BOTH recordings have data on their regular-rate
    # streams (each recorder starts/stops at a slightly different instant; LabRecorder in
    # particular can resolve slowly). The comparison then runs over the common span.
    def span(streams):
        los, his = [], []
        for s in streams.values():
            if float(s["info"]["nominal_srate"][0]) <= 0:
                continue                                   # skip irregular (markers/mouse)
            ts = np.asarray(s["time_stamps"], dtype=float)
            if ts.size > 1:
                los.append(ts[0]); his.append(ts[-1])
        return (max(los), min(his)) if los else (None, None)

    olo, ohi = span(ours); llo, lhi = span(lr)
    if None in (olo, llo):
        sys.exit("one recording has no regular-rate data")
    w0 = max(w0, olo, llo) + 0.05
    w1 = min(w1, ohi, lhi) - 0.05
    print(f"window: [{w0:.3f}, {w1:.3f}]  ({w1 - w0:.2f}s)\n")
    if w1 <= w0:
        sys.exit("the two recordings do not overlap the marker window")

    names = sorted((set(ours) & set(lr)) - {"CompareControl"})
    fails = 0
    for n in names:
        ok, msg = compare_stream(ours[n], lr[n], w0, w1)
        print(f"  [{'PASS' if ok else 'FAIL'}] {n:<18} {msg}")
        fails += not ok
    only = (set(ours) ^ set(lr)) - {"CompareControl"}
    if only:
        print(f"\n  note: streams in only one recording: {sorted(only)}")

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{'OK' if fails == 0 else 'FAILED'}: {len(names) - fails}/{len(names)} streams identical")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
