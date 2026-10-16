# The Problems pill and popup after a Play over a build list of several
# scenes, driven through the GUI on courtyard copies (level2, level3 beside
# it, scenes.list naming them). pcsx-redux and the engine are /bin/true, so
# Play gets past its tool check, builds, and the "game" exits at once. The
# editor prints "build report: Play scenes N errors E warnings W" per finished
# build (what the open scene's live export does not already list),
# "problems: <pill text>" and "playstatus: <status line>" when they change.
# usage: python3 buildproblemscheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/bin/true', SPLASHEDIT_PSXSPLASH='/bin/true')
WARN = {'enabled': True, 'title': ''}                                 # two warnings: no title, no icon
ERR = {'enabled': True, 'title': 'x' * 40, 'region': 'bad!'}          # two errors, plus the icon warning


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(listed, open_mc=None, args=None, shot=None):
    """A courtyard copy with level2 (warnings only), level3 (errors) and
    zbroken.scene (not a scene; sorts after courtyard, which is what opens);
    scenes.list holds `listed`. Presses F5, then `args`, and opens the Problems
    popup. Returns the printed lines and the end-of-run control rects."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    base = json.load(open(os.path.join(project, 'courtyard.scene')))

    def scene(name, mc):
        s = json.loads(json.dumps(base))
        if mc: s['settings']['memCard'] = mc
        json.dump(s, open(os.path.join(project, name), 'w'))
    scene('courtyard.scene', open_mc)
    scene('level2.scene', WARN)
    scene('level3.scene', ERR)
    open(os.path.join(project, 'zbroken.scene'), 'w').write('{ not json')
    open(os.path.join(project, 'scenes.list'), 'w').write(''.join(l + '\n' for l in listed))
    open(os.path.join(project, 'one.list'), 'w').write('courtyard.scene\n')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '30', '--key', 'f5', '--wait', '60', '--wait-play'] + \
          [a.replace('@PROJECT', project) for a in (args or [])] + \
          ['--wait-export', '--click', '@problems', '--wait', '5', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return lines, rects


def tagged(lines, prefix):
    return [l for l in lines if l.startswith(prefix)]


# Warnings only in another scene: the pill counts them, the popup lists them under their build.
lines, rects = run(['courtyard.scene', 'level2.scene'])
check('a warning-only build is reported', tagged(lines, 'build report:') == ['build report: Play scenes 2 errors 0 warnings 2'],
      tagged(lines, 'build report:'))
check('the pill counts the other scene\'s warnings', tagged(lines, 'problems:')[-1:] == ['problems: 2 warnings'],
      tagged(lines, 'problems:'))
check('the popup heads the build\'s part', 'problems.build' in rects, sorted(rects))

# The open scene's own warnings are in the live export and the build: counted once.
lines, rects = run(['courtyard.scene', 'level2.scene'], open_mc=WARN)
check('the open scene\'s copies are left out', tagged(lines, 'build report:') == ['build report: Play scenes 2 errors 0 warnings 2'],
      tagged(lines, 'build report:'))
check('live and build add up once', tagged(lines, 'problems:')[-1:] == ['problems: 4 warnings'], tagged(lines, 'problems:'))

# Errors in scene 2: every error and warning is counted, the status line leads with the count.
lines, rects = run(['courtyard.scene', 'level2.scene', 'level3.scene'], shot=sys.argv[2] if len(sys.argv) > 2 else None)
check('a failed build is reported whole', tagged(lines, 'build report:') == ['build report: Play scenes 3 errors 2 warnings 3'],
      tagged(lines, 'build report:'))
check('the pill counts all of it', tagged(lines, 'problems:')[-1:] == ['problems: 2 errors, 3 warnings'], tagged(lines, 'problems:'))
status = tagged(lines, 'playstatus:')
check('the status line names the count and the first error',
      any(l.startswith('playstatus: Play, 2 errors: scene 2 (level3): ') for l in status), status)
pill, ps = rects.get('problems'), rects.get('playstatus')
check('the long status line stops short of the pill', pill and ps and ps[2] < pill[0], (ps, pill))

# A scene that does not load.
lines, rects = run(['courtyard.scene', 'zbroken.scene'])
check('an unloadable scene is a build error', tagged(lines, 'build report:') == ['build report: Play scenes 2 errors 1 warnings 0'],
      tagged(lines, 'build report:'))
check('its status line names it', any(l.startswith('playstatus: Play: Cannot load zbroken.scene: ') for l in tagged(lines, 'playstatus:')),
      tagged(lines, 'playstatus:'))

# The next build replaces the report: a clean one-scene list leaves no problems.
lines, rects = run(['courtyard.scene', 'level2.scene', 'level3.scene'],
                   args=['--copy-file', '@PROJECT/one.list@@PROJECT/scenes.list', '--wait', '5', '--key', 'f5', '--wait', '60', '--wait-play'])
check('the second build replaces the first', tagged(lines, 'build report:') ==
      ['build report: Play scenes 3 errors 2 warnings 3', 'build report: Play scenes 1 errors 0 warnings 0'],
      tagged(lines, 'build report:'))
check('and the pill is clear again', tagged(lines, 'problems:')[-1:] == ['problems: No problems'], tagged(lines, 'problems:'))
check('with no build part in the popup', 'problems.build' not in rects, sorted(rects))

print('%d failure(s)' % fails)
sys.exit(1 if fails else 0)
