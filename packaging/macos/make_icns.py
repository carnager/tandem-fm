#!/usr/bin/env python3
"""Builds packaging/macos/tandem-fm.icns from data/tandem-fm.svg.

macOS app icons sit on a rounded square tile (Apple's grid: 824 of 1024 px,
with a soft shadow), so the glyph is placed on one. Every size is rendered
from that.

Like Apple's iconutil, which only runs on macOS: 16 and 32 px as ARGB, the
rest as PNG. Needs Pillow and rsvg-convert.

    python3 packaging/macos/make_icns.py
"""
import io
import re
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parents[2]
glyph = re.sub(r"(?s)^.*?<svg[^>]*>|</svg>\s*$", "", (root / "data" / "tandem-fm.svg").read_text())
tile = f"""<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#dfe3ea"/>
    </linearGradient>
    <filter id="shadow" x="-10%" y="-10%" width="120%" height="125%">
      <feDropShadow dx="0" dy="10" stdDeviation="12" flood-color="#000" flood-opacity="0.3"/>
    </filter>
  </defs>
  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg)" filter="url(#shadow)"/>
  <g transform="translate(147 150) scale(2.85)">{glyph}</g>
</svg>"""


def png(size):
    data = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size)],
                          input=tile.encode(), check=True, capture_output=True).stdout
    image = Image.open(io.BytesIO(data)).convert("RGBA")
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
