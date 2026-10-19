# Imports the small Unity project in fixtures/unity-mini with
# `splashpack-cli import-unity` and checks the new project: the scene order,
# the object tree (children in Unity's m_Children order, the prefab instance
# unpacked with its overrides), the copied Lua and texture, the report (the
# UI canvas listed, the camera left out) and its summary line, and that the
# imported scene exports. Then the refusals: a folder that is not empty, a
# scene name that is not there, a folder inside the Unity project.
# When /home/pixel/sources/psxsus (or $PSXSUS) is there, imports its five
# game scenes in the game's order and exports each of them; otherwise prints
# SKIP for that part.
# usage: python3 unityimportcheck.py <splashpack-cli>
import json, os, re, subprocess, sys, tempfile

cli = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
mini = os.path.join(here, 'fixtures', 'unity-mini')
psxsus = os.environ.get('PSXSUS', '/home/pixel/sources/psxsus')
tmp = tempfile.mkdtemp()
fails = checks = 0
SUMMARY = re.compile(r'^imported scenes=(\d+) objects=(\d+) meshes=(\d+) scripts=(\d+) textures=(\d+) skipped=(\d+)$')


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def run(*args):
    r = subprocess.run([cli] + list(args), capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout, r.stderr


def tree(objs):
    return [(o['name'], tree(o.get('children', []))) for o in objs]


def comp(o, kind):
    return next((c for c in o.get('components', []) if c['type'] == kind), None)


# ---- the fixture ----
dest = os.path.join(tmp, 'mini')
rc, out, err = run('import-unity', mini, dest)
check('import of the fixture succeeds', rc == 0, err)
lines = out.splitlines()
summary = [l for l in lines if SUMMARY.match(l)]
check('the summary line is printed last', lines and SUMMARY.match(lines[-1]) is not None, out[-300:])
check('the summary counts', summary == ['imported scenes=1 objects=10 meshes=4 scripts=2 textures=1 skipped=3'], summary)
check('the canvas is in the printed report', '[later] Mini / HUD: UI canvas with 1 element (1 Text) not imported yet' in lines, out)
check('the camera is reported as left out', '[left out] Mini / Main Camera: Camera, AudioListener (no PS1 equivalent)' in lines, out)
check('the approximated sphere is reported', any(l.startswith('[changed] Mini / RenamedLamp/Bulb:') for l in lines), out)
report = open(os.path.join(dest, 'import-report.txt')).read() if os.path.exists(os.path.join(dest, 'import-report.txt')) else ''
check('import-report.txt holds the report and the summary', 'UI canvas with 1 element' in report and summary and summary[0] in report, report[:400])
check('scenes.list names the scene', open(os.path.join(dest, 'scenes.list')).read().split() == ['Mini.scene'] if os.path.exists(os.path.join(dest, 'scenes.list')) else False)
for f in ['scripts/cube.lua', 'scripts/scene.lua', 'textures/checker.png', 'meshes/unity_cube.mesh', 'meshes/unity_quad.mesh',
          'meshes/unity_sphere.mesh', 'models/tri.obj', 'vram.json']:
    check('the project has ' + f.replace('/', ' '), os.path.isfile(os.path.join(dest, f)), f)
scene = json.load(open(os.path.join(dest, 'Mini.scene'))) if os.path.exists(os.path.join(dest, 'Mini.scene')) else {'objects': [], 'settings': {}}
want = [('Parent', [('ChildB', []), ('ChildA', [])]), ('Cube', []), ('Quad', []),
        ('RenamedLamp', [('Bulb', []), ('Extra', [])]), ('Main Camera', []), ('Tri', [])]
check('the object tree in Unity order', tree(scene['objects']) == want, tree(scene['objects']))
by = {o['name']: o for o in scene['objects']}
lamp = by.get('RenamedLamp', {})
check('the prefab root keeps its position override', lamp.get('transform', {}).get('position') == [3, 1.5, 0], lamp.get('transform'))
bulb = (lamp.get('children') or [{}])[0]
check('the prefab child keeps its bit depth override', (comp(bulb, 'mesh') or {}).get('bitDepth') == 16, comp(bulb, 'mesh'))
cube = by.get('Cube', {})
check('the cube carries its Lua file', (comp(cube, 'script') or {}).get('lua') == 'scripts/cube.lua', comp(cube, 'script'))
check('the cube starts hidden', cube.get('startActive') is False, cube.get('startActive'))
quad = by.get('Quad', {})
check('the quad carries its material texture', ((comp(quad, 'mesh') or {}).get('materials') or [{}])[0].get('texture') == 'textures/checker.png',
      comp(quad, 'mesh'))
st = scene['settings']
check('the scene settings', st.get('gteScaling') == 150 and st.get('sceneType') == 'interior' and st.get('networkId') == 'mini/main' and
      st.get('script') == 'scripts/scene.lua', st)
rc, out, err = run('export', os.path.join(dest, 'Mini.scene'), '-o', os.path.join(tmp, 'mini.splashpack'))
check('the imported fixture scene exports', rc == 0 and os.path.getsize(os.path.join(tmp, 'mini.splashpack')) > 0, err)

# ---- refusals ----
rc, out, err = run('import-unity', mini, dest)
check('a folder that is not empty is refused', rc == 1 and 'not empty' in err, (rc, err))
rc, out, err = run('import-unity', mini, os.path.join(tmp, 'none'), '--scenes', 'Nope')
check('an unknown scene name is refused and writes nothing', rc == 1 and 'Nope' in err and not os.path.exists(os.path.join(tmp, 'none')),
      (rc, err))
inside = os.path.join(mini, 'Assets', 'imported')
rc, out, err = run('import-unity', mini, inside)
check('a folder inside the Unity project is refused', rc == 1 and not os.path.exists(inside), (rc, err))
rc, out, err = run('import-unity', mini, os.path.join(tmp, 'both'), '--scenes', 'Empty,Mini')
check('scenes come in the order asked', rc == 0 and open(os.path.join(tmp, 'both', 'scenes.list')).read().split() == ['Empty.scene', 'Mini.scene'],
      (rc, err))

# ---- psxsus ----
if not os.path.isdir(os.path.join(psxsus, 'Assets')):
    print('SKIP psxsus: %s is not there' % psxsus)
else:
    order = ['SCN_Menu', 'SCN_Lobby', 'SCN_Game_Hulk', 'SCN_Game_Rim', 'SCN_Game_Core']
    pdest = os.path.join(tmp, 'psxsus')
    rc, out, err = run('import-unity', psxsus, pdest, '--scenes', ','.join(order))
    check('psxsus imports', rc == 0, err)
    lines = out.splitlines()
    print('psxsus: ' + (lines[-1] if lines else '(no output)'))
    m = SUMMARY.match(lines[-1]) if lines else None
    check('psxsus: a summary line with five scenes', m is not None and m.group(1) == '5', lines[-1:] if lines else out)
    check('psxsus: scenes.list in the game order',
          os.path.exists(os.path.join(pdest, 'scenes.list')) and open(os.path.join(pdest, 'scenes.list')).read().split() == [s + '.scene' for s in order])
    check('psxsus: the audio prefab is unpacked under its new name',
          any(l.startswith('[later] SCN_Game_Hulk / GameAudio/SFX_') for l in lines), out[-500:])
    for s in order:
        sc = os.path.join(pdest, s + '.scene')
        rc, out, err = run('export', sc, '-o', os.path.join(tmp, s + '.splashpack'))
        check('psxsus: %s exports' % s, rc == 0, err[-400:])

print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
