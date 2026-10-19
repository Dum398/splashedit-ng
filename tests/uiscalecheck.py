# UI scale (#136): at --ui-scale 2 the whole editor draws twice as large in
# the same window: the title and status bars are twice as tall, every control
# sits where 1x puts it in a window half the size, a click at UI coordinates
# lands on the control drawn there, and the window chrome's hit test (asked in
# window coordinates, as the OS asks) finds the title bar, its buttons and the
# resize band at their scaled places. Also SPLASHEDIT_UI_SCALE, --ui-scale
# winning over it, a bad value ignored, and the default window size following
# the scale. Drives the editor headless on copies of the courtyard example.
# usage: python3 uiscalecheck.py <splashedit>   (needs a 1600x960 display: xvfb-run -a -s "-screen 0 1600x960x24")
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
ENV.pop('SPLASHEDIT_UI_SCALE', None)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args, size='1600x960', env=None):
    """Runs the editor on a fresh courtyard copy with `args`. Returns the
    output lines, the named controls, the screenshot and the project."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor] + (['--size', size] if size else []) + ['--project', project] + args + ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].split(' at ')
            a, b = at.split(' ')
            controls[name] = tuple(float(v) for v in a.split(',') + b.split(','))
    return lines, controls, shot, project


# The PNG reader vramlayoutcheck.py uses (no third-party modules).
def png_rgb(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGBA or RGB PNG."""
    data = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            bpp = 4 if ctype == 6 else 3
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[i] for i in range(stride) if i % bpp < 3))
        prev = line
    return w, h, rows


def bars(shot):
    """(width, height, title bar height, status bar height) in pixels: the
    title bar ends at the first row from the top with under a fifth of its
    pixels in the title bar's colour (the gap below it has none); the status
    bar likewise from the bottom."""
    w, h, rows = png_rgb(shot)

    def frac(y, c):
        return sum(1 for x in range(w) if rows[y][3 * x:3 * x + 3] == c) / w

    top_c, bot_c = rows[0][3 * (w // 2):3 * (w // 2) + 3], rows[h - 1][3 * (w // 2):3 * (w // 2) + 3]
    title = next((y for y in range(h) if frac(y, top_c) < 0.2), h)
    status = next((h - 1 - y for y in range(h - 1, -1, -1) if frac(y, bot_c) < 0.2), h)
    return w, h, title, status


def centre(r):
    return int((r[0] + r[2]) / 2), int((r[1] + r[3]) / 2)


def hits(lines):
    """{(x, y): result} from the --hittest lines."""
    out = {}
    for l in lines:
        if l.startswith('hittest: '):
            xy, res = l.split()[1:3]
            out[tuple(int(v) for v in xy.split(','))] = res
    return out


def names(project):
    found = []

    def walk(objs):
        for o in objs:
            found.append(o['name'])
            walk(o.get('children', []))

    walk(json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))['objects'])
    return found


# 1x and 2x in the same 1600x960 window.
l1, c1, shot1, _ = run(['--ui-scale', '1'])
l2, c2, shot2, _ = run(['--ui-scale', '2'])
check('1x reports its scale', 'ui: scale 1' in l1, [l for l in l1 if l.startswith('ui:')])
check('2x reports its scale', 'ui: scale 2' in l2, [l for l in l2 if l.startswith('ui:')])
w1, h1, title1, status1 = bars(shot1)
w2, h2, title2, status2 = bars(shot2)
check('both fill the 1600x960 window', (w1, h1, w2, h2) == (1600, 960, 1600, 960), (w1, h1, w2, h2))
check('the 1x title bar is drawn', 20 <= title1 <= 60, title1)
check('the 2x title bar is twice as tall', title2 == 2 * title1, f'{title2} px at 2x, {title1} px at 1x')
check('the 2x status bar is twice as tall', status2 == 2 * status1, f'{status2} px at 2x, {status1} px at 1x')

# 2x lays out as 1x in a window half the size (UI coordinates; text widths
# can differ by a unit, the glyphs being rasterized at another density).
lh, ch, shoth, _ = run(['--ui-scale', '1'], size='800x480')
same = [n for n in ('play', 'menu.file', 'tree.Floor', 'tree.Back Wall', 'vp.view', 'tab.scene')
        if n in c2 and n in ch and all(abs(a - b) <= 1 for a, b in zip(c2[n], ch[n]))]
check('controls at 2x in 1600x960 sit where 1x puts them in 800x480', len(same) == 6,
      {n: (c2.get(n), ch.get(n)) for n in ('play', 'menu.file', 'tree.Floor', 'vp.view')})

# Hit test (UI coordinates in, window coordinates asked) and a click at 2x.
title_ui = title2 // 2
play, floor = c2.get('play'), c2.get('tree.Floor')
check('the Play button and the Floor row are drawn at 2x', play and floor, sorted(c2))
if play and floor:
    pc = centre(play)
    strip = (pc[0], int((play[3] + title_ui) / 2))  # title bar, below the Play button
    edge = (5, 200)  # 10 window pixels in: inside the 2x resize band (12), outside the 1x one (6)
    inside = (5 * title_ui, 200)  # the scene tree, under the title bar
    args = []
    for p in (pc, strip, edge, inside):
        args += ['--hittest', '%d,%d' % p]
    args += ['--click', '%d,%d' % centre(floor), '--wait', '3', '--key', 'delete', '--wait', '3', '--key', 'ctrl+s', '--wait', '5']
    l3, c3, shot3, project3 = run(['--ui-scale', '2'] + args)
    h3 = hits(l3)
    check('the Play button takes its click', h3.get(pc) == 'normal', [l for l in l3 if l.startswith('hittest')])
    check('the title bar beside it drags', play[3] < strip[1] < title_ui and h3.get(strip) == 'draggable',
          (strip, title_ui, [l for l in l3 if l.startswith('hittest')]))
    check('the resize band is twice as deep', h3.get(edge) == 'resize-left', [l for l in l3 if l.startswith('hittest')])
    check('below the title bar is the client area', h3.get(inside) == 'normal', [l for l in l3 if l.startswith('hittest')])
    left = names(project3)
    check('a click on the Floor row selects it (Delete then removes it)', 'Floor' not in left and 'Back Wall' in left,
          [l for l in l3 if l.startswith('status')] + left)

# The environment, the flag winning over it, a bad value ignored.
le, _, _, _ = run([], env={'SPLASHEDIT_UI_SCALE': '2'})
check('SPLASHEDIT_UI_SCALE sets the scale', 'ui: scale 2' in le, [l for l in le if l.startswith('ui:')])
lf, _, _, _ = run(['--ui-scale', '1.5'], env={'SPLASHEDIT_UI_SCALE': '2'})
check('--ui-scale wins over SPLASHEDIT_UI_SCALE', 'ui: scale 1.5' in lf and 'ui: scale 2' not in lf,
      [l for l in lf if l.startswith('ui:')])
lb, _, _, _ = run(['--ui-scale', '9'])
check('an out-of-range --ui-scale is refused and the display scale used',
      any('--ui-scale 9' in l for l in lb) and 'ui: scale 1' in lb, [l for l in lb if 'scale' in l])

# Without --size the default window follows the scale (0.5: 800x480).
ld, _, shotd, _ = run(['--ui-scale', '0.5'], size=None)
wd, hd, _, _ = bars(shotd)
check('the default window follows the scale', (wd, hd) == (800, 480), (wd, hd, [l for l in ld if l.startswith('wrote')]))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
