# Dragging a multi-selection in the Scene panel (#263), driven through the
# GUI. A copy of the courtyard gets an empty "Holder" and three lamps,
# LampA/LampB/LampC, last in the Scene panel.
# A row that is part of a multi-selection drags every selected object: with
# LampA and LampC selected, LampA's row dropped onto Holder puts both lamps
# in Holder, in tree order, keeping where they are in the world, both still
# selected (LampC primary); one Ctrl+Z puts them back with the selection.
# A row that is not selected drags only itself. A drop into one of the
# dragged objects is refused for the whole set.
# usage: python3 multidragcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives before-drag.png (two rows selected) and after-drop.png.
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
LAMPS = {'LampA': [0.0, 1.0, 2.0], 'LampB': [2.0, 3.0, 2.0], 'LampC': [-2.0, 0.5, 3.0]}
HOLDER = [1.0, 0.0, 0.0]


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def obj(name, pos, components=()):
    return {'name': name, 'active': True,
            'transform': {'position': pos, 'rotation': [0, 0, 0, 1], 'scale': [1.0, 1.0, 1.0]},
            'components': list(components), 'children': []}


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append(obj('Holder', HOLDER))
    for name, pos in LAMPS.items():
        s['objects'].append(obj(name, pos, [{'type': 'light', 'kind': 'point', 'color': [1.0, 1.0, 1.0], 'intensity': 1.0,
                                             'range': 5.0, 'spotAngle': 30.0, 'innerSpotAngle': 0.0, 'enabled': True}]))
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


W = lambda n: ['--wait', str(n)]
UNDO = ['--key', 'ctrl+z'] + W(3)
SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)


def layout(s):
    """Top-level names, Holder's children's names and their local positions."""
    top = [o['name'] for o in s['objects']]
    holder = next((o for o in s['objects'] if o['name'] == 'Holder'), {'children': []})
    kids = [(c['name'], [round(float(v), 4) for v in c['transform']['position']]) for c in holder['children']]
    return top, kids


def selections(lines):
    """The 'selection:' lines, in order, as (count, primary, [all])."""
    out = []
    for l in lines:
        m = re.match(r'selection: (\d+)(?: primary=(.*?))? all=(.*)$', l)
        if m: out.append((int(m.group(1)), m.group(2), [n for n in m.group(3).split(';') if n]))
    return out


def run(name, args, select='LampA'):
    """A fresh copy with `select` selected, then `args` and Ctrl+S; returns
    the saved layout, the layout saved at SAVE_MID (or None), the output
    lines and the named controls' rects."""
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap).replace('{dir}', project) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select] + W(20) + args + \
        ['--key', 'ctrl+s'] + W(10) + ['--screenshot', os.path.join(project, name + '.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    mid = layout(json.load(open(snap, encoding='utf-8'))) if os.path.exists(snap) else None
    return layout(json.load(open(scene, encoding='utf-8'))), mid, lines, rects, project


def ctrl(*names):
    out = ['--keydown', 'ctrl'] + W(2)
    for n in names: out += ['--click', '@tree.' + n] + W(3)
    return out + ['--keyup', 'ctrl'] + W(3)


# Where the rows are: a run that only looks.
orig, _, _, rects, _ = run('rows', [])
rows = {n: rects.get('tree.' + n) for n in ['Holder', 'LampA', 'LampB', 'LampC']}
print('rows:', rows)
check('the Holder and lamp rows are drawn', all(rows.values()), rows)
if not all(rows.values()):
    sys.exit(1)
cy = lambda r: (r[1] + r[3]) / 2


def drag(src, dst):
    """Drags row `src` by its middle onto the middle of row `dst` (into it)."""
    return ['--drag', '@tree.%s,0,0,0,%d' % (src, round(cy(rows[dst]) - cy(rows[src])))] + W(8)


ORIG_TOP = orig[0]
check('at the start the lamps are top level and Holder is empty',
      orig[1] == [] and ORIG_TOP[-4:] == ['Holder', 'LampA', 'LampB', 'LampC'], orig)
rel = lambda n: [round(a - b, 4) for a, b in zip(LAMPS[n], HOLDER)]

# LampA and LampC selected (LampC primary): LampA's row dropped onto Holder takes both.
shots = ['--shot', '{dir}/before-drag.png'] + W(2)
after, mid, lines, _, project = run('multi', ctrl('LampC') + shots + drag('LampA', 'Holder') +
                                    ['--shot', '{dir}/after-drop.png'] + W(2) + SAVE_MID + UNDO)
sel = selections(lines)
print('after the drop:', mid, 'selections:', sel[-4:])
check('dragging a selected row moves every selected object into Holder, in tree order',
      mid is not None and [k[0] for k in mid[1]] == ['LampA', 'LampC'], mid)
check('the unselected lamp stays at the top level', mid is not None and 'LampB' in mid[0] and 'LampA' not in mid[0], mid)
check('both keep where they are in the world (local to Holder)',
      mid is not None and mid[1] == [('LampA', rel('LampA')), ('LampC', rel('LampC'))], mid)
# The panel prints the selection when it changes: from the Ctrl+click on,
# it stays LampA + LampC with LampC primary, through the drop and the undo.
TWO = (2, 'LampC', ['LampA', 'LampC'])
first = sel.index(TWO) if TWO in sel else len(sel)
check('both stay selected after the drop, LampC primary', first < len(sel) and all(x == TWO for x in sel[first:]), sel[-4:])
check('one Ctrl+Z puts both back', after == orig, after)
check('the undo restores the selection', sel[-1:] == [TWO], sel[-3:])
if keep:
    os.makedirs(keep, exist_ok=True)
    for f in ('before-drag.png', 'after-drop.png'):
        if os.path.exists(os.path.join(project, f)): shutil.copy(os.path.join(project, f), keep)
check('screenshots before the drag and after the drop',
      all(os.path.exists(os.path.join(project, f)) for f in ('before-drag.png', 'after-drop.png')))

# A row that is not selected drags only itself.
after, mid, lines, _, _ = run('single', ctrl('LampC') + drag('LampB', 'Holder') + SAVE_MID + UNDO)
print('after dragging LampB:', mid, 'selections:', selections(lines)[-3:])
check('dragging an unselected row moves only that object',
      mid is not None and mid[1] == [('LampB', rel('LampB'))] and 'LampA' in mid[0] and 'LampC' in mid[0], mid)
check('Ctrl+Z puts it back', after == orig, after)

# Holder and LampA selected: LampA dropped into Holder (one of the dragged) is refused for both.
after, mid, lines, _, _ = run('refused', ctrl('Holder') + drag('LampA', 'Holder') + SAVE_MID)
print('after dropping into a dragged object:', mid)
check('a drop into one of the dragged objects moves none of them', mid is not None and mid == orig, mid)
check('the selection stays as it was', selections(lines)[-1:] == [(2, 'Holder', ['Holder', 'LampA'])], selections(lines)[-3:])

print('multidragcheck:', 'FAILED (%d)' % fails if fails else 'passed')
sys.exit(1 if fails else 0)
