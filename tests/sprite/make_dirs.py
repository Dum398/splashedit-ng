# Writes dirs.png (committed; run again only to change it), the direction set
# checks' sheet: 64x64, 16x16 cells, 4 x 4 grid. Cells 2d and 2d+1 are direction
# d of an 8-direction set of 2 frames (n ne e se s sw w nw), a square filled with
# that direction's colour, so a screenshot tells which direction plays; the
# second frame has a dark bar. 11 colours (4 bpp holds 15 and transparent).
# usage: python3 make_dirs.py
import os, struct, zlib

here = os.path.dirname(os.path.abspath(__file__))
COLOURS = [(255, 40, 40), (255, 150, 0), (255, 240, 0), (40, 220, 40), (0, 230, 230), (40, 80, 255), (160, 60, 255), (255, 60, 200)]


def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def dirs(x, y):
    cell, cx, cy = (y // 16) * 4 + x // 16, x % 16, y % 16
    if not (2 <= cx <= 13 and 2 <= cy <= 13):
        return (0, 0, 0, 0)
    if cx in (2, 13) or cy in (2, 13):
        return (255, 255, 255, 255)
    if cell % 2 and cy == 7:
        return (20, 20, 20, 255)
    return COLOURS[cell // 2] + (255,)


png(os.path.join(here, 'dirs.png'), 64, 64, dirs)
print('wrote dirs.png')
