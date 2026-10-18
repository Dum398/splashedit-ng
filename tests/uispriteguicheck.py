# HUD sprites in the UI tab: the Image inspector's Sheet menu (None and the
# scene's sprite sheets), the cell picker drawn over the sheet's texture (a
# named control per cell, ui.sheetcell/<n>) and Snap size (ui.snapsize). Opens
# a project holding tests/sprite's two sheets and a canvas with one plain
# image, selects the image on the canvas, picks sheet 'hero', clicks cell 6
# and Snap size, saves, and reads the editor's scripted lines ("uicanvas:
# image sheet <sheet> cell <n> of <cells> grid <c>x<r> size <w>x<h>") and the
# saved scene. Each pick is one undo step: a second run takes back Snap size
# and the cell with two Ctrl+Z. Picking None makes it a plain image again,
# saved without "sheet"/"cell". The canvas must show only the cell: its pixels
# in the screenshot are those of hero's cell 6. Needs a display (xvfb-run -a).
# usage: python3 uispriteguicheck.py <splashedit>
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
IMG = (100, 100, 32, 32)  # the image's rect on the 320x240 canvas


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project():
    """tests/sprite's sheets in a project of their own, no objects, and a
    canvas 'hud' with one plain image 'icon'."""
    d = os.path.join(tempfile.mkdtemp(), 'hud')
    os.makedirs(d)
    s = json.load(open(os.path.join(here, 'sprite', 'sprite.scene'), encoding='utf-8'))
    for f in ('hero.png', 'fx.png', 'sprite.lua'):
        shutil.copy(os.path.join(here, 'sprite', f), d)
    s['objects'] = []
    s['canvases'] = [{'name': 'hud', 'visible': True, 'sortOrder': 0, 'defaultFont': None, 'elements': [
        {'type': 'image', 'name': 'icon', 'visible': True, 'rect': list(IMG), 'anchorMin': [0, 0], 'anchorMax': [0, 0],
         'color': [0.5, 0.5, 0.5], 'texture': None, 'bitDepth': 8, 'cutout': True}]}]
    json.dump(s, open(os.path.join(d, 'hud.scene'), 'w'), indent=1)
    return d


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
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p_ = a + b - c
                pa, pb, pc = abs(p_ - a), abs(p_ - b), abs(p_ - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def run(args, d=None):
    """Runs the editor on a fresh project with the UI tab open, then Ctrl+S.
    Returns the project, the saved scene, its text, the output and the named controls."""
    d = d or project()
    cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '5', '--screenshot', os.path.join(d, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    text = open(os.path.join(d, 'hud.scene'), encoding='utf-8').read()
    controls = {m.group(1): tuple(map(int, m.groups()[1:])) for m in re.finditer(r'^control (.+?) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', out, re.M)}
    return d, json.loads(text), text, out, controls


def said(out):
    return re.findall(r'^uicanvas: image sheet (.*)$', out, re.M)


def icon(scene):
    return scene['canvases'][0]['elements'][0]


# First, where the canvas is.
d, scene, text, out, ctl = run([])
m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out)
check('--ui opens the UI tab and reports its layout', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
X, Y, S = map(float, m.groups())
CLICK = '%d,%d' % (round(X + (IMG[0] + 8.5) * S), round(Y + (IMG[1] + 8.5) * S))  # inside the image

# Selected, the plain image shows Sheet None and the Sheet row.
d, scene, text, out, ctl = run(['--click', CLICK, '--wait', '5'])
check('the Image inspector draws the Sheet row', 'ui.sheet' in ctl, sorted(k for k in ctl if k.startswith('ui.'))[:20])
check('a plain image reports sheet none', said(out) == ['none'], said(out))
check('no cell picker for a plain image', not any(k.startswith('ui.sheetcell') for k in ctl) and 'ui.snapsize' not in ctl,
      sorted(k for k in ctl if k.startswith('ui.sheet')))

# The menu lists None and both sheets.
d, scene, text, out, ctl = run(['--click', CLICK, '--wait', '5', '--click', '@ui.sheet', '--wait', '5'])
check('the Sheet menu offers None and the scene\'s sheets',
      all(k in ctl for k in ('ui.sheet/None', 'ui.sheet/hero', 'ui.sheet/fx')), sorted(k for k in ctl if k.startswith('ui.sheet/')))

# Pick hero, cell 6, Snap size.
PICK = ['--click', CLICK, '--wait', '5', '--click', '@ui.sheet', '--wait', '5', '--click', '@ui.sheet/hero', '--wait', '5',
        '--click', '@ui.sheetcell/6', '--wait', '5', '--click', '@ui.snapsize', '--wait', '5']
EMPTY = '%d,%d' % (round(X + 20.5 * S), round(Y + 200.5 * S))  # empty canvas: deselects, so no handles cover the image
d, scene, text, out, ctl = run(PICK + ['--click', EMPTY, '--wait', '5', '--key', 'ctrl+1', '--wait', '5'])
lines = said(out)
check('picking hero shows its cell 0 of 8 in a 4x2 grid', 'hero cell 0 of 8 grid 4x2 size 32x32' in lines, lines)
check('clicking a cell in the picker selects it', 'hero cell 6 of 8 grid 4x2 size 32x32' in lines, lines)
check('Snap size makes it one cell, 16x16', lines[-1:] == ['hero cell 6 of 8 grid 4x2 size 16x16'], lines)
ctl = run(PICK)[4]
check('the picker names every cell', all('ui.sheetcell/%d' % i in ctl for i in range(8)) and 'ui.sheetcell/8' not in ctl,
      sorted(k for k in ctl if k.startswith('ui.sheetcell')))
check('the picker and Snap size are drawn', 'ui.sheetcells' in ctl and 'ui.snapsize' in ctl, sorted(k for k in ctl if k.startswith('ui.')))
e = icon(scene)
check('saved: sheet hero, cell 6, 16x16 at the same place', (e.get('sheet'), e.get('cell'), e['rect']) == ('hero', 6, [100, 100, 16, 16]), e)
check('saved: the tint is left as it was', e['color'] == [0.5, 0.5, 0.5], e['color'])
# The canvas shows only cell 6 (column 2, row 1) of hero, at 16x16, tint 128:
# at 100% (Ctrl+1), where canvas pixels are screen pixels.
X1, Y1, S1 = map(float, re.findall(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out)[-1])
check('Ctrl+1 shows the canvas at 100%', S1 == 1, S1)
w, h, bpp, rows = readpng(os.path.join(d, 'shot.png'))
sw, sh, sbpp, srows = readpng(os.path.join(here, 'sprite', 'hero.png'))
bad, opaque = 0, 0
for y in range(16):
    for x in range(16):
        # The preview samples u = x * 15 / 16 across a 16 px rect, as the export's UVs land.
        u, v = 32 + x * 15 // 16, 16 + y * 15 // 16
        if srows[v][u * sbpp + 3] < 128: continue
        opaque += 1
        sx, sy = int(X1 + (IMG[0] + x + 0.5) * S1), int(Y1 + (IMG[1] + y + 0.5) * S1)
        got = tuple(rows[sy][sx * bpp:sx * bpp + 3])
        want = tuple(srows[v][u * sbpp:u * sbpp + 3])
        bad += any(abs(a - b) > 12 for a, b in zip(got, want))
check('the canvas draws hero cell 6 in the image', opaque > 0 and bad == 0, (bad, opaque))

# Undo: Snap size, then the cell, each one step.
d, scene, text, out, ctl = run(PICK + ['--key', 'ctrl+z', '--wait', '5'])
e = icon(scene)
check('one Ctrl+Z takes back Snap size only', (e.get('sheet'), e.get('cell'), e['rect']) == ('hero', 6, [100, 100, 32, 32]), e)
d, scene, text, out, ctl = run(PICK + ['--key', 'ctrl+z', '--wait', '5', '--key', 'ctrl+z', '--wait', '5'])
e = icon(scene)
check('a second Ctrl+Z takes back the cell', (e.get('sheet'), e.get('cell')) == ('hero', 0), e)

# None: a plain image again, saved without the keys.
d, scene, text, out, ctl = run(PICK + ['--click', '@ui.sheet', '--wait', '5', '--click', '@ui.sheet/None', '--wait', '5'])
e = icon(scene)
check('None makes it a plain image, saved without sheet or cell', 'sheet' not in e and 'cell' not in e and said(out)[-1:] == ['none'],
      (e, said(out)[-2:]))

print(f'{fails} failures' if fails else 'uispriteguicheck: all checks passed')
sys.exit(1 if fails else 0)
