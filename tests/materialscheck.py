# The mesh inspector's materials, driven through the GUI on a courtyard copy
# whose Crate has three materials: its crate texture, a texture path that
# does not exist, and an untextured one. The inspector lists a texture slot
# per material (obj.texture.<i>), a File not found card for the missing one
# (obj.fixtex.1), a caption and a Colour row for the untextured one; a colour
# picked there is saved on that material and one Ctrl+Z puts it back; a
# texture dropped from the Project panel onto Material 3 sets that material
# only. A one-material crate keeps its Texture row (obj.texture).
# usage: python3 materialscheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep_shot = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# At 1600x960 with the Project panel at its default height (assetbrowsercheck's).
SEARCH = '880,731'  # the panel's search field
TILE = '530,812'    # the first file in the grid
WHITE = [1.0, 1.0, 1.0, 1.0]


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def three(d):
    """The Crate draws three materials: textured, a missing texture, plain."""
    mesh = crate_mesh(d)
    mesh['materials'] = [{'texture': 'textures/crate.png', 'color': WHITE},
                         {'texture': 'textures/nope.png', 'color': WHITE},
                         {'texture': '', 'color': WHITE}]


def crate_mesh(d):
    crates = next(o for o in d['objects'] if o['name'] == 'Crates')
    crate = next(o for o in crates['children'] if o['name'] == 'Crate')
    return next(c for c in crate['components'] if c['type'] == 'mesh')


mid = None  # with SAVE_MID in the script: the Crate's materials as saved at that point
rects = {}  # the last run's named controls


def run(args, edit=None, shot=None):
    """Runs the editor on a fresh courtyard copy with Crate selected, then
    `args` and Ctrl+S; returns the saved Crate's materials and the output
    (without its list of controls). `edit` changes the scene JSON first."""
    global mid, rects
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    if edit:
        d = json.load(open(scene, encoding='utf-8'))
        edit(d)
        json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Crate', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--move', '760,940', '--wait', '5', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    mid = crate_mesh(json.load(open(snap, encoding='utf-8')))['materials'] if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return crate_mesh(json.load(open(scene, encoding='utf-8')))['materials'], out


def inside(name, outer='obj.panel'):
    a, b = rects.get(name), rects.get(outer)
    return bool(a and b and a[0] >= b[0] and a[1] >= b[1] and a[2] <= b[2] and a[3] <= b[3])


SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']

# --- one material: today's Texture row, no material list ---
mats, out = run([])
check('one material: the Texture row is drawn in the inspector', inside('obj.texture'), sorted(k for k in rects if k.startswith('obj.')))
check('one material: no per-material rows, caption or colour row',
      not any(k.startswith(('obj.texture.', 'obj.matcol', 'obj.matcount')) for k in rects), sorted(k for k in rects if k.startswith('obj.')))
check('one material: the scripted lines', 'materials: 1 textured 1' in out and
      'material 0: texture crate.png colour #FFFFFF' in out, out[-400:])

# --- one untextured material: a Colour row under the Texture row ---
def plain(d):
    crate_mesh(d)['materials'] = [{'texture': '', 'color': [0.5, 0.25, 0.0, 1.0]}]
mats, out = run([], plain)
check('one untextured material: a Colour row below the Texture row',
      inside('obj.texture') and inside('obj.matcol.0') and rects['obj.matcol.0'][1] >= rects['obj.texture'][3], rects.get('obj.matcol.0'))
check('one untextured material: its colour reported', 'material 0: texture none colour #804000' in out, out[-400:])

# --- three materials: rows, fix card, caption; pick a colour, undo it ---
script = ['--click', '@obj.matcol.2', '--wait', '5', '--click', '@obj.matcolpick.2,-40,0', '--wait', '5', '--key', 'escape', '--wait', '3'] + \
    SAVE_MID + ['--key', 'ctrl+z', '--wait', '3']
mats, out = run(script, three, shot=keep_shot)
check('a row per material, inside the inspector', all(inside('obj.texture.%d' % i) for i in range(3)),
      [(k, rects.get(k)) for k in ('obj.panel', 'obj.texture.0', 'obj.texture.1', 'obj.texture.2')])
check('rows in material order', inside('obj.texture.2') and rects['obj.texture.0'][1] < rects['obj.texture.1'][1] < rects['obj.texture.2'][1],
      [rects.get('obj.texture.%d' % i) for i in range(3)])
check('the single Texture row is gone', 'obj.texture' not in rects, rects.get('obj.texture'))
check('the missing texture gets its File not found card', 'obj.fixtex.1' in rects and 'obj.fixtex.0' not in rects and 'obj.fixtex.2' not in rects,
      sorted(k for k in rects if k.startswith('obj.fix')))
check('only the untextured material has a Colour row', 'obj.matcol.2' in rects and 'obj.matcol.0' not in rects and 'obj.matcol.1' not in rects,
      sorted(k for k in rects if k.startswith('obj.matcol')))
check('the caption is drawn', inside('obj.matcount'), rects.get('obj.matcount'))
check('the scripted lines list the materials', 'materials: 3 textured 2' in out and
      'material 0: texture crate.png colour #FFFFFF' in out and 'material 1: texture nope.png colour #FFFFFF' in out and
      'material 2: texture none colour #FFFFFF' in out, out[-600:])
picked = re.findall(r'material 2: texture none colour (#[0-9A-F]{6})', out)
check('the pick is reported as a new colour', len(picked) >= 2 and picked[1] != '#FFFFFF', picked)
check('the picked colour is saved on material 3 only', mid is not None and len(mid) == 3 and mid[2]['color'] != WHITE and
      mid[0]['color'] == WHITE and mid[1]['color'] == WHITE, mid)
check('one Ctrl+Z puts the colour back', len(mats) == 3 and mats[2]['color'] == WHITE, mats)
check('the textures are untouched', [m.get('texture') or '' for m in mats] == ['textures/crate.png', 'textures/nope.png', ''], mats)

# --- a texture dropped from the Project panel onto Material 3 ---
row = rects.get('obj.texture.2')
if row:
    to = '%d,%d' % ((row[0] + row[2]) // 2, (row[1] + row[3]) // 2)
    mats, out = run(['--click', SEARCH + ',0', '--text', 'brick', '--wait', '5', '--drag', TILE + ',' + to + ',0', '--wait', '10'], three)
    check('dropping brick.png on Material 3 sets that material only',
          [m.get('texture') for m in mats] == ['textures/crate.png', 'textures/nope.png', 'textures/brick.png'], str(mats) + ' | ' + out[-300:])
    check('the drop is reported', 'materials: 3 textured 3' in out and 'material 2: texture brick.png' in out, out[-400:])
else:
    check('Material 3 row found for the drop', False, sorted(rects))

print('materialscheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
