#!/usr/bin/env python3
"""Regenerates the application icons in assets/brand:

  icon-16.png, icon-24.png    hand-pixelled, from tools/small_icons.py.
  icon-64.png, icon-128.png,  icon.svg with the plate's corners rounded the
  icon-256.png                way Windows 11 rounds app icons (a radius of
                              3/32 of the size, where icon.svg has 22%).
  tile-150.png, tile-70.png   the S and PSX of icon.svg without the plate,
                              for the Start tiles of
                              packaging/windows/splashedit.VisualElementsManifest.xml.
  icon.ico    Windows: the 16, 24, 32, 48, 64, 128 and 256 PNGs as 32-bit
              RGBA PNGs (Windows does not draw palette PNGs inside an .ico).
  icon.icns   macOS: the icon drawn at 824/1024 of the canvas with a clear
              margin, the size macOS draws other app icons at in the Dock.

icon-32.png and icon-48.png are drawn by hand and icon-512.png (which keeps
icon.svg's 22% corners, the macOS shape) is left as it is; the .icns sizes are
drawn from icon-512.png. The SVGs are drawn with rsvg-convert, the rest with
ImageMagick (`magick` or `convert`).

    python3 tools/make_icons.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import small_icons  # noqa: E402

BRAND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "brand")
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
PLATE_SIZES = [64, 128, 256]
# icon.svg's plate, a 512 square with 22% corners, and the Windows 11 radius: 3/32 of the size.
PLATE = '<rect width="512" height="512" rx="112.64" fill="#ffd23f"/>'
WINDOWS_RX = 512 * 3 / 32
# (file, tile size, size of the art's longer side on it)
TILES = [("tile-150.png", 150, 96), ("tile-70.png", 70, 46)]
# (type, pixels). Every one of these holds a PNG.
ICNS_TYPES = [("icp4", 16), ("icp5", 32), ("ic11", 32), ("icp6", 64), ("ic12", 64), ("ic07", 128),
              ("ic08", 256), ("ic13", 256), ("ic09", 512), ("ic14", 512)]


def read(path):
    with open(path, "rb") as f:
        return f.read()


def rgba(tool, src, path):
    subprocess.run([tool, src, "-strip", "-define", "png:exclude-chunks=date,time", f"PNG32:{path}"], check=True)
    return read(path)


def svg(rect):
    """icon.svg with its plate replaced by rect."""
    with open(os.path.join(BRAND, "icon.svg"), encoding="utf-8") as f:
        text = f.read()
    assert text.count(PLATE) == 1, "icon.svg's plate is not " + PLATE
    return text.replace(PLATE, rect).encode()


def rsvg(text, n, path):
    subprocess.run(["rsvg-convert", "-w", str(n), "-h", str(n), "-o", path], input=text, check=True)


def plates(tool, tmp):
    text = svg(PLATE.replace('rx="112.64"', f'rx="{WINDOWS_RX:g}"'))
    for n in PLATE_SIZES:
        path = os.path.join(tmp, f"plate-{n}.png")
        rsvg(text, n, path)
        rgba(tool, path, os.path.join(BRAND, f"icon-{n}.png"))


def tiles(tool, tmp):
    path = os.path.join(tmp, "art.png")
    rsvg(svg(""), 1024, path)
    for name, n, body in TILES:
        subprocess.run([tool, path, "-trim", "+repage", "-filter", "Lanczos", "-resize", f"{body}x{body}",
                        "-background", "none", "-gravity", "center", "-extent", f"{n}x{n}", "-strip",
                        "-define", "png:exclude-chunks=date,time", f"PNG32:{os.path.join(BRAND, name)}"], check=True)


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
    if not shutil.which("rsvg-convert"):
        sys.exit("make_icons.py needs rsvg-convert (librsvg) on PATH")
    small_icons.write()
    with tempfile.TemporaryDirectory() as tmp:
        plates(tool, tmp)
        tiles(tool, tmp)
    with tempfile.TemporaryDirectory() as tmp, open(os.path.join(BRAND, "icon.ico"), "wb") as f:
        f.write(ico(tool, ICO_SIZES, tmp))
    with open(os.path.join(BRAND, "icon.icns"), "wb") as f:
        f.write(icns(tool))


if __name__ == "__main__":
    main()
