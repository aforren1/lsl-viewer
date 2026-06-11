#!/usr/bin/env bash
# Launch the LSL Stream Viewer under WSLg.
#
# WSLg keeps its Wayland socket in /mnt/wslg/runtime-dir (not the usual
# $XDG_RUNTIME_DIR), and the SDL build here is Wayland-only (X11 disabled), so we
# point SDL at the right runtime dir. Pass --x11 to force the X11/Xwayland path
# instead (requires an SDL built with X11 support).
set -euo pipefail
cd "$(dirname "$0")"

BIN=build/lsl_viewer
[ -x "$BIN" ] || { echo "not built yet — run: cmake --build build"; exit 1; }

if [ "${1:-}" = "--x11" ]; then
    exec env SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY:-:0}" "$BIN"
fi

exec env XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir SDL_VIDEODRIVER=wayland "$BIN"
