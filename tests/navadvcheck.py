# The nav bake's advanced settings on the Player inspector, driven through the
# GUI: the Indoor / Terrain / Multi-Level preset buttons (obj.ppreset.<key>),
# the caption saying which preset the settings match (obj.ppresetnow), the
# Bake details disclosure (obj.pbakedetails) with the Method dropdown
# (obj.ppart) and the region fields (obj.pminarea ... obj.pplaneerr), and the
# warning for regions whose floor plane strays from the ground by more than
# Max plane error (obj.navplane). Under scripted runs the editor prints
#   player preset: Matches Indoor|Matches Terrain|Matches Multi-Level|Custom
#   player bake: preset P method M area MIN/MERGE simplify S edge E detail D/ERR plane X
#   nav plane: N over X
# as they change. The courtyard copy gets a gently rolling floor (a few
# centimetres), so its regions sit within the default 0.15 m of their planes
# but not within 0.001 m.
# usage: python3 navadvcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, math, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
OPEN = ['--click', '@obj.pbakedetails'] + W(3) + ['--wheel', '1420,500,-10'] + W(3)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def fixture():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    path = os.path.join(project, 'meshes', 'floor.mesh')
    mesh = json.load(open(path, encoding='utf-8'))
    p = mesh['positions']
    for i in range(0, len(p), 3):
        p[i + 1] += 0.05 * math.sin(p[i] * 1.3) * math.cos(p[i + 2] * 1.1)
    json.dump(mesh, open(path, 'w', encoding='utf-8'))
    return project


def run(name, args):
    """Runs the editor on a fresh fixture with the Player Start selected and
    its export done, then `args`; returns the project, the printed lines of
    interest and the named controls' rects."""
    project = fixture()
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Player Start'] + W(20) + ['--wait-export'] + \
        args + ['--move', '760,940'] + W(5) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=280)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    keep = [l for l in lines if l.startswith(('player preset: ', 'player bake: ', 'nav plane: '))]
    return project, keep, rects


def of(lines, prefix):
    return [l for l in lines if l.startswith(prefix)]


INDOOR = 'player bake: preset indoor method watershed area 8/20 simplify 1.3 edge 12 detail 6/0.025 plane 0.15'
TERRAIN = 'player bake: preset terrain method monotone area 4/6 simplify 0.4 edge 5 detail 4/0.02 plane 0.1'
PLANE = re.compile(r'nav plane: (\d+) over (\S+)$')

# Default: Indoor, nothing off its plane. Then Terrain, and undo back to Indoor.
_, lines, rects = run('presets', W(5) + ['--click', '@obj.ppreset.terrain'] + W(5) + ['--wait-export'] + W(5) +
                      ['--key', 'ctrl+z'] + W(5) + ['--wait-export'] + W(5))
bake, pre, plane = of(lines, 'player bake: '), of(lines, 'player preset: '), of(lines, 'nav plane: ')
check('the default settings are the Indoor preset', bake[:1] == [INDOOR] and pre[:1] == ['player preset: Matches Indoor'],
      (bake, pre))
check('the default bake has no region off its plane', plane[:1] == ['nav plane: 0 over 0.15'], plane)
check('Terrain sets the Terrain values', TERRAIN in bake, bake)
check('the caption says Matches Terrain', 'player preset: Matches Terrain' in pre, pre)
VALUES = {INDOOR.split(' method ')[1]: 'indoor', TERRAIN.split(' method ')[1]: 'terrain'}
torn = [l for l in bake if VALUES.get(l.split(' method ')[1], l.split()[3]) != l.split()[3]]
check('every bake line names the preset its values match (no half-edited frame)', not torn, torn)
check('Terrain is one undo step back to Indoor', bake[-2:] == [TERRAIN, INDOOR] and pre[-1:] == ['player preset: Matches Indoor'],
      bake)
check('the preset buttons and caption are drawn',
      all(n in rects for n in ('obj.ppreset.indoor', 'obj.ppreset.terrain', 'obj.ppreset.multilevel', 'obj.ppresetnow')),
      sorted(k for k in rects if 'preset' in k))
panel = rects.get('s_player', (0, 0, 0, 0))
check('the preset buttons fit in the panel', all(rects.get(n, (0, 0, 1e9, 0))[2] <= panel[2] for n in
                                                 ('obj.ppreset.indoor', 'obj.ppreset.terrain', 'obj.ppreset.multilevel')),
      ({n: rects.get(n) for n in ('obj.ppreset.multilevel', 's_player')}))
check('the details start closed', 'obj.pbakedetails' in rects and 'obj.ppart' not in rects and 'obj.pminarea' not in rects,
      sorted(k for k in rects if k.startswith('obj.p')))
check('no plane warning at the default', 'obj.navplane' not in rects and plane[-1:] == ['nav plane: 0 over 0.15'], plane)

# Method and Min region area, saved.
project, lines, rects = run('edit', OPEN + ['--click', '@obj.ppart'] + W(3) + ['--click', '@obj.ppart/Layer'] + W(3) +
                            ['--dblclick', '@obj.pminarea'] + W(3) + ['--key', 'ctrl+a', '--text', '5', '--key', 'enter'] + W(3) +
                            ['--key', 'ctrl+s'] + W(5))
check('the details show every field',
      all(('obj.p' + n) in rects for n in ('part', 'minarea', 'mergearea', 'simplify', 'edgelen', 'detdist', 'deterr', 'planeerr')),
      sorted(k for k in rects if k.startswith('obj.p')))
bake = of(lines, 'player bake: ')
check('the bake line follows Layer and 5', bake[-1:] == [
    'player bake: preset custom method layer area 5/20 simplify 1.3 edge 12 detail 6/0.025 plane 0.15'], bake)
check('the caption says Custom', of(lines, 'player preset: ')[-1:] == ['player preset: Custom'], lines)
scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
comp = next((c for o in scene['objects'] if o.get('name') == 'Player Start' for c in o.get('components', [])
             if c.get('type') == 'player'), {})
check('the saved scene has navPartitionMethod layer', comp.get('navPartitionMethod') == 'layer', comp)
check('the saved scene has navMinRegionArea 5', comp.get('navMinRegionArea') == 5, comp)

# Max plane error 0.001: the rolling floor's regions are over it.
_, lines, rects = run('plane', OPEN + ['--dblclick', '@obj.pplaneerr'] + W(3) + ['--key', 'ctrl+a', '--text', '0.001', '--key', 'enter'] +
                      W(5) + ['--wait-export'] + W(10))
plane = of(lines, 'nav plane: ')
ms = [PLANE.match(l) for l in plane]
check('the plane line starts at 0 over 0.15 (positive control)', plane[:1] == ['nav plane: 0 over 0.15'], plane)
last = ms[-1] if ms and ms[-1] else None
check('with 0.001 some regions are over it', last is not None and last.group(2) == '0.001' and int(last.group(1)) > 0, plane)
check('the plane warning is drawn', 'obj.navplane' in rects, sorted(k for k in rects if 'nav' in k))
nr = rects.get('obj.navplane', (0, 0, 0, 0))
check('the plane warning fits in the panel', panel[0] <= nr[0] and nr[2] <= panel[2], (nr, panel))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
