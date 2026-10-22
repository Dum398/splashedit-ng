# Import settings' Triangles row: drops a 3200-triangle grid (tests write it,
# 40 x 40 quads with UVs) on the viewport of a copy of the courtyard example,
# types 800 into the inspector's Triangles field and checks that the model is
# imported again simplified: the row reads 3200 -> at most 800, its share of
# the 2000 in-view guide turns from bad to good, the sidecar keeps the target
# and the count it started from, and the baked mesh on disk has at most 800.
# usage: python3 importtrianglescheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
GOOD = (0x46, 0xc9, 0x8a)  # color::good


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def png_rgb(path):
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


root = tempfile.mkdtemp()
project = os.path.join(root, 'courtyard')
shutil.copytree(example, project)
grid = os.path.join(root, 'grid.obj')
with open(grid, 'w') as o:
    for y in range(41):
        for x in range(41):
            o.write('v %g %g 0\nvt %g %g\n' % (x * 0.1, y * 0.1, x / 40, y / 40))
    o.write('vn 0 0 1\n')
    for y in range(40):
        for x in range(40):
            a = y * 41 + x + 1
            b, c, d = a + 1, a + 42, a + 41
            o.write('f %d/%d/1 %d/%d/1 %d/%d/1\n' % (a, a, b, b, c, c))
            o.write('f %d/%d/1 %d/%d/1 %d/%d/1\n' % (a, a, c, c, d, d))
shot = os.path.join(root, 'shot.png')
W = lambda n: ['--wait', str(n)]
# The drop imports in the background: the row is there once it is done.
cmd = [editor, '--size', '1600x960', '--project', project] + W(5) + ['--drop-now', grid + '@800,400'] + W(10) + ['--wait-import'] + W(10) + \
      ['--dblclick', '@itris'] + W(3) + ['--key', 'ctrl+a', '--text', '800', '--key', 'enter'] + W(10) + \
      ['--wait-import'] + W(30) + ['--move', '800,400'] + W(5) + ['--screenshot', shot]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
out = (r.stdout + r.stderr).strip()
if keep and os.path.exists(shot): shutil.copyfile(shot, keep)
for l in out.splitlines():
    if l.startswith('@'): print('editor: ' + l)  # a scripted control it could not find

rows = [m.groups() for m in re.finditer(r'^import triangles: (\d+) -> (\d+) target (\d+) (\w+)$', out, re.M)]
check('the grid imports and the row shows all its triangles, bad against the guide',
      bool(rows) and rows[0] == ('3200', '3200', '0', 'bad'), rows[:1] or out[-600:])
last = rows[-1] if rows else ('0', '0', '0', '')
check('after typing 800 the row reads 3200 -> at most 800, good against the guide',
      last[0] == '3200' and 0 < int(last[1]) <= 800 and last[2] == '800' and last[3] == 'good', last)

model = os.path.join(project, 'models', 'grid.obj')
side = json.load(open(model + '.import.json')) if os.path.exists(model + '.import.json') else {}
check('the sidecar keeps the target and the count before simplifying',
      side.get('targetTriangles') == 800 and side.get('sourceTriangles') == 3200, side)
mesh = os.path.join(project, 'models', 'grid.mesh')
tris = sum(len(s) // 3 for s in json.load(open(mesh))['submeshes']) if os.path.exists(mesh) else -1
check('the baked mesh on disk was simplified', 0 < tris <= 800 and str(tris) == last[1], tris)

# The share is drawn in the good colour in the inspector.
if os.path.exists(shot):
    w, h, px = png_rgb(shot)
    good = 0
    for y in range(80, min(h, 900)):
        row = px[y]
        for x in range(1256, min(w, 1590)):
            p = row[x * 3:x * 3 + 3]
            good += all(abs(p[i] - GOOD[i]) <= 24 for i in range(3))
    check('the share is drawn in the good colour', good >= 20, f'{good} good-coloured pixels')
else:
    check('the screenshot was written', False, out[-600:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
