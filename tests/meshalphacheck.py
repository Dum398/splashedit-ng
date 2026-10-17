# A mesh texture with an alpha hole keeps it, in the editor and on the
# console. A fresh project with one quad (a red 32x32 texture whose middle
# 16x16 texels are alpha 0) in front of a green background: the viewport shows
# green through the hole and red around it, the mesh inspector prints
# `texture: holes yes <path>` and draws the obj.texholes caption, and
# splashpack-cli exports the texture cutout with the hole texels as VRAM word
# 0x0000 (what the GPU skips). The same quad with an opaque texture is the
# control: `texture: holes no`, no caption, no hole in the viewport or VRAM.
# usage: python3 meshalphacheck.py <splashedit> <splashpack-cli> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2])
keep_shot = sys.argv[3] if len(sys.argv) > 3 else None
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
GREEN = (0x20, 0xc0, 0x40)
N, HOLE = 32, range(8, 24)  # texture size; the hole's texel rows and columns


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def write_png(path, w, h, px):
    """8-bit RGBA PNG; px(x, y) -> (r, g, b, a), y = 0 the top row."""
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    chunk = lambda k, d: struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def read_png(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGBA or RGB PNG."""
    data = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            bpp = 4 if ctype == 6 else 3
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[i] for i in range(stride) if i % bpp < 3))
        prev = line
    return w, h, rows


def read_vram(path):
    """The .vram file as a 1024 x 512 word list (atlases, then CLUTs)."""
    d = open(path, 'rb').read()
    u16 = lambda p: d[p] | d[p + 1] << 8 if p + 1 < len(d) else 0
    vram = [0] * (1024 * 512)
    if d[:2] != b'VR': return None
    na, nc, p = u16(2), u16(4), 8
    for _ in range(na):
        x, y, w, h = u16(p), u16(p + 2), u16(p + 4), u16(p + 6)
        p += 8
        for r in range(h):
            for c in range(w):
                if x + c < 1024 and y + r < 512: vram[(y + r) * 1024 + x + c] = u16(p)
                p += 2
        p = (p + 3) & ~3
    return vram


def project(holes):
    """A temp project: meshalpha.scene with a 4 x 4 quad named Quad, textured
    by textures/quad.png (16 bpp), on a green background."""
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, 'meshes'))
    os.makedirs(os.path.join(root, 'textures'))
    write_png(os.path.join(root, 'textures', 'quad.png'), N, N,
              lambda x, y: (255, 0, 0, 0 if holes and x in HOLE and y in HOLE else 255))
    mesh = {'format': 'splashedit-ng/mesh', 'version': 1,
            'positions': [-2, -2, 0, 2, -2, 0, 2, 2, 0, -2, 2, 0],
            'normals': [0, 0, -1] * 4,
            'uv': [0, 0, 1, 0, 1, 1, 0, 1],
            'submeshes': [[0, 1, 2, 0, 2, 3]]}
    json.dump(mesh, open(os.path.join(root, 'meshes', 'quad.mesh'), 'w'), indent=1)
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': '',
                          'fog': {'enabled': False, 'color': [c / 255 for c in GREEN], 'density': 5}},
             'objects': [{'name': 'Quad', 'active': True,
                          'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                          'components': [{'type': 'mesh', 'mesh': 'meshes/quad.mesh', 'bitDepth': 16, 'vertexColors': 'flat',
                                          'materials': [{'texture': 'textures/quad.png', 'color': [1, 1, 1, 1]}]}],
                          'children': []}]}
    json.dump(scene, open(os.path.join(root, 'meshalpha.scene'), 'w'), indent=1)
    return root


def run(root, shot):
    """The editor on `root`, Quad selected, overlays off; returns the
    named controls and the output lines."""
    # --clean: perspective-correct UVs, so the hole is the quad's middle
    # square on screen too (affine UVs on two big triangles skew it).
    cmd = [editor, '--size', '1600x960', '--project', root, '--select', 'Quad', '--no-overlays', '--clean', '--wait', '10',
           '--move', '760,940', '--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
    lines = (r.stdout + r.stderr).strip().splitlines()
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+(?: \S+)*) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return rects, [l for l in lines if not l.startswith('control ')]


def viewport(rects, shot):
    """(pixels around the centre, share of red pixels) of the viewport in the
    screenshot. The camera frames the quad's middle, the hole's middle; the
    four pixels sit 30 px off it diagonally, clear of the selection gizmo."""
    v = rects.get('vp.view')
    if not v or not os.path.exists(shot): return None, 0
    _, _, rows = read_png(shot)
    cx, cy = (v[0] + v[2]) // 2, (v[1] + v[3]) // 2
    centre = [tuple(rows[cy + dy][3 * (cx + dx):3 * (cx + dx) + 3]) for dx in (-30, 30) for dy in (-30, 30)]
    n = red = 0
    for y in range(v[1] + 4, v[3] - 4, 4):
        for x in range(v[0] + 4, v[2] - 4, 4):
            p = rows[y][3 * x:3 * x + 3]
            n += 1
            red += p[0] > 160 and p[1] < 60 and p[2] < 60
    return centre, red / max(n, 1)


def exported(root):
    """splashpack-cli export: (cutout flag, hole words that are 0, other
    words that are 0) for the quad's texture, or None."""
    out = os.path.join(root, 'out', 'scene.splashpack')
    os.makedirs(os.path.dirname(out))
    r = subprocess.run([cli, 'export', os.path.join(root, 'meshalpha.scene'), '-o', out, '--stats'], capture_output=True, text=True)
    m = re.search(r'texture bpp=(\d+) cutout=(\d) size=(\d+)x(\d+) vram=(\d+),(\d+) ', r.stdout)
    vram = read_vram(os.path.join(root, 'out', 'scene.vram')) if r.returncode == 0 else None
    if not m or vram is None: return None, r.stdout + r.stderr
    bpp, cut, x0, y0 = int(m.group(1)), m.group(2) == '1', int(m.group(5)), int(m.group(6))
    if bpp != 16: return None, 'not 16 bpp: ' + m.group(0)
    hole = sum(vram[(y0 + y) * 1024 + x0 + x] == 0 for y in HOLE for x in HOLE)
    rest = sum(vram[(y0 + y) * 1024 + x0 + x] == 0 for y in range(N) for x in range(N)) - hole
    return (cut, hole, rest), m.group(0)


# --- the holed quad ---
root = project(True)
shot = keep_shot or os.path.join(root, 'shot.png')
rects, out = run(root, shot)
holes = [l for l in out if l.startswith('texture: ')]
check('the inspector reports the hole', holes == ['texture: holes yes textures/quad.png'], holes)
a, p = rects.get('obj.texholes'), rects.get('obj.panel')
check('the hole caption is drawn inside the inspector',
      bool(a and p and a[0] >= p[0] and a[1] >= p[1] and a[2] <= p[2] and a[3] <= p[3]), (a, p))
centre, red = viewport(rects, shot)
check('the viewport shows the background through the hole',
      centre is not None and all(c[1] > 120 and c[0] < 100 for c in centre), centre)
check('the viewport shows the red texture around the hole', red > 0.02, red)
ex, detail = exported(root)
check('the export is cutout, every hole texel word 0x0000 and no other',
      ex == (True, len(HOLE) ** 2, 0), (ex, detail))

# --- control: the same quad with an opaque texture ---
root = project(False)
shot = os.path.join(root, 'shot.png')
rects, out = run(root, shot)
holes = [l for l in out if l.startswith('texture: ')]
check('an opaque texture reports no hole', holes == ['texture: holes no textures/quad.png'], holes)
check('no hole caption for an opaque texture', 'obj.texholes' not in rects and 'obj.panel' in rects, rects.get('obj.texholes'))
centre, red = viewport(rects, shot)
check('the viewport middle shows the texture', centre is not None and all(c[0] > 160 and c[1] < 60 for c in centre), centre)
ex, detail = exported(root)
check('the opaque export is not cutout and has no word 0', ex == (False, 0, 0), (ex, detail))

print('meshalphacheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
