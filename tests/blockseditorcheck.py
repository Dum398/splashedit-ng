# The block editor (a .blocks script tab), driven through the GUI on copies
# of the courtyard example whose Game Logic runs scripts/door.blocks, an
# empty block script (blocks/editor/empty.blocks). Edit script opens it in a
# blocks tab; "on_interact" is dragged from the palette onto the canvas,
# "hide" from the palette's Objects section under it, where it must snap
# (its control sits right under the hat), its field typed to "Door" and the
# tab saved with Ctrl+S: the file must then read exactly as the expected
# JSON and `splashpack-cli blocks` must turn it into the expected Lua. Then
# Ctrl+Z / Ctrl+Y (one step per edit), Delete on a selected block, and a
# drop away from the hat (a second, loose stack: the negative control for
# the snap). Palette blocks, the canvas and the blocks on it are aimed at by
# name (blocks.palette.<op>, blocks.canvas, blocks.block.<path>,
# blocks.field.<path>; see src/editor/blocks_view.hh); a first run reads
# where they are drawn.
# usage: python3 blockseditorcheck.py <splashedit> [<splashpack-cli>]   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fixture = os.path.join(here, 'blocks', 'editor', 'empty.blocks')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" button at 1600x960 (as in scripteditorcheck.py)
OPEN = ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '10']
HAT_AT = (40, 40)  # where the hat is dropped, in canvas coordinates


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    text = open(scene, encoding='utf-8').read()
    assert text.count('"lua": "scripts/game_logic.lua"') == 1
    open(scene, 'w', encoding='utf-8', newline='\n').write(text.replace('"lua": "scripts/game_logic.lua"', '"lua": "scripts/door.blocks"'))
    shutil.copy(fixture, os.path.join(project, 'scripts', 'door.blocks'))
    return project


def run(args):
    """The editor on a fresh project copy, then `args`: the project and the named controls as last drawn."""
    project = project_copy()
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + OPEN + args + [
        '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the run names was drawn', not missing, missing)
    controls = {}
    for l in lines:
        if l.startswith('control '):
            name, rest = l[len('control '):].rsplit(' at ', 1)
            a, b = rest.split(' ')
            x0, y0 = map(int, a.split(','))
            x1, y1 = map(int, b.split(','))
            controls[name] = (x0, y0, x1, y1)
    return project, controls


def centre(r):
    return ((r[0] + r[2]) / 2, (r[1] + r[3]) / 2)


def read(project):
    with open(os.path.join(project, 'scripts', 'door.blocks'), 'rb') as f:
        return f.read().decode('utf-8')


def lua(project):
    r = subprocess.run([cli, 'blocks', os.path.join(project, 'scripts', 'door.blocks')], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr.strip()


# Where things are: the palette before and after picking Objects, and the canvas.
_, c = run([])
check('a .blocks script opens in a blocks tab', 'blocks.canvas' in c and 'blocks.palette.on_interact' in c and 'tab.0' in c,
      sorted(k for k in c if k.startswith('blocks.'))[:8])
check('the empty script has no blocks', not [k for k in c if k.startswith('blocks.block.')], [k for k in c if k.startswith('blocks.block.')])
check('hide is not in view before Objects is picked', 'blocks.palette.hide' not in c)
_, c2 = run(['--click', '@blocks.category.Objects', '--wait', '5'])
check('the Objects chip brings hide into view', 'blocks.palette.hide' in c2)
if fails:
    print(f'{fails} failures')
    sys.exit(1)

canvas, hatPal, hidePal = c['blocks.canvas'], c['blocks.palette.on_interact'], c2['blocks.palette.hide']
# The canvas shows canvas 0,0 at its corner plus 24 px. A drag keeps the
# grabbed point where it is on the block, so the block's corner moves by the
# drag's offset.
origin = (canvas[0] + 24, canvas[1] + 24)
hatScreen = (origin[0] + HAT_AT[0], origin[1] + HAT_AT[1])
DRAG_HAT = ['--drag', '@blocks.palette.on_interact,0,0,%d,%d' % (hatScreen[0] - hatPal[0], hatScreen[1] - hatPal[1]), '--wait', '5']
OBJECTS = ['--click', '@blocks.category.Objects', '--wait', '5']

# The hat alone, to learn its height on the canvas.
_, c3 = run(DRAG_HAT)
hat = c3.get('blocks.block.s0.b0')
check('the hat lands where it was dropped', hat is not None and (hat[0], hat[1]) == hatScreen, (hat, hatScreen))
if not hat:
    print(f'{fails} failures')
    sys.exit(1)


def drag_hide(dx, dy):
    """hide from the palette, its corner to the hat's bottom left plus dx, dy."""
    return ['--drag', '@blocks.palette.hide,0,0,%d,%d' % (hat[0] + dx - hidePal[0], hat[3] + dy - hidePal[1]), '--wait', '5']


EDIT_FIELD = ['--click', '@blocks.field.s0.b1', '--wait', '5', '--text', 'Door', '--key', 'enter', '--wait', '5']
SAVE = ['--key', 'ctrl+s', '--wait', '5']

WANT = '''{
  "format": "splashedit-blocks",
  "version": 1,
  "variables": [],
  "stacks": [
    {
      "x": %d,
      "y": %d,
      "blocks": [
        {
          "op": "on_interact"
        },
        {
          "op": "hide",
          "field": "%s"
        }
      ]
    }
  ]
}
'''
WANT_LUA = '''function onInteract(self)
    Entity.SetActive(%s, false)
end
'''

# Drag, snap, type, save.
project, c4 = run(DRAG_HAT + OBJECTS + drag_hide(6, 5) + EDIT_FIELD + SAVE)
h0, h1 = c4.get('blocks.block.s0.b0'), c4.get('blocks.block.s0.b1')
check('hide snaps right under the hat', h0 and h1 and h1[0] == h0[0] and h1[1] == h0[3], (h0, h1))
check('no second stack', not [k for k in c4 if k.startswith('blocks.block.s1')])
got = read(project)
want = WANT % (HAT_AT[0], HAT_AT[1], 'Door')
check('the saved file is the expected JSON byte for byte', got == want, got)
rc, out, err = lua(project)
check('splashpack-cli blocks makes the expected Lua', rc == 0 and out == WANT_LUA % 'Entity.Find("Door")', (rc, out, err))

# Undo: the field, then the drop; redo brings the drop back; save.
project, _ = run(DRAG_HAT + OBJECTS + drag_hide(6, 5) + EDIT_FIELD + ['--key', 'ctrl+z', '--wait', '3', '--key', 'ctrl+z', '--wait', '3',
                                                                    '--key', 'ctrl+y', '--wait', '3'] + SAVE)
got = read(project)
check('Ctrl+Z takes back the typing and the drop, Ctrl+Y the drop again', got == WANT % (HAT_AT[0], HAT_AT[1], 'self'), got)
rc, out, err = lua(project)
check('... and it makes Lua for self', rc == 0 and out == WANT_LUA % 'self', (rc, out, err))

# Undo past the first edit, then save: the file is the empty script again.
project, _ = run(DRAG_HAT + ['--key', 'ctrl+z', '--wait', '3'] + SAVE)
check('undoing every edit and saving writes the empty script', read(project) == open(fixture, encoding='utf-8').read(), read(project))

# Delete: select hide (its label, not its field) and press Delete.
project, c5 = run(DRAG_HAT + OBJECTS + drag_hide(6, 5) + ['--click', '@blocks.block.s0.b1,-%d,0' % ((h1[2] - h1[0]) // 2 - 6), '--wait', '3',
                                                         '--key', 'delete', '--wait', '3'] + SAVE)
got = read(project)
check('Delete removes the selected block', 'hide' not in got and '"on_interact"' in got and 'blocks.block.s0.b1' not in c5, got)

# Negative control: hide dropped well away from the hat is a stack of its own.
project, c6 = run(DRAG_HAT + OBJECTS + drag_hide(160, 120) + SAVE)
s1 = c6.get('blocks.block.s1.b0')
check('a drop away from the hat makes a second stack', s1 is not None and 'blocks.block.s0.b1' not in c6, sorted(k for k in c6 if 'block.' in k))
rc, out, err = lua(project)
check('... which makes no code', rc == 0 and out == 'function onInteract(self)\nend\n', (rc, out, err))

print(f'{fails} failures')
sys.exit(1 if fails else 0)
