#!/usr/bin/env python3
"""Checks the application icons tools/make_icons.py writes into assets/brand.

icon.ico (Windows): the 16, 24, 32, 48, 64, 128 and 256 pixel icons, each a
32-bit RGBA PNG whose size matches its directory entry, and each the same
picture as the brand PNG of that size.
The brand PNGs: 16 to 48 pixels have no plate (clear corners, the S in the
middle, edges clear), 16 and 24 are what tools/small_icons.py draws; 64 to 256 have the
Windows 11 corner radius, 3/32 of the size, so the top row is opaque from 12%
of the way in, where icon.svg's 22% corners are still clear; icon-512.png (for
macOS) keeps the 22% corners. tile-150.png and tile-70.png, the Start tile
logos, are the art alone on a clear background.
icon.icns (macOS): every PNG type at its size, RGBA, and the picture drawn
inside a clear margin (the edge rows and columns of each image transparent).

    python3 tests/iconcheck.py
"""

import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import small_icons  # noqa: E402

BRAND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "brand")
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
SMALL_SIZES = [16, 24, 32, 48]
PLATE_SIZES = [64, 128, 256]
ICNS_SIZES = {"icp4": 16, "icp5": 32, "ic11": 32, "icp6": 64, "ic12": 64, "ic07": 128, "ic08": 256,
              "ic13": 256, "ic09": 512, "ic14": 512}
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def png(data):
    """(width, height, rows of RGBA tuples), or None when it is not a PNG this can read."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    i, idat, palette, alpha = 8, b"", None, None
    while i < len(data):
        n, kind = struct.unpack(">I4s", data[i:i + 8])
        body = data[i + 8:i + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[j:j + 3]) for j in range(0, len(body), 3)]
        elif kind == b"tRNS":
            alpha = list(body)
        elif kind == b"IDAT":
            idat += body
        i += 12 + n
    if depth != 8 or interlace or ctype not in (3, 6):
        return None
    bpp = 4 if ctype == 6 else 1
    raw, stride, prev, rows, p = zlib.decompress(idat), w * bpp, bytearray(w * bpp), [], 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if ctype == 6:
            rows.append([tuple(line[x:x + 4]) for x in range(0, stride, 4)])
        else:
            rows.append([palette[k] + ((alpha[k] if alpha and k < len(alpha) else 255),) for k in line])
    return w, h, ctype, rows


def read(name):
    with open(os.path.join(BRAND, name), "rb") as f:
        return f.read()


def checkIco():
    data = read("icon.ico")
    reserved, kind, count = struct.unpack("<HHH", data[:6])
    check(reserved == 0 and kind == 1, "icon.ico is an icon file")
    sizes = []
    for k in range(count):
        w, h, _, _, planes, bits, n, off = struct.unpack("<BBBBHHII", data[6 + 16 * k:22 + 16 * k])
        w, h = w or 256, h or 256
        sizes.append(w)
        image = png(data[off:off + n])
        check(image is not None and image[2] == 6, f"icon.ico {w}px is an RGBA PNG")
        if image is None:
            continue
        check(image[:2] == (w, h) and bits == 32, f"icon.ico {w}px entry matches its image ({image[0]}x{image[1]})")
        drawn = brand(f"icon-{w}.png")
        check(drawn is not None and drawn[3] == image[3], f"icon.ico {w}px is icon-{w}.png")
    check(sorted(sizes) == ICO_SIZES, f"icon.ico holds {ICO_SIZES} pixels ({sorted(sizes)})")


def brand(name):
    """The brand PNG's image, or None when it is missing or not a PNG this check reads."""
    return png(read(name)) if os.path.isfile(os.path.join(BRAND, name)) else None


def corners(rows):
    return [rows[0][0], rows[0][-1], rows[-1][0], rows[-1][-1]]


def topRow(rows, x):
    """Alpha at x pixels in from each end of the top and bottom rows."""
    return [rows[y][c][3] for y in (0, -1) for c in (x, -1 - x)]


def checkBrand():
    for n in SMALL_SIZES:
        image = brand(f"icon-{n}.png")
        if image is None or image[:2] != (n, n):
            check(False, f"icon-{n}.png is a {n}px PNG this check reads")
            continue
        rows = image[3]
        edge = rows[0] + rows[-1] + [r[0] for r in rows] + [r[-1] for r in rows]
        check(all(px[3] == 0 for px in corners(rows)) and all(px[3] == 0 for px in edge),
              f"icon-{n}.png has no plate (clear corners and edges)")
        middle = [px for row in rows[n // 4:n - n // 4] for px in row[n // 4:n - n // 4]]
        check(any(px[3] == 255 for px in middle), f"icon-{n}.png is drawn in the middle")
    for n, grid in ((16, small_icons.ICON_16), (24, small_icons.ICON_24)):
        image = brand(f"icon-{n}.png")
        drawn = [[small_icons.PALETTE[c] for c in row] for row in grid]
        check(image is not None and image[3] == drawn, f"icon-{n}.png is what tools/small_icons.py draws")
    for n in PLATE_SIZES + [512]:
        image = brand(f"icon-{n}.png")
        if image is None or image[:2] != (n, n):
            check(False, f"icon-{n}.png is a {n}px PNG this check reads")
            continue
        rows, x = image[3], math.ceil(n * 0.12)
        check(all(px[3] == 0 for px in corners(rows)), f"icon-{n}.png has round corners")
        check(rows[n // 2][n // 2][3] == 255, f"icon-{n}.png is drawn in the middle")
        if n == 512:
            check(all(a == 0 for a in topRow(rows, x)), "icon-512.png keeps icon.svg's 22% corners for macOS")
        else:
            check(all(a == 255 for a in topRow(rows, x)),
                  f"icon-{n}.png has the Windows 11 corner radius (opaque {x}px in on the edge rows: {topRow(rows, x)})")
    for name, n in (("tile-150.png", 150), ("tile-70.png", 70)):
        image = brand(name)
        if image is None or image[:3] != (n, n, 6):
            check(False, f"{name} is a {n}px RGBA PNG")
            continue
        rows = image[3]
        edge = rows[0] + rows[-1] + [r[0] for r in rows] + [r[-1] for r in rows]
        check(all(px[3] == 0 for px in edge), f"{name} is the art alone, with a clear margin")
        check(any(px[3] == 255 for row in rows for px in row), f"{name} has the art on it")


def checkIcns():
    data = read("icon.icns")
    check(data[:4] == b"icns" and struct.unpack(">I", data[4:8])[0] == len(data), "icon.icns is an icns file")
    i, seen = 8, {}
    while i + 8 <= len(data):
        kind, n = data[i:i + 4].decode("latin-1"), struct.unpack(">I", data[i + 4:i + 8])[0]
        seen[kind] = png(data[i + 8:i + n])
        i += max(n, 8)
    check(sorted(seen) == sorted(ICNS_SIZES), f"icon.icns holds {sorted(ICNS_SIZES)} ({sorted(seen)})")
    for kind, size in ICNS_SIZES.items():
        image = seen.get(kind)
        if image is None:
            check(False, f"icon.icns {kind} is a PNG this check reads")
            continue
        w, h, ctype, rows = image
        check((w, h, ctype) == (size, size, 6), f"icon.icns {kind} is {size}px RGBA ({w}x{h})")
        edge = rows[0] + rows[-1] + [r[0] for r in rows] + [r[-1] for r in rows]
        check(all(px[3] == 0 for px in edge), f"icon.icns {kind} has a clear margin")
        check(rows[h // 2][w // 2][3] == 255, f"icon.icns {kind} is drawn in the middle")


checkIco()
checkBrand()
checkIcns()
print(f"{len(failures)} failures")
sys.exit(1 if failures else 0)
