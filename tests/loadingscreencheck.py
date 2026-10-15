# The scene inspector's Loading screen section, driven through the GUI on a
# courtyard copy given two UI canvases: Title (with a progress bar named
# "loading") and Bare (no bar, one Line). Pick each from the Canvas menu,
# save with Ctrl+S and read settings.loadingCanvas back; the inspector's
# `scene: loading ...` lines say what it picked and which warnings it drew.
# Ctrl+Z takes a pick back. Controls are clicked by name (--click @scene.loading).
# usage: python3 loadingscreencheck.py <splashedit> [title.png [bare.png]]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = sys.argv[2:4]
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def element(type_, name, **kw):
    e = {'type': type_, 'name': name, 'rect': [20, 20, 100, 16]}
    e.update(kw)
    return e


CANVASES = [
    {'name': 'Title', 'elements': [element('box', 'back', rect=[0, 0, 320, 240], color=[0.1, 0.1, 0.2]),
                                   element('progress', 'loading', rect=[60, 200, 200, 8], value=40,
                                           background=[0.2, 0.2, 0.2])]},
    {'name': 'Bare', 'elements': [element('box', 'panel', rect=[40, 40, 240, 160]),
                                  element('line', 'rule', **{'from': [40, 120], 'to': [280, 120]})]},
]


def run(args, shot=None):
    """Runs the editor on a fresh courtyard copy with the two canvases
    (nothing selected, so the inspector shows the scene), then `args` and
    Ctrl+S; returns the saved scene's settings and the editor's output
    (without its list of controls) and the control list."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    j = json.load(open(scene, encoding='utf-8'))
    j['canvases'] = CANVASES
    json.dump(j, open(scene, 'w', encoding='utf-8'), indent=2)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    controls = [l for l in lines if l.startswith('control ')]
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8'))['settings'], out, controls


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def pick(item):
    return click('scene.loading') + click('scene.loading/' + item)


UNDO = ['--key', 'ctrl+z', '--wait', '3']

s, out, controls = run([])
check('the Canvas dropdown is drawn', any(c.startswith('control scene.loading at') for c in controls), controls[:5])
check('nothing is reported before a pick', 'scene: loading' not in out, out[-300:])
check('save alone sets no loading canvas', 'loadingCanvas' not in s, s)

s, out, _ = run(pick('Title'), shots[0] if shots else None)
check('picking Title reports it', 'scene: loading Title' in out, out[-300:])
check('the saved scene names Title', s.get('loadingCanvas') == 'Title', s)
check('Title has its bar: no warning', 'scene: loading warnings' not in out, out[-300:])

s, out, _ = run(pick('Bare'), shots[1] if len(shots) > 1 else None)
check('picking Bare reports it', 'scene: loading Bare' in out, out[-300:])
check('the saved scene names Bare', s.get('loadingCanvas') == 'Bare', s)
check('Bare draws the no-bar and the lines warnings', 'scene: loading warnings bar,line' in out, out[-300:])

s, out, _ = run(pick('Title') + pick('None'))
check('picking None reports none', 'scene: loading none' in out, out[-300:])
check('the saved scene has no loading canvas', 'loadingCanvas' not in s, s)

s, out, _ = run(pick('Title') + pick('Bare') + UNDO)
check('one Ctrl+Z puts Title back', s.get('loadingCanvas') == 'Title' and out.count('scene: loading Title') == 2,
      f'{s.get("loadingCanvas")} | ' + out[-300:])
warns = [l for l in out.splitlines() if l.startswith('scene: loading warnings')]
check('and the warnings go with Bare', warns == ['scene: loading warnings bar,line', 'scene: loading warnings none'], warns)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
