# Import settings' Convert units row: drops unit_cm.fbx (a 1 x 3 x 2 cm box,
# from fixtures/unity-models) on the viewport of a copy of the courtyard
# example, checks it comes in converted to metres (0.01 x 0.03 x 0.02), clicks
# the inspector's Convert units toggle off and checks that the model is
# imported again at the file's numbers (1 x 3 x 2) and the sidecar keeps it off.
# usage: python3 importunitscheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


root = tempfile.mkdtemp()
src = os.path.join(root, 'unit_cm.fbx')
shutil.copyfile(os.path.join(here, 'fixtures', 'unity-models', 'Assets', 'Units', 'unit_cm_on1.fbx'), src)
W = lambda n: ['--wait', str(n)]


def run(name, args):
    """The drop on a fresh copy of the courtyard, then `args`: the editor's
    output and the size of the baked mesh, and the sidecar."""
    project = os.path.join(root, name)
    shutil.copytree(example, project)
    shot = os.path.join(root, name + '.png')
    r = subprocess.run([editor, '--size', '1600x960', '--project', project] + W(5) + ['--drop-now', src + '@800,400'] + W(10) +
                       ['--wait-import'] + W(10) + args + ['--screenshot', shot], capture_output=True, text=True, env=ENV, timeout=300)
    if keep and name == 'off' and os.path.exists(shot): shutil.copyfile(shot, keep)
    out = (r.stdout + r.stderr).strip()
    for l in out.splitlines():
        if l.startswith('@'): print('editor: ' + l)  # a scripted control it could not find
    mesh = os.path.join(project, 'models', 'unit_cm.mesh')
    size = None
    if os.path.exists(mesh):
        p = json.load(open(mesh))['positions']
        size = [max(v) - min(v) for v in (p[0::3], p[1::3], p[2::3])]
    side = os.path.join(project, 'models', 'unit_cm.fbx.import.json')
    return out, size, json.load(open(side)) if os.path.exists(side) else {}


def near(got, want):
    return got is not None and all(abs(g - w) <= 1e-3 * w for g, w in zip(got, want))


out, size, side = run('on', [])
check('the drop imports the cm box in metres', near(size, [0.01, 0.03, 0.02]), size or out[-600:])
check('the row shows Convert units on', 'import convert units: on' in out, out[-600:])
out, size, side = run('off', ['--click', '@iunits'] + W(10) + ['--wait-import'] + W(20))
lines = [l for l in out.splitlines() if l.startswith('import convert units: ')]
check('the toggle turns Convert units off', lines[-2:] == ['import convert units: on', 'import convert units: off'], lines or out[-600:])
check('the sidecar keeps it off', side.get('convertUnits') is False, side)
check("the model is imported again at the file's numbers", near(size, [1, 3, 2]), size)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
