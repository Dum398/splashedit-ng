# The Scale lock and the scale gizmo's plane handles (#264), driven through
# the GUI. A copy of the courtyard gets a plain "Box" scaled 1/2/1.
# Inspector: with the lock off, typing X = 2 changes X alone; with the lock
# (obj.scllock) on, it gives 2/4/2, a scrub keeps the proportions, and one
# Ctrl+Z puts 1/2/1 back. Scene view, scale tool (R): dragging a plane
# square (vp.scale.xy, vp.scale.yz) out through it scales exactly its two
# axes by one factor and leaves the third alone, in one undo step; an axis
# handle still scales its axis alone; hovering a square lights it.
# usage: python3 scalelockcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives gizmo.png (a square hovered) and inspector.png (the lock on).
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
START = [1.0, 2.0, 1.0]


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


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append({'name': 'Box', 'active': True, 'transform': {'position': [0, 1, 0], 'rotation': [0, 0, 0, 1],
                         'scale': START}, 'components': [], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


W = lambda n: ['--wait', str(n)]
mid = None  # with SAVE_MID in the script: the scene as saved at that point


def run(name, args, save=True):
    """A fresh copy with Box selected and the scale tool on, then `args`
    (and Ctrl+S); returns Box's saved scale, the named controls' rects and
    the screenshot."""
    global mid
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Box'] + W(20) + ['--key', 'r'] + W(5) + args + \
        (['--key', 'ctrl+s'] + W(10) if save else []) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    mid = None
    if os.path.exists(snap):
        mid = box_scale(json.load(open(snap, encoding='utf-8')))
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return box_scale(json.load(open(scene, encoding='utf-8'))), rects, shot


def box_scale(s):
    for o in s['objects']:
        if o['name'] == 'Box':
            return [round(float(v), 4) for v in o['transform']['scale']]
    return None


def near(a, b, eps=1e-3):
    return a is not None and b is not None and all(abs(x - y) <= eps for x, y in zip(a, b))


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


LOCK = ['--click', '@obj.scllock'] + W(3)
UNDO = ['--key', 'ctrl+z'] + W(3)
SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)

# The layout, with nothing done: where the controls are.
s, rects, base = run('probe', [], save=False)
need = ('obj.scllock', 'obj.scl.x', 'vp.scale.xyz', 'vp.scale.x', 'vp.scale.xy', 'vp.scale.yz', 'vp.scale.xy.area')
check('the lock, the scale fields and the plane handles are drawn', all(n in rects for n in need),
      sorted(n for n in need if n not in rects))

# Inspector, lock off: one axis.
s, _, _ = run('unlocked', typed('obj.scl.x', '2'))
check('lock off: typing X = 2 changes X alone', near(s, [2, 2, 1]), s)

# Lock on: the typed axis sets the factor for all three.
s, _, lockshot = run('locked', LOCK + typed('obj.scl.x', '2'))
check('lock on: X 1 -> 2 on 1/2/1 gives 2/4/2', near(s, [2, 4, 2]), s)
s, _, _ = run('lockedy', LOCK + typed('obj.scl.y', '1'))
check('lock on: Y 2 -> 1 on 1/2/1 gives 0.5/1/0.5', near(s, [0.5, 1, 0.5]), s)
s, _, _ = run('lockedundo', LOCK + typed('obj.scl.x', '2') + SAVE_MID + UNDO)
check('lock on: one Ctrl+Z puts 1/2/1 back', near(mid, [2, 4, 2]) and near(s, START), (mid, s))
# A scrub under the lock: every frame from the start, one undo step.
s, _, _ = run('lockedscrub', LOCK + ['--drag', '@obj.scl.x,0,0,60,0'] + W(5) + SAVE_MID + UNDO)
f = mid and mid[0] / START[0]
print('scrubbed under the lock:', mid)
check('lock on: scrubbing X keeps 1/2/1 proportions',
      mid is not None and abs(f - 1) > 0.05 and near(mid, [START[0] * f, START[1] * f, START[2] * f], 2e-3), mid)
check('lock on: one Ctrl+Z undoes the whole scrub', near(s, START), s)
# The lock off again: back to one axis.
s, _, _ = run('relocked', LOCK + LOCK + typed('obj.scl.x', '2'))
check('lock clicked twice: off again, X alone', near(s, [2, 2, 1]), s)
# The lock is saved with Play's tools (play.cfg) and read back next time.
cfg = os.path.join(tempfile.mkdtemp(), 'play.cfg')
run('lockpersist', ['--play-settings', cfg] + LOCK, save=False)
saved = open(cfg, encoding='utf-8').read() if os.path.exists(cfg) else ''
check('the lock is saved with Play\'s tools', 'scale_lock=1' in saved.splitlines(), saved)
s, _, _ = run('lockreload', ['--play-settings', cfg] + typed('obj.scl.x', '2'))
check('a saved lock is on in the next session: 2/4/2', near(s, [2, 4, 2]), s)


def outward(name, px=45):
    """A drag from the handle `name`, `px` pixels on away from the gizmo's centre."""
    def centre(n):
        r = rects.get(n, (0, 0, 0, 0))
        return ((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)
    (cx, cy), (hx, hy) = centre('vp.scale.xyz'), centre(name)
    l = max(((hx - cx) ** 2 + (hy - cy) ** 2) ** 0.5, 1e-3)
    return ['--drag', '@%s,0,0,%d,%d' % (name, round((hx - cx) / l * px), round((hy - cy) / l * px))] + W(5)


# Scene view: the plane squares.
for plane, keepi in (('xy', 2), ('yz', 0)):
    s, _, _ = run('plane' + plane, outward('vp.scale.' + plane) + SAVE_MID + UNDO)
    a, b = 'xyz'.index(plane[0]), 'xyz'.index(plane[1])
    print('plane %s dragged:' % plane, START, '->', mid)
    fa = mid and mid[a] / START[a]
    fb = mid and mid[b] / START[b]
    check('plane %s: drags %s and %s by one factor' % (plane, plane[0].upper(), plane[1].upper()),
          mid is not None and fa > 1.05 and abs(fa - fb) < 1e-3, mid)
    check('plane %s: leaves %s alone' % (plane, 'XYZ'[keepi]), mid is not None and mid[keepi] == START[keepi], mid)
    check('plane %s: one Ctrl+Z undoes the drag' % plane, near(s, START), s)

# An axis handle still scales its axis alone (the squares do not steal it).
s, _, _ = run('axisx', outward('vp.scale.x'))
print('x handle dragged:', START, '->', s)
check('the X handle scales X alone', s is not None and s[0] > 1.05 and s[1:] == START[1:], s)

# Hover: the square under the mouse lights, and hovering edits nothing.
s, _, hov = run('hover', ['--move', '@vp.scale.xy'] + W(20))
check('hovering a square changes nothing', near(s, START), s)
_, _, away = run('away', ['--move', '760,940'] + W(20), save=False)
area = rects.get('vp.scale.xy.area', (0, 0, 0, 0))
w, h, ha = png_rgb(hov)
_, _, aa = png_rgb(away)
lit = 0
for y in range(max(0, area[1]), min(h, area[3])):
    for x in range(max(0, area[0]), min(w, area[2])):
        p, q = ha[y][3 * x:3 * x + 3], aa[y][3 * x:3 * x + 3]
        if sum(p) - sum(q) > 60: lit += 1
print('xy square pixels lit by hover:', lit)
check('hovering the XY square lights it', lit > 20, lit)

if keep:
    os.makedirs(keep, exist_ok=True)
    shutil.copyfile(hov, os.path.join(keep, 'gizmo.png'))
    shutil.copyfile(lockshot, os.path.join(keep, 'inspector.png'))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
