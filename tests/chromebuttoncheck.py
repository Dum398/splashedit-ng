#!/usr/bin/env python3
"""Window chrome check (#162): the maximize button shows the restore icon
while the window is maximized.

Usage: chromebuttoncheck.py <splashedit> [out.png]
Reads the button name, the window state (--hittest) and the drawn glyph in
a normal window and in one drawn as maximized."""

import os, shutil, struct, subprocess, sys, tempfile, zlib

editor = sys.argv[1]
out_png = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
failures = 0


def check(name, ok, detail=''):
    global failures
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else '  ' + str(detail)))
    if not ok:
        failures += 1


def run(args):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '10', '--hittest', '800,500',
                                                                          '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].split(' at ')
            a, b = at.split(' ')
            controls[name] = tuple(float(v) for v in a.split(',') + b.split(','))
    maxed = next((l.split('maximized=')[1].split()[0] for l in lines if l.startswith('hittest: ')), None)
    return lines, controls, shot, maxed


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


def glyph(shot, r):
    """Vertical strokes inside the button: runs of adjacent columns holding at
    least four bright pixels. A square has two (its sides); the restore icon
    has three (the front square's sides and the back square's right edge)."""
    w, h, rows = png_rgb(shot)
    x0, y0, x1, y1 = (int(v) for v in r)
    bright = [sum(1 for y in range(y0, y1) if sum(rows[y][3 * x:3 * x + 3]) > 300) for x in range(x0, x1)]
    strokes, inside = 0, False
    for n in bright:
        if n >= 4 and not inside:
            strokes += 1
        inside = n >= 4
    return strokes


lines, c0, shot0, m0 = run([])
check('a fresh window is not maximized', m0 == '0', m0)
check('the button is named chrome.maximize', 'chrome.maximize' in c0 and 'chrome.restore' not in c0, sorted(k for k in c0 if k.startswith('chrome')))
g0 = glyph(shot0, c0.get('chrome.maximize', (0, 0, 0, 0)))
print('glyph normal', g0)
check('it draws a single square', g0 == 2, g0)

# Under xvfb there is no window manager, so SDL_MaximizeWindow never sets
# the maximized flag; --assume-maximized draws the chrome as if it had.
lines, c1, shot1, m1 = run(['--assume-maximized'])
check('the button is then named chrome.restore', 'chrome.restore' in c1 and 'chrome.maximize' not in c1, sorted(k for k in c1 if k.startswith('chrome')))
g1 = glyph(shot1, c1.get('chrome.restore', (0, 0, 0, 0)))
print('glyph maximized', g1)
check('it draws the restore icon', g1 == 3, g1)
if out_png:
    shutil.copy(shot1, out_png)

print('%d failures' % failures)
sys.exit(1 if failures else 0)
