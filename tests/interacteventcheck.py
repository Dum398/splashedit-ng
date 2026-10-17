# The Interactable section's Event row, driven through the GUI on copies of
# the courtyard example. Using an Interactable makes psxsplash call
# onInteract(self) in the object's own script; the row says whether that
# function is there. A Lever with an Interactable and no Script: the editor
# prints "interact: event none", and New Lua (@newilua) adds the Script
# component with scripts/lever.lua holding onInteract ("interact: event
# defined scripts/lever.lua"); one Ctrl+Z takes the component back off. New
# blocks (@newiblocks) writes lever.blocks with an on_interact stack. Game
# Logic, given an Interactable, has a script without onInteract ("interact:
# event missing scripts/game_logic.lua"); Add onInteract (@iaddevent) puts the
# stub in the file once and the row turns to defined. A --shot of that
# section in the missing state goes to the path given as the second argument
# (default: none).
# usage: python3 interacteventcheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shot_out = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
OUTLINER = '140,750'  # empty Scene panel: takes the keyboard from the script tab
INTERACTABLE = {'type': 'interactable', 'radius': 2, 'button': 14, 'repeatable': True, 'cooldownFrames': 30, 'showPrompt': False,
                'promptCanvas': '', 'lineOfSight': False, 'facingAngle': 90}
STUB = 'function onInteract(self)'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    """The courtyard plus a Lever (Interactable, no Script) and an
    Interactable on Game Logic, whose game_logic.lua has no onInteract."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append({'name': 'Lever', 'active': True,
                         'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                         'components': [dict(INTERACTABLE)], 'children': []})
    logic = next(o for o in s['objects'] if o['name'] == 'Game Logic')
    logic['components'].append(dict(INTERACTABLE))
    json.dump(s, open(scene, 'w', encoding='utf-8', newline='\n'), indent=2)
    return project


def run(project, args):
    """The editor on `project` with `args`: its interact: lines, all it printed and the named controls as last drawn."""
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
    return [l for l in out.splitlines() if l.startswith('interact: ')], out, controls


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8')
    except OSError:
        return None


def components(project, name):
    s = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    o = next(o for o in s['objects'] if o['name'] == name)
    return o['components']


def rect(controls, name):
    a, b = controls[name].split(' ')
    return tuple(map(int, a.split(','))) + tuple(map(int, b.split(',')))


SAVE = ['--wait', '5', '--click', OUTLINER, '--key', 'ctrl+s', '--wait', '5']

# --- No Script: New Lua adds the component and a script with onInteract ---
project = project_copy()
lines, out, c = run(project, ['--select', 'Lever', '--wait', '5', '--click', '@newilua', '--wait', '10'] + SAVE)
check('a Lever without a Script says nothing happens', lines[:1] == ['interact: event none'], lines)
check('New Lua makes the event defined in scripts/lever.lua', lines[1:] == ['interact: event defined scripts/lever.lua'], lines)
lua = read(os.path.join(project, 'scripts', 'lever.lua'))
check('scripts/lever.lua has onInteract(self) and onCreate(self)',
      lua is not None and lua.count(STUB) == 1 and 'function onCreate(self)' in lua, (lua or '')[:200])
comps = components(project, 'Lever')
check('the saved Lever has a Script component naming it',
      {'type': 'script', 'lua': 'scripts/lever.lua'} in comps and any(x['type'] == 'interactable' for x in comps), comps)
check('Edit script shows once it is defined', 'iedit' in c and 'newilua' not in c, sorted(k for k in c if k.startswith('i')))

# One Ctrl+Z takes the New Lua step back: the Script component goes.
project = project_copy()
lines, out, c = run(project, ['--select', 'Lever', '--wait', '5', '--click', '@newilua', '--wait', '10', '--click', OUTLINER,
                              '--key', 'ctrl+z', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
comps = components(project, 'Lever')
check('one Ctrl+Z removes the Script component New Lua added', [x['type'] for x in comps] == ['interactable'], comps)
check('and the row goes back to none',
      lines == ['interact: event none', 'interact: event defined scripts/lever.lua', 'interact: event none'], lines)

# New blocks: an on_interact stack.
project = project_copy()
lines, out, c = run(project, ['--select', 'Lever', '--wait', '5', '--click', '@newiblocks', '--wait', '10'] + SAVE)
check('New blocks makes the event defined in scripts/lever.blocks',
      lines == ['interact: event none', 'interact: event defined scripts/lever.blocks'], lines)
try:
    doc = json.loads(read(os.path.join(project, 'scripts', 'lever.blocks')) or '')
    ops = [[b['op'] for b in st['blocks']] for st in doc['stacks']]
except ValueError as e:
    ops = str(e)
check('scripts/lever.blocks has when-created and when-interacts stacks', ops == [['on_create'], ['on_interact']], ops)

# --- A Script without onInteract: Add onInteract appends the stub once ---
project = project_copy()
logic_lua = os.path.join(project, 'scripts', 'game_logic.lua')
before = read(logic_lua)
lines, out, c = run(project, ['--select', 'Game Logic', '--wait', '10', '--click', '@iaddevent', '--wait', '10'] + SAVE)
check('Game Logic says its script lacks onInteract', lines[:1] == ['interact: event missing scripts/game_logic.lua'], lines)
check('Add onInteract makes it defined', lines[1:] == ['interact: event defined scripts/game_logic.lua'], lines)
after = read(logic_lua) or ''
check('game_logic.lua on disk now has the stub exactly once', after.count(STUB) == 1, after[-200:])
check('and the rest of the file is kept, the stub after it', before is not None and after.startswith(before.rstrip('\n')),
      after[:120])

# A second Add onInteract cannot happen: the button is gone once it is defined.
check('Add onInteract shows no more once it is defined', 'iaddevent' not in c and 'iedit' in c, sorted(k for k in c if k.startswith('i')))

if shot_out:
    # The section in the missing state, cropped from a --shot.
    project = project_copy()
    src = os.path.join(project, 'missing.png')
    lines, out, c = run(project, ['--select', 'Game Logic', '--wait', '10', '--shot', src])
    check('the shot run is in the missing state', lines == ['interact: event missing scripts/game_logic.lua'], lines)
    try:
        from PIL import Image
        img = Image.open(src)
        x0, y0, _, _ = rect(c, 's_interact')
        _, _, x1, y1 = rect(c, 'obj.ievent')
        img.crop((x0 - 4, y0 - 4, max(x1, x0 + 340) + 8, min(img.height, y1 + 80))).save(shot_out)
    except Exception as e:  # no PIL, or the section was not drawn: the whole window
        print('note: whole screenshot kept (' + str(e) + ')')
        shutil.copy(src, shot_out)
    check('the missing-state screenshot was written', os.path.exists(shot_out), shot_out)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
