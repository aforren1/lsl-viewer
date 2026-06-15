#!/usr/bin/env python3
# Builds packaging/lsl-viewer.ico (multi-resolution, PNG-compressed entries) from
# packaging/lsl-viewer.png, using ffmpeg to rescale. Run after regenerating the PNG:
#   uv run packaging/make_ico.py
# The .ico is compiled into lsl_viewer.exe via packaging/lsl-viewer.rc (see CMakeLists).
import struct, subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "lsl-viewer.png"
OUT = HERE / "lsl-viewer.ico"
SIZES = [16, 32, 48, 64, 128, 256]   # standard Windows icon sizes


def scaled_png(size: int) -> bytes:
    # High-quality downscale (Lanczos) straight to PNG on stdout.
    return subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(SRC),
         "-vf", f"scale={size}:{size}:flags=lanczos", "-f", "image2pipe",
         "-vcodec", "png", "-"],
        check=True, capture_output=True).stdout


def main():
    imgs = [(s, scaled_png(s)) for s in SIZES]
    # ICONDIR header, then one ICONDIRENTRY per image, then the PNG payloads.
    offset = 6 + 16 * len(imgs)
    header = struct.pack("<HHH", 0, 1, len(imgs))          # reserved, type=icon, count
    entries, payload = b"", b""
    for size, data in imgs:
        b = size & 0xFF                                    # 256 is encoded as 0
        entries += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        payload += data
        offset += len(data)
    OUT.write_bytes(header + entries + payload)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, sizes {SIZES})")


if __name__ == "__main__":
    main()
