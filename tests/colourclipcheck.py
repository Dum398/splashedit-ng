# Copy and Paste on a colour field (#176): a right click on a colour well opens
# a menu with Copy (its #RRGGBB on the clipboard) and Paste (the clipboard's
# colour, one undo step; offered only while the clipboard holds a colour). The
# left click still opens the picker. Drives the editor headless on copies of
# tests/ui/ui.scene (UI canvas colours) and tests/light/light.scene (a light's
# colour and the scene's background colour) and reads the saved scenes.
# usage: python3 colourclipcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = checks = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = ['--wait', '3']


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def ui_project():
    """tests/ui/ui.scene as a project of its own, fonts and textures copied in."""
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
    return d, os.path.join(d, 'ui.scene')


def light_project():
    d = os.path.join(tempfile.mkdtemp(), 'light')
    shutil.copytree(os.path.join(here, 'light'), d)
    return d, os.path.join(d, 'light.scene')


def run(make, head, args):
    """Runs the editor on a fresh project from `make`, `head` then `args`;
    returns the saved scene, the editor's output (without its controls) and
    the named controls of the last frame."""
    d, scene = make()
    cmd = [editor, '--size', '1600x960', '--project', d, '--scene', scene] + head + args + [
        '--wait', '5', '--screenshot', os.path.join(d, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=150)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')), out, rects


def centre(r, button=None):
    s = '%d,%d' % ((r[0] + r[2]) // 2, (r[1] + r[3]) // 2)
    return s if button is None else s + ',%d' % button


def q(c):
    """A colour as the field shows it: 0..255 per channel."""
    return [round(v * 255) for v in c[:3]]


# ---------------- The UI canvas: an element's Colour ----------------
UI = ['--ui', '--wait', '5']
_, out, _ = run(ui_project, UI, [])
m = re.search(r'uicanvas: screen [\d.]+,[\d.]+ scale [\d.]+ list ([\d.]+),([\d.]+) row ([\d.]+)', out)
check('--ui reports where the element list is', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
LX, LY, LH = map(float, m.groups())


def row(i):
    return '%d,%d' % (round(LX + 40), round(LY + (i + 0.5) * LH))


BAR, CORNER = row(0), row(1)  # two boxes: 'bar' (#1A4DCC) and 'corner' (#FF0000)
orig = {e['name']: e for e in json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))['canvases'][0]['elements']}
assert q(orig['bar']['color']) != q(orig['corner']['color'])

_, out, rects = run(ui_project, UI, ['--click', CORNER] + W)
check("clicking the list's second row selects 'corner'", 'uicanvas: inspector element corner' in out, out[-300:])
check('the Colour well is named ui.col', 'ui.col' in rects, sorted(rects)[:20])
if 'ui.col' not in rects:
    print(f'{fails} failures')
    sys.exit(1)
COL = rects['ui.col']

# A right click opens the menu, not the picker; a left click the picker, not the menu.
_, out, rects = run(ui_project, UI, ['--click', CORNER] + W + ['--click', centre(COL, 1)] + W)
check('a right click on the Colour well opens its Copy / Paste menu',
      'ui.col.menu/Copy' in rects and 'ui.col.menu/Paste' in rects and 'popups open: yes' in out, (sorted(rects), out[-200:]))
check('... and not the colour picker', 'ui.colpick' not in rects, sorted(rects))
_, out, rects = run(ui_project, UI, ['--click', CORNER] + W + ['--click', centre(COL)] + W)
check('a left click still opens the colour picker', 'ui.colpick' in rects, sorted(rects))
check('... and not the menu', 'ui.col.menu/Copy' not in rects, sorted(rects))

# Copy on 'corner', Paste on 'bar'.
COPY_PASTE = (['--click', CORNER] + W + ['--click', centre(COL, 1)] + W + ['--click', '@ui.col.menu/Copy'] + W +
              ['--click', BAR] + W + ['--click', centre(COL, 1)] + W + ['--click', '@ui.col.menu/Paste'] + W)
s, out, rects = run(ui_project, UI, COPY_PASTE + ['--key', 'ctrl+s'] + W)
els = {e['name']: e for e in s['canvases'][0]['elements']}
check("Copy on 'corner' then Paste on 'bar' gives 'bar' corner's colour",
      q(els['bar']['color']) == q(orig['corner']['color']), (els['bar']['color'], out[-300:]))
check("'corner' keeps its colour", els['corner']['color'] == orig['corner']['color'], els['corner']['color'])
check('the other elements keep theirs', all(els[n]['color'] == orig[n]['color'] for n in orig if n not in ('bar',)),
      {n: els[n]['color'] for n in els})
check('Paste closes the menu', 'popups open: no' in out, out[-200:])

# One Ctrl+Z takes the paste back.
s, out, _ = run(ui_project, UI, COPY_PASTE + ['--key', 'ctrl+z'] + W + ['--key', 'ctrl+s'] + W)
els = {e['name']: e for e in s['canvases'][0]['elements']}
check("one Ctrl+Z after the paste puts back 'bar''s colour", q(els['bar']['color']) == q(orig['bar']['color']),
      (els['bar']['color'], out[-300:]))

# Not a colour on the clipboard (Ctrl+C on an element puts its JSON there):
# Paste is offered but disabled, a click on it does nothing and leaves the menu open.
GARBAGE = (['--click', BAR] + W + ['--key', 'ctrl+c'] + W + ['--click', centre(COL, 1)] + W +
           ['--click', '@ui.col.menu/Paste'] + W)
_, out, rects = run(ui_project, UI, GARBAGE)
check('with JSON on the clipboard a click on Paste leaves the menu open (Paste disabled)',
      'popups open: yes' in out and 'ui.col.menu/Paste' in rects, out[-200:])
# Then Escape, and a Duplicate so the save is certain to write.
s, out, _ = run(ui_project, UI, GARBAGE + ['--key', 'escape'] + W + ['--click', BAR] + W + ['--key', 'ctrl+d'] + W + ['--key', 'ctrl+s'] + W)
els = {e['name']: e for e in s['canvases'][0]['elements']}
check('the Duplicate after it was saved (the save wrote)', len(s['canvases'][0]['elements']) == len(orig) + 1,
      ([e['name'] for e in s['canvases'][0]['elements']], out[-200:]))
check("... and 'bar''s colour is unchanged", els['bar']['color'] == orig['bar']['color'], els['bar']['color'])

# ---------------- A light's Colour and the scene's background ----------------
lorig = json.load(open(os.path.join(here, 'light', 'light.scene'), encoding='utf-8'))


def light_of(s, name):
    o = next(o for o in s['objects'] if o['name'] == name)
    return next(c for c in o['components'] if c['type'] == 'light')


RED = light_of(lorig, 'Red')['color']
assert q(RED) != q(light_of(lorig, 'Spare')['color']) and q(RED) != q(lorig['settings']['fog']['color'])
_, out, rects = run(light_project, ['--select', 'Red', '--wait', '20'], [])
check("a light's Colour well is named obj.lcol", 'obj.lcol' in rects, sorted(rects)[:20])
_, _, srects = run(light_project, ['--select', 'Spare', '--wait', '20'], [])
_, _, frects = run(light_project, ['--wait', '20'], [])
check('the scene background well is named scene.fogcol', 'scene.fogcol' in frects, sorted(frects)[:20])
if 'obj.lcol' not in rects or 'obj.lcol' not in srects or 'scene.fogcol' not in frects or 'tree.Lit' not in rects:
    print(f'{fails} failures')
    sys.exit(1)
LCOL, SCOL, FOG = rects['obj.lcol'], srects['obj.lcol'], frects['scene.fogcol']
lit = rects['tree.Lit']
ROOT = '%d,%d' % ((lit[0] + lit[2]) // 2, lit[1] - (lit[3] - lit[1]) // 2)  # the scene's own row, above the first object

# Copy Red's colour, Paste it on Spare's, then on the scene background.
LIGHT_PASTE = (['--click', centre(LCOL, 1)] + W + ['--click', '@obj.lcol.menu/Copy'] + W + ['--click', '@tree.Spare'] + W +
               ['--click', centre(SCOL, 1)] + W + ['--click', '@obj.lcol.menu/Paste'] + W)
FOG_PASTE = ['--click', ROOT] + W + ['--click', centre(FOG, 1)] + W + ['--click', '@scene.fogcol.menu/Paste'] + W
HEAD = ['--select', 'Red', '--wait', '20']
s, out, _ = run(light_project, HEAD, LIGHT_PASTE + FOG_PASTE + ['--key', 'ctrl+s'] + W)
check("Copy on Red's light colour, Paste on Spare's: Spare gets Red's colour", q(light_of(s, 'Spare')['color']) == q(RED),
      (light_of(s, 'Spare')['color'], out[-300:]))
check('Red keeps its colour', light_of(s, 'Red')['color'] == RED, light_of(s, 'Red')['color'])
check("Paste on the scene's Background sets the fog colour to it", q(s['settings']['fog']['color']) == q(RED),
      (s['settings']['fog']['color'], out[-300:]))
check('the rest of the fog block is as it was',
      {k: v for k, v in s['settings']['fog'].items() if k != 'color'} ==
      {k: v for k, v in lorig['settings']['fog'].items() if k != 'color'}, s['settings']['fog'])

# Each paste is one undo step: one Ctrl+Z takes back the fog paste only, a second the light's.
s, out, _ = run(light_project, HEAD, LIGHT_PASTE + FOG_PASTE + ['--key', 'ctrl+z'] + W + ['--key', 'ctrl+s'] + W)
check('one Ctrl+Z after the background paste puts its colour back',
      q(s['settings']['fog']['color']) == q(lorig['settings']['fog']['color']), (s['settings']['fog']['color'], out[-300:]))
check("... and leaves Spare's pasted colour", q(light_of(s, 'Spare')['color']) == q(RED), light_of(s, 'Spare')['color'])
# (then a Duplicate of the selection, so the save is certain to write)
s, out, _ = run(light_project, HEAD, LIGHT_PASTE + FOG_PASTE + ['--key', 'ctrl+z'] + W + ['--key', 'ctrl+z'] + W +
                ['--click', '@tree.Lit'] + W + ['--key', 'ctrl+d'] + W + ['--key', 'ctrl+s'] + W)
check('the Duplicate after them was saved (the save wrote)', len(s['objects']) == len(lorig['objects']) + 1,
      ([o['name'] for o in s['objects']], out[-300:]))
check("a second Ctrl+Z puts back Spare's colour", light_of(s, 'Spare')['color'] == light_of(lorig, 'Spare')['color'],
      (light_of(s, 'Spare')['color'], out[-300:]))

print(f'{checks - fails}/{checks} checks passed')
print(f'{fails} failures')
sys.exit(1 if fails else 0)
