# Writes the sprite fixture's two sheets (committed; run again only to change them):
#   hero.png  64x32, 16x16 cells, 4 x 2 grid: a body whose bar walks right one
#             cell to the next, row 2 a jump (the body rises)
#   fx.png    32x16, 8x8 cells, 4 x 2 grid: a spark that grows cell by cell
# Few colours (4 bpp holds 15 and transparent), transparent outside the shapes.
# usage: python3 make_sheets.py
import os, struct, zlib

here = os.path.dirname(os.path.abspath(__file__))


def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


CLEAR = (0, 0, 0, 0)


def hero(x, y):
    cell, cx, cy = (y // 16) * 4 + x // 16, x % 16, y % 16
    lift = 0 if cell < 4 else (cell - 4) * 2  # the jump row rises
    top, bottom = 3 - lift + 2, 15 - lift
    if not (3 <= cx <= 12 and top <= cy <= bottom):
        return CLEAR
    if cx == 3 or cx == 12 or cy == top or cy == bottom:
        return (255, 255, 255, 255)  # outline
    if cx == 4 + (cell % 4) * 2:
        return (255, 220, 40, 255)  # the walking bar
    return (40, 200, 90, 255) if cell < 4 else (60, 120, 255, 255)


def fx(x, y):
    cell, cx, cy = (y // 8) * 4 + x // 8, x % 8, y % 8
    r = 1 + cell // 2  # grows every other cell
    if abs(cx - 3.5) <= r / 2 + 0.5 and abs(cy - 3.5) <= r / 2 + 0.5:
        return (255, 80, 200, 255) if (cx + cy) % 2 else (255, 255, 120, 255)
    return CLEAR


png(os.path.join(here, 'hero.png'), 64, 32, hero)
png(os.path.join(here, 'fx.png'), 32, 16, fx)
print('wrote hero.png fx.png')
