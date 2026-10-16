#!/usr/bin/env python3
"""Checks the application icons tools/make_icons.py writes into assets/brand.

icon.ico (Windows): the 16, 32, 64, 128 and 256 pixel icons, each a 32-bit RGBA
PNG whose size matches its directory entry, and each the same picture as the
brand PNG of that size.
icon.icns (macOS): every PNG type at its size, RGBA, and the picture drawn
inside a clear margin (the edge rows and columns of each image transparent).

    python3 tests/iconcheck.py
"""

import os
import struct
import sys
import zlib

BRAND = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "brand")
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
        brand = png(read(f"icon-{w}.png"))
        check(brand is not None and brand[3] == image[3], f"icon.ico {w}px is icon-{w}.png")
    check(sorted(sizes) == [16, 32, 64, 128, 256], f"icon.ico holds 16 to 256 pixels ({sorted(sizes)})")


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
checkIcns()
print(f"{len(failures)} failures")
sys.exit(1 if failures else 0)
