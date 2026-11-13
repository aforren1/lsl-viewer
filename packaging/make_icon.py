#!/usr/bin/env python3
# Generates packaging/lsl-viewer.png (256x256): two random walks that cross, on white
# axis lines and a dark rounded tile. numpy -> raw RGBA -> ffmpeg.
#   uv run packaging/make_icon.py
# Then rebuild the Windows icon from it:
#   uv run packaging/make_ico.py
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
import argparse, subprocess, numpy as np
from pathlib import Path

HERE = Path(__file__).parent           # resolve output next to this script, not the CWD

# The seed sets the shape of the walks. The default is fixed, so the icon is the same on
# each run. To look at other shapes: --seed <n> --out <file>.
ap = argparse.ArgumentParser(description="Generate the application icon.")
ap.add_argument("--seed", type=int, default=7)
ap.add_argument("--out", type=Path, default=HERE / "lsl-viewer.png")
args = ap.parse_args()
SEED, OUT = args.seed, args.out

N  = 256                               # output size
SS = 4                                 # supersampling factor: the lines are drawn at SS*N, then averaged
S  = N * SS

MAGENTA = np.array([0xBF, 0x00, 0xBF], np.float32) / 255.0
BLUE    = np.array([0x00, 0x00, 0xFF], np.float32) / 255.0
WHITE   = np.array([1.0, 1.0, 1.0], np.float32)
# Near-black and neutral: a navy tile would take the contrast from the pure blue trace.
TILE    = np.array([0x12, 0x12, 0x16], np.float32) / 255.0

STEPS = 28                             # few, large steps: fine detail is lost at 16 px
ROUGH = 0.34                           # size of the random part against the trend

# All positions below are in units of the output image (0..256).
yy, xx = (np.mgrid[0:S, 0:S].astype(np.float32) + 0.5) / SS

# Rounded-rect tile (alpha = inside mask).
r, m = 46.0, 14.0                      # corner radius, margin
cx = np.clip(np.abs(xx - N / 2) - (N / 2 - m - r), 0, None)
cy = np.clip(np.abs(yy - N / 2) - (N / 2 - m - r), 0, None)
inside = (cx * cx + cy * cy) <= r * r

img = np.empty((S, S, 3), np.float32)
img[:] = TILE


def polyline(points, width, color):
    """Draw a polyline with round joins and caps: a pixel is in if it is near a segment."""
    d2 = np.full((S, S), np.inf, np.float32)
    for (x0, y0), (x1, y1) in zip(points[:-1], points[1:]):
        dx, dy = x1 - x0, y1 - y0
        t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / max(dx * dx + dy * dy, 1e-9), 0.0, 1.0)
        d2 = np.minimum(d2, (xx - (x0 + t * dx)) ** 2 + (yy - (y0 + t * dy)) ** 2)
    img[d2 <= (width / 2) ** 2] = color


# Plot area, and the two axis lines: the Y axis along its left side, and the X axis at
# mid-height, as for a signal that is centered on zero.
X0, X1, Y0, Y1 = 58.0, 212.0, 50.0, 206.0   # Y0 = top
YMID = (Y0 + Y1) / 2
polyline([(X0 - 12, Y0 - 8), (X0 - 12, Y1 + 8)], 7.0, WHITE)
polyline([(X0 - 12, YMID), (X1 + 8, YMID)], 7.0, WHITE)


def walk(rng, start, end):
    """A random walk from `start` to `end` (fractions of the plot height, 0 = bottom).

    The random part is a bridge (it is zero at both ends), so the ends stay where the
    trend puts them. One walk goes up and the other goes down, thus they must cross.
    """
    steps = rng.standard_normal(STEPS)
    path = np.concatenate([[0.0], np.cumsum(steps)])
    u = np.linspace(0.0, 1.0, STEPS + 1)
    bridge = path - u * path[-1]
    bridge *= ROUGH / np.abs(bridge).max()
    v = np.clip(start + u * (end - start) + bridge, 0.0, 1.0)
    return [(X0 + ui * (X1 - X0), Y1 - vi * (Y1 - Y0)) for ui, vi in zip(u, v)]


rng = np.random.default_rng(SEED)
polyline(walk(rng, 0.85, 0.15), 11.0, BLUE)      # high to low
polyline(walk(rng, 0.15, 0.85), 11.0, MAGENTA)   # low to high, on top

rgba = np.dstack([img, inside.astype(np.float32)])
rgba[..., :3] *= rgba[..., 3:4]                                   # premultiply, so the edge does not darken
rgba = rgba.reshape(N, SS, N, SS, 4).mean(axis=(1, 3))            # box filter down to N
alpha = rgba[..., 3:4]
rgba[..., :3] = np.where(alpha > 0, rgba[..., :3] / np.maximum(alpha, 1e-6), 0.0)

out = (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8).tobytes()
subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
                "-s", f"{N}x{N}", "-i", "-", str(OUT)], input=out, check=True)
print(f"wrote {OUT}")
