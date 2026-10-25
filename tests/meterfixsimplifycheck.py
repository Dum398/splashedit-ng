# Budget meters suggest fixes (#194), Triangles: simplifying a mesh to half
# is one click and one undo step. A fresh project gets a 3200-triangle grid
# (tests write it, 40 x 40 quads with UVs) dropped in and saved, which puts
# the Triangles meter in the red. A second run clicks the meter, waits for the
# fixes to be measured, applies the first (it sets the model's Triangles and
# imports it again), copies the sidecar and the mesh aside, undoes with
# Ctrl+Z, copies them again, redoes with Ctrl+Shift+Z, copies them, and undoes
# once more. Checks: the row is an Apply for the dropped model's mesh; the
# meter moved by exactly the saving the row showed, back on undo and down
# again on redo; the sidecar had the target while applied and the mesh the
# triangles listed; after undo every file in models/ is byte for byte as it
# was before Apply.
# usage: python3 meterfixsimplifycheck.py <splashedit> [screenshot dir]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = sys.argv[2] if len(sys.argv) > 2 else None
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(cmd, timeout=400):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=timeout)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        print('editor timed out')
        return (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')


def tree(d):
    """{path relative to d: bytes} of every file under d."""
    out = {}
    for base, _, files in os.walk(d):
        for f in files:
            p = os.path.join(base, f)
            out[os.path.relpath(p, d)] = open(p, 'rb').read()
    return out


def mesh_tris(path):
    m = json.load(open(path))
    return sum(len(s) // 3 for s in m['submeshes'])


root = tempfile.mkdtemp()
project = os.path.join(root, 'game')
os.makedirs(project)
scene = {'format': 'splashedit-ng/scene', 'version': 1,
         'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': ''}, 'objects': []}
json.dump(scene, open(os.path.join(project, 'grid.scene'), 'w'), indent=1)
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

W = lambda n: ['--wait', str(n)]
base = [editor, '--size', '1600x960', '--project', project, '--scene', 'grid.scene']
out1 = run(base + W(5) + ['--drop-now', grid + '@800,400'] + W(10) + ['--wait-import'] + W(5) + ['--key', 'ctrl+s'] + W(5) +
           ['--screenshot', os.path.join(root, 'placed.png')])
models = os.path.join(project, 'models')
side = os.path.join(models, 'grid.obj.import.json')
sc = json.load(open(os.path.join(project, 'grid.scene')))
meshes = []
def walk(objs):
    for o in objs:
        for c in o.get('components', []):
            if c.get('type') == 'mesh': meshes.append(c['mesh'])
        walk(o.get('children', []))
walk(sc['objects'])
check('the grid was imported and placed', os.path.exists(side) and len(meshes) == 1, (meshes, out1[-2000:]))
if fails:
    print('meterfixsimplifycheck: %d failed' % fails)
    sys.exit(1)
mesh = os.path.join(project, meshes[0])
was = tree(models)

aside = os.path.join(root, 'aside')
os.makedirs(aside)
cp = lambda tag: ['--copy-file', side + '@' + os.path.join(aside, tag + '.json'), '--copy-file',
                  mesh + '@' + os.path.join(aside, tag + '.mesh')]
shot1 = os.path.join(shots or root, 'meterfixsimplify-open.png')
shot2 = os.path.join(shots or root, 'meterfixsimplify-applied.png')
shot3 = os.path.join(shots or root, 'meterfixsimplify-undone.png')
WAIT = W(3) + ['--wait-export'] + W(5)
cmd = base + W(20) + ['--wait-export'] + W(5) + ['--click', '@m_tris'] + WAIT + ['--shot', shot1, '--click',
      '@meterfix.tris.0.apply'] + WAIT + ['--shot', shot2] + cp('applied') + ['--key', 'ctrl+z'] + WAIT + cp('undone') + \
      ['--shot', shot3, '--key', 'ctrl+shift+z'] + WAIT + cp('redone') + ['--key', 'ctrl+z'] + WAIT + ['--screenshot',
      os.path.join(root, 'final.png')]
out = run(cmd)
lines = out.splitlines()
for l in lines:
    if l.startswith(('meterfix:', 'statusbar: tris')): print('  ' + l)

rows = [(int(m.group(1)), m.group(2), int(m.group(3)), m.group(4))
        for m in (re.match(r'meterfix: tris (\d+) (.*) saves (\d+) (apply|select)( about)?$', l) for l in lines) if m]
first = [r for r in rows if r[0] == 0]
check('the Triangles list has the grid, simplified to half, as a one-click Apply',
      bool(first) and first[0][1] == os.path.basename(mesh) + ': 3200 triangles' and first[0][3] == 'apply' and first[0][2] > 0,
      first[:1])
applied = [i for i, l in enumerate(lines) if l.startswith('meterfix: applied ')]
check('Apply on row 0 applied it', len(applied) == 1 and first and lines[applied[0]] == 'meterfix: applied tris 0 saves %d' % first[0][2],
      [lines[i] for i in applied] or [l for l in lines if l.startswith('meterfix: failed')])
tris = [(i, int(m.group(1))) for i, m in ((i, re.match(r'statusbar: tris (\d+)/\d+ (good|warn|bad)$', l)) for i, l in enumerate(lines)) if m]
seq = []
if applied:
    pre = [v for i, v in tris if i < applied[0]]
    seq = pre[-1:] + [v for i, v in tris if i > applied[0]]
    seq = [v for k, v in enumerate(seq) if k == 0 or v != seq[k - 1]]
check('the meter moved by exactly the saving shown, back on undo, down on redo, back on undo',
      len(seq) == 5 and first and seq[0] - seq[1] == first[0][2] and seq[2] == seq[0] and seq[3] == seq[1] and seq[4] == seq[0],
      (seq, first[:1]))
ok = all(os.path.exists(os.path.join(aside, t + e)) for t in ('applied', 'undone', 'redone') for e in ('.json', '.mesh'))
check('the files were copied at each step', ok)
if ok:
    a = json.load(open(os.path.join(aside, 'applied.json')))
    check('applied: the sidecar has Triangles 1600 and the 3200 it started from',
          a.get('targetTriangles') == 1600 and a.get('sourceTriangles') == 3200, a)
    n = mesh_tris(os.path.join(aside, 'applied.mesh'))
    check('applied: the mesh has what the row listed (3200 less its saving)', first and n == 3200 - first[0][2] and n <= 1600,
          (n, first[:1]))
    check('undone: the sidecar and mesh as they were',
          open(os.path.join(aside, 'undone.json'), 'rb').read() == was['grid.obj.import.json'] and
          open(os.path.join(aside, 'undone.mesh'), 'rb').read() == was[os.path.relpath(mesh, models)])
    check('redone: the sidecar and mesh simplified again',
          open(os.path.join(aside, 'redone.json'), 'rb').read() == open(os.path.join(aside, 'applied.json'), 'rb').read() and
          open(os.path.join(aside, 'redone.mesh'), 'rb').read() == open(os.path.join(aside, 'applied.mesh'), 'rb').read())
now = tree(models)
diff = sorted(k for k in set(was) | set(now) if was.get(k) != now.get(k))
check('after the last Ctrl+Z every file in models/ is byte for byte as before Apply', not diff, diff)

print('meterfixsimplifycheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
