# Terrain sculpting and painting in the editor, driven through the GUI. The
# fixture is a 32 x 32 cell, 40 m terrain ("Ground", flat, layer 1 stone,
# layer 2 brick) with a player on it. With Ground selected the Scene view
# shows the terrain tool row (vp.terrain.raise ... vp.terrain.paint); a drag
# over the terrain (vp.view) applies the tool every frame, and the whole
# stroke is one undo step. Under scripted runs the editor prints
#   terrain: tool <raise|lower|smooth|flatten|paint|off>
#   terrain: stroke <tool> steps N         (applications that changed the grid)
#   terrain: <name> res N size X Z tris T hmin .. hmax .. hsum S layers a,b brush L
# and the status bar's statusbar: tris used/2000 band line. Ctrl+S writes the
# grid back to terrain/ground.terrain (SPTR v1), which the test reads.
#   1. A raise drag and a paint drag (brick picked with obj.tpaint.1): the
#      saved file is higher in the middle and has brick cells; the brush ring
#      shows in the screenshot (overlays off, so only the ring is that green).
#   2. A raise drag then Ctrl+Z: the saved file is the fixture again.
#   3. Resolution typed to 16: the triangle meter reads 512, the file is 16.
# usage: python3 terraincheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
RING = (0x8f, 0xbf, 0x5a)
N, SIZE = 32, 40.0


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


def read_terrain(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'SPTR', b[:4]
    version, n = struct.unpack('<II', b[4:12])
    nv = (n + 1) * (n + 1)
    return n, list(struct.unpack('<%df' % nv, b[12:12 + nv * 4])), list(b[12 + nv * 4:])


def fixture():
    project = os.path.join(tempfile.mkdtemp(), 'ground')
    os.makedirs(os.path.join(project, 'terrain'))
    os.makedirs(os.path.join(project, 'textures'))
    for t in ('stone.png', 'brick.png'):
        shutil.copy(os.path.join(example, 'textures', t), os.path.join(project, 'textures', t))
    with open(os.path.join(project, 'terrain', 'ground.terrain'), 'wb') as f:
        f.write(b'SPTR' + struct.pack('<II', 1, N) + bytes(4 * (N + 1) * (N + 1)) + bytes(N * N))
    court = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
    lights = [o for o in court['objects'] if o['name'] in ('Sun', 'Fill')]
    ground = {'name': 'Ground', 'active': True, 'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
              'components': [{'type': 'mesh', 'mesh': '', 'materials': [{'texture': 'textures/stone.png', 'color': [1, 1, 1, 1]},
                                                                        {'texture': 'textures/brick.png', 'color': [1, 1, 1, 1]}]},
                             {'type': 'collider', 'kind': 'static', 'platform': False},
                             {'type': 'terrain', 'data': 'terrain/ground.terrain', 'sizeX': SIZE, 'sizeZ': SIZE, 'tileCells': 4}],
              'children': []}
    player = {'name': 'Player Start', 'active': True, 'transform': {'position': [12, 0, 12], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
              'components': [{'type': 'player'}], 'children': []}
    json.dump({'format': 'splashedit-ng/scene', 'version': 1, 'settings': court['settings'], 'objects': [ground, player] + lights},
              open(os.path.join(project, 'ground.scene'), 'w', encoding='utf-8'), indent=2)
    return project


def run(name, args):
    """Runs the editor on a fresh fixture with Ground selected and its first
    export done, the scene overlays off, then `args`, then saves; returns the
    saved grid, the printed lines and the screenshot."""
    project = fixture()
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Ground'] + W(20) + ['--wait-export'] + \
        ['--click', '@vp.gizmos'] + W(3) + args + ['--key', 'ctrl+s'] + W(10) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=400)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    return read_terrain(os.path.join(project, 'terrain', 'ground.terrain')), lines, shot


def strokes(lines, tool):
    return [int(m.group(1)) for m in (re.match(r'terrain: stroke %s steps (\d+)$' % tool, l) for l in lines) if m]


def last_grid_line(lines):
    g = [l for l in lines if l.startswith('terrain: Ground ')]
    return g[-1] if g else ''


RAISE = ['--click', '@vp.terrain.raise'] + W(3) + ['--drag', '@vp.view,-60,10,60,0'] + W(10)
PAINT = ['--click', '@vp.terrain.paint'] + W(3) + ['--scroll-to', '@obj.tpaint.1'] + W(3) + ['--click', '@obj.tpaint.1'] + W(3) + \
    ['--drag', '@vp.view,-40,60,40,60'] + W(10)

# 1. Sculpt, then paint.
(n, h, layers), lines, shot = run('sculpt', RAISE + PAINT + ['--move', '@vp.view,0,-20'] + W(10))
check('the raise tool turns on', 'terrain: tool raise' in lines, [l for l in lines if l.startswith('terrain: tool')])
check('the raise drag changed the grid', any(s > 0 for s in strokes(lines, 'raise')), strokes(lines, 'raise'))
check('the paint drag changed the grid', any(s > 0 for s in strokes(lines, 'paint')), strokes(lines, 'paint'))
check('the saved grid is higher', n == N and max(h) > 0.5 and min(h) >= 0, (n, max(h), min(h)))
mid = h[(N // 2) * (N + 1) + N // 2]
check('the raise is around the middle, not at the edges', mid > 0.2 and h[0] == 0 and h[-1] == 0, (mid, h[0], h[-1]))
check('the saved grid has brick cells', 0 < sum(1 for l in layers if l == 1) < N * N, sum(layers))
gl = last_grid_line(lines)
check('the inspector line counts the painted cells', re.search(r'layers \d+,[1-9]\d* brush 1$', gl) is not None, gl)
w, hh, rows = png_rgb(shot)
ring = sum(1 for y in range(80, 700) for x in range(280, 1240)
           if all(abs(rows[y][x * 3 + k] - RING[k]) <= 6 for k in range(3)))
check('the brush ring shows on the terrain', ring > 150, ring)
if keep: shutil.copy(shot, keep)

# 2. One stroke, one undo step.
(n, h, layers), lines, _ = run('undo', RAISE + ['--key', 'ctrl+z'] + W(10))
check('the stroke took several applications', any(s > 1 for s in strokes(lines, 'raise')), strokes(lines, 'raise'))
check('one Ctrl+Z takes the whole stroke back', max(h) == 0 and min(h) == 0, (max(h), min(h)))

# 3. Resolution: resampled, and the triangle meter follows.
typed = ['--scroll-to', '@obj.tres'] + W(3) + ['--dblclick', '@obj.tres'] + W(3) + ['--key', 'ctrl+a', '--text', '16', '--key', 'enter'] + \
    W(5) + ['--wait-export'] + W(5)
(n, h, layers), lines, _ = run('resolution', typed)
tris = [l for l in lines if l.startswith('statusbar: tris ')]
check('the triangle meter starts at 32 x 32 cells', tris and tris[0].startswith('statusbar: tris 2048/2000'), tris)
check('the triangle meter follows the resolution', tris and tris[-1] == 'statusbar: tris 512/2000 good', tris)
check('the saved grid is 16 x 16 cells', n == 16 and len(h) == 17 * 17 and len(layers) == 256, (n, len(h)))

print('terraincheck: %s' % ('FAIL (%d)' % fails if fails else 'all passed'))
sys.exit(1 if fails else 0)
