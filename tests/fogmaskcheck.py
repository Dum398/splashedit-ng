# Fog mask CLUTs. A fogged scene with a 4bpp, an 8bpp and a 16bpp cutout quad
# exports one 16-entry and one 256-entry mask CLUT ({0x0000, 0xFFFF, ...}) into
# the .vram, after the texture CLUTs and clear of them, and writes their CLUT
# words into the header (offsets 58 and 66). The same scene with fog off is the
# control: both words 0 and no extra CLUT.
# usage: python3 fogmaskcheck.py <splashpack-cli>
import json, os, struct, subprocess, sys, tempfile, zlib

cli = os.path.abspath(sys.argv[1])
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def write_png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    chunk = lambda k, d: struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def export(fog):
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, 'meshes'))
    os.makedirs(os.path.join(root, 'textures'))
    write_png(os.path.join(root, 'textures', 'cut.png'), 32, 32,
              lambda x, y: (0, 0, 0, 0) if ((x // 8) + (y // 8)) % 2 else (230, 40, 30, 255))
    mesh = {'format': 'splashedit-ng/mesh', 'version': 1,
            'positions': [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'normals': [0, 0, -1] * 4,
            'uv': [0, 0, 1, 0, 1, 1, 0, 1], 'submeshes': [[0, 1, 2, 0, 2, 3]]}
    json.dump(mesh, open(os.path.join(root, 'meshes', 'quad.mesh'), 'w'))
    objs = [{'name': 'Q%d' % bpp, 'active': True,
             'transform': {'position': [i * 2.5, 0, 6], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
             'components': [{'type': 'mesh', 'mesh': 'meshes/quad.mesh', 'bitDepth': bpp, 'vertexColors': 'flat',
                             'materials': [{'texture': 'textures/cut.png', 'color': [1, 1, 1, 1]}]}],
             'children': []} for i, bpp in enumerate((4, 8, 16))]
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': '', 'script': None,
                          'fog': {'enabled': fog, 'color': [0.5, 0.5, 0.6], 'density': 5}},
             'objects': objs}
    json.dump(scene, open(os.path.join(root, 'fog.scene'), 'w'))
    out = os.path.join(root, 'out.splashpack')
    r = subprocess.run([cli, 'export', os.path.join(root, 'fog.scene'), '-o', out], capture_output=True, text=True)
    check('export (fog %s) rc 0' % fog, r.returncode == 0, r.stdout + r.stderr)
    pack = open(out, 'rb').read()
    vram = open(out[:-len('.splashpack')] + '.vram', 'rb').read()
    return pack, vram


def cluts(pack, vram):
    """The .vram CLUT records as [(x, y, words)]."""
    na, nc = struct.unpack_from('<HH', vram, 2)
    p = 8
    for _ in range(na):
        w, h = struct.unpack_from('<HH', vram, p + 4)
        p = (p + 8 + w * h * 2 + 3) & ~3
    out = []
    for _ in range(nc):
        cx, cy, n = struct.unpack_from('<HHH', vram, p)
        out.append((cx * 16, cy, list(struct.unpack_from('<%dH' % n, vram, p + 8))))
        p = (p + 8 + n * 2 + 3) & ~3
    return struct.unpack_from('<H', pack, 10)[0], out


pack, vram = export(True)
w4, w8 = struct.unpack_from('<H', pack, 58)[0], struct.unpack_from('<H', pack, 66)[0]
headerCluts, recs = cluts(pack, vram)
check('fog on: both header words set', w4 != 0 and w8 != 0, (w4, w8))
check('fog on: .vram has the header CLUTs plus 2', len(recs) == headerCluts + 2, (len(recs), headerCluts))
masks = recs[headerCluts:]
for word, n in ((w4, 16), (w8, 256)):
    m = [r for r in masks if r[0] == (word & 63) * 16 and r[1] == word >> 6]
    check('fog on: mask at word 0x%04x has %d entries' % (word, n), len(m) == 1 and len(m[0][2]) == n, m and len(m[0][2]))
    check('fog on: mask at word 0x%04x is {0x0000, 0xFFFF...}' % word,
          len(m) == 1 and m[0][2][0] == 0 and all(c == 0xFFFF for c in m[0][2][1:]))
spans = [(x, y, len(c)) for x, y, c in recs]
overlap = [(a, b) for i, a in enumerate(spans) for b in spans[i + 1:]
           if a[1] == b[1] and a[0] < b[0] + b[2] and b[0] < a[0] + a[2]]
check('fog on: no two CLUTs overlap', not overlap, overlap)

pack, vram = export(False)
headerCluts, recs = cluts(pack, vram)
check('fog off: header words 0', struct.unpack_from('<H', pack, 58)[0] == 0 and struct.unpack_from('<H', pack, 66)[0] == 0)
check('fog off: no extra CLUT', len(recs) == headerCluts, (len(recs), headerCluts))

print('%d failed' % fails if fails else 'all checks passed')
sys.exit(1 if fails else 0)
