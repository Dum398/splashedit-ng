# Multi-object selection (#263), driven through the GUI. A copy of the
# courtyard gets three point lights, LampA/LampB/LampC, last in the Scene
# panel, with different intensities, ranges, positions and scales.
# Scene panel: Ctrl+click adds and takes away a row, Shift+click selects the
# rows from the anchor to the clicked one, a plain click selects one alone.
# Inspector with several selected (obj.multi): fields whose values differ
# read "mixed" (light intensity and range, position); typing an intensity
# or a position Y sets it on all of them, and one Ctrl+Z puts every one back.
# Scene view: the move gizmo sits at the centre of the selection
# (vp.selcentre) and dragging its Y arrow moves all of them by the same
# delta, one undo step. Scale lock with two selected: each object keeps its
# own proportions, typed and through the gizmo's centre handle. Delete,
# Ctrl+D and Ctrl+C/Ctrl+V act on the whole selection, each one undo step:
# the copies become the selection, and undoing a delete restores it.
# A switch whose values differ (Runtime) draws its knob in the middle, and
# a click turns it on for all of them.
# usage: python3 multiselectcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives inspector-mixed.png and gizmo-centre.png.
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
LAMPS = {
    'LampA': {'pos': [0.0, 1.0, 2.0], 'scale': [1.0, 2.0, 1.0], 'intensity': 1.0, 'range': 5.0},
    'LampB': {'pos': [2.0, 3.0, 2.0], 'scale': [2.0, 1.0, 3.0], 'intensity': 2.0, 'range': 5.0, 'runtime': True},
    'LampC': {'pos': [-2.0, 0.5, 3.0], 'scale': [1.0, 1.0, 1.0], 'intensity': 1.0, 'range': 6.0},
}


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
    for name, l in LAMPS.items():
        s['objects'].append({'name': name, 'active': True,
                             'transform': {'position': l['pos'], 'rotation': [0, 0, 0, 1], 'scale': l['scale']},
                             'components': [{'type': 'light', 'kind': 'point', 'color': [1.0, 1.0, 1.0], 'intensity': l['intensity'],
                                             'range': l['range'], 'spotAngle': 30.0, 'innerSpotAngle': 0.0, 'enabled': True,
                                             'runtime': l.get('runtime', False)}],
                             'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


W = lambda n: ['--wait', str(n)]
mid = None  # with SAVE_MID in the script: the lamps as saved at that point
mid_rot = None  # and their rotations
mid_names = None  # and the top-level object names
saved_names = None  # the top-level object names as saved at the end


def lamps(s):
    """name -> (position, scale, intensity, range) of each lamp in a saved scene."""
    out = {}
    for o in s['objects']:
        if o['name'] in LAMPS:
            light = next((c for c in o.get('components', []) if c.get('type') == 'light'), {})
            out[o['name']] = ([round(float(v), 4) for v in o['transform']['position']],
                              [round(float(v), 4) for v in o['transform']['scale']],
                              round(float(light.get('intensity', -1)), 4), round(float(light.get('range', -1)), 4))
    return out


def run(name, args, save=True, select='LampA'):
    """A fresh copy with `select` selected, then `args` (and Ctrl+S); returns
    the saved lamps, the output lines, the named controls' rects and the
    screenshot."""
    global mid, mid_rot, mid_names, saved_names
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select] + W(20) + args + \
        (['--key', 'ctrl+s'] + W(10) if save else []) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    mid = lamps(json.load(open(snap, encoding='utf-8'))) if os.path.exists(snap) else None
    mid_names = [o['name'] for o in json.load(open(snap, encoding='utf-8'))['objects']] if os.path.exists(snap) else None
    saved_names = [o['name'] for o in json.load(open(scene, encoding='utf-8'))['objects']]
    mid_rot = {o['name']: [float(v) for v in o['transform']['rotation']] for o in json.load(open(snap, encoding='utf-8'))['objects']
               if o['name'] in LAMPS} if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return lamps(json.load(open(scene, encoding='utf-8'))), lines, rects, shot


def near(a, b, eps=1e-3):
    return a is not None and b is not None and len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


def tree(name):
    return ['--click', '@tree.' + name] + W(3)


def ctrl(*names):
    out = ['--keydown', 'ctrl'] + W(2)
    for n in names: out += tree(n)
    return out + ['--keyup', 'ctrl'] + W(3)


def shift(name):
    return ['--keydown', 'shift'] + W(2) + tree(name) + ['--keyup', 'shift'] + W(3)


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


def selections(lines):
    """The 'selection:' lines, in order, as (count, primary, [all])."""
    out = []
    for l in lines:
        m = re.match(r'selection: (\d+)(?: primary=(.*?))? all=(.*)$', l)
        if m: out.append((int(m.group(1)), m.group(2), [n for n in m.group(3).split(';') if n]))
    return out


def shown(lines, field):
    """What the multi inspector last reported for `field`."""
    v = None
    for l in lines:
        if l.startswith('inspector: ' + field + ' '): v = l[len('inspector: ' + field + ' '):]
    return v


ALL3 = shift('LampC')  # from LampA (selected at start) to LampC
UNDO = ['--key', 'ctrl+z'] + W(3)
SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)
ORIG = {n: (l['pos'], l['scale'], l['intensity'], l['range']) for n, l in LAMPS.items()}

# Scene panel clicks.
_, lines, rects, _ = run('clicks', ctrl('LampB', 'LampC') + ctrl('LampB') + tree('LampB') + shift('LampC') + shift('Game Logic') +
                         ctrl('LampA'), save=False)
sel = selections(lines)
print('selections:', sel)
want = [
    (1, 'LampA', ['LampA']),
    (2, 'LampB', ['LampA', 'LampB']),                      # Ctrl+click adds
    (3, 'LampC', ['LampA', 'LampB', 'LampC']),
    (2, 'LampC', ['LampA', 'LampC']),                      # Ctrl+click again takes away
    (1, 'LampB', ['LampB']),                               # a plain click selects one
    (2, 'LampC', ['LampB', 'LampC']),                      # Shift+click: anchor LampB to LampC
    (3, 'Game Logic', ['Game Logic', 'LampA', 'LampB']),   # the range from the same anchor, upwards
    (2, 'Game Logic', ['Game Logic', 'LampB']),            # Ctrl+click takes LampA out of it
]
check('Ctrl/Shift/plain clicks select as expected, in order', sel == want, sel)
check('the Scene panel draws a multi-selection with the multi inspector', 'obj.multi' in rects, sorted(rects)[:5])

# One object: the usual inspector (its name), not the multi one.
_, lines, rects, _ = run('single', [], save=False)
check('one object selected: the usual inspector', 'obj.name' in rects and 'obj.multi' not in rects, sorted(k for k in rects if k.startswith('obj.'))[:8])

# Mixed values: three lamps, the move tool, the gizmo at their centre.
_, lines, rects, shot = run('mixed', ALL3 + ['--key', 'w'] + W(8), save=False)
check('Shift+click from LampA to LampC selects the three lamps', selections(lines)[-1:] == [(3, 'LampC', ['LampA', 'LampB', 'LampC'])],
      selections(lines)[-1:])
check('differing intensities show "mixed"', shown(lines, 'lint') == 'mixed', shown(lines, 'lint'))
check('differing ranges show "mixed"', shown(lines, 'lrange') == 'mixed', shown(lines, 'lrange'))
check('differing position Y shows "mixed"', shown(lines, 'pos.y') == 'mixed', shown(lines, 'pos.y'))
check('a shared value shows itself (rotation X 0)', shown(lines, 'rot.x') == '0', shown(lines, 'rot.x'))
check('a shared kind shows itself (Point)', shown(lines, 'lkind') == 'Point', shown(lines, 'lkind'))


def centre(n):
    r = rects.get(n)
    return r and ((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)


check('the move gizmo sits at the selection centre', centre('vp.move.c') is not None and centre('vp.selcentre') is not None and
      abs(centre('vp.move.c')[0] - centre('vp.selcentre')[0]) <= 1 and abs(centre('vp.move.c')[1] - centre('vp.selcentre')[1]) <= 1,
      (centre('vp.move.c'), centre('vp.selcentre')))
# Every selected lamp's marker is ringed in the selection colour (radius 16).
w, h, px = png_rgb(shot)

# A switch whose values differ (Runtime: only LampB on) reads "mixed" and
# draws its knob in the middle of the track, neither on nor off; a click
# sets it on for all three.
check('differing Runtime switches show "mixed"', shown(lines, 'lruntime') == 'mixed', shown(lines, 'lruntime'))
k = centre('obj.lruntime')
knob = k and px[int(k[1]) + 3][3 * int(k[0]):3 * int(k[0]) + 3]
check('the mixed switch draws its knob in the middle', knob is not None and min(knob) > 215, (k, knob))


def ringed(n):
    c = centre('vp.marker.' + n)
    if not c: return 0
    hits = 0
    for y in range(int(c[1]) - 19, int(c[1]) + 20):
        for x in range(int(c[0]) - 19, int(c[0]) + 20):
            d = ((x - c[0]) ** 2 + (y - c[1]) ** 2) ** 0.5
            if 14.5 <= d <= 17.5 and 0 <= x < w and 0 <= y < h:
                r, g, b = px[y][3 * x:3 * x + 3]
                hits += abs(r - 0xb3) < 40 and abs(g - 0xa8) < 40 and b > 200
    return hits


rings = {n: ringed(n) for n in LAMPS}
print('marker ring pixels:', rings, 'Torch Light (not selected):', ringed('Torch Light'))
check('the Scene view rings every selected lamp', all(v >= 20 for v in rings.values()), rings)
check('an unselected light is not ringed', ringed('Torch Light') < 5, ringed('Torch Light'))
if keep:
    shutil.copy(shot, os.path.join(keep, 'inspector-mixed.png'))
    shutil.copy(shot, os.path.join(keep, 'gizmo-centre.png'))

# One intensity typed: all three take it, one Ctrl+Z puts every one back.
s, lines, _, _ = run('intensity', ALL3 + typed('obj.lint', '3') + SAVE_MID + UNDO)
print('intensity typed:', mid)
check('typed intensity 3 sets all three', mid is not None and all(mid[n][2] == 3 for n in LAMPS), mid)
check('typing the intensity leaves the ranges alone', mid is not None and all(mid[n][3] == LAMPS[n]['range'] for n in LAMPS), mid)
check('one Ctrl+Z restores every intensity', s == ORIG, s)

# A typed range on the mixed field, equal to the primary's: still sets all.
s, _, _, _ = run('range', ALL3 + typed('obj.lrange', '6'))
check('typed range 6 (the primary\'s own) sets all three', all(s[n][3] == 6 for n in LAMPS), s)

# Position Y typed: all on the same Y, X and Z kept.
s, _, _, _ = run('posy', ALL3 + typed('obj.pos.y', '4') + SAVE_MID + UNDO)
print('position Y typed:', mid)
check('typed position Y 4 puts all three at Y 4, X and Z kept',
      mid is not None and all(near(mid[n][0], [LAMPS[n]['pos'][0], 4, LAMPS[n]['pos'][2]]) for n in LAMPS), mid)
check('one Ctrl+Z puts every position back', s == ORIG, s)

# The move gizmo: its Y arrow dragged up moves all three by one delta.
s, _, _, _ = run('gizmo', ALL3 + ['--key', 'w'] + W(5) + ['--drag', '@vp.move.y,0,0,0,-50'] + W(5) + SAVE_MID + UNDO)
print('gizmo moved:', mid)
if mid:
    d = {n: [round(mid[n][0][i] - LAMPS[n]['pos'][i], 3) for i in range(3)] for n in LAMPS}
    print('deltas:', d)
    dy = d['LampA'][1]
    check('the drag moves the selection up', dy > 0.05, d)
    check('every lamp moves by the same delta, along Y only', all(near(d[n], [0, dy, 0], 2e-3) for n in LAMPS), d)
else:
    check('the drag was saved', False, mid)
check('one Ctrl+Z undoes the whole drag for all of them', s == ORIG, s)

# The rotate gizmo with two selected: LampA (0,1,2) and LampB (2,3,2) turn
# about world Y through their centre (1,2,2): the same turn for both, each
# origin swung round the centre by it, one undo step.
def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)]


# Where the Y ring's grab point and the centre are; the drag goes 40 degrees round the centre.
_, _, rects, _ = run('rotprobe', ctrl('LampB') + ['--key', 'e'] + W(8), save=False)
import math
p, c0 = centre('vp.rotate.y'), centre('vp.selcentre')
check('the rotate ring and the selection centre are drawn', p is not None and c0 is not None, (p, c0))
p, c0 = p or (0, 0), c0 or (0, 0)
a = math.radians(40)
q = (c0[0] + (p[0] - c0[0]) * math.cos(a) - (p[1] - c0[1]) * math.sin(a), c0[1] + (p[0] - c0[0]) * math.sin(a) + (p[1] - c0[1]) * math.cos(a))
s, _, _, _ = run('rotate', ctrl('LampB') + ['--key', 'e'] + W(8) +
                 ['--drag', '@vp.rotate.y,0,0,%d,%d' % (round(q[0] - p[0]), round(q[1] - p[1]))] + W(5) + SAVE_MID + UNDO)
print('rotated:', mid, mid_rot)
if mid and mid_rot:
    qa, qb = mid_rot['LampA'], mid_rot['LampB']
    c = [1.0, 2.0, 2.0]
    check('the drag turns both about Y', abs(qa[1]) > 0.05 and abs(qa[0]) < 1e-3 and abs(qa[2]) < 1e-3, qa)
    check('both take the same turn', near(qa, qb, 1e-4), (qa, qb))
    for n in ('LampA', 'LampB'):
        want_p = [c[i] + v for i, v in enumerate(qrot(qa, [LAMPS[n]['pos'][i] - c[i] for i in range(3)]))]
        check(n + ' swings round the selection centre by that turn', near(mid[n][0], want_p, 2e-3), (mid[n][0], want_p))
    check('LampC (not selected) stays put', mid['LampC'][0] == LAMPS['LampC']['pos'], mid['LampC'])
else:
    check('the rotate drag was saved', False, mid)
check('one Ctrl+Z undoes the turn for both', s == ORIG, s)

_, lines, _, _ = run('mixedswitch', ALL3 + ['--click', '@obj.lruntime'] + W(5), save=False)
check('a click on a mixed switch turns it on for all', shown(lines, 'lruntime') == 'on', shown(lines, 'lruntime'))

# Scale lock with two selected: LampA 1/2/1 and LampB 2/1/3 each keep their proportions.
TWO = ctrl('LampB')
s, _, _, _ = run('lock', TWO + ['--click', '@obj.scllock'] + W(3) + typed('obj.scl.x', '4') + SAVE_MID + UNDO)
print('lock, X typed 4:', mid)
check('lock on, X = 4: LampA 1/2/1 -> 4/8/4', mid is not None and near(mid['LampA'][1], [4, 8, 4]), mid)
check('lock on, X = 4: LampB 2/1/3 -> 4/2/6', mid is not None and near(mid['LampB'][1], [4, 2, 6]), mid)
check('lock on: LampC (not selected) untouched', mid is not None and mid['LampC'][1] == LAMPS['LampC']['scale'], mid)
check('lock on: one Ctrl+Z restores both scales', s == ORIG, s)
s, _, _, _ = run('nolock', TWO + typed('obj.scl.y', '5'))
check('lock off, Y = 5: Y alone on both', near(s['LampA'][1], [1, 5, 1]) and near(s['LampB'][1], [2, 5, 3]), s)

# The scale gizmo's centre handle with two selected: each keeps its proportions.
s, _, grects, _ = run('scalegizmo', TWO + ['--key', 'r'] + W(5) + ['--drag', '@vp.scale.xyz,0,0,40,-40'] + W(5) + SAVE_MID + UNDO)
print('scale gizmo:', mid)
if mid:
    fa = mid['LampA'][1][0] / 1.0
    fb = mid['LampB'][1][0] / 2.0
    check('the centre handle scales both', abs(fa - 1) > 0.05 and abs(fb - 1) > 0.05, mid)
    check('LampA keeps 1/2/1', near(mid['LampA'][1], [fa, 2 * fa, fa], 2e-3), mid)
    check('LampB keeps 2/1/3', near(mid['LampB'][1], [2 * fb, fb, 3 * fb], 2e-3), mid)
else:
    check('the scale drag was saved', False, mid)
check('one Ctrl+Z undoes the scale drag for both', s == ORIG, s)

# Delete, Duplicate and Copy/Paste act on the whole selection, each one undo step.
LAMP_ROWS = ['LampA', 'LampB', 'LampC']
lamp_rows = lambda names: [n for n in (names or []) if n.startswith('Lamp')]
s, lines, _, _ = run('delete', ALL3 + ['--key', 'delete'] + W(5) + SAVE_MID + UNDO)
print('after delete:', lamp_rows(mid_names), 'after undo:', lamp_rows(saved_names))
check('Delete with three selected removes all three', mid_names is not None and lamp_rows(mid_names) == [], mid_names)
check('Delete leaves the other objects', mid_names is not None and 'Torch Light' in mid_names and 'Game Logic' in mid_names, mid_names)
check('one Ctrl+Z brings all three back', s == ORIG and lamp_rows(saved_names) == LAMP_ROWS, (s, saved_names))
check('the undo restores the three-object selection', selections(lines)[-1:] == [(3, 'LampC', LAMP_ROWS)], selections(lines)[-3:])

s, lines, _, _ = run('duplicate', TWO + ['--key', 'ctrl+d'] + W(5) + SAVE_MID + UNDO)
print('after Ctrl+D:', lamp_rows(mid_names), 'selections:', selections(lines)[-3:])
check('Ctrl+D with two selected makes two copies, each after its original',
      lamp_rows(mid_names) == ['LampA', 'LampA (2)', 'LampB', 'LampB (2)', 'LampC'], mid_names)
check('the copies are the selection, the primary\'s copy primary',
      (2, 'LampB (2)', ['LampA (2)', 'LampB (2)']) in selections(lines), selections(lines)[-3:])
check('one Ctrl+Z removes both copies', s == ORIG and lamp_rows(saved_names) == LAMP_ROWS, saved_names)

s, lines, _, _ = run('copypaste', TWO + ['--key', 'ctrl+c'] + W(3) + ['--key', 'ctrl+v'] + W(5) + SAVE_MID + UNDO)
print('after copy+paste:', lamp_rows(mid_names), 'selections:', selections(lines)[-3:])
check('Copy + Paste with two selected pastes both after the primary',
      lamp_rows(mid_names) == ['LampA', 'LampB', 'LampA (2)', 'LampB (2)', 'LampC'], mid_names)
check('the pasted pair is the selection', (2, 'LampB (2)', ['LampA (2)', 'LampB (2)']) in selections(lines), selections(lines)[-3:])
check('one Ctrl+Z removes both pasted', s == ORIG and lamp_rows(saved_names) == LAMP_ROWS, saved_names)

print('FAILS=%d' % fails)
sys.exit(1 if fails else 0)
