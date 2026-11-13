# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Take the screenshots of the usage pages in docs/ and write them to docs/images/.

The viewer takes the shots itself: this script starts a test build of the viewer
(-DLSL_VIEWER_TESTS=ON) with the built-in demo streams, runs the `capture_readme`
UI test for the shots below, and copies the results. No other process is necessary.

    uv run tools/make_doc_screenshots.py
    uv run tools/make_doc_screenshots.py --shots plot,combined
    uv run tools/make_doc_screenshots.py --exe build-test/lsl_viewer --out /tmp/shots

To add a shot: add it to the usage-pages block of `capture_readme` in
tests/ui_tests.cpp, and add its name to SHOTS here.

The README images (montage, spectrum, spectrogram, erp, live.gif) are not taken
here. They need streams from tools/lsl_test_streams.py and a larger display. See
the comment above `capture_readme` for how they were taken.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Shot name in LSL_README_SHOTS -> file name in docs/images/.
SHOTS = {
    "overview": "overview.png",
    "plot": "stream-plot.png",
    "combined": "combined-plot.png",
    "markers": "marker-events.png",
    "recording": "rail-recording.png",
    "playback": "rail-playback.png",
}

# The rail lists all streams on the network. Limit discovery to this machine, so
# that the streams of a neighbor do not show in the images.
LSL_CFG = """[multicast]
ResolveScope = machine
[lab]
KnownPeers = {127.0.0.1}
"""

EXE_CANDIDATES = [
    "build-test/Release/lsl_viewer.exe",
    "build-test/RelWithDebInfo/lsl_viewer.exe",
    "build-test/lsl_viewer.exe",
    "build-test/lsl_viewer",
]


def find_exe() -> Path | None:
    for rel in EXE_CANDIDATES:
        p = REPO / rel
        if p.is_file():
            return p
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", type=Path, help="viewer built with -DLSL_VIEWER_TESTS=ON (default: look in build-test/)")
    ap.add_argument("--out", type=Path, default=REPO / "docs" / "images", help="output folder (default: docs/images)")
    ap.add_argument("--shots", default=",".join(SHOTS), help="comma list from: " + ", ".join(SHOTS))
    ap.add_argument("--window", default="1280x800", help="window size, WxH (default: 1280x800)")
    ap.add_argument("--timeout", type=float, default=300.0, help="seconds to wait for the viewer (default: 300)")
    args = ap.parse_args()

    shots = [s for s in args.shots.split(",") if s]
    unknown = [s for s in shots if s not in SHOTS]
    if unknown:
        ap.error("unknown shots: " + ", ".join(unknown))

    exe = args.exe.resolve() if args.exe else find_exe()
    if exe is None or not exe.is_file():
        print("No test build of the viewer. Build one first:\n"
              "  cmake -S . -B build-test -DLSL_VIEWER_TESTS=ON\n"
              "  cmake --build build-test --config Release", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lsl_viewer_shots_") as tmp:
        work = Path(tmp)
        cfg = work / "lsl_api.cfg"
        cfg.write_text(LSL_CFG)

        env = dict(os.environ)
        # Variables of the caller that change the look of the window.
        for var in ("LSL_LIGHT", "LSL_FULLSCREEN", "LSL_MAXIMIZE", "LSL_AUTOCONNECT", "LSL_REPLAY", "LSL_RC_PORT"):
            env.pop(var, None)
        env.update({
            "LSL_DEMO": "1",
            "LSL_WINDOW": args.window,
            "LSL_README_SHOTS": ",".join(shots),
            "LSLAPICFG": str(cfg),
        })

        # The test writes to ./output/captures, so the temporary folder is the working
        # directory. A --tests run keeps its settings apart from those of the user.
        print(f"running {exe.name} --tests capture_readme ({', '.join(shots)})", flush=True)
        try:
            proc = subprocess.run([str(exe), "--tests", "capture_readme"], cwd=work, env=env, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print(f"the viewer did not finish in {args.timeout:.0f} s", file=sys.stderr)
            return 1
        if proc.returncode != 0:
            print(f"the capture test failed (exit code {proc.returncode})", file=sys.stderr)
            return 1

        args.out.mkdir(parents=True, exist_ok=True)
        missing = []
        for name in shots:
            src = work / "output" / "captures" / f"readme_{name}.png"
            if not src.is_file():
                missing.append(name)
                continue
            dst = args.out / SHOTS[name]
            shutil.copyfile(src, dst)
            print(f"  {dst}")
        if missing:
            print("no image for: " + ", ".join(missing), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
