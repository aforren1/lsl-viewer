#!/usr/bin/env python3
"""Record the same (clock-skewed) LSL streams with our headless `xdf_record` and, if
available, LabRecorder's CLI, then compare the two XDF files.

The two recordings can't be byte-identical (independent inlets, start/stop timing),
so we check the things that MUST agree: stream metadata, data layout, channel
labels, and — anchored on shared markers — clock alignment.

Run from the repo root after `cmake --build build`:
    python tests/compare_recording.py
Set LABRECORDER_CLI=/path/to/LabRecorderCLI to force a specific binary.
"""
import os, sys, subprocess, time, shutil, signal, tempfile
import numpy as np
import pyxdf

REPO     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS_CLI = os.path.join(REPO, "build", "xdf_record")
LAB_CLI  = os.environ.get("LABRECORDER_CLI") or shutil.which("LabRecorderCLI")
NAMES    = ["MockEEG", "MockAccel", "MockMarkers", "MockFlaky"]   # actual LSL stream names
QUERY    = "type='EEG' or type='Accelerometer' or type='Markers'"  # flaky is type EEG too
DUR      = 12.0   # long enough to span a flaky disconnect/reconnect (~3 s down / ~8 s up)


def load(path):
    streams, _ = pyxdf.load_xdf(path, dejitter_timestamps=True)
    return {s["info"]["name"][0]: s for s in streams}


def chan_labels(s):
    try:
        return [c["label"][0] for c in s["info"]["desc"][0]["channels"][0]["channel"]]
    except Exception:
        return None


def main():
    senders = subprocess.Popen(
        [sys.executable, os.path.join(REPO, "tools", "lsl_test_streams.py"),
         "--streams", "eeg,accel,markers,flaky", "--eeg-channels", "8", "--clock-skew", "2.0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    tmp = tempfile.mkdtemp()
    ours, theirs = os.path.join(tmp, "ours.xdf"), os.path.join(tmp, "theirs.xdf")

    lab = None
    if LAB_CLI:
        lab = subprocess.Popen([LAB_CLI, theirs, QUERY], stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ours_p = subprocess.Popen([OURS_CLI, ours, "--streams", ",".join(NAMES), "--resolve-wait", "1.0"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(DUR)
    ours_p.send_signal(signal.SIGTERM); ours_p.wait(timeout=5)
    if lab:
        try: lab.communicate(b"\n", timeout=5)        # Enter quits LabRecorderCLI
        except Exception: lab.kill()
    senders.terminate()
    time.sleep(0.3)

    # ---- validate ours ----
    A = load(ours)
    print(f"[ours] {len(A)} streams: {', '.join(A)}")
    assert A, "no streams recorded"
    for nm, s in A.items():
        info = s["info"]
        print(f"  {nm:11s} {info['type'][0]:13s} {info['channel_count'][0]}ch "
              f"{info['channel_format'][0]:8s} n={len(s['time_stamps'])}  labels={chan_labels(s)}")
        assert len(s["time_stamps"]) > 0, f"{nm} empty"
    t0 = {nm: s["time_stamps"][0] for nm, s in A.items() if len(s["time_stamps"])}
    spread = max(t0.values()) - min(t0.values())
    print(f"  start-time spread across streams = {spread:.2f}s (clock skew exercised)")
    assert spread > 1.0, "expected distinct per-stream clock offsets from --clock-skew"

    if not lab:
        print("\nLabRecorder CLI not found -> cross-comparison skipped. RESULT: OK (ours validated).")
        return 0

    # ---- cross-compare with LabRecorder ----
    B = load(theirs)
    print(f"[theirs] {len(B)} streams: {', '.join(B)}")
    common = sorted(set(A) & set(B))
    assert common, "no common streams between the two recordings"
    for nm in common:
        a, b = A[nm], B[nm]
        assert a["info"]["channel_count"][0]  == b["info"]["channel_count"][0],  f"{nm} channel_count"
        assert a["info"]["channel_format"][0] == b["info"]["channel_format"][0], f"{nm} format"
        assert a["info"]["type"][0]           == b["info"]["type"][0],           f"{nm} type"
        if chan_labels(a) and chan_labels(b):
            assert chan_labels(a) == chan_labels(b), f"{nm} channel labels differ"
        print(f"  {nm}: metadata + layout + labels match")

    # marker-anchored clock alignment: each marker in ours should have a same-label
    # marker in theirs at (dejittered) ~the same time.
    mk = next((nm for nm in common if A[nm]["info"]["channel_format"][0] == "string"), None)
    if mk:
        ta, va = A[mk]["time_stamps"], [x[0] for x in A[mk]["time_series"]]
        tb, vb = B[mk]["time_stamps"], [x[0] for x in B[mk]["time_series"]]
        diffs = []
        for t, v in zip(ta, va):
            cand = [abs(t - tb[i]) for i in range(len(tb)) if vb[i] == v]
            if cand: diffs.append(min(cand))
        if diffs:
            print(f"  markers matched: {len(diffs)}/{len(ta)}, median |Δt| = {np.median(diffs)*1000:.2f} ms")
            assert np.median(diffs) < 0.05, "marker times misaligned > 50 ms"

    # data spot-check: a regular stream's overlapping samples should carry identical
    # values (both inlets pull the same outlet).
    dn = next((nm for nm in common if A[nm]["info"]["channel_format"][0] != "string"), None)
    if dn:
        da = np.asarray(A[dn]["time_series"]); db = np.asarray(B[dn]["time_series"])
        # match a distinctive sample from ours inside theirs (first channel signature)
        probe = da[len(da)//2]
        hits = np.where(np.all(np.isclose(db, probe, atol=1e-3), axis=1))[0]
        print(f"  data '{dn}': mid-sample of ours found in theirs at {len(hits)} position(s)")
        assert len(hits) >= 1, f"{dn} data values not found in the other recording"

    print("RESULT: OK (cross-compared with LabRecorder: metadata, layout, clocks, data).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
