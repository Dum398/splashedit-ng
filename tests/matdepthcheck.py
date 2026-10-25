# Per-material bit depth (#229), driven through the GUI on a courtyard copy
# whose Crate has three materials: crate.png twice and an untextured one. The
# inspector draws a Depth dropdown (obj.matdepth.<i>) under each textured
# material only; picking 16 bpp on Material 2 saves "bitDepth": 16 on that
# material alone and one Ctrl+Z takes it off; picking "Object (8 bpp)" on a
# material set to 16 clears its override.
# usage: python3 matdepthcheck.py <splashedit> [keep dir for screenshots]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
WHITE = [1.0, 1.0, 1.0, 1.0]


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def crate_mesh(d):
    crates = next(o for o in d['objects'] if o['name'] == 'Crates')
    crate = next(o for o in crates['children'] if o['name'] == 'Crate')
    return next(c for c in crate['components'] if c['type'] == 'mesh')


def three(d, depth1=None):
    mesh = crate_mesh(d)
    mesh['bitDepth'] = 8
    mesh['materials'] = [{'texture': 'textures/crate.png', 'color': WHITE},
                         {'texture': 'textures/crate.png', 'color': WHITE},
                         {'texture': '', 'color': WHITE}]
    if depth1:
        mesh['materials'][1]['bitDepth'] = depth1


mid = None  # the Crate's materials as saved by SAVE_MID
rects = {}  # the last run's named controls


def run(args, edit, shots=None):
    """Runs the editor on a fresh courtyard copy with Crate selected, then
    `args` and Ctrl+S; returns the saved Crate's mesh component and the output."""
    global mid, rects
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    d = json.load(open(scene, encoding='utf-8'))
    edit(d)
    json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = shots or os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Crate', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--move', '760,940', '--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    mid = crate_mesh(json.load(open(snap, encoding='utf-8'))) if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return crate_mesh(json.load(open(scene, encoding='utf-8'))), out


def inside(name, outer='obj.panel'):
    a, b = rects.get(name), rects.get(outer)
    return bool(a and b and a[0] >= b[0] and a[1] >= b[1] and a[2] <= b[2] and a[3] <= b[3])


def depths(mesh):
    return [m.get('bitDepth') for m in mesh['materials']] if mesh else None


SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']
popup = os.path.join(keep, 'matdepth-popup.png') if keep else os.path.join(tempfile.mkdtemp(), 'popup.png')
final = os.path.join(keep, 'matdepth-inspector.png') if keep else None

# --- Material 2 to 16 bpp, then Ctrl+Z ---
script = ['--click', '@obj.matdepth.1', '--wait', '5', '--shot', popup, '--click', '@obj.matdepth.1/16 bpp', '--wait', '5'] + \
    SAVE_MID + ['--key', 'ctrl+z', '--wait', '3']
mesh, out = run(script, three, final)
check('a Depth row under each textured material, inside the inspector', inside('obj.matdepth.0') and inside('obj.matdepth.1'),
      sorted(k for k in rects if k.startswith('obj.matdepth')))
check('no Depth row for the untextured material', 'obj.matdepth.2' not in rects, sorted(k for k in rects if k.startswith('obj.matdepth')))
check('the Depth row sits under its texture row', inside('obj.matdepth.1') and inside('obj.texture.1') and
      rects['obj.matdepth.1'][1] >= rects['obj.texture.1'][3], (rects.get('obj.texture.1'), rects.get('obj.matdepth.1')))
check('16 bpp is saved on Material 2 only', depths(mid) == [None, 16, None] and mid['bitDepth'] == 8, mid)
check('one Ctrl+Z takes it off', depths(mesh) == [None, None, None] and mesh['bitDepth'] == 8, mesh)
check('the textures are untouched', [m.get('texture') or '' for m in mesh['materials']] == ['textures/crate.png'] * 2 + [''], mesh)

# --- Object clears an override ---
mesh, out = run(['--click', '@obj.matdepth.1', '--wait', '5', '--click', '@obj.matdepth.1/Object (8 bpp)', '--wait', '5'],
                lambda d: three(d, 16))
check('Object (8 bpp) clears the override', depths(mesh) == [None, None, None] and mesh['bitDepth'] == 8, mesh)

# --- The object's Bit depth row moves the object and keeps an override ---
mesh, out = run(['--click', '@obj.bpp', '--wait', '5', '--click', '@obj.bpp/4 bpp', '--wait', '5'], lambda d: three(d, 16))
check('a Bit depth row for the object, inside the inspector', inside('obj.bpp'), rects.get('obj.bpp'))
check('Bit depth 4 bpp moves the object, Material 2 stays at 16', depths(mesh) == [None, 16, None] and mesh['bitDepth'] == 4, mesh)

print('matdepthcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
