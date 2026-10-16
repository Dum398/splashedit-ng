# New blocks and New Lua, driven through the GUI on copies of the courtyard
# example. In the scene inspector, on Game Logic's Script component (with no
# file set; game_logic.lua is there, so the new stem is numbered) and on a
# Trigger component, New blocks writes scripts/<name>.blocks with the starter's
# hats, the saved scene names it, and it opens in a blocks tab; New Lua still
# writes a .lua. The asset browser's background menu writes
# new_script.blocks. Then block help: a palette block under the pointer, and
# a selected canvas block, put their op in the status line (the editor prints
# "blocks: help <op>" when that changes).
# usage: python3 newscriptcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
OUTLINER = '140,750'  # empty Scene panel: takes the keyboard from the script tab
ASSETS_EMPTY = '1150,880'  # empty space in the Project panel's file grid at 1600x960


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy(logic_lua=None, trigger=False):
    """The courtyard; Game Logic's Script file set to `logic_lua` when given,
    and a Trigger component with no script added with `trigger`."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    data = json.load(open(scene, encoding='utf-8'))
    logic = next(o for o in data['objects'] if o['name'] == 'Game Logic')
    assert logic['components'] == [{'type': 'script', 'lua': 'scripts/game_logic.lua'}], logic
    if logic_lua is not None:
        logic['components'][0]['lua'] = logic_lua
    if trigger:
        logic['components'].append({'type': 'trigger', 'size': [1, 1, 1], 'lua': None})
    json.dump(data, open(scene, 'w', encoding='utf-8', newline='\n'), indent=2)
    return project


def run(project, args):
    """The editor on `project` with `args`: what it printed and the named controls as last drawn."""
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '10'] + args + [
        '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    controls = {}
    for l in out.splitlines():
        if l.startswith('control '):
            name, rest = l[len('control '):].rsplit(' at ', 1)
            controls[name] = rest
    missing = [l for l in out.splitlines() if 'no such control drawn' in l]
    check('every control the run names was drawn', not missing, missing)
    return out, controls


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8')
    except OSError:
        return None


def hats(path):
    text = read(path)
    if text is None:
        return None
    try:
        doc = json.loads(text)
    except ValueError:
        return 'not JSON: ' + text[:60]
    return [[b['op'] for b in s['blocks']] for s in doc['stacks']]


def scene(project):
    return json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))


def logic(project):
    return next(o for o in scene(project)['objects'] if o['name'] == 'Game Logic')


def comp(project, kind):
    return next((c for c in logic(project)['components'] if c['type'] == kind), None)


SAVE = ['--wait', '5', '--click', OUTLINER, '--key', 'ctrl+s', '--wait', '5']

# --- The scene inspector ---
project = project_copy()
out, c = run(project, ['--click', '@newsceneblocks'] + SAVE)
h = hats(os.path.join(project, 'scripts', 'courtyard.blocks'))
check('scene New blocks writes scripts/courtyard.blocks with a when-the-scene-starts stack', h == [['on_scene_start']], h)
check('scene New blocks writes no .lua', not os.path.exists(os.path.join(project, 'scripts', 'courtyard.lua')))
check('the saved scene names the scene script', scene(project)['settings']['script'] == 'scripts/courtyard.blocks',
      scene(project)['settings']['script'])
check('scene New blocks opens a blocks tab', 'blocks.canvas' in c, out[-300:])

# --- Game Logic's Script component, no file set ---
project = project_copy('')
out, c = run(project, ['--select', 'Game Logic', '--click', '@newblocks'] + SAVE)
h = hats(os.path.join(project, 'scripts', 'game_logic_2.blocks'))
check('object New blocks writes scripts/game_logic_2.blocks (game_logic.lua takes the stem) with a when-created stack',
      h == [['on_create']], h)
check('object New blocks leaves no game_logic.blocks', not os.path.exists(os.path.join(project, 'scripts', 'game_logic.blocks')))
check('the saved Script component names it', comp(project, 'script')['lua'] == 'scripts/game_logic_2.blocks', comp(project, 'script'))
check('object New blocks opens a blocks tab', 'blocks.canvas' in c, out[-300:])

project = project_copy('')
out, c = run(project, ['--select', 'Game Logic', '--click', '@newlua'] + SAVE)
lua = read(os.path.join(project, 'scripts', 'game_logic_2.lua'))
check('object New Lua still writes scripts/game_logic_2.lua', lua is not None and 'function onCreate(self)' in lua, (lua or '')[:60])
check('object New Lua writes no .blocks', not os.path.exists(os.path.join(project, 'scripts', 'game_logic_2.blocks')))
check('the saved Script component names the .lua', comp(project, 'script')['lua'] == 'scripts/game_logic_2.lua', comp(project, 'script'))
check('object New Lua opens no blocks tab', 'blocks.canvas' not in c)

# --- A Trigger component ---
project = project_copy(trigger=True)
out, c = run(project, ['--select', 'Game Logic', '--click', '@newtblocks'] + SAVE)
h = hats(os.path.join(project, 'scripts', 'game_logic_trigger.blocks'))
check('Trigger New blocks writes scripts/game_logic_trigger.blocks with enter and leave stacks',
      h == [['on_trigger_enter'], ['on_trigger_exit']], h)
check('the saved Trigger component names it', (comp(project, 'trigger') or {}).get('lua') == 'scripts/game_logic_trigger.blocks',
      comp(project, 'trigger'))
check('Trigger New blocks opens a blocks tab', 'blocks.canvas' in c, out[-300:])
check('the Script component keeps its own file', comp(project, 'script')['lua'] == 'scripts/game_logic.lua', comp(project, 'script'))

# --- The asset browser's background menu ---
project = project_copy()
out, c = run(project, ['--click', ASSETS_EMPTY + ',1', '--wait', '5', '--click', '@assets.menu/New blocks script', '--wait', '5'])
h = hats(os.path.join(project, 'new_script.blocks'))
check('asset browser New blocks script writes new_script.blocks with a when-created stack', h == [['on_create']], h)
project = project_copy()
out, c = run(project, ['--click', ASSETS_EMPTY + ',1', '--wait', '5', '--click', '@assets.menu/New Lua script', '--wait', '5'])
lua = read(os.path.join(project, 'new_script.lua'))
check('asset browser New Lua script writes new_script.lua', lua is not None and 'function onCreate(self)' in lua, (lua or '')[:60])


# --- Block help in the status line ---
def helps(out):
    return [l.split('blocks: help ', 1)[1] for l in out.splitlines() if l.startswith('blocks: help ')]


NEW_TAB = ['--select', 'Game Logic', '--click', '@newblocks', '--wait', '10']
project = project_copy('')
out, c = run(project, NEW_TAB + ['--click', '@blocks.category.Objects', '--wait', '5', '--move', '@blocks.palette.hide', '--wait', '40',
                                 '--shot', os.path.join(project, 'help.png')])
check('a palette block under the pointer shows its help', helps(out)[-1:] == ['hide'], helps(out))
canvas = c.get('blocks.canvas')
if canvas:
    a, b = canvas.split(' ')
    x0, y0 = map(int, a.split(','))
    x1, y1 = map(int, b.split(','))
    EMPTY = '%d,%d' % (x1 - 40, y1 - 40)  # bottom right of the canvas: no block there
    project = project_copy('')
    out, c = run(project, NEW_TAB + ['--move', '@blocks.block.s0.b0', '--wait', '5', '--move', EMPTY, '--wait', '5'])
    check('a canvas block under the pointer shows its help, and nothing when the pointer leaves', helps(out) == ['on_create', 'none'],
          helps(out))
    project = project_copy('')
    out, c = run(project, NEW_TAB + ['--click', '@blocks.block.s0.b0', '--wait', '5', '--move', EMPTY, '--wait', '5'])
    check('a selected canvas block keeps its help shown', helps(out)[-1:] == ['on_create'], helps(out))
else:
    check('the blocks canvas is drawn', False, sorted(c)[:10])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
