#!/usr/bin/env python3
"""Builds packaging/macos/tandem-fm.icns from the app icons in data/.

Like Apple's iconutil, which only runs on macOS: 16 and 32 px as ARGB, the
rest as PNG. 1024 px is rendered from the SVG. Needs Pillow and rsvg-convert.

    python3 packaging/macos/make_icns.py
"""
import io
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parents[2]
icons = root / "data" / "icons"


def png(size):
    if size == 1024:
        data = subprocess.run(["rsvg-convert", "-w", "1024", "-h", "1024", str(root / "data" / "tandem-fm.svg")],
                              check=True, capture_output=True).stdout
        image = Image.open(io.BytesIO(data))
    else:
        image = Image.open(icons / f"{size}x{size}" / "tandem-fm.png")
    image = image.convert("RGBA")
    if image.size != (size, size):
        sys.exit(f"{size} px icon is {image.size}")
    return image


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, "PNG", optimize=True)
    return out.getvalue()


def rle(channel):
    """The icns run-length encoding: runs of 3-130 equal bytes, literals of 1-128."""
    out, i, n = bytearray(), 0, len(channel)
    while i < n:
        run = 1
        while i + run < n and run < 130 and channel[i + run] == channel[i]:
            run += 1
        if run >= 3:
            out += bytes([run + 125, channel[i]])
            i += run
            continue
        start = i
        while i < n and i - start < 128:
            if i + 2 < n and channel[i] == channel[i + 1] == channel[i + 2]:
                break
            i += 1
        out.append(i - start - 1)
        out += channel[start:i]
    return bytes(out)


def argb(image):
    a, r, g, b = (c.tobytes() for c in image.split()[3:] + image.split()[:3])
    return b"ARGB" + b"".join(rle(c) for c in (a, r, g, b))


# (type, pixels, encoding)
entries = [
    (b"ic04", 16, argb), (b"ic05", 32, argb),
    (b"ic11", 32, png_bytes), (b"ic12", 64, png_bytes),
    (b"ic07", 128, png_bytes), (b"ic13", 256, png_bytes),
    (b"ic08", 256, png_bytes), (b"ic14", 512, png_bytes),
    (b"ic09", 512, png_bytes), (b"ic10", 1024, png_bytes),
]
chunks = b"".join(kind + struct.pack(">I", len(data) + 8) + data
                  for kind, size, encode in entries for data in [encode(png(size))])
target = root / "packaging" / "macos" / "tandem-fm.icns"
target.write_bytes(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)
print(f"{target.relative_to(root)}: {len(entries)} sizes")
