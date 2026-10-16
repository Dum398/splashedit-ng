#!/usr/bin/env python3
"""Regenerates the application icons from the brand PNGs in assets/brand:

  icon.ico   Windows: the 16, 32, 64, 128 and 256 PNGs as 32-bit RGBA PNGs
             (Windows does not draw palette PNGs inside an .ico).
  icon.icns  macOS: the icon drawn at 824/1024 of the canvas with a clear
             margin, the size macOS draws other app icons at in the Dock.

Both are written with ImageMagick (`magick` or `convert`); the .icns sizes are
drawn from icon-512.png.

    python3 tools/make_icons.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

BRAND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "brand")
ICO_SIZES = [16, 32, 64, 128, 256]
# (type, pixels). Every one of these holds a PNG.
ICNS_TYPES = [("icp4", 16), ("icp5", 32), ("ic11", 32), ("icp6", 64), ("ic12", 64), ("ic07", 128),
              ("ic08", 256), ("ic13", 256), ("ic09", 512), ("ic14", 512)]


def read(path):
    with open(path, "rb") as f:
        return f.read()


def rgba(tool, src, path):
    subprocess.run([tool, src, "-strip", "-define", "png:exclude-chunks=date,time", f"PNG32:{path}"], check=True)
    return read(path)


def ico(tool, sizes, tmp):
    pngs = [rgba(tool, os.path.join(BRAND, f"icon-{n}.png"), os.path.join(tmp, f"win-{n}.png")) for n in sizes]
    out = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    for n, png in zip(sizes, pngs):
        d = 0 if n == 256 else n
        out += struct.pack("<BBBBHHII", d, d, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    return out + b"".join(pngs)


def macPng(tool, n, tmp):
    body = round(n * 824 / 1024)
    path = os.path.join(tmp, f"mac-{n}.png")
    subprocess.run([tool, os.path.join(BRAND, "icon-512.png"), "-filter", "Lanczos", "-resize", f"{body}x{body}",
                    "-background", "none", "-gravity", "center", "-extent", f"{n}x{n}", "-strip",
                    "-define", "png:exclude-chunks=date,time", f"PNG32:{path}"], check=True)
    return read(path)


def icns(tool):
    with tempfile.TemporaryDirectory() as tmp:
        cache = {}
        body = b""
        for kind, n in ICNS_TYPES:
            if n not in cache:
                cache[n] = macPng(tool, n, tmp)
            body += kind.encode() + struct.pack(">I", 8 + len(cache[n])) + cache[n]
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def main():
    tool = shutil.which("magick") or shutil.which("convert")
    if not tool:
        sys.exit("make_icons.py needs ImageMagick (magick or convert) on PATH")
    with tempfile.TemporaryDirectory() as tmp, open(os.path.join(BRAND, "icon.ico"), "wb") as f:
        f.write(ico(tool, ICO_SIZES, tmp))
    with open(os.path.join(BRAND, "icon.icns"), "wb") as f:
        f.write(icns(tool))


if __name__ == "__main__":
    main()
