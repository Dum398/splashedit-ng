# The scene inspector's Rendering section, driven through the GUI: turn fog
# on, type its density, type a GTE scaling, pick Interior, save with Ctrl+S
# and read the saved scene's settings; then the undo arms, each edit one
# Ctrl+Z. Controls are clicked by name (--click @scene.fog): the editor aims at
# where the frame before drew them, so no pixel positions live here.
# usage: python3 scenefogcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


mid = None  # with SAVE_MID in the script: the settings as saved at that point


def run(args):
    """Runs the editor on a fresh courtyard copy (nothing selected, so the
    inspector shows the scene), then `args` and Ctrl+S; returns the saved
    scene's settings and the editor's output (without its list of controls)."""
    global mid
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    mid = json.load(open(snap, encoding='utf-8'))['settings'] if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8'))['settings'], out


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    """Double-click a number field, replace its text, Enter."""
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']


def summary(s):
    f = s.get('fog', {})
    return (f.get('enabled'), f.get('density'), s.get('gteScaling'), s.get('sceneType'))


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))['settings']
START = (False, 5, 100.0, 'exterior')
assert summary(original) == START, 'the example\'s settings changed: ' + str(summary(original))

s, out = run([])
check('save alone keeps the example\'s settings', summary(s) == START, f'{summary(s)} | ' + out[-200:])

s, out = run(click('scene.fog'))
check('the Fog switch turns fog on', summary(s) == (True, 5, 100.0, 'exterior'), f'{summary(s)} | ' + out[-200:])

ALL = click('scene.fog') + typed('scene.fogdensity', '7') + typed('scene.gte', '50') + click('scene.scenetype') + \
    click('scenetype/Interior')
s, out = run(ALL)
check('fog, density, GTE scaling and scene type all land in the saved scene', summary(s) == (True, 7, 50.0, 'interior'),
      f'{summary(s)} | ' + out[-200:])
check('the inspector reports the new values', 'scene: fog 1 density 7 colour #808099 gte 50 type interior' in out, out[-300:])

s, out = run(click('scene.fog') + typed('scene.fogdensity', '42'))
check('density is clamped to 10', summary(s)[1] == 10, f'{summary(s)} | ' + out[-200:])

# Undo arms: each edit goes back in one Ctrl+Z, leaving the ones before it.
s, out = run(ALL + SAVE_MID + UNDO)
check('one Ctrl+Z puts Exterior back and keeps the rest',
      mid and summary(mid) == (True, 7, 50.0, 'interior') and summary(s) == (True, 7, 50.0, 'exterior'),
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(click('scene.fog') + typed('scene.fogdensity', '7') + SAVE_MID + UNDO)
check('one Ctrl+Z takes the typed density back', mid and summary(mid)[1] == 7 and summary(s) == (True, 5, 100.0, 'exterior'),
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(click('scene.fog') + SAVE_MID + UNDO)
check('one Ctrl+Z turns fog back off', mid and summary(mid)[0] is True and summary(s) == START,
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
