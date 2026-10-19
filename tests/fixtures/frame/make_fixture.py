# Writes frame.gpd beside this script: a hand-built GPU dump for
# framedebugcheck.py. A 64x48 drawing area at 0,0; the restore part uploads a
# 16-entry CLUT at 0,500 and a 16x16 4-bit texture at 64,0 (texel = (u + v)
# mod 16, entry 0 transparent); the frame then draws, in order:
#   #0 fill 64x48 (20,30,40)          #3 rect16, 4-bit raw texture, at 4,30
#   #1 flat red triangle 2,2 30,2 2,30 #4 semi-transparent (B+F) grey rect 24,26 20x12
#   #2 gouraud quad 34,2 .. 62,22      #5 yellow line 40,30 - 62,46
# usage: python3 -P make_fixture.py
import os, struct

MAGIC = b'PSXGPUDUMPv1r1\x00\x00'


def packet(kind, words):
    return [(kind << 24) | len(words)] + list(words)


def xy(x, y):
    return ((y & 0xffff) << 16) | (x & 0xffff)


def rgb15(r, g, b):
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10


def upload(x, y, w, h, halfwords):
    if len(halfwords) % 2:
        halfwords = halfwords + [0]
    return [0xa0000000, xy(x, y), xy(w, h)] + [halfwords[i] | halfwords[i + 1] << 16 for i in range(0, len(halfwords), 2)]


clut = [0] + [rgb15(16 * i, 255 - 16 * i, (64 + 32 * i) & 255) | 0 for i in range(1, 16)]
texels = [(u + v) & 15 for v in range(16) for u in range(16)]
tex = [texels[i] | texels[i + 1] << 4 | texels[i + 2] << 8 | texels[i + 3] << 12 for i in range(0, 256, 4)]

w = []
w += packet(0x06, [2])                       # GPUVersion
w += packet(0x12, [0x6e617248, 0x00646564])  # Comment "Handed"
w += packet(0x01, [0x00000000])              # reset
w += packet(0x00, upload(0, 500, 16, 1, clut) + upload(64, 0, 4, 16, tex))
w += packet(0x00, [0xe1000001, 0xe2000000, 0xe3000000, 0xe4000000 | (47 << 10) | 63, 0xe5000000, 0xe6000000])
w += packet(0x01, [0x03000000, 0x08000001, 0x06c60260, 0x07040010, 0x05000000])
w += packet(0x05, [])                        # TraceBegin
frame = [0x02402820, xy(0, 0), xy(64, 48)]
frame += [0x200000ff, xy(2, 2), xy(30, 2), xy(2, 30)]
frame += [0x380000ff, xy(34, 2), 0x0000ff00, xy(62, 2), 0x00ff0000, xy(34, 22), 0x00ffffff, xy(62, 22)]
frame += [0x7d000000, xy(4, 30), (500 << 6) << 16 | 0x0000]
# Split across two packets on purpose: a command may continue in the next GP0 packet.
w += packet(0x00, frame + [0xe1000021, 0x62808080])
w += packet(0x00, [xy(24, 26), xy(20, 12), 0x4000ffff, xy(40, 30), xy(62, 46)])
w += packet(0x02, [0x00000000, 0x00000000])  # VSync
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'frame.gpd')
open(out, 'wb').write(MAGIC + struct.pack('<%dI' % len(w), *w))
print(out, os.path.getsize(out), 'bytes')
