# The UI tab: the canvas editor. Opens a copy of tests/ui/ui.scene with --ui,
# clicks and drags elements on the canvas, resizes one by its corner handle,
# nudges with the arrow keys, reorders, duplicates and deletes, hides the
# canvas, then saves and reads the scene back. One Ctrl+Z must take back one
# whole drag. Needs a display (xvfb-run -a).
# usage: python3 uicanvascheck.py <splashedit>
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project():
    """tests/ui/ui.scene as a project of its own: fonts and texture copied in."""
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


def run(args, shot=None):
    """Runs the editor on a fresh project with the UI tab open, then Ctrl+S;
    returns the saved canvas, the editor's output and the project."""
    d = project()
    cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '5', '--screenshot', shot or os.path.join(d, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    out = (r.stdout + r.stderr).strip()
    scene = json.load(open(os.path.join(d, 'ui.scene'), encoding='utf-8'))
    return scene['canvases'][0], out, d


def elements(cv):
    return {e['name']: e for e in cv['elements']}


def names(cv):
    return [e['name'] for e in cv['elements']]


# Where the canvas and the element list are: the editor prints it with --ui.
cv0, out, _ = run([])
m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+) list ([\d.]+),([\d.]+) row ([\d.]+) eye ([\d.]+),([\d.]+)', out)
check('--ui opens the UI tab and reports its layout', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
X, Y, S, LX, LY, LH, EX, EY = map(float, m.groups())
original = json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))['canvases'][0]
check('saving alone keeps the canvas', names(cv0) == names(original) and elements(cv0)['hp']['rect'] == [8, 80, 120, 10],
      names(cv0))


def at(x, y):
    """Screen point of canvas pixel (x, y), at the pixel's centre."""
    return '%d,%d' % (round(X + (x + 0.5) * S), round(Y + (y + 0.5) * S))


def by(x0, y0, dx, dy):
    """A drag from canvas pixel (x0, y0) by (dx, dy) canvas pixels."""
    return '%s,%d,%d' % (at(x0, y0), round(X + (x0 + 0.5 + dx) * S), round(Y + (y0 + 0.5 + dy) * S))


def row(i):
    return '%d,%d' % (round(LX + 40), round(LY + (i + 0.5) * LH))


HP = (68, 85)  # inside the progress bar 'hp' (8, 80, 120 x 10), clear of the others

# Drag on the canvas: picks the element up and moves it, one pixel per canvas pixel.
cv, out, _ = run(['--drag', by(*HP, 30, 20), '--wait', '3'])
check('a drag moves the element it starts on', elements(cv)['hp']['rect'] == [38, 100, 120, 10], elements(cv)['hp']['rect'])
check('nothing else moves', all(elements(cv)[n] == elements(cv0)[n] for n in names(cv0) if n != 'hp'),
      [n for n in names(cv0) if n != 'hp' and elements(cv).get(n) != elements(cv0)[n]])

# Snapping: let go 2 px from the screen's left edge and it lands on it.
cv, out, _ = run(['--drag', by(*HP, -6, 0), '--wait', '3'])
check('a drag snaps to the screen edge', elements(cv)['hp']['rect'][:2] == [0, 80], elements(cv)['hp']['rect'])

# Corner handle: the bottom-right corner of hp is at (128, 90).
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--drag', '%d,%d,%d,%d' % (
    round(X + 128 * S), round(Y + 90 * S), round(X + 138 * S), round(Y + 95 * S)), '--wait', '3'])
check('the corner handle resizes, the opposite corner stays', elements(cv)['hp']['rect'] == [8, 80, 130, 15],
      elements(cv)['hp']['rect'])

# Arrow keys nudge the selection by 1 px, 8 with Shift.
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'right', '--key', 'right', '--key', 'right',
                  '--key', 'shift+down', '--wait', '3'])
check('arrows nudge 1 px, shift+arrow 8 px', elements(cv)['hp']['rect'][:2] == [11, 88], elements(cv)['hp']['rect'])

# One Ctrl+Z takes back one whole drag, not one frame of it.
cv, out, _ = run(['--drag', by(*HP, 30, 20), '--wait', '3', '--key', 'ctrl+z', '--wait', '3'])
check('ctrl+z after a drag puts the element back', elements(cv)['hp']['rect'] == [8, 80, 120, 10], elements(cv)['hp']['rect'])
cv, out, _ = run(['--drag', by(*HP, 30, 20), '--wait', '3', '--drag', by(HP[0] + 30, HP[1] + 20, 0, 40), '--wait', '3',
                  '--key', 'ctrl+z', '--wait', '3'])
check('ctrl+z after two drags takes back only the second', elements(cv)['hp']['rect'] == [38, 100, 120, 10],
      elements(cv)['hp']['rect'])

# Z-order: Ctrl+] brings the selection forward one place.
order = names(original)
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+]', '--wait', '3'])
i = order.index('hp')
check('ctrl+] brings the element forward', names(cv) == order[:i] + [order[i + 1], 'hp'] + order[i + 2:], names(cv))
# Dragging a row in the element list: 'bar' (row 0) dropped under row 3.
cv, out, _ = run(['--drag', '%s,%d,%d' % (row(0), round(LX + 40), round(LY + 4 * LH)), '--wait', '3'])
check('dragging a list row reorders', names(cv) == order[1:4] + ['bar'] + order[4:], names(cv))

# Duplicate and delete.
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+d', '--wait', '3'])
n = names(cv)
check('ctrl+d duplicates in front of the original', n[n.index('hp') + 1:n.index('hp') + 2] == ['hp (2)'], n)
check('the copy sits 8 px down and right', elements(cv).get('hp (2)', {}).get('rect') == [16, 88, 120, 10],
      elements(cv).get('hp (2)'))
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'delete', '--wait', '3'])
check('delete removes the selected element', 'hp' not in names(cv) and len(names(cv)) == len(order) - 1, names(cv))
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'delete', '--wait', '3', '--key', 'ctrl+z', '--wait', '3'])
check('ctrl+z brings it back in place', names(cv) == order, names(cv))

# The canvas's eye hides it (the scene's visible flag).
cv, out, _ = run(['--click', '%d,%d' % (EX, EY), '--wait', '3'])
check('the eye hides the canvas', cv.get('visible') is False, cv.get('visible'))

# A click on empty canvas leaves nothing selected: arrows then move nothing.
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--click', at(200, 74), '--wait', '3', '--key', 'right', '--wait', '3'])
check('a click on empty canvas deselects', elements(cv)['hp']['rect'] == [8, 80, 120, 10], elements(cv)['hp']['rect'])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
