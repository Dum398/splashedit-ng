# The Tilemap tab's tools, driven through the GUI on copies of the courtyard
# example with tests/tilemap/tiles.png as its one sprite sheet and an 8 x 6
# tilemap already in the scene: the tool strip (Paint, Rectangle, Fill, Pick),
# Fill as a bucket (right button: flood erase), Shift and Alt working in every
# tool, and one Ctrl+Z a flood. Each lands in the saved scene and prints one
# "tiles:" line. Controls and cells are clicked by name (--click @tiles.tool.fill).
# usage: python3 tilemap2check.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
shotOut = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
START = ['--wait', '20', '--wait-export']
mid = None
drawn = set()  # the controls the last run drew
rects = {}  # their rectangles, x0, y0, x1, y1

# A wall ring (brush 1) round a hole and a floor cell, a water pond (brush 2)
# and a strip of floor along the bottom; the rest empty.
MAP = [
    [-1, -1, -1, -1, -1, -1, -1, -1],
    [-1, 1, 1, 1, 1, -1, -1, -1],
    [-1, 1, -1, 0, 1, -1, 2, 2],
    [-1, 1, 1, 1, 1, -1, 2, 2],
    [-1, -1, -1, -1, -1, -1, -1, -1],
    [0, 0, 0, -1, -1, -1, -1, 1],
]
TILEMAP = {'tileset': 'tiles', 'width': 8, 'height': 6,
           'brushes': [{'name': 'floor', 'cell': 0, 'walkable': True, 'kind': 0, 'id': -1, 'color': [0.4, 0.8, 0.45]},
                       {'name': 'wall', 'cell': 1, 'walkable': False, 'kind': 0, 'id': -1, 'color': [0.9, 0.4, 0.4]},
                       {'name': 'water', 'cell': 3, 'walkable': True, 'kind': 0, 'id': -1, 'color': [0.35, 0.6, 0.95]}],
           'kindNames': ['(none)'], 'cells': [' '.join('%2d' % v for v in r) for r in MAP]}


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def rclick(name):
    return ['--click', '@' + name + ',0,0,1', '--wait', '3']


def stroke(a, b, mods=(), button=0):
    """Holds a button from cell a to cell b (with modifier keys held)."""
    down = [x for m in mods for x in ('--keydown', m)]
    up = [x for m in mods for x in ('--keyup', m)]
    return down + ['--mousedown', '@tiles.cell.%d.%d,0,0,%d' % (a + (button,)), '--wait', '2', '--move', '@tiles.cell.%d.%d' % b,
                   '--wait', '2', '--mouseup', str(button), '--wait', '3'] + up


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']
TAB = click('tab.tiles') + ['--wait', '5']


def run(args, shot=None, tilemap=TILEMAP):
    """Runs `args` on a fresh courtyard copy whose one sprite sheet is the
    tileset (and with `tilemap`), then Ctrl+S; returns the saved tilemap and
    the editor's output without its list of controls."""
    global mid, drawn, rects
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shutil.copy(os.path.join(here, 'tilemap', 'tiles.png'), os.path.join(project, 'textures', 'tiles.png'))
    scene = os.path.join(project, 'courtyard.scene')
    j = json.load(open(scene, encoding='utf-8'))
    j['spriteSheets'] = [{'name': 'tiles', 'texture': 'textures/tiles.png', 'bitDepth': 4, 'cellWidth': 16, 'cellHeight': 16,
                          'anims': []}]
    if tilemap: j['tilemap'] = tilemap
    json.dump(j, open(scene, 'w', encoding='utf-8'), indent=2)
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project] + START + args + [
        '--wait-export', '--wait', '5', '--key', 'ctrl+s', '--wait', '10', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    out = (r.stdout + r.stderr).strip()
    lines = out.splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    mid = json.load(open(snap, encoding='utf-8')).get('tilemap') if os.path.exists(snap) else None
    found = [m for m in (re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l) for l in lines) if m]
    drawn = {m.group(1) for m in found}
    rects = {m.group(1): tuple(int(v) for v in m.groups()[1:]) for m in found}
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')).get('tilemap'), out


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


def pixel(png, x, y):
    w, h, bpp, rows = png
    return tuple(rows[y][x * bpp:x * bpp + 3])


def rows(t):
    """The cells as lists of ints: the scene keeps one string a row (" 0  1 -1")."""
    return [[int(v) for v in r.split()] for r in t['cells']] if t else None


def last(pattern, out):
    found = re.findall(pattern, out, re.M)
    return found[-1] if found else None


def lines(prefix, out):
    return re.findall(r'^tiles: (%s) (.*)$' % prefix, out, re.M)


def changed(t, base=MAP):
    """{(x, y): brush} of the cells that differ from `base`."""
    r = rows(t)
    if r is None: return None
    return {(x, y): v for y, row in enumerate(r) for x, v in enumerate(row) if v != base[y][x]}


# The tool strip: four tools, Paint to start with.
t, out = run(TAB)
check('the tool strip is drawn', {'tiles.tool.paint', 'tiles.tool.rect', 'tiles.tool.fill', 'tiles.tool.pick'} <= drawn,
      sorted(n for n in drawn if n.startswith('tiles.tool')))
check('Paint is the tool to start with', lines('tool', out) == [('tool', 'paint')], lines('tool', out))
check('the preset map loads unchanged', rows(t) == MAP, rows(t))

# Fill: the outside empty region takes the water brush, a right click empties
# the wall ring, a fill with the region's own brush changes nothing; Ctrl+Z
# takes back the erase alone.
OUTSIDE = {(x, y) for y in range(6) for x in range(8) if MAP[y][x] == -1} - {(2, 2)}
RING = {(x, y) for y in range(1, 4) for x in range(1, 5) if MAP[y][x] == 1}
t, out = run(TAB + click('tiles.tool.fill') + click('tiles.brush.2') + click('tiles.cell.0.0') + rclick('tiles.cell.1.1') +
             click('tiles.cell.6.2') + SAVE_MID + UNDO)
check('Fill is the tool', lines('tool', out) == [('tool', 'paint'), ('tool', 'fill')], lines('tool', out))
check('one scripted line a flood', lines('flood', out) == [
    ('flood', 'brush 2 cells %d at 0,0' % len(OUTSIDE)), ('flood', 'erase cells %d at 1,1' % len(RING)),
    ('flood', 'brush 2 cells 0 at 6,2')], lines('flood', out))
check('the floods land in the saved cells', changed(mid) == {**{c: 2 for c in OUTSIDE}, **{c: -1 for c in RING}}, changed(mid))
check('one Ctrl+Z takes the erase back (the no-op fill left no step)', changed(t) == {c: 2 for c in OUTSIDE}, changed(t))
check('no paint or rectangle lines from the Fill tool', not lines('paint|fill|erase', out), lines('paint|fill|erase', out))

# Shift and Alt in the Fill tool; the Rectangle tool without Shift, and its
# right button; the Pick tool picks once and goes back to Paint.
t, out = run(TAB + click('tiles.tool.fill') + click('tiles.brush.0') + stroke((0, 4), (2, 4), mods=('shift',)) +
             ['--keydown', 'alt'] + click('tiles.cell.6.3') + ['--keyup', 'alt'] +
             click('tiles.tool.rect') + stroke((5, 0), (7, 1)) + stroke((5, 0), (6, 0), button=1) +
             click('tiles.tool.pick') + click('tiles.cell.1.2'), shot=shotOut)
check('the tools in order, Pick back to Paint after one pick',
      [v for _, v in lines('tool', out)] == ['paint', 'fill', 'rect', 'pick', 'paint'], lines('tool', out))
check('Shift and Alt in Fill, a rectangle each button in Rectangle, a pick in Pick', lines('fill|pick|flood|paint|erase', out) == [
    ('fill', 'brush 0 cells 3 at 0,4..2,4'), ('pick', 'brush 2 at 6,3'), ('fill', 'brush 2 cells 6 at 5,0..7,1'),
    ('fill', 'erase cells 2 at 5,0..6,0'), ('pick', 'brush 1 at 1,2')], lines('fill|pick|flood|paint|erase', out))
want = {(0, 4): 0, (1, 4): 0, (2, 4): 0, (7, 0): 2, (5, 1): 2, (6, 1): 2, (7, 1): 2}
check('the rectangles land in the saved cells', changed(t) == want, changed(t))
check('the picked brush paints', last(r'^tiles: view .* brush (-?\d+) ', out) == '1', out[-300:])

# The right button erases in Paint and Pick too (a one-cell stroke).
t, out = run(TAB + click('tiles.tool.pick') + rclick('tiles.cell.3.2'))
check('right click in Pick erases', changed(t) == {(3, 2): -1} and lines('erase', out) == [('erase', 'cells 1 at 3,2..3,2')],
      f'{changed(t)} {lines("erase", out)}')

# The Grid toggle: on to start with; off leaves the tile edges as the tiles
# draw them, on draws a light line over a dark band there; the scene file
# never hears of it.
shots = tempfile.mkdtemp()
GON, GOFF = os.path.join(shots, 'gridon.png'), os.path.join(shots, 'gridoff.png')
t, out = run(TAB + ['--shot', GON] + click('tiles.grid') + ['--shot', GOFF] + click('tiles.grid') + click('tiles.grid'))
check('the Grid toggle is drawn', 'tiles.grid' in drawn, sorted(n for n in drawn if n.startswith('tiles.') and 'cell' not in n))
check('a scripted line each toggle', lines('grid', out) == [('grid', 'on'), ('grid', 'off'), ('grid', 'on'), ('grid', 'off')],
      lines('grid', out))
check('the toggle is not in the scene', t == TILEMAP and 'grid' not in json.dumps(t), t)
if os.path.exists(GON) and os.path.exists(GOFF) and 'tiles.cell.3.1' in rects:
    on, off = readpng(GON), readpng(GOFF)
    x0, y0, x1, y1 = rects['tiles.cell.3.1']  # a wall between walls: the same tile each side of its left edge
    ys = range(y0 + 3, y1 - 3)
    edge = sum(pixel(on, x0, y) != pixel(off, x0, y) for y in ys)
    band = sum(sum(pixel(on, x0 - 1, y)) < sum(pixel(off, x0 - 1, y)) for y in ys)
    light = sum(sum(pixel(on, x0, y)) > sum(pixel(on, x0 - 1, y)) for y in ys)
    inside = sum(pixel(on, (x0 + x1) // 2, y) != pixel(off, (x0 + x1) // 2, y) for y in ys)
    check('grid on: the edge is drawn over, darker beside it and lighter on it; inside untouched',
          edge == len(ys) and band == len(ys) and light == len(ys) and inside == 0, (edge, band, light, inside, len(ys)))
else:
    check('grid screenshots taken', False, (os.path.exists(GON), os.path.exists(GOFF), 'tiles.cell.3.1' in rects))

# The inspector's brush Colour: the background colour copied from its field and
# pasted on the water brush is one undo step; the cells and the other brushes
# stay; a left click opens the picker.
SCROLL = ['--wheel', '1420,500,-200', '--wait', '3']
t, out = run(rclick('scene.fogcol') + click('scene.fogcol.menu/Copy') + SCROLL + click('scene.tilebrush.2') + SCROLL +
             rclick('scene.tilebrushcolor') + click('scene.tilebrushcolor.menu/Paste') + SAVE_MID + UNDO)
colours = re.findall(r'^scene: tilemap brush (\d+) color (#[0-9A-F]{6})$', out, re.M)
check('the Colour row is drawn', 'scene.tilebrushcolor' in drawn, sorted(n for n in drawn if n.startswith('scene.tilebrush')))
check('a scripted line for the selected brush\'s colour', colours == [('0', '#66CC73'), ('2', '#5999F2'), ('2', '#808099'),
                                                                      ('2', '#5999F2')], colours)
hexOf = lambda c: '#' + ''.join('%02X' % int(min(max(v, 0), 1) * 255 + 0.5) for v in c)
check('the paste lands in the saved brush, nothing else changes',
      mid and hexOf(mid['brushes'][2]['color']) == '#808099' and rows(mid) == MAP and mid['brushes'][:2] == TILEMAP['brushes'][:2],
      mid and mid['brushes'])
check('one Ctrl+Z takes it back', t and t['brushes'] == TILEMAP['brushes'], t and t['brushes'])
check('the existing tilemap line is unchanged in form',
      last(r'^scene: tilemap (tileset .*)$', out) == 'tileset tiles size 8x6 brushes floor:0W:0:-1 wall:1S:0:-1 water:3W:0:-1 kinds none '
                                                     'selected 2', out[-400:])
t, out = run(SCROLL + click('scene.tilebrushcolor'))
check('a left click on the Colour opens the picker', 'popups open: yes' in out, out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
