# Writes the tilemap fixture's tileset (committed; run again only to change it):
#   tiles.png  64x32, 16x16 cells, 4 x 2 grid, opaque, few colours (4 bpp):
#     0 floor  dark green with a lighter speck pattern
#     1 wall   red bricks
#     2 task   floor with a yellow diamond (an object tile)
#     3 water  blue with a light wave
#     4..7     the same four with a white border, so a cell index off by 4 shows
# usage: python3 make_tiles.py
import os, struct, zlib

here = os.path.dirname(os.path.abspath(__file__))


def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def tile(x, y):
    cell, cx, cy = (y // 16) * 4 + x // 16, x % 16, y % 16
    if cell >= 4 and (cx in (0, 15) or cy in (0, 15)):
        return (240, 240, 240, 255)
    kind = cell % 4
    if kind == 0 or kind == 2:
        if kind == 2 and abs(cx - 7.5) + abs(cy - 7.5) <= 5:
            return (250, 210, 40, 255)
        return (90, 150, 80, 255) if (cx * 3 + cy * 5) % 7 == 0 else (40, 90, 50, 255)
    if kind == 1:
        row = cy // 4
        mortar = cy % 4 == 3 or (cx + (4 if row % 2 else 0)) % 8 == 7
        return (60, 50, 50, 255) if mortar else (170, 60, 50, 255)
    wave = (cy - 6) == (2 if (cx // 4) % 2 else 0)
    return (170, 210, 250, 255) if wave else (40, 80, 170, 255)


png(os.path.join(here, 'tiles.png'), 64, 32, tile)
