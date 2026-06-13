#!/usr/bin/env python3
# Generates packaging/lsl-viewer.png (256x256) — a spectrum-bars motif on a dark
# rounded tile, matching the viewer's spectral/EEG focus. numpy -> raw RGBA -> ffmpeg.
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy"]
# ///
import subprocess, numpy as np

N = 256
img = np.zeros((N, N, 4), np.float32)

# rounded-rect dark tile (alpha = inside mask), subtle vertical gradient
yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
r, m = 46.0, 14.0                       # corner radius, margin
cx = np.clip(np.abs(xx - (N-1)/2) - ((N-1)/2 - m - r), 0, None)
cy = np.clip(np.abs(yy - (N-1)/2) - ((N-1)/2 - m - r), 0, None)
inside = (cx*cx + cy*cy) <= r*r
g = 0.06 + 0.05 * (yy / N)
img[..., 0] = 0.07; img[..., 1] = 0.09 + g*0.2; img[..., 2] = 0.13 + g
img[..., 3] = inside.astype(np.float32)

# viridis-ish stops (blue -> teal -> green -> yellow)
stops = np.array([[0.27,0.00,0.33],[0.13,0.36,0.55],[0.12,0.56,0.55],
                  [0.21,0.72,0.47],[0.60,0.84,0.20],[0.99,0.91,0.14]], np.float32)
def ramp(t):
    t = np.clip(t, 0, 1) * (len(stops)-1); i = np.floor(t).astype(int)
    i = np.clip(i, 0, len(stops)-2); f = (t-i)[..., None]
    return stops[i]*(1-f) + stops[i+1]*f

# equalizer bars with rounded tops
heights = np.array([0.40, 0.66, 0.52, 0.86, 0.62, 0.95, 0.55, 0.74, 0.46])
nb = len(heights); pad, gap = 40, 8
bw = (N - 2*pad - (nb-1)*gap) / nb
base = N - 52
for k, h in enumerate(heights):
    x0 = pad + k*(bw+gap); x1 = x0 + bw
    top = base - h*(base-58)
    col = ramp(np.full((N, N), 1 - (yy-top)/(base-top)))   # color by height within bar
    sel = (xx >= x0) & (xx < x1) & (yy >= top) & (yy <= base)
    # round the top corners of each bar
    rr = bw/2
    near_top = yy < top + rr
    bx = np.clip(np.abs(xx-(x0+x1)/2) - (bw/2 - rr), 0, None)
    by = np.clip((top+rr) - yy, 0, None)
    sel &= ~(near_top & (bx*bx + by*by > rr*rr))
    for c in range(3): img[..., c] = np.where(sel & inside, col[..., c], img[..., c])

out = (np.clip(img, 0, 1)*255).astype(np.uint8).tobytes()
subprocess.run(["ffmpeg","-v","error","-y","-f","rawvideo","-pix_fmt","rgba",
                "-s",f"{N}x{N}","-i","-","packaging/lsl-viewer.png"], input=out, check=True)
print("wrote packaging/lsl-viewer.png")
