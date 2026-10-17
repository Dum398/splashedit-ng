# Light, Persist and name blocks in the block editor, driven through the GUI
# on copies of the courtyard example whose Game Logic runs scripts/door.blocks
# (the empty fixture blocks/editor/empty.blocks), with three lights added: a
# runtime point light "Beacon Light", a baked one "Fill Light" and a runtime
# one on an inactive object "Off Beacon". "turn light ... on" goes from the
# palette's Objects section under a "when the player interacts" hat; its
# field's list must offer only Beacon Light (what the export ships and
# Light.Find can reach), which is picked; "remember ... as ..." from Variables
# goes under it, its name typed. The Lua panel's scripted line must count the
# Lua the saved file makes through `splashpack-cli blocks`, which must be the
# expected Lua. A light name the scene has no runtime light for stays in the
# list (marked) rather than being dropped. Last, blocks/beacon.blocks opened
# in the tab with --shot <png> saves a picture of every new block, the Lua
# panel and Beacon Light's inspector. The Light section's Runtime toggle
# (obj.lruntime) adds a light to the picker and Ctrl+Z takes it out; with no
# runtime lights the picker says to turn Runtime on; over 16 runtime lights
# the inspector warns (obj.lruntimemax) and its fix turns this light's off.
# usage: python3 blockslightcheck.py <splashedit> [<splashpack-cli>] [--shot <png>]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

args = sys.argv[1:]
shot = None
if '--shot' in args:
    i = args.index('--shot')
    shot = os.path.abspath(args[i + 1])
    del args[i:i + 2]
editor = os.path.abspath(args[0])
cli = os.path.abspath(args[1]) if len(args) > 1 else os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fixture = os.path.join(here, 'blocks', 'editor', 'empty.blocks')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" button at 1600x960 (as in blockseditorcheck.py)
OPEN = ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '10']
HAT_AT = (40, 40)  # where the hat is dropped, in canvas coordinates


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def light(name, runtime, active=True, x=0.0):
    comp = {'type': 'light', 'kind': 'point', 'color': [1.0, 0.8, 0.2], 'intensity': 2.0, 'range': 3.0}
    if runtime:
        comp['runtime'] = True
    return {'name': name, 'active': active,
            'transform': {'position': [x, 1.0, 2.0], 'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0]},
            'components': [comp], 'children': []}


def project_copy(blocks=None, lights=None):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    logic = [o for o in s['objects'] if o['name'] == 'Game Logic']
    assert len(logic) == 1 and logic[0]['components'] == [{'type': 'script', 'lua': 'scripts/game_logic.lua'}], logic
    logic[0]['components'][0]['lua'] = 'scripts/door.blocks'
    s['objects'] += lights if lights is not None else [light('Beacon Light', True), light('Fill Light', False, x=2.0),
                                                       light('Off Beacon', True, False, x=-2.0)]
    json.dump(s, open(scene, 'w', encoding='utf-8', newline='\n'), indent=2)
    dest = os.path.join(project, 'scripts', 'door.blocks')
    if blocks is None:
        shutil.copy(fixture, dest)
    else:
        open(dest, 'w', encoding='utf-8', newline='\n').write(blocks)
    return project


out = ''  # the last run's output


def run(args, blocks=None, png=None, lights=None):
    """The editor on a fresh project copy, then `args`: the project and the
    named controls as last drawn; `out` gets what it printed."""
    global out
    project = project_copy(blocks, lights)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + OPEN + args + [
        '--wait', '10', '--screenshot', png or os.path.join(project, 'shot.png')]
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


def picks(c):
    return sorted(k[len('blocks.pick/'):] for k in c if k.startswith('blocks.pick/'))


def lua(project):
    r = subprocess.run([cli, 'blocks', os.path.join(project, 'scripts', 'door.blocks')], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr.strip()


SAVE = ['--key', 'ctrl+s', '--wait', '5']

# The hat, then the light block under it.
_, c = run([])
canvas, hatPal = need(c, 'blocks.canvas', 'blocks.palette.on_interact')
origin = (canvas[0] + 24, canvas[1] + 24)
hatScreen = (origin[0] + HAT_AT[0], origin[1] + HAT_AT[1])
steps = drop('blocks.palette.on_interact', hatPal, hatScreen[0], hatScreen[1]) + chip('Objects')
_, c = run(steps)
hat, onPal, offPal, namePal = need(c, 'blocks.block.s0.b0', 'blocks.palette.light_on', 'blocks.palette.light_off',
                                   'blocks.palette.my_name')
check('the Objects section offers turn light on, turn light off and my name', True)
steps += drop('blocks.palette.light_on', onPal, hat[0], hat[3])
_, c = run(steps + ['--click', '@blocks.field.s0.b1', '--wait', '5'])
check("the light field's list offers the active runtime light only", picks(c) == ['Beacon Light'], picks(c))
steps += ['--click', '@blocks.field.s0.b1', '--wait', '5', '--click', '@blocks.pick/Beacon Light', '--wait', '3'] + chip('Variables')
_, c = run(steps)
onBlk, remPal, gotPal = need(c, 'blocks.block.s0.b1', 'blocks.palette.remember', 'blocks.palette.remembered')
check('the Variables section offers remember and remembered', True)
steps += drop('blocks.palette.remember', remPal, onBlk[0], onBlk[3]) + [
    '--click', '@blocks.field.s0.b2', '--wait', '5', '--text', 'beacon_lit', '--key', 'enter', '--wait', '5']
project, c = run(steps + SAVE)
saved = json.load(open(os.path.join(project, 'scripts', 'door.blocks'), encoding='utf-8'))
WANT = [{'op': 'on_interact'}, {'op': 'light_on', 'field': 'Beacon Light'},
        {'op': 'remember', 'field': 'beacon_lit', 'args': [{'op': 'number', 'field': '0'}]}]
check('the light picked and the name typed are saved', saved['stacks'] and saved['stacks'][0]['blocks'] == WANT, json.dumps(saved))
rc, lout, err = lua(project)
WANT_LUA = '''function onInteract(self)
    Light.SetEnabled(Light.Find("Beacon Light"), true)
    Persist.Set("beacon_lit", 0)
end
'''
check('splashpack-cli blocks makes the expected Lua', rc == 0 and lout == WANT_LUA, (rc, lout, err))
luaLines = [l for l in out.splitlines() if l.startswith('blocks: lua')]
check('the Lua panel counts that Lua, with no errors', luaLines and luaLines[-1] == 'blocks: lua %d lines, 0 errors' % lout.count('\n'),
      luaLines[-3:])

# Before the name is typed the Lua panel reports the missing name.
_, c = run(steps[:-10])
check('remember without a name is an error in the Lua panel',
      'blocks: lua error stack 1, block 3 (remember): no name to remember it by' in out,
      [l for l in out.splitlines() if l.startswith('blocks: lua')])

# A light the scene has no runtime light for: kept in the list, still makes its Lua.
STRAY = json.dumps({'format': 'splashedit-blocks', 'version': 1, 'variables': [], 'stacks': [
    {'x': HAT_AT[0], 'y': HAT_AT[1], 'blocks': [{'op': 'on_interact'}, {'op': 'light_off', 'field': 'Fill Light'}]}]})
_, c = run(['--click', '@blocks.field.s0.b1', '--wait', '5'], blocks=STRAY)
check("a light that is not a runtime light stays in the field's list", picks(c) == ['Beacon Light', 'Fill Light'], picks(c))
check('... and makes Lua all the same (Light.Find gives nil, SetEnabled ignores it)',
      'blocks: lua 3 lines, 0 errors' in out, [l for l in out.splitlines() if l.startswith('blocks: lua')])

# The Light section's Runtime toggle: on, the light joins the picker; Ctrl+Z takes it back out.
ONE = json.dumps({'format': 'splashedit-blocks', 'version': 1, 'variables': [], 'stacks': [
    {'x': HAT_AT[0], 'y': HAT_AT[1], 'blocks': [{'op': 'on_interact'}, {'op': 'light_on'}]}]})
PICK = ['--click', '@blocks.field.s0.b1', '--wait', '5']
# --select is applied before the run (it would aim Edit script elsewhere): the tree's rows pick instead.
def tree(name):
    return ['--click', '@tree.' + name, '--wait', '5']


RUNTIME_ON = tree('Fill Light') + ['--click', '@obj.lruntime', '--wait', '5']
project, c = run(RUNTIME_ON, blocks=ONE)
check('the Light section of a point light has a Runtime toggle', 'obj.lruntime' in c, sorted(k for k in c if k.startswith('obj.l')))
check('... and no over-the-limit warning with two runtime lights', 'obj.lruntimemax' not in c and 'light: ' not in out)
_, c = run(RUNTIME_ON + tree('Game Logic') + PICK, blocks=ONE)
check('turning Runtime on puts the light in the picker', picks(c) == ['Beacon Light', 'Fill Light'], picks(c))
_, c = run(RUNTIME_ON + ['--key', 'ctrl+z', '--wait', '5'] + tree('Game Logic') + PICK, blocks=ONE)
check('Ctrl+Z takes it out again', picks(c) == ['Beacon Light'], picks(c))
project, _ = run(RUNTIME_ON + ['--key', 'ctrl+s', '--wait', '5'], blocks=ONE)
fill = [o for o in json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))['objects'] if o['name'] == 'Fill Light']
check('the toggle is saved in the light component', fill and fill[0]['components'][0].get('runtime') is True,
      fill and fill[0]['components'])

# No runtime lights: the picker says how to make one.
_, c = run(PICK, blocks=ONE, lights=[light('Fill Light', False)])
check('with no runtime lights the picker says to turn Runtime on', picks(c) == ['No runtime lights'], picks(c))
_, c = run(PICK + ['--click', '@blocks.pick/No runtime lights', '--wait', '3', '--key', 'ctrl+s', '--wait', '5'],
           blocks=ONE, lights=[light('Fill Light', False)])
check('... and that row cannot be picked', 'blocks: lua error stack 1, block 2 (light_on): no light chosen' in out,
      [l for l in out.splitlines() if l.startswith('blocks: lua')])

# More than 16 runtime lights: a warning on each, with a fix that turns this one's off.
MANY = [light('Lamp %02d' % i, True, x=float(i)) for i in range(18)]
_, c = run(tree('Lamp 03'), blocks=ONE, lights=MANY)
warned = [l for l in out.splitlines() if l.startswith('light: ')]
check('18 runtime lights warn, naming the two left out',
      'obj.lruntimemax' in c and warned and warned[-1] == 'light: The scene has 18 runtime point lights and the PS1 holds 16; '
      'left out: Lamp 16, Lamp 17. Turn Runtime off on lights that never change while the game runs.', warned)
project, c = run(tree('Lamp 03') + ['--click', '@obj.lruntimemax.fix', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'],
                 blocks=ONE, lights=MANY)
lamp = [o for o in json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))['objects'] if o['name'] == 'Lamp 03']
check("the warning's fix turns this light's Runtime off", lamp and not lamp[0]['components'][0].get('runtime'), lamp)
check('... and the warning leaves this light', 'obj.lruntime' in c and 'obj.lruntimemax' not in c,
      sorted(k for k in c if k.startswith('obj.lrun')))

# The picture: every new block, and the Lua panel.
if shot:
    beacon = open(os.path.join(here, 'blocks', 'beacon.blocks'), encoding='utf-8').read()
    _, c = run(['--click', '@blocks.category.Objects', '--wait', '5'] + tree('Beacon Light'), blocks=beacon, png=shot)
    check('the beacon script opens with its blocks', 'blocks.block.s2.b2' in c and 'blocks: lua 22 lines, 0 errors' in out,
          [l for l in out.splitlines() if l.startswith('blocks: lua')])
    check('the picture is saved', os.path.exists(shot), shot)

print(f'{fails} failures' if fails else 'all passed')
sys.exit(1 if fails else 0)
