# The UI tab's element inspector, by hand: a drag on a pixel field (a text's
# Size W) moves it one per pixel, not by a percentage of the value; the Tint
# picker stays where it opened while the colour it sets brings the "Tinted"
# warning (the warning shows once it closes); Tab and Shift+Tab onto a number
# field put it in typing, so what is typed next replaces its value; while a
# file dialog is open the editor takes no input and opens no second one; the
# Texture field takes an image dragged from the Project panel and its Browse
# opens in the project folder.
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
    copied in, and a second texture textures/other.png (the crate's)."""
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
    shutil.copy(os.path.join(d, 'textures', 'crate.png'), os.path.join(d, 'textures', 'other.png'))
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

# ---- #157: the Tint picker does not move while it is open. 'crate' is tinted
# #808080; a drag in the picker's colour square makes it red, which brings the
# "Tinted" card above the fields. While the picker is open the cards stay as
# they were (nothing moves under the pointer); once it closes the card shows.
cv, out, _ = run(row('crate'), save=False)
base = control(out, 'ui.col')
cv, out, _ = run(row('crate') + ['--click', '@ui.col', '--wait', '3'], save=False)
pick = control(out, 'ui.colpick')
check('clicking the Tint well opens its picker under it', base and pick and pick[1] >= base[3] and 'popups open: yes' in out,
      (base, pick, out[-200:]))
if base and pick:
    square = ['--drag', '%d,%d,%d,%d,0' % (pick[0] + 160, pick[1] + 24, pick[0] + 180, pick[1] + 44), '--wait', '3']
    cv, out, d = run(row('crate') + ['--click', '@ui.col', '--wait', '3'] + square, save=False)
    check('while the picker is open the Tint row stays put', control(out, 'ui.col') == base, (base, control(out, 'ui.col')))
    check('and so does the picker', control(out, 'ui.colpick') == pick and 'popups open: yes' in out,
          (pick, control(out, 'ui.colpick')))
    # Closed by a click on the inspector's title: the card shows, the row moves down.
    cv, out, d = run(row('crate') + ['--click', '@ui.col', '--wait', '3'] + square + ['--click', '1282,61,0', '--wait', '3'])
    tint = elements(cv)['crate']['color']
    after = control(out, 'ui.col')
    check('the drag in the picker tinted the crate', abs(tint[0] - 0.5) > 0.1 or abs(tint[1] - 0.5) > 0.1, tint)
    check('once the picker closes the Tinted card shows above it', after and after[1] > base[1] + 40, (base, after))

# ---- #158: Tab onto a number field types into it. Position X typed, Tab, and
# what is typed next goes into Y; Shift+Tab goes back the same way. 'title'
# sits at 8, 8.
cv, out, _ = run(row('title') + ['--dblclick', '@ui.posa', '--wait', '3', '--text', '40', '--key', 'tab', '--wait', '3',
                                 '--text', '50', '--key', 'enter', '--wait', '3'])
check('typing after Tab from Position X sets Y', elements(cv)['title']['rect'][:2] == [40, 50], elements(cv)['title']['rect'])
cv, out, _ = run(row('title') + ['--dblclick', '@ui.posb', '--wait', '3', '--text', '60', '--key', 'shift+tab', '--wait', '3',
                                 '--text', '30', '--key', 'enter', '--wait', '3'])
check('typing after Shift+Tab from Position Y sets X, and Y keeps what it was given',
      elements(cv)['title']['rect'][:2] == [30, 60], elements(cv)['title']['rect'])
cv, out, _ = run(row('title') + ['--dblclick', '@ui.posa', '--wait', '3', '--text', '12', '--key', 'tab', '--wait', '3',
                                 '--text', '7', '--key', 'tab', '--wait', '3', '--text', '200', '--key', 'enter', '--wait', '3'])
check('Tab runs on through Size W', elements(cv)['title']['rect'][:3] == [12, 7, 200], elements(cv)['title']['rect'])

# ---- #159: the file dialog is modal. With SPLASHEDIT_FILE_DIALOG=hold no
# dialog shows and the editor acts as if one stayed open, printing
# "file dialog: <kind> in <folder>". While it is open the editor takes no input:
# a click on another row does not select it, a second Browse opens nothing.
HOLD = {'SPLASHEDIT_FILE_DIALOG': 'hold'}
cv, out, _ = run(row('crate') + ['--click', '@ui.texbrowse', '--wait', '3'], save=False, env=HOLD)
check("the Texture's Browse opens a file dialog", out.count('file dialog: ui-texture in') == 1, out[-300:])
cv, out, _ = run(row('crate') + ['--click', '@ui.texbrowse', '--wait', '3'] + row('hp') +
                 ['--click', '@ui.texbrowse', '--wait', '3', '--key', 'delete', '--wait', '3'], save=False, env=HOLD)
check('while it is open a click on another row selects nothing', 'uicanvas: inspector element hp' not in out and
      'uicanvas: inspector element crate' in out, out[-300:])
check('and a second Browse opens no second dialog', out.count('file dialog: ui-texture') == 1, out[-300:])
check('and Delete deletes nothing (the scene is unchanged)', 'unsaved changes: no' in out, out[-300:])

# ---- #160: the Texture field takes an image dragged from the Project panel, as
# a Script's File takes a .lua; its Browse opens in the project folder.
SEARCH = '880,731'  # the Project panel's search field, under the UI tab at 1600x960
TILE = '530,812'    # the first file the search shows
cv, out, _ = run(row('crate'), save=False)
tex = control(out, 'ui.tex')
check('the Texture field is drawn', tex is not None, out[-200:])
if tex:
    at = '%d,%d' % ((tex[0] + tex[2]) // 2, (tex[1] + tex[3]) // 2)
    cv, out, d = run(row('crate') + ['--click', SEARCH + ',0', '--text', 'other', '--wait', '5', '--drag', TILE + ',' + at + ',0',
                                     '--wait', '10'])
    check('an image dragged from the Project panel onto Texture is set', elements(cv)['crate'].get('texture') == 'textures/other.png',
          (elements(cv)['crate'].get('texture'), out[-300:]))
    cv, out, d = run(row('crate') + ['--click', SEARCH + ',0', '--text', 'ui.scene', '--wait', '5', '--drag', TILE + ',' + at + ',0',
                                     '--wait', '10'])
    check('a scene dropped there is refused and leaves it', elements(cv)['crate'].get('texture') == 'textures/crate.png' and
          'That field does not take' in out, (elements(cv)['crate'].get('texture'), out[-300:]))
cv, out, d = run(row('crate') + ['--click', '@ui.texbrowse', '--wait', '3'], save=False, env=HOLD)
check("the Texture's Browse opens in the project folder", 'file dialog: ui-texture in ' + d in out.splitlines(),
      [l for l in out.splitlines() if 'file dialog' in l])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
