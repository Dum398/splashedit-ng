# The scene inspector's Tilemap section and the Tilemap tab, driven through the
# GUI on copies of the courtyard example with tests/tilemap/tiles.png as its one
# sprite sheet: Add tilemap, name a kind and give a new brush it, then in the
# Tilemap tab paint a stroke, fill a rectangle (Shift), erase (right button),
# paint two object cells and pick a brush (Alt); each lands in the saved scene,
# each stroke prints one "tiles:" line and goes back in one Ctrl+Z, and the
# background export's tilemap line (the splashpack-cli --stats one) matches.
# Controls and cells are clicked by name (--click @tiles.cell.1.1).
# usage: python3 tilemapcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shotOut = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# Tilemap is the inspector's last section: the wheel over the inspector scrolls
# to the bottom, which brings it into view. It grows as brushes are added, so
# the script scrolls again after those.
SCROLL = ['--wheel', '1420,500,-200', '--wait', '3']
START = ['--wait', '20', '--wait-export'] + SCROLL
mid = None
drawn = set()  # the controls the last run drew


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def stroke(a, b, mods=()):
    """Holds the left button from cell a to cell b (with modifier keys held)."""
    down = [x for m in mods for x in ('--keydown', m)]
    up = [x for m in mods for x in ('--keyup', m)]
    return down + ['--mousedown', '@tiles.cell.%d.%d' % a, '--wait', '2', '--move', '@tiles.cell.%d.%d' % b, '--wait', '2',
                   '--mouseup', '0', '--wait', '3'] + up


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']
ADD = click('scene.tileadd') + SCROLL
TAB = click('tab.tiles') + ['--wait', '5']
# Kind 1 named, then a brush made an object of that kind.
KIND = click('scene.tilekindadd') + SCROLL + click('scene.tilebrushadd') + SCROLL + click('scene.tilebrushkind') + \
    click('scene.tilebrushkind/1: kind 1') + SCROLL


def run(args, shot=None):
    """Runs `args` on a fresh courtyard copy whose one sprite sheet is the
    tileset, then Ctrl+S; returns the saved tilemap (None when the key is
    absent) and the editor's output without its list of controls."""
    global mid, drawn
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shutil.copy(os.path.join(here, 'tilemap', 'tiles.png'), os.path.join(project, 'textures', 'tiles.png'))
    scene = os.path.join(project, 'courtyard.scene')
    j = json.load(open(scene, encoding='utf-8'))
    j['spriteSheets'] = [{'name': 'tiles', 'texture': 'textures/tiles.png', 'bitDepth': 4, 'cellWidth': 16, 'cellHeight': 16,
                          'anims': []}]
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
    drawn = {m.group(1) for m in (re.match(r'control (\S+) at ', l) for l in lines) if m}
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')).get('tilemap'), out


def painted(t):
    """{(x, y): brush} of the painted cells."""
    if not t: return None
    return {(x, y): b for y, row in enumerate(t['cells']) for x, b in enumerate(row) if b != -1}


def last(pattern, out):
    found = re.findall(pattern, out, re.M)
    return found[-1] if found else None


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
assert 'tilemap' not in original, 'the example already has a tilemap'

t, out = run([])
check('save alone writes no tilemap', t is None, str(t) + ' | ' + out[-200:])
check('the section reports none', last(r'^scene: tilemap (none|tileset .*)$', out) == 'none', out[-300:])
check('no Tilemap tab without a tilemap', 'tab.ui' in drawn and 'tab.tiles' not in drawn, sorted(n for n in drawn if n.startswith('tab.')))

# Add: 20 x 15 empty, from the sheet, a floor and a wall brush.
t, out = run(ADD)
check('Add tilemap adds a 20x15 map on the sheet', t and t['tileset'] == 'tiles' and t['width'] == 20 and t['height'] == 15 and
      len(t['cells']) == 15 and all(len(r) == 20 and set(r) == {-1} for r in t['cells']), str(t)[:300])
check('the section reports it', last(r'^scene: tilemap (tileset.*)$', out) ==
      'tileset tiles size 20x15 brushes floor:0W:0:-1 wall:1S:0:-1 kinds none selected 0', out[-400:])
check('an empty map exports its 620 bytes',
      last(r'^scene: tilemap export (.*)$', out) == 'tilemap 20x15 tile=16x16 sheet=0 objects=0 bytes=620', out[-400:])

# The painter: a stroke, a Shift rectangle, a right-button erase, two object
# cells with the new brush, and an Alt+click pick.
PAINT = ADD + KIND + TAB + click('tiles.brush.0') + stroke((1, 1), (4, 1)) + click('tiles.brush.1') + \
    stroke((0, 3), (2, 4), mods=('shift',)) + ['--click', '@tiles.cell.2.1,0,0,1', '--wait', '3'] + click('tiles.brush.2') + \
    click('tiles.cell.5.5') + click('tiles.cell.6.5') + ['--keydown', 'alt'] + click('tiles.cell.0.3') + ['--keyup', 'alt']
t, out = run(PAINT, shot=shotOut)
want = {(1, 1): 0, (3, 1): 0, (4, 1): 0, (5, 5): 2, (6, 5): 2}
want.update({(x, y): 1 for x in range(3) for y in (3, 4)})
check('the new brush is kind 1 on cell 2', t and t['brushes'][2]['kind'] == 1 and t['brushes'][2]['cell'] == 2 and
      t['kindNames'] == ['(none)', 'kind 1'], str(t and t['brushes']))
check('the strokes land in the saved cells', painted(t) == want, f'{painted(t)} | ' + out[-300:])
strokes = re.findall(r'^tiles: (paint|fill|erase|pick) (.*)$', out, re.M)
check('one scripted line a stroke', strokes == [
    ('paint', 'brush 0 cells 4 at 1,1..4,1'), ('fill', 'brush 1 cells 6 at 0,3..2,4'), ('erase', 'cells 1 at 2,1..2,1'),
    ('paint', 'brush 2 cells 1 at 5,5..5,5'), ('paint', 'brush 2 cells 1 at 6,5..6,5'), ('pick', 'brush 1 at 0,3')], strokes)
check('Alt+click picked the wall brush', last(r'^tiles: view .* brush (-?\d+) ', out) == '1', out[-300:])
check('the export line counts the two objects',
      last(r'^scene: tilemap export (.*)$', out) == 'tilemap 20x15 tile=16x16 sheet=0 objects=2 bytes=632', out[-400:])
check('no problems', last(r'^scene: tilemap problems (.*)$', out) == 'none', out[-300:])

# Undo: a whole stroke is one Ctrl+Z, and so is a rectangle.
t, out = run(ADD + TAB + stroke((1, 1), (4, 1)) + SAVE_MID + UNDO)
check('one Ctrl+Z takes the whole stroke back', painted(mid) == {(x, 1): 0 for x in range(1, 5)} and painted(t) == {},
      f'{painted(mid)} then {painted(t)} | ' + out[-200:])
t, out = run(ADD + TAB + stroke((1, 1), (2, 1)) + stroke((0, 3), (2, 4), mods=('shift',)) + SAVE_MID + UNDO)
check('one Ctrl+Z takes the rectangle back, leaving the stroke before it',
      painted(mid) and len(painted(mid)) == 8 and painted(t) == {(1, 1): 0, (2, 1): 0}, f'{painted(mid)} then {painted(t)}')
t, out = run(ADD + SAVE_MID + UNDO)
check('one Ctrl+Z removes the added tilemap', mid is not None and t is None, f'{mid is not None} then {t}')

# A brush on a cell the sheet does not have is reported, and export refuses.
t, out = run(ADD + ['--dblclick', '@scene.tilebrushcellnum', '--wait', '3', '--key', 'ctrl+a', '--text', '9', '--key', 'enter',
                    '--wait', '3'])
check('a brush past the sheet is reported',
      last(r'^scene: tilemap problems (.*)$', out) == "Tile brush 'floor' draws tileset cell 9, but 'tiles' has 8 (0..7)", out[-400:])
check('export fails', last(r'^scene: tilemap export (.*)$', out) == 'failed', out[-300:])

# Remove.
t, out = run(ADD + click('scene.tileremove'))
check('Remove removes it', t is None, str(t)[:200])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
