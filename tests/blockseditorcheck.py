# The block editor (a .blocks script tab), driven through the GUI on copies
# of the courtyard example whose Game Logic runs scripts/door.blocks, an
# empty block script (blocks/editor/empty.blocks), with an object Door (its
# audio clip door_creak) and a cutscene "intro" added. Edit script opens it in a
# blocks tab; "on_interact" is dragged from the palette onto the canvas,
# "hide" from the palette's Objects section under it, where it must snap
# (its control sits right under the hat), "Door" searched for in its field's
# object list and the
# tab saved with Ctrl+S: the file must then read exactly as the expected
# JSON and `splashpack-cli blocks` must turn it into the expected Lua. Then
# Ctrl+Z / Ctrl+Y (one step per edit), Delete on a selected block, and a
# drop away from the hat (a second, loose stack: the negative control for
# the snap). Palette blocks, the canvas and the blocks on it are aimed at by
# name (blocks.palette.<op>, blocks.canvas, blocks.block.<path>,
# blocks.field.<path>; see src/editor/blocks_view.hh); a first run reads
# where they are drawn. Then the variables column: the door example built
# with no typing but a variable's name (its fields picked from the scene's
# lists), its Lua and the Lua panel's error count; a refused variable rename;
# and a cutscene renamed in the inspector while the tab has unsaved changes
# (ScriptTabs::rewriteUnsavedBlocks), the tab then saved.
# usage: python3 blockseditorcheck.py <splashedit> [<splashpack-cli>]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

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


def project_copy(blocks=None):
    """The courtyard with Game Logic running scripts/door.blocks (`blocks`, else
    the empty fixture), an object Door whose audio clip is door_creak, and a
    cutscene "intro"."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    logic = [o for o in s['objects'] if o['name'] == 'Game Logic']
    assert len(logic) == 1 and logic[0]['components'] == [{'type': 'script', 'lua': 'scripts/game_logic.lua'}], logic
    logic[0]['components'][0]['lua'] = 'scripts/door.blocks'
    assert not [o for o in s['objects'] if o['name'] == 'Door']
    s['objects'].append({'name': 'Door', 'active': True,
                         'transform': {'position': [2.0, 0.0, 3.0], 'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0]},
                         'components': [{'type': 'audio', 'clip': None, 'clipName': 'door_creak'}], 'children': []})
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 90, 'audioEvents': [], 'tracks': []}]
    json.dump(s, open(scene, 'w', encoding='utf-8', newline='\n'), indent=2)
    dest = os.path.join(project, 'scripts', 'door.blocks')
    if blocks is None:
        shutil.copy(fixture, dest)
    else:
        open(dest, 'w', encoding='utf-8', newline='\n').write(blocks)
    return project


out = ''  # the last run's output


def run(args, blocks=None):
    """The editor on a fresh project copy (door.blocks: `blocks`, else the
    empty fixture), then `args`: the project and the named controls as last
    drawn; `out` gets what it printed."""
    global out
    project = project_copy(blocks)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + OPEN + args + [
        '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    lines = out.splitlines()
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

# --- Variables and the field lists: the door, built with no typing but the
# variable's name. Make a variable "opened", True-false (false); then from the
# palette: the hat, an if under it, a not in its slot, the palette's ready
# "opened" reporter in the not, a set (its variable already opened) in the
# if, a bool (true) in its slot, hide and play sound under it with their
# fields picked from the scene's lists (Door, its clip door_creak). Each run
# replays the steps so far and reads where the next ones go.
MAKE_VAR = ['--click', '@blocks.vars.add', '--wait', '5', '--text', 'opened', '--key', 'enter', '--wait', '3',
            '--click', '@blocks.var.0.kind', '--wait', '3', '--click', '@blocks.var.0.kind/True-false', '--wait', '3']


def chip(c):
    return ['--click', '@blocks.category.' + c, '--wait', '5']


def drop(name, rect, x, y):
    """Drag control `name` (drawn at `rect`) so its top left lands on x, y."""
    return ['--drag', '@%s,0,0,%d,%d' % (name, x - rect[0], y - rect[1]), '--wait', '5']


def need(c, *names):
    missing = [n for n in names if n not in c]
    if missing:
        check('drawn: ' + ', '.join(missing), False, sorted(k for k in c if k.startswith('blocks.'))[:60])
        print(f'{fails} failures')
        sys.exit(1)
    return [c[n] for n in names]


steps = MAKE_VAR + DRAG_HAT + chip('Control')
_, c = run(steps)
hat, ifPal, catPal = need(c, 'blocks.block.s0.b0', 'blocks.palette.if', 'blocks.category.Events')
# not is the last of the Operators: the wheel over the palette brings it up.
steps += drop('blocks.palette.if', ifPal, hat[0], hat[3]) + chip('Operators') + [
    '--wheel', '%d,%d,-4' % (catPal[0] + 40, catPal[3] + 200), '--wait', '3']
_, c = run(steps)
ifSlot, notPal = need(c, 'blocks.slot.s0.b1.0', 'blocks.palette.not')
steps += drop('blocks.palette.not', notPal, ifSlot[0], ifSlot[1]) + chip('Variables')
_, c = run(steps)
notSlot, varPal, setPal = need(c, 'blocks.slot.s0.b1.a0.0', 'blocks.palette.var.opened', 'blocks.palette.set_var')
check('Make a variable adds one, with its own reporter in the palette', 'blocks.var.0.name' in c and 'blocks.var.1.name' not in c,
      sorted(k for k in c if k.startswith('blocks.var')))
steps += drop('blocks.palette.var.opened', varPal, notSlot[0], notSlot[1])
_, c = run(steps)
iff, ifSlot = need(c, 'blocks.block.s0.b1', 'blocks.slot.s0.b1.0')
# The if's mouth: under its row (6 px under the slot), 16 px in.
steps += drop('blocks.palette.set_var', setPal, iff[0] + 16, ifSlot[3] + 6) + chip('Operators')
_, c = run(steps)
setSlot, boolPal = need(c, 'blocks.slot.s0.b1.d0.b0.0', 'blocks.palette.bool')
steps += drop('blocks.palette.bool', boolPal, setSlot[0], setSlot[1]) + chip('Objects')
_, c = run(steps)
setBlk, hidePal2 = need(c, 'blocks.block.s0.b1.d0.b0', 'blocks.palette.hide')
# The Object list shows ten at a time, self first then the scene in tree
# order: Door, added last, is down the list.
steps += drop('blocks.palette.hide', hidePal2, setBlk[0], setBlk[3]) + [
    '--click', '@blocks.field.s0.b1.d0.b1', '--wait', '5', '--wheel', '%d,%d,-10' % (setBlk[0] + 60, setBlk[3] + 150), '--wait', '3',
    '--click', '@blocks.pick/Door', '--wait', '3'] + chip('Sound and scenes')
_, c = run(steps)
hideBlk, soundPal = need(c, 'blocks.block.s0.b1.d0.b1', 'blocks.palette.play_sound')
steps += drop('blocks.palette.play_sound', soundPal, hideBlk[0], hideBlk[3]) + [
    '--click', '@blocks.field.s0.b1.d0.b2', '--wait', '5', '--click', '@blocks.pick/door_creak', '--wait', '3']
# The delete button is off while blocks use the variable.
project, c = run(steps + ['--click', '@blocks.var.0.delete', '--wait', '3'] + SAVE)
saved = json.loads(read(project))
WANT_DOOR = {'format': 'splashedit-blocks', 'version': 1, 'variables': [{'name': 'opened', 'value': 'false'}],
             'stacks': [{'x': HAT_AT[0], 'y': HAT_AT[1], 'blocks': [
                 {'op': 'on_interact'},
                 {'op': 'if', 'args': [{'op': 'not', 'args': [{'op': 'var', 'field': 'opened'}]}], 'bodies': [[
                     {'op': 'set_var', 'field': 'opened', 'args': [{'op': 'bool', 'field': 'true'}]},
                     {'op': 'hide', 'field': 'Door'},
                     {'op': 'play_sound', 'field': 'door_creak'}]]}]}]}
check('the door script, built by dragging and picking, is saved', saved == WANT_DOOR, json.dumps(saved))
rc, lout, err = lua(project)
WANT_DOOR_LUA = '''local opened = false

function onInteract(self)
    if not opened then
        opened = true
        Entity.SetActive(Entity.Find("Door"), false)
        Audio.Play("door_creak")
    end
end
'''
check("... and makes the door's Lua", rc == 0 and lout == WANT_DOOR_LUA, (rc, lout, err))
luaLines = [l for l in out.splitlines() if l.startswith('blocks: lua ')]
check('the Lua panel shows 9 lines and no errors', luaLines and luaLines[-1] == 'blocks: lua 9 lines, 0 errors', luaLines[-3:])
# A field list offers the scene: self, then the scene's objects.
_, c = run(steps + ['--click', '@blocks.field.s0.b1.d0.b1', '--wait', '5'])
check("the Object list offers self and the scene's objects", 'blocks.pick/self' in c and 'blocks.pick/Crate' in c and
      'blocks.pick/Back Wall' in c, sorted(k for k in c if k.startswith('blocks.pick')))

# A name that is not one is refused: the variable keeps its name, the reason prints.
RENAME_VAR = ['--click', '@blocks.var.0.name', '--wait', '3', '--text']
project, _ = run(MAKE_VAR + RENAME_VAR + ['1bad', '--key', 'enter', '--wait', '3'] + SAVE)
saved = json.loads(read(project))
check('renaming a variable to 1bad is refused', saved['variables'] == [{'name': 'opened', 'value': 'false'}], saved['variables'])
check('... saying why', 'blocks: rename refused: not a name' in out, [l for l in out.splitlines() if 'refused' in l])
# The negative control: a good name is taken.
project, _ = run(MAKE_VAR + RENAME_VAR + ['door_open', '--key', 'enter', '--wait', '3'] + SAVE)
saved = json.loads(read(project))
check('renaming it to door_open is taken', saved['variables'] == [{'name': 'door_open', 'value': 'false'}], saved['variables'])

# --- A cutscene rename reaches a block tab with unsaved changes: the stack
# moved (not saved), the cutscene renamed in the scene inspector, then the
# tab saved: the file plays "opening" from where the stack was moved to.
PLAYS = json.dumps({'format': 'splashedit-blocks', 'version': 1, 'variables': [], 'stacks': [
    {'x': 40, 'y': 40, 'blocks': [{'op': 'on_interact'}, {'op': 'play_cutscene', 'field': 'intro'}]}]})
SCENE_ROOT = '90,127'        # the scene's row at the top of the tree: the inspector shows the scene
INSPECTOR_NAME = '1301,301'  # its first cutscene's name (as in timelineeditcheck.py)
RENAME_CUT = ['--click', SCENE_ROOT, '--wait', '5', '--dblclick', INSPECTOR_NAME, '--wait', '3', '--key', 'ctrl+a', '--text', 'opening',
              '--key', 'enter', '--wait', '5']
FOCUS_TAB = ['--click', '@blocks.canvas,0,150', '--wait', '3']
project, _ = run(['--drag', '@blocks.block.s0.b0,0,0,30,20', '--wait', '5'] + RENAME_CUT + FOCUS_TAB + SAVE, blocks=PLAYS)
saved = json.loads(read(project))
st = saved['stacks'][0]
check('the saved tab plays the renamed cutscene', st['blocks'][1] == {'op': 'play_cutscene', 'field': 'opening'}, st['blocks'])
check('... and keeps its unsaved edit (the stack moved)', (st['x'], st['y']) == (70, 60), (st['x'], st['y']))

print(f'{fails} failures')
sys.exit(1 if fails else 0)
