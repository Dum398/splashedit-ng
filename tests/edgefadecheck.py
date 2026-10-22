# The scene inspector's Edge fade switch, driven through the GUI: on for the
# example (its scene file has no edgeFade key), one click turns it off and
# Ctrl+S saves "edgeFade": false; a second click turns it back on and the key
# is gone again; one Ctrl+Z undoes the click; a scene saved with it off opens
# with it off. The switch is clicked by name (--click @scene.edgefade): the
# editor aims at where the frame before drew it, so no pixel positions live here.
# usage: python3 edgefadecheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

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


def run(args, edit=None):
    """Runs the editor on a fresh courtyard copy (nothing selected, so the
    inspector shows the scene), then `args` and Ctrl+S; returns the saved
    scene's settings and the editor's output (without its list of controls).
    `edit` changes the copied scene's JSON before the editor opens it."""
    global mid
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    if edit:
        d = json.load(open(scene, encoding='utf-8'))
        edit(d)
        json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
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
    named = any(re.match(r'control scene\.edgefade at ', l) for l in lines)
    check('the Edge fade switch is drawn', named, [l for l in lines if l.startswith('control scene.')][:8])
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8'))['settings'], out


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def edge(s):
    """The switch as saved: no key is on."""
    return s.get('edgeFade', True)


def reported(out):
    shown = re.findall(r'^scene: fog .* edgefade (\d)$', out, re.M)
    return shown[-1] if shown else None


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']

original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))['settings']
assert 'edgeFade' not in original, 'the example now saves edgeFade: this check reads its absence as on'

s, out = run([])
check('save alone keeps no edgeFade key', 'edgeFade' not in s, s)
check('the inspector reports the edge fade on', reported(out) == '1', out[-300:])

s, out = run(click('scene.edgefade'))
check('the switch turns it off and the scene saves "edgeFade": false', s.get('edgeFade') is False, s)
check('the inspector reports the edge fade off', reported(out) == '0', out[-300:])
check('fog is untouched', s.get('fog', {}).get('enabled') is False, s.get('fog'))

s, out = run(click('scene.edgefade') + click('scene.edgefade'))
check('a second click turns it back on and the key is gone', 'edgeFade' not in s, s)

s, out = run(click('scene.edgefade') + SAVE_MID + UNDO)
check('one Ctrl+Z turns it back on', mid is not None and mid.get('edgeFade') is False and edge(s) is True,
      (mid and mid.get('edgeFade'), s.get('edgeFade')))


def off(d):
    d['settings']['edgeFade'] = False


s, out = run([], off)
check('a scene saved with it off opens with it off', s.get('edgeFade') is False and reported(out) == '0',
      (s.get('edgeFade'), out[-300:]))
s, out = run(click('scene.edgefade'), off)
check('and the switch turns it on there', edge(s) is True and reported(out) == '1', (s.get('edgeFade'), out[-300:]))

print(f'{fails} failures')
sys.exit(1 if fails else 0)
