# Scrubbing a "mixed" field of the multi-object inspector, and Move to Top
# Level on a multi-selection (#263), driven through the GUI. A copy of the
# courtyard gets two point lights, LampA at X 1 and LampB at X 4 (LampB
# turned 30 degrees about Y, a brighter light), and a "Rack" at X 5 holding
# ShelfA and ShelfB.
# A drag on a mixed field moves every selected object by the same delta, each
# keeping its offset (position X, rotation Y, light intensity), a clamp
# stopping only the one it reaches; one Ctrl+Z undoes the whole drag. The
# field reads "mixed" all through the drag. A value typed into the mixed field
# still sets it on all. Move to Top Level from the row menu with ShelfA and
# ShelfB selected puts both at the top level after Rack, in tree order,
# keeping where they are in the world; one Ctrl+Z puts both back.
# usage: python3 multiscrubcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives mixed-scrub.png (a drag on Position X held).
import json, math, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
Q30 = [0.0, math.sin(math.radians(15)), 0.0, math.cos(math.radians(15))]  # 30 degrees about Y
LAMPS = {
    'LampA': {'pos': [1.0, 1.0, 2.0], 'rot': [0.0, 0.0, 0.0, 1.0], 'intensity': 1.0},
    'LampB': {'pos': [4.0, 1.0, 2.0], 'rot': Q30, 'intensity': 2.5},
}
RACK = [5.0, 0.0, 0.0]
SHELVES = {'ShelfA': [1.0, 0.0, 0.0], 'ShelfB': [2.0, 0.0, 0.0]}


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def obj(name, pos, rot=(0.0, 0.0, 0.0, 1.0), components=(), children=()):
    return {'name': name, 'active': True,
            'transform': {'position': pos, 'rotation': list(rot), 'scale': [1.0, 1.0, 1.0]},
            'components': list(components), 'children': list(children)}


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    for name, l in LAMPS.items():
        s['objects'].append(obj(name, l['pos'], l['rot'], [{'type': 'light', 'kind': 'point', 'color': [1.0, 1.0, 1.0],
                                                            'intensity': l['intensity'], 'range': 5.0, 'spotAngle': 30.0,
                                                            'innerSpotAngle': 0.0, 'enabled': True}]))
    s['objects'].append(obj('Rack', RACK, children=[obj(n, p) for n, p in SHELVES.items()]))
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


W = lambda n: ['--wait', str(n)]
UNDO = ['--key', 'ctrl+z'] + W(3)
SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)


def yaw(q):
    """Degrees about Y of a quaternion turned about Y alone."""
    return math.degrees(2 * math.atan2(q[1], q[3]))


def lamps(s):
    """name -> (position, yaw, intensity) of each lamp in a saved scene."""
    out = {}
    for o in s['objects']:
        if o['name'] in LAMPS:
            light = next((c for c in o.get('components', []) if c.get('type') == 'light'), {})
            out[o['name']] = ([round(float(v), 4) for v in o['transform']['position']], round(yaw(o['transform']['rotation']), 3),
                              round(float(light.get('intensity', -1)), 4))
    return out


def layout(s):
    """Top-level names, Rack's children's names and each shelf's world position (Rack is only moved)."""
    top = [o['name'] for o in s['objects']]
    rack = next((o for o in s['objects'] if o['name'] == 'Rack'), {'children': [], 'transform': {'position': [0, 0, 0]}})
    kids = [c['name'] for c in rack['children']]
    world = {}
    for o in s['objects']:
        if o['name'] in SHELVES: world[o['name']] = [round(float(v), 4) for v in o['transform']['position']]
    for c in rack['children']:
        world[c['name']] = [round(float(v) + float(r), 4) for v, r in zip(c['transform']['position'], rack['transform']['position'])]
    return top, kids, world


def run(name, args, save=True, select='LampA'):
    """A fresh copy with `select` selected, then `args` (and Ctrl+S); returns
    the saved scene, the scene saved at SAVE_MID (or None), the output lines
    and the screenshot."""
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select] + W(20) + args + \
        (['--key', 'ctrl+s'] + W(10) if save else []) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    mid = json.load(open(snap, encoding='utf-8')) if os.path.exists(snap) else None
    return json.load(open(scene, encoding='utf-8')), mid, lines, shot


def near(a, b, eps=1e-3):
    return a is not None and b is not None and len(a) == len(b) and all(abs(x - y) <= eps for x, y in zip(a, b))


def tree(name, button=None):
    return ['--click', '@tree.' + name + (',0,0,%d' % button if button is not None else '')] + W(3)


def ctrl(*names):
    out = ['--keydown', 'ctrl'] + W(2)
    for n in names: out += tree(n)
    return out + ['--keyup', 'ctrl'] + W(3)


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


def scrub(name, dx):
    return ['--scroll-to', '@' + name, '--drag', '@%s,0,0,%d,0' % (name, dx)] + W(5)


def shown(lines, field):
    """Every value the multi inspector reported for `field`, in order."""
    return [l[len('inspector: ' + field + ' '):] for l in lines if l.startswith('inspector: ' + field + ' ')]


TWO = ctrl('LampB')  # LampA, then LampB: LampB (X 4) primary
ORIG = {n: (l['pos'], round(yaw(l['rot']), 3), l['intensity']) for n, l in LAMPS.items()}

# Position X: both mixed, a drag to the right moves both by one delta.
s, mid, lines, _ = run('posx', TWO + scrub('obj.pos.x', 60) + SAVE_MID + UNDO)
m = lamps(mid) if mid else None
print('position X scrubbed:', m)
check('position X reads "mixed" with LampA at 1 and LampB at 4', shown(lines, 'pos.x')[:1] == ['mixed'], shown(lines, 'pos.x'))
check('it reads "mixed" all through the drag', set(shown(lines, 'pos.x')) == {'mixed'}, shown(lines, 'pos.x'))
if m:
    da, db = m['LampA'][0][0] - 1.0, m['LampB'][0][0] - 4.0
    print('deltas: LampA %.4f LampB %.4f' % (da, db))
    check('the drag moves position X', da > 0.05, (da, db))
    check('both move by the same delta', abs(da - db) < 2e-3, (da, db))
    check('they keep their 3.0 offset', abs((m['LampB'][0][0] - m['LampA'][0][0]) - 3.0) < 2e-3, m)
    check('Y and Z stay', all(near(m[n][0][1:], LAMPS[n]['pos'][1:]) for n in LAMPS), m)
else:
    check('the scrub was saved', False, mid)
check('one Ctrl+Z puts both positions back', lamps(s) == ORIG, lamps(s))

# Rotation Y: LampA at 0, LampB at 30 degrees: both turn by one delta.
s, mid, lines, _ = run('roty', TWO + scrub('obj.rot.y', 40) + SAVE_MID + UNDO)
m = lamps(mid) if mid else None
print('rotation Y scrubbed:', m)
check('rotation Y reads "mixed"', set(shown(lines, 'rot.y')) == {'mixed'}, shown(lines, 'rot.y'))
if m:
    da, db = m['LampA'][1] - 0.0, m['LampB'][1] - 30.0
    print('turns: LampA %.3f LampB %.3f' % (da, db))
    check('the drag turns them', abs(da) > 1, (da, db))
    check('both turn by the same angle, keeping 30 degrees between them', abs(da - db) < 0.05, (da, db))
else:
    check('the rotation scrub was saved', False, mid)
check('one Ctrl+Z puts both rotations back', lamps(s) == ORIG, lamps(s))

# Intensity 1 and 2.5 dragged left by more than 1: LampA stops at 0, LampB goes on.
s, mid, lines, _ = run('lint', TWO + scrub('obj.lint', -60) + SAVE_MID + UNDO)
m = lamps(mid) if mid else None
print('intensity scrubbed left:', m)
check('intensity reads "mixed"', shown(lines, 'lint')[:1] == ['mixed'], shown(lines, 'lint'))
if m:
    check('LampA is held at the clamp, 0', m['LampA'][2] == 0, m)
    check('LampB moves on past where LampA stopped (down by more than 1, still above 0)', 0.2 < m['LampB'][2] < 1.5, m)
else:
    check('the intensity scrub was saved', False, mid)
check('one Ctrl+Z puts both intensities back', lamps(s) == ORIG, lamps(s))

# Typing into the mixed field still sets that one value on both.
s, _, _, _ = run('typed', TWO + typed('obj.pos.x', '2'))
check('typed position X 2 puts both at X 2', lamps(s)['LampA'][0][0] == 2 and lamps(s)['LampB'][0][0] == 2, lamps(s))
s, _, _, _ = run('typedlint', TWO + typed('obj.lint', '3'))
check('typed intensity 3 sets both', all(lamps(s)[n][2] == 3 for n in LAMPS), lamps(s))

# A drag held: the field still reads "mixed" while scrubbing (screenshot).
_, _, lines, shot = run('hold', TWO + ['--scroll-to', '@obj.pos.x', '--drag-hold', '@obj.pos.x,0,0,50,0'] + W(5), save=False)
check('held mid-drag, position X reads "mixed"', shown(lines, 'pos.x')[-1:] == ['mixed'], shown(lines, 'pos.x'))
if keep: shutil.copy(shot, os.path.join(keep, 'mixed-scrub.png'))

# Move to Top Level with ShelfA and ShelfB selected.
START = layout(json.load(open(os.path.join(project_copy(), 'courtyard.scene'), encoding='utf-8')))
s, mid, lines, _ = run('toplevel', ctrl('ShelfB') + tree('ShelfB', 1) + ['--click', '@treemenu/Move to Top Level'] + W(5) + SAVE_MID +
                       UNDO, select='ShelfA')
got = layout(mid) if mid else None
print('after Move to Top Level:', got and (got[0][-4:], got[1], got[2]))
if got:
    check('both shelves sit at the top level after Rack, in tree order', got[0][-3:] == ['Rack', 'ShelfA', 'ShelfB'], got[0][-5:])
    check('Rack keeps no children', got[1] == [], got[1])
    check('each keeps where it is in the world', near(got[2]['ShelfA'], [6, 0, 0]) and near(got[2]['ShelfB'], [7, 0, 0]), got[2])
else:
    check('the move was saved', False, mid)
check('one Ctrl+Z puts both back in Rack', layout(s) == START, (layout(s), START))

print('FAILS=%d' % fails)
sys.exit(1 if fails else 0)
