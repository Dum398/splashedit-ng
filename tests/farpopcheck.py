# "Far scenery pops in", driven through the GUI. A courtyard copy made into
# tests/boot/walk.sh's world (floor 25 times larger, world streaming on) with
# fog and the edge fade off: the draw distance is then a hard edge, and the export says so. Run
# one: the Problems popup lists the warning and the scene inspector draws it
# as a card ("stream: streamwarnN <title>" names which). Run two: scroll to
# that card, press its "Turn fog on", save with Ctrl+S: fog is on at density 3,
# whose far wall (20000 / 3 = 6666 depth units) is inside the 8192 draw
# distance, and the warning is gone. The same world with fog already on never
# shows it, nor does the fixed scene opened again.
# usage: python3 farpopcheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
TITLE = 'Far scenery pops in'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def world(fog):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    d = json.load(open(scene, encoding='utf-8'))
    for o in d['objects']:
        for c in o.get('children', []):
            if c['name'] == 'Floor':
                c['transform']['scale'] = [25.0, 1.0, 25.0]
    d['settings']['streamWorld'] = True
    d['settings']['fog'] = {'enabled': fog, 'color': [0.5, 0.5, 0.6], 'density': 10 if fog else 5}
    d['settings']['edgeFade'] = False  # on, the edge is not hard and there is no warning
    json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    return project, scene


def run(project, scene, args, shot):
    cmd = [editor, '--size', '1600x960', '--project', project, '--scene', scene, '--wait', '30', '--wait-export'] + args + \
          ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    check('the screenshot was written', os.path.exists(shot), shot)
    return lines


def cards(lines):
    out = {}
    for l in lines:
        m = re.match(r'stream: streamwarn(\d+) (.*)$', l)
        if m: out[m.group(2)] = int(m.group(1))
    return out


project, scene = world(False)
first = shots + '-problems.png' if shots else os.path.join(project, 'problems.png')
lines = run(project, scene, ['--click', '@problems', '--wait', '5'], first)
problems = [l for l in lines if l.startswith('problems: ')]
check('the export warns', bool(problems) and 'warning' in problems[-1], problems[-3:])
found = cards(lines)
check('the scene inspector draws the warning as a card', TITLE in found, found)

if TITLE in found:
    name = 'streamwarn%d' % found[TITLE]
    second = shots + '-fixed.png' if shots else os.path.join(project, 'fixed.png')
    lines = run(project, scene, ['--scroll-to', '@' + name, '--wait', '5'] + (['--shot', shots + '-card.png'] if shots else []) +
                ['--click', '@' + name, '--wait', '5', '--wait-export', '--wait', '5', '--key', 'ctrl+s', '--wait', '10'], second)
    fog = json.load(open(scene, encoding='utf-8'))['settings'].get('fog', {})
    check('the fix turns fog on', fog.get('enabled') is True, fog)
    check('at density 3: its far wall, 6666, is inside the 8192 draw distance', fog.get('density') == 3, fog)
    shown = [l for l in lines if l.startswith('scene: fog ')]
    check('the inspector shows fog on at density 3', bool(shown) and shown[-1].startswith('scene: fog 1 density 3 '), shown[-2:])

    # Opened again as saved: the export no longer warns.
    lines = run(project, scene, [], os.path.join(project, 'reopened.png'))
    check('reopened with the fix, no card carries the warning', TITLE not in cards(lines), cards(lines))

project, scene = world(True)
lines = run(project, scene, [], os.path.join(project, 'fogged.png'))
check('with thick fog (density 10) the world shows no such card', TITLE not in cards(lines), cards(lines))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
