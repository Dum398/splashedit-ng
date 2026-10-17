# The mesh inspector's UV offset row (obj.uvoff), driven through the GUI on a
# courtyard copy whose crate mesh is split into four parts drawn by materials
# [untextured, crate.png, stone.png, crate.png]. The row lists one item per
# distinct texture, numbered as the exporter numbers them; picking one is saved
# as uvOffsetMaterial and one Ctrl+Z puts it back. Each saved scene is exported
# with splashpack-cli: the triangles flagged for the UV offset are exactly the
# materials the inspector's `uvoffset:` line names. An out-of-range value shows
# a warning and flags nothing; a crate with no texture has no row.
# usage: python3 uvoffsetcheck.py <splashedit> <splashpack-cli> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2])
keep_shot = sys.argv[3] if len(sys.argv) > 3 else None
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import splashdiff  # noqa: E402

example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
WHITE = [1.0, 1.0, 1.0, 1.0]
CRATE_ITEM = 'obj.uvoff/Materials 2 and 4 (crate.png)'
STONE_ITEM = 'obj.uvoff/Material 3 (stone.png)'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def crate_mesh(d):
    crates = next(o for o in d['objects'] if o['name'] == 'Crates')
    crate = next(o for o in crates['children'] if o['name'] == 'Crate')
    return next(c for c in crate['components'] if c['type'] == 'mesh')


def four(d):
    crate_mesh(d)['materials'] = [{'texture': '', 'color': WHITE},
                                  {'texture': 'textures/crate.png', 'color': WHITE},
                                  {'texture': 'textures/stone.png', 'color': WHITE},
                                  {'texture': 'textures/crate.png', 'color': WHITE}]


rects = {}


def run(args, edit=None, shot=None):
    """Editor on a fresh courtyard copy (crate mesh in four parts), Crate
    selected, then `args` and Ctrl+S. Returns the saved Crate's mesh
    component, the `uvoffset:` lines, the output, and the project path."""
    global rects
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    mp = os.path.join(project, 'meshes', 'crate.mesh')
    m = json.load(open(mp, encoding='utf-8'))
    idx = m['submeshes'][0]
    q = len(idx) // 4
    m['submeshes'] = [idx[0:q], idx[q:2 * q], idx[2 * q:3 * q], idx[3 * q:]]
    json.dump(m, open(mp, 'w', encoding='utf-8'), indent=2)
    scene = os.path.join(project, 'courtyard.scene')
    d = json.load(open(scene, encoding='utf-8'))
    (edit or four)(d)
    json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Crate', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--move', '760,940', '--wait', '5', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        mm = re.match(r'control (\S+(?: \S+)*) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if mm: rects[mm.group(1)] = tuple(int(v) for v in mm.groups()[1:])
    uv = [l for l in lines if l.startswith('uvoffset: ')]
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return crate_mesh(json.load(open(scene, encoding='utf-8'))), uv, out, project


def flagged(project):
    """Exports the saved scene; returns (flagged tri count, flagged tris share a
    texture no unflagged tri has, total tris) for the first object named Crate."""
    out = os.path.join(os.path.dirname(project), 'out.splashpack')
    r = subprocess.run([cli, 'export', os.path.join(project, 'courtyard.scene'), '-o', out], capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stdout + r.stderr, 0
    pk = splashdiff.Pack(out)
    o = pk.objects[pk.names.index('Crate')]
    tris = [splashdiff.tri_fields(t) for t in o['tris']]
    key = lambda f: (f['tpage'], f['clutX'], f['clutY'])
    on = {key(f) for f in tris if f['flags'] & 1}
    off = {key(f) for f in tris if not f['flags'] & 1}
    return sum(1 for f in tris if f['flags'] & 1), len(on) <= 1 and not (on & off), len(tris)


def inside(name, outer='obj.panel'):
    a, b = rects.get(name), rects.get(outer)
    return bool(a and b and a[0] >= b[0] and a[1] >= b[1] and a[2] <= b[2] and a[3] <= b[3])


# --- as loaded: slot 0 is crate.png, materials 2 and 4 (indices 1, 3) ---
mesh, uv, out, project = run([])
check('the UV offset row is drawn in the inspector', inside('obj.uvoff'), sorted(k for k in rects if k.startswith('obj.')))
check('slot 0 names materials 1 and 3', uv[-1:] == ['uvoffset: 0 materials 1,3'], uv)
n, one, total = flagged(project)
check('the export flags the crate.png parts only', n == 2 * total // 4 and one, (n, one, total))

# --- pick stone.png from the menu: saved as slot 1, the export follows ---
mesh, uv, out, project = run(['--click', '@obj.uvoff', '--wait', '3', '--click', '@' + STONE_ITEM, '--wait', '3'],
                             shot=keep_shot)
check('the pick is saved as slot 1', mesh.get('uvOffsetMaterial') == 1, mesh.get('uvOffsetMaterial'))
check('the line names material 2', uv[-1:] == ['uvoffset: 1 materials 2'], uv)
n, one, total = flagged(project)
check('the export flags the stone.png part only', n == total // 4 and one, (n, one, total))

# --- the menu left open: one item per texture, inside the window ---
mesh, uv, out, project = run(['--click', '@obj.uvoff', '--wait', '3'])
check('both textures are offered', CRATE_ITEM in rects and STONE_ITEM in rects and len([k for k in rects if k.startswith('obj.uvoff/')]) == 2,
      sorted(k for k in rects if k.startswith('obj.uvoff')))

# --- one Ctrl+Z puts it back ---
mesh, uv, out, project = run(['--click', '@obj.uvoff', '--wait', '3', '--click', '@' + STONE_ITEM, '--wait', '3', '--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z restores slot 0', mesh.get('uvOffsetMaterial') == 0, mesh.get('uvOffsetMaterial'))


# --- a value with no texture: warning, flags nothing ---
def far(d):
    four(d)
    crate_mesh(d)['uvOffsetMaterial'] = 5


mesh, uv, out, project = run([], edit=far)
check('an out-of-range value shows the warning', inside('obj.uvoff.warn') and inside('obj.uvoff'), [(k, rects.get(k)) for k in ('obj.panel', 'obj.uvoff', 'obj.uvoff.warn')])
check('the line names no material', uv[-1:] == ['uvoffset: 5 materials none'], uv)
n, one, total = flagged(project)
check('the export flags nothing', n == 0 and total > 0, (n, total))


# --- no textured material: no row (the Colour rows are the positive control) ---
def plain(d):
    crate_mesh(d)['materials'] = [{'texture': '', 'color': WHITE}] * 2


mesh, uv, out, project = run([], edit=plain)
check('the material rows are drawn', inside('obj.texture.0') and inside('obj.texture.1'), sorted(k for k in rects if k.startswith('obj.')))
check('no UV offset row without a texture', 'obj.uvoff' not in rects and not uv, (rects.get('obj.uvoff'), uv))

print('uvoffsetcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
