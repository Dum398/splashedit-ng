#!/usr/bin/env python3
"""Writes the hand-pixelled small application icons, assets/brand/icon-16.png
and icon-24.png, from the grids below (one character per pixel).

At these sizes the yellow plate is left off, as on icon-32.png and icon-48.png:
the outlined red S stands on its own on any taskbar or Explorer background.
icon-16.png drops "PSX", which cannot be read at 16 pixels.

    python3 tools/small_icons.py [output directory]

tools/make_icons.py runs this before it builds icon.ico.
"""

import os
import struct
import sys
import zlib

BRAND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "brand")

# The colours of icon-32.png and icon-48.png.
PALETTE = {
    ".": (0, 0, 0, 0),
    "#": (0x16, 0x12, 0x2B, 255),  # outline
    "R": (0xFF, 0x3B, 0x3B, 255),  # S
    "W": (0xFF, 0xFF, 0xFF, 255),  # PSX
}

ICON_16 = [
    "................",
    "...##########...",
    "..#RRRRRRRRRR#..",
    ".#RRRRRRRRRRR##.",
    ".#RRRR#########.",
    ".#RRRR#########.",
    ".#RRRRRRRRRR###.",
    ".##RRRRRRRRRRR#.",
    ".###RRRRRRRRRR#.",
    ".#########RRRR#.",
    ".#########RRRR#.",
    ".#RRRRRRRRRRRR#.",
    ".#RRRRRRRRRRR##.",
    ".##RRRRRRRRR##..",
    "..###########...",
    "................",
]

ICON_24 = [
    "........................",
    "........................",
    "......###########.......",
    ".....#WWW#WWW#W#W#......",
    ".....#W#W#W###W#W#......",
    ".....#WWW#WWW##W##......",
    ".....#W#####W#W#W#......",
    ".....#W###WWW#W#W#......",
    ".....#############......",
    ".....#RRRRRRRRRRR##.....",
    ".....#RRRRRRRRRRR##.....",
    ".....#RRRR#########.....",
    ".....#RRRR#########.....",
    ".....#RRRRRRRRRRR##.....",
    ".....##RRRRRRRRRRR#.....",
    ".....###RRRRRRRRRR#.....",
    ".....#########RRRR#.....",
    ".....#########RRRR#.....",
    ".....#RRRRRRRRRRRR#.....",
    ".....#RRRRRRRRRRR##.....",
    ".....##RRRRRRRRR###.....",
    "......############......",
    "........................",
    "........................",
]


def encode(grid):
    """The grid as PNG bytes: 8-bit RGBA, no ancillary chunks."""
    size = len(grid)
    assert all(len(row) == size for row in grid), f"{size}px grid is not square"
    raw = b"".join(b"\0" + b"".join(bytes(PALETTE[c]) for c in row) for row in grid)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def write(out=BRAND):
    for size, grid in ((16, ICON_16), (24, ICON_24)):
        assert len(grid) == size, f"icon-{size} grid has {len(grid)} rows"
        with open(os.path.join(out, f"icon-{size}.png"), "wb") as f:
            f.write(encode(grid))


if __name__ == "__main__":
    write(sys.argv[1] if len(sys.argv) > 1 else BRAND)
