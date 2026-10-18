# The UI tab's element inspector, by hand: a drag on a pixel field (a text's
# Size W) moves it one per pixel, not by a percentage of the value.
# Opens a copy of tests/ui/ui.scene with --ui, selects elements by their
# list rows, drives the inspector's named fields and reads the saved scene and
# the editor's scripted lines. Needs a display (xvfb-run -a).
# usage: python3 uiinspectorcheck.py <splashedit>
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project():
    """tests/ui/ui.scene as a project of its own, its fonts and texture
    copied in."""
    d = os.path.join(tempfile.mkdtemp(), 'ui')
    os.makedirs(os.path.join(d, 'fonts'))
    os.makedirs(os.path.join(d, 'textures'))
    s = json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))
    for f in s['fonts']:
        shutil.copy(os.path.join(here, 'ui', f['source']), os.path.join(d, 'fonts'))
        f['source'] = 'fonts/' + os.path.basename(f['source'])
    for e in s['canvases'][0]['elements']:
        if 'texture' in e:
            shutil.copy(os.path.join(here, 'ui', e['texture']), os.path.join(d, 'textures'))
            e['texture'] = 'textures/' + os.path.basename(e['texture'])
    json.dump(s, open(os.path.join(d, 'ui.scene'), 'w'), indent=1)
    return d


def run(args, save=True, env=None, d=None):
    """Runs the editor on a fresh project (or `d`) with the UI tab open, then
    (with `save`) Ctrl+S; returns the saved canvas, the output and the
    project. run.shot is the end screenshot."""
    d = d or project()
    run.shot = os.path.join(d, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5'] + args + \
        (['--key', 'ctrl+s'] if save else []) + ['--wait', '5', '--screenshot', run.shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=120)
    out = (r.stdout + r.stderr).strip()
    scene = json.load(open(os.path.join(d, 'ui.scene'), encoding='utf-8'))
    return scene['canvases'][0], out, d


def elements(cv):
    return {e['name']: e for e in cv['elements']}


def control(out, name):
    """The rect (x0, y0, x1, y1) the editor printed for control `name`."""
    m = re.search(r'control ' + re.escape(name) + r' at (-?\d+),(-?\d+) (-?\d+),(-?\d+)', out)
    return tuple(map(int, m.groups())) if m else None


# Where the element list is: the editor prints it with --ui.
cv0, out, _ = run([], save=False)
m = re.search(r'uicanvas: screen [\d.]+,[\d.]+ scale [\d.]+ list ([\d.]+),([\d.]+) row ([\d.]+)', out)
check('--ui reports the element list', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
LX, LY, LH = map(float, m.groups())
ORDER = [e['name'] for e in cv0['elements']]


def row(name):
    """A click on `name`'s row in the element list."""
    return ['--click', '%d,%d' % (round(LX + 40), round(LY + (ORDER.index(name) + 0.5) * LH)), '--wait', '3']


# ---- #174: a pixel field scrubs one per pixel. 'title' is 300 px wide; a 60 px
# drag on Size W (the first 6 px or so are the drag threshold) adds at most 60,
# where 1% of the value per pixel added about 3 per pixel.
w0 = elements(cv0)['title']['rect'][2]
cv, out, _ = run(row('title') + ['--drag', '@ui.sizea,0,0,60,0', '--wait', '3'])
w = elements(cv)['title']['rect'][2]
check('a 60 px drag on Size W of a %d px text adds one per pixel' % w0, w0 + 45 <= w <= w0 + 60, (w0, w))
cv, out, _ = run(row('title') + ['--drag', '@ui.posa,0,0,-30,0', '--wait', '3'])
x = elements(cv)['title']['rect'][0]
check('a 30 px drag left on Position X takes off one per pixel', 8 - 30 <= x <= 8 - 20, x)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
