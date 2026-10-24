# Safe area and TV preview, driven through the GUI. Play runs a fake
# pcsx-redux (SPLASHEDIT_REDUX, a shell script) that execs emulator_tests
# (beside the editor) with SPLASHEDIT_FAKE_REDUX=<w>x<h>, which publishes one
# frame of colour bars that size. The Game view's display bar is clicked by
# name (@game.safearea, @game.aspect/1:1, @game.look/Composite); the frame's
# rect comes back as "control game.frame at x0,y0 x1,y1". With the guides on,
# every edge of the action-safe (central 90%) and title-safe (80%) rectangles
# must be drawn in its colour on the screenshot, and with them off none;
# 4:3 shows a 512x240 frame 4:3, 1:1 at 512:240; Composite blurs a colour
# bar edge wider than S-video, RGB not at all, and adds scanlines. The UI
# tab's header toggle (@ui.safearea) draws the same guides over the screen
# (@ui.screen). Every toggle is saved in the play settings (--play-settings).
# usage: python3 safeareacheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import json, os, shutil, stat, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
display = os.path.join(os.path.dirname(editor), 'emulator_tests')
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
engine = os.path.join(work, 'psxsplash.ps-exe')
with open(engine, 'wb') as fh:
    fh.write(b'PS-X EXE fake')
ACTION, TITLE = (255, 170, 0), (80, 220, 255)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


# The PNG reader uiscalecheck.py uses (no third-party modules).
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


def px(img, x, y):
    w, h, rows = img
    return tuple(rows[y][x * 3:x * 3 + 3])


def safe(r, fraction):
    """The rectangle editor::tv::safeRect gives: each side inset by round(size * (1 - f) / 2)."""
    x0, y0, x1, y1 = r
    ix, iy = round((x1 - x0) * (1 - fraction) / 2), round((y1 - y0) * (1 - fraction) / 2)
    return x0 + ix, y0 + iy, x1 - ix, y1 - iy


def edge_points(s):
    """Pixels on each of the four 1-pixel lines of rectangle s, away from the corners."""
    x0, y0, x1, y1 = s
    pts = []
    for t in (0.2, 0.35, 0.5, 0.65, 0.8):
        xm, ym = int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t)
        pts += [('top', xm, y0), ('bottom', xm, y1 - 1), ('left', x0, ym), ('right', x1 - 1, ym)]
    return pts


def guides(img, r):
    """For each guide, how many of its edge pixels are in its colour, of how many."""
    out = {}
    for name, fraction, col in (('action', 0.9, ACTION), ('title', 0.8, TITLE)):
        pts = edge_points(safe(r, fraction))
        out[name] = (sum(px(img, x, y) == col for _, x, y in pts), len(pts))
    return out


def anywhere(img, r, col):
    """Whether colour col shows anywhere inside r."""
    x0, y0, x1, y1 = r
    w, h, rows = img
    needle = bytes(col)
    for y in range(y0, y1):
        row = rows[y][x0 * 3:x1 * 3]
        for i in range(0, len(row), 3):
            if row[i:i + 3] == needle:
                return True
    return False


def run(args, settings, size=None, ui=False, shot=None):
    project = os.path.join(tempfile.mkdtemp(), 'p')
    if ui:
        os.makedirs(os.path.join(project, 'fonts'))
        os.makedirs(os.path.join(project, 'textures'))
        s = json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))
        for f in s['fonts']:
            shutil.copy(os.path.join(here, 'ui', f['source']), os.path.join(project, 'fonts'))
            f['source'] = 'fonts/' + os.path.basename(f['source'])
        for e in s['canvases'][0]['elements']:
            if 'texture' in e:
                shutil.copy(os.path.join(here, 'ui', e['texture']), os.path.join(project, 'textures'))
                e['texture'] = 'textures/' + os.path.basename(e['texture'])
        json.dump(s, open(os.path.join(project, 'ui.scene'), 'w'), indent=1)
        cmd = [editor, '--size', '1600x960', '--project', project, '--ui']
    else:
        shutil.copytree(example, project)
        cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'courtyard.scene'),
               '--wait', '10', '--key', 'f5', '--wait', '240']
    redux = os.path.join(work, 'fake-redux.sh')
    with open(redux, 'w', newline='\n') as fh:
        fh.write('#!/bin/sh\nSPLASHEDIT_FAKE_REDUX=%s exec "%s"\n' % (size or '1', display))
    os.chmod(redux, os.stat(redux).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    env = dict(os.environ, SPLASHEDIT_REDUX=redux, SPLASHEDIT_PSXSPLASH=engine)
    out = shot or os.path.join(project, 'shot.png')
    # The pointer leaves the clicked control, so no tooltip covers the frame.
    cmd += ['--play-settings', settings] + args + ['--move', '1100,880', '--wait', '20', '--screenshot', out]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=300)
    lines = (r.stdout + r.stderr).strip().splitlines()
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].rsplit(' at ', 1)
            a, b = at.split(' ')
            controls[name] = tuple(int(v) for v in a.split(',') + b.split(','))
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn (%s)' % ' '.join(args), not missing, missing)
    return lines, controls, (png_rgb(out) if os.path.exists(out) else None)


def setting(path, key):
    for l in open(path, encoding='utf-8').read().splitlines():
        if l.startswith(key + '='):
            return l[len(key) + 1:]
    return None


def shot(name):
    return shots + '-' + name + '.png' if shots else None


def last(lines, prefix):
    found = [l for l in lines if l.startswith(prefix)]
    return found[-1] if found else None


def bar_edge(img, r):
    """Pixels across the yellow/cyan bar edge (a third down the frame) that
    are neither bar's colour, and the colours there."""
    x0, y0, x1, y1 = r
    y = y0 + (y1 - y0) // 3
    ex = x0 + (x1 - x0) * 2 // 8
    span = [px(img, x, y) for x in range(ex - 40, ex + 40)]
    left, right = span[0], span[-1]
    return sum(c != left and c != right for c in span), left, right


def scanlines(img, r):
    """Distinct colours down the white bar, clear of the guide lines (between
    the action-safe line at 5% and the title-safe one at 10% across, below
    the title-safe line at 10% down)."""
    x0, y0, x1, y1 = r
    x = x0 + (x1 - x0) // 16
    return {px(img, x, y) for y in range(y0 + (y1 - y0) * 12 // 100, y0 + (y1 - y0) * 30 // 100)}


# The Game view: guides on, then off, at 320x240 and 4:3.
cfg = os.path.join(work, 'play.cfg')
lines, c, img = run(['--click', '@game.safearea'], cfg, '320x240', shot=shot('guides'))
check('the Game view shows the frame', 'game.frame' in c and img is not None, sorted(c)[:20])
check('the display bar reports guides on, 4:3, RGB', last(lines, 'game: display ') == 'game: display safe on aspect 4:3 look rgb',
      last(lines, 'game: display '))
if 'game.frame' in c and img:
    r = c['game.frame']
    check('the frame shows 4:3', abs((r[2] - r[0]) * 3 - (r[3] - r[1]) * 4) <= 4, r)
    g = guides(img, r)
    check('action-safe guide on every edge at 90%', g['action'][0] == g['action'][1], g)
    check('title-safe guide on every edge at 80%', g['title'][0] == g['title'][1], g)
    rgb_edge = bar_edge(img, r)
    check('RGB keeps a bar edge sharp', rgb_edge[0] == 0, rgb_edge)
    check('RGB has no scanlines', len(scanlines(img, r)) == 1, scanlines(img, r))
check('the guides are saved on', setting(cfg, 'game_safe_area') == '1', open(cfg).read() if os.path.exists(cfg) else 'no file')

lines, c, img = run(['--click', '@game.safearea'], cfg, '512x240')
check('the display bar reports guides off', last(lines, 'game: display ') == 'game: display safe off aspect 4:3 look rgb',
      last(lines, 'game: display '))
if 'game.frame' in c and img:
    r = c['game.frame']
    check('a 512x240 frame shows 4:3', abs((r[2] - r[0]) * 3 - (r[3] - r[1]) * 4) <= 4, r)
    g = guides(img, r)
    check('no guide pixels with the guides off', g['action'][0] == 0 and g['title'][0] == 0, g)
    check('neither guide colour anywhere in the frame', not anywhere(img, r, ACTION) and not anywhere(img, r, TITLE), r)
else:
    check('the Game view shows the 512x240 frame', False, sorted(c)[:20])
check('the guides are saved off', setting(cfg, 'game_safe_area') == '0')

# Square pixels: 512x240 shows 512:240, the guides follow the frame.
lines, c, img = run(['--click', '@game.aspect/1:1', '--wait', '5', '--click', '@game.safearea'], cfg, '512x240')
check('the display bar reports square pixels', last(lines, 'game: display ') == 'game: display safe on aspect square look rgb',
      last(lines, 'game: display '))
if 'game.frame' in c and img:
    r = c['game.frame']
    check('a 512x240 frame shows 512:240 with square pixels', abs((r[2] - r[0]) * 240 - (r[3] - r[1]) * 512) <= 512 * 2, r)
    g = guides(img, r)
    check('the guides follow the square-pixel frame', g['action'][0] == g['action'][1] and g['title'][0] == g['title'][1], g)
else:
    check('the Game view shows the square-pixel frame', False, sorted(c)[:20])
check('square pixels are saved', setting(cfg, 'game_aspect') == 'square')

# The looks, on fresh settings. S-video in the Game view as Play opens it,
# under 480 screen rows: no room for a 240-line frame's scanlines, so none
# (they would alias). Composite with the Scene/Game split dragged left, so
# the frame shows 480 rows or more: scanlines. Blur is compared in frame pixels.
edges = {}
for look, widen in (('S-video', False), ('Composite', True)):
    cfg2 = os.path.join(work, 'look-%s.cfg' % look)
    drag = ['--mousedown', '@viewport.split', '--move', '300,400', '--mouseup', '0', '--wait', '5'] if widen else []
    lines, c, img = run(drag + ['--click', '@game.look/' + look], cfg2, '320x240', shot=shot(look.lower()))
    key = {'S-video': 'svideo', 'Composite': 'composite'}[look]
    check(look + ' is reported', last(lines, 'game: display ') == 'game: display safe off aspect 4:3 look ' + key,
          last(lines, 'game: display '))
    check(look + ' is saved', setting(cfg2, 'game_look') == key)
    if 'game.frame' in c and img:
        r = c['game.frame']
        n, a, b = bar_edge(img, r)
        edges[look] = (n * 320.0 / (r[2] - r[0]), n, a, b)
        rows = r[3] - r[1]
        if widen:
            check('the dragged split shows the frame 480 rows or more', rows >= 480, r)
            check(look + ' adds scanlines to a 240-line frame shown 2 rows a line', len(scanlines(img, r)) >= 2, scanlines(img, r))
        else:
            check('Play opens the Game view under 480 rows', rows < 480, r)
            check(look + ' leaves out scanlines under 2 rows a line', len(scanlines(img, r)) == 1, scanlines(img, r))
    else:
        check('the Game view shows the frame under ' + look, False, sorted(c)[:20])
if len(edges) == 2:
    print('     blur across the yellow/cyan edge, in frame pixels: S-video %.1f, Composite %.1f' %
          (edges['S-video'][0], edges['Composite'][0]))
    check('S-video blurs a colour bar edge', edges['S-video'][0] >= 1, edges)
    check('Composite blurs it wider than S-video', edges['Composite'][0] > edges['S-video'][0] * 1.5, edges)

# The UI canvas: the same guides over the screen, on and off.
ucfg = os.path.join(work, 'ui.cfg')
for arm, want in (('on', True), ('off', False)):
    lines, c, img = run(['--click', '@ui.safearea'], ucfg, ui=True, shot=shot('ui-' + arm))
    check('the UI tab reports the guides ' + arm, last(lines, 'uicanvas: safe area ') == 'uicanvas: safe area ' + arm,
          last(lines, 'uicanvas: safe area '))
    check('the UI guides are saved ' + arm, setting(ucfg, 'ui_safe_area') == ('1' if want else '0'))
    if 'ui.screen' in c and img:
        r = c['ui.screen']
        g = guides(img, r)
        if want:
            check('UI action-safe guide on every edge at 90%', g['action'][0] == g['action'][1], (r, g))
            check('UI title-safe guide on every edge at 80%', g['title'][0] == g['title'][1], (r, g))
        else:
            check('no UI guide pixels with the guides off', g['action'][0] == 0 and g['title'][0] == 0, (r, g))
    else:
        check('the UI tab draws the screen', False, sorted(c)[:20])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
