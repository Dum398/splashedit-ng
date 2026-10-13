# The UI tab: the canvas editor. Opens a copy of tests/ui/ui.scene with --ui,
# clicks and drags elements on the canvas, resizes one by its corner handle,
# nudges with the arrow keys, reorders, duplicates and deletes, hides the
# canvas, then saves and reads the scene back. One Ctrl+Z must take back one
# whole drag. Needs a display (xvfb-run -a).
# usage: python3 uicanvascheck.py <splashedit>
import json, math, os, re, shutil, struct, subprocess, sys, tempfile, zlib

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


def readpng(path):
    """8-bit RGB or RGBA PNG: width, height, bytes per pixel, rows."""
    d = open(path, 'rb').read()
    p, idat = 8, b''
    while p < len(d):
        n, = struct.unpack_from('>I', d, p)
        t = d[p + 4:p + 8]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', d, p + 8)
            assert depth == 8 and ctype in (2, 6), (depth, ctype)
            bpp = 3 if ctype == 2 else 4
        if t == b'IDAT':
            idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a, b, c = line[i - bpp] if i >= bpp else 0, prev[i], prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


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

# ---- Copy, cut and paste: through the system clipboard, one undo step each.
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+c', '--key', 'ctrl+v', '--wait', '3'])
n = names(cv)
check('ctrl+c, ctrl+v pastes a copy in front of the original', n == order[:i + 1] + ['hp (2)'] + order[i + 1:], n)
check('the pasted copy sits 8 px down and right', elements(cv).get('hp (2)', {}).get('rect') == [16, 88, 120, 10],
      elements(cv).get('hp (2)'))
check('the original stays put', elements(cv)['hp'] == elements(cv0)['hp'], elements(cv)['hp'])
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+c', '--key', 'ctrl+v', '--wait', '3', '--key', 'ctrl+v',
                  '--wait', '3'])
check('a second paste lands past the first copy', elements(cv).get('hp (3)', {}).get('rect') == [24, 96, 120, 10],
      names(cv))
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+c', '--key', 'ctrl+v', '--wait', '3', '--key', 'ctrl+z',
                  '--wait', '3'])
check('one ctrl+z takes the paste back', names(cv) == order, names(cv))
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+x', '--wait', '3'])
check('ctrl+x removes the element', names(cv) == order[:i] + order[i + 1:], names(cv))
cv, out, _ = run(['--click', at(*HP), '--wait', '3', '--key', 'ctrl+x', '--wait', '3', '--key', 'ctrl+v', '--wait', '3'])
check('ctrl+x then ctrl+v puts it back in place, on top, under its own name',
      names(cv) == order[:i] + order[i + 1:] + ['hp'] and elements(cv)['hp'] == elements(cv0)['hp'], names(cv))

# The right-click menu, on the canvas and on the list. Where its rows are: the
# editor prints the first row and the step when it opens at the pointer.
_, out, _ = run(['--click', at(*HP) + ',1', '--wait', '3'])
mm = re.findall(r'uicanvas: menu ([\d.]+),([\d.]+) step ([\d.]+)', out)
check('right click on the canvas opens the element menu', bool(mm), out[-300:])
if mm:
    hx, hy = map(int, at(*HP).split(','))
    MX, MY, MS = float(mm[-1][0]) - hx, float(mm[-1][1]) - hy, float(mm[-1][2])

    def item(click, k):
        """Row k of the menu opened by a right click at screen point `click`."""
        cx, cy = map(int, click.split(','))
        return '%d,%d' % (round(cx + MX), round(cy + MY + k * MS))

    CUT, COPY, PASTE = 0, 1, 2
    cv, out, _ = run(['--click', at(*HP) + ',1', '--wait', '3', '--click', item(at(*HP), COPY), '--wait', '3',
                      '--click', row(i) + ',1', '--wait', '3', '--click', item(row(i), PASTE), '--wait', '3'])
    n = names(cv)
    check('menu copy on the canvas, menu paste on the list row', n == order[:i + 1] + ['hp (2)'] + order[i + 1:], n)
    cv, out, _ = run(['--click', row(i) + ',1', '--wait', '3', '--click', item(row(i), CUT), '--wait', '3'])
    check('menu cut on a list row removes that element', names(cv) == order[:i] + order[i + 1:], names(cv))


# ---- Zoom and pan. The editor reprints the transform whenever it changes;
# the last line is the one in force.
def transforms(out):
    return [tuple(map(float, t)) for t in re.findall(r'uicanvas: screen ([\d.-]+),([\d.-]+) scale ([\d.]+)', out)]


ZOOM_AT = (170, 110)  # empty canvas between the bar 'hp' and the crate
zoom = ['--keydown', 'ctrl', '--wheel', at(*ZOOM_AT) + ',1', '--wait', '3', '--keyup', 'ctrl', '--wait', '3']
_, out, _ = run(zoom)
t = transforms(out)
X2, Y2, S2 = t[-1]
check('ctrl+wheel zooms in', S2 > S, t)
cx, cy = map(int, at(*ZOOM_AT).split(','))
check('the canvas point under the pointer stays under it',
      abs((cx - X) / S - (cx - X2) / S2) <= 1 / S2 and abs((cy - Y) / S - (cy - Y2) / S2) <= 1 / S2,
      ((cx - X) / S, (cy - Y) / S, (cx - X2) / S2, (cy - Y2) / S2))
check('zoomed origin on a whole pixel', X2 == int(X2) and Y2 == int(Y2), (X2, Y2))
_, out, _ = run(['--wheel', at(*ZOOM_AT) + ',1', '--wait', '3', '--wheel', at(*ZOOM_AT) + ',-1', '--wait', '3'])
t = transforms(out)
check('the wheel alone zooms in and back out', any(x[2] > S for x in t) and t[-1][2] == S, t)


def at2(x, y):
    return '%d,%d' % (round(X2 + (x + 0.5) * S2), round(Y2 + (y + 0.5) * S2))


# 'title' (8..307 x 8..27): zoomed in, it is where the fit view shows nothing,
# so a click going through a stale transform would select nothing.
TITLE = (60, 15)
cv, out, _ = run(zoom + ['--click', at2(*TITLE), '--wait', '3', '--key', 'right', '--wait', '3'])
check('zoomed in, a click selects the element under the pointer', elements(cv)['title']['rect'] == [9, 8, 300, 20],
      [(k, elements(cv)[k].get('rect')) for k in names(cv) if elements(cv)[k] != elements(cv0)[k]])
cv, out, _ = run(zoom + ['--drag', '%s,%d,%d' % (at2(*HP), round(X2 + (HP[0] + 30.5) * S2), round(Y2 + (HP[1] + 20.5) * S2)),
                         '--wait', '3'])
check('zoomed in, a drag moves one canvas pixel per canvas pixel', elements(cv)['hp']['rect'] == [38, 100, 120, 10],
      elements(cv)['hp']['rect'])
cv, out, _ = run(zoom + ['--click', at2(*HP), '--wait', '3', '--drag', '%d,%d,%d,%d' % (
    round(X2 + 128 * S2), round(Y2 + 90 * S2), round(X2 + 138 * S2), round(Y2 + 95 * S2)), '--wait', '3'])
check('zoomed in, the corner handle resizes', elements(cv)['hp']['rect'] == [8, 80, 130, 15], elements(cv)['hp']['rect'])

# Middle drag pans; Space + drag pans without moving what it starts on; Ctrl+0 fits again.
cv, out, _ = run(['--drag', '%s,%d,%d,2' % (at(*ZOOM_AT), cx + 100, cy + 50), '--wait', '3'])
t = transforms(out)
check('middle drag pans the canvas', t[-1] == (X + 100, Y + 50, S), t)
cv, out, _ = run(['--keydown', 'space', '--drag', by(*HP, 30, 20), '--keyup', 'space', '--wait', '3', '--key', 'ctrl+0',
                  '--wait', '3'])
t = transforms(out)
check('space + drag pans', (X + round(30 * S), Y + round(20 * S), S) in t, t)
check('space + drag moves no element', elements(cv)['hp'] == elements(cv0)['hp'], elements(cv)['hp'])
check('ctrl+0 fits the screen again', t[-1] == (X, Y, S), t)

# 100% (Ctrl+1): the canvas on screen is ui/ui-runtime.png (psxsplash's own
# frame) pixel for pixel in 15-bit colour, wherever the game drew something,
# except the 1 px element outlines the editor draws over it.
d = tempfile.mkdtemp()
_, out, _ = run(['--key', 'ctrl+1', '--wait', '3'], shot=os.path.join(d, 'shot.png'))
X1, Y1, S1 = transforms(out)[-1]
check('ctrl+1 shows the screen at 100%', S1 == 1 and X1 == int(X1) and Y1 == int(Y1), transforms(out))
if S1 == 1:
    w, h, bpp, shot = readpng(os.path.join(d, 'shot.png'))
    _, _, rb, ref = readpng(os.path.join(here, 'ui', 'ui-runtime.png'))
    rects = []
    for e in original['elements']:
        if e['type'] == 'line':
            continue
        (x, y, ew, eh), a0, a1 = e['rect'], e.get('anchorMin', [0, 0]), e.get('anchorMax', [0, 0])
        l, t0 = round(a0[0] * 320) + x, round(a0[1] * 240) + y
        rects.append((l, t0, round(a1[0] * 320) + x + ew if a1[0] != a0[0] else l + ew,
                      round(a1[1] * 240) + y + eh if a1[1] != a0[1] else t0 + eh))
    lines = [(e['from'], e['to']) for e in original['elements'] if e['type'] == 'line']

    def outlined(x, y):
        if any(l - 1 <= x <= r and t0 - 1 <= y <= b and (x <= l or x >= r - 1 or y <= t0 or y >= b - 1)
               for l, t0, r, b in rects):
            return True
        for (ax, ay), (bx, by) in lines:
            k = max(0, min(1, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2)))
            if math.hypot(x - ax - k * (bx - ax), y - ay - k * (by - ay)) < 2:
                return True
        return False

    compared, differ = 0, []
    for y in range(240):
        for x in range(320):
            want = ref[y][x * rb:x * rb + 3]
            if not any(want) or outlined(x, y):
                continue
            compared += 1
            o = (int(Y1) + y, (int(X1) + x) * bpp)
            got = shot[o[0]][o[1]:o[1] + 3]
            if [round(v * 31 / 255) for v in want] != [v >> 3 for v in got]:
                differ.append((x, y, tuple(want), tuple(got)))
    check('at 100%%, the canvas matches the runtime frame pixel for pixel (%d compared)' % compared,
          compared > 8000 and not differ, (compared, len(differ), differ[:5]))

print(f'{fails} failures')
sys.exit(1 if fails else 0)
