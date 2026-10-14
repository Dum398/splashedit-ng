# Script editor and Play: a build reads scripts from disk, so F5 (Play) and
# Ctrl+F5 (Run on hardware) pressed while the script editor has the keyboard
# must write its unsaved tabs first. Then New script: in the scene inspector
# and on an object's empty Script component it writes scripts/<name>.lua,
# sets it (one undo step) and opens it; on a Trigger component it writes
# scripts/<name>_trigger.lua. Then the editor itself: Replace all (Ctrl+H) and
# its single undo step, brackets and quotes closing as they are typed, and a
# script changed on disk by another program: reloaded when the tab has no
# edits, else the Reload / Keep mine bar; one deleted on disk: "(deleted)" on
# its tab and a Save / Close bar. Drives the editor headless on copies of the
# courtyard example.
# usage: python3 scripteditorcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
MARK = 'zz_typed_marker = 1 '
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" / "New script" button at 1600x960
NEW_SCENE_SCRIPT = '1428,221'  # the scene inspector's "New script"
OUTLINER = '140,750'  # empty Scene panel: takes the keyboard from the script editor
CODE = '700,500'  # the script editor's text, below a short script
REPLACE_ALL = '1132,132'  # the Replace row's "Replace all"
MATCH_CASE = '1124,103'  # the find row's "Aa"
RELOAD, KEEP_MINE = '1105,100', '1186,100'  # the changed-on-disk bar
SAVE_DELETED, CLOSE_DELETED = '1138,100', '1200,100'  # the deleted-on-disk bar
BAR = (52, 45, 39)  # either bar's background
NEW_TRIGGER_SCRIPT = '1412,573'  # Game Logic's Trigger box "New script", with a trigger added
CHANGED = '-- changed elsewhere\nfunction onCreate(self)\n  print("hi")\nend\n'
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    return project


def editor_run(project, args):
    cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '30', '--screenshot',
                                                                         os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    return (r.stdout + r.stderr).strip()


def readpng(path):
    """(width, rows of bytes, bytes per pixel) of an 8-bit RGB or RGBA PNG."""
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR': w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        if kind == b'IDAT': idat += body
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw, rows, prev = zlib.decompress(idat), [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (w * bpp + 1)], bytearray(raw[y * (w * bpp + 1) + 1:(y + 1) * (w * bpp + 1)])
        for i in range(len(line)):
            a, b = line[i - bpp] if i >= bpp else 0, prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, rows, bpp


def read(path):
    return open(path, encoding='utf-8').read() if os.path.exists(path) else None


def run(keys):
    """Opens game_logic.lua in the script editor, types MARK, presses `keys`;
    returns the file's text afterwards and the editor's output."""
    args = ['--text', MARK]
    for k in keys:
        args += ['--key', k]
    return run_args(args)


def run_args(args):
    """Opens game_logic.lua in the script editor, then the scripted `args`."""
    project = project_copy()
    out = editor_run(project, ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '5'] + args)
    return read(os.path.join(project, 'scripts', 'game_logic.lua')), out


def scene_script(project):
    return json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))['settings']['script']


original = read(os.path.join(example, 'scripts', 'game_logic.lua'))

# Without a key nothing saves, so a saved file below is the key's doing.
lua, out = run([])
check('typing alone leaves the file on disk as it was', lua == original, (lua or '')[:80] + ' | ' + out[-200:])

lua, out = run(['f5'])
check('F5 in the script editor saves the typed text before Play', (lua or '').startswith(MARK),
      repr((lua or '')[:60]) + ' | ' + out[-200:])

lua, out = run(['ctrl+f5'])
check('Ctrl+F5 in the script editor saves the typed text before Run on hardware', (lua or '').startswith(MARK),
      repr((lua or '')[:60]) + ' | ' + out[-200:])

# New script for the scene: written, set and opened; the saved scene names it.
project = project_copy()
out = editor_run(project, ['--click', NEW_SCENE_SCRIPT, '--wait', '5', '--click', OUTLINER, '--key', 'ctrl+s', '--wait', '5'])
lua = read(os.path.join(project, 'scripts', 'courtyard.lua'))
check('scene New script writes scripts/courtyard.lua', lua is not None and 'function onSceneCreationStart()' in lua, out[-200:])
check('scene New script sets the scene script', scene_script(project) == 'scripts/courtyard.lua', scene_script(project))

# Undo takes the setting back (the file stays).
project = project_copy()
out = editor_run(project, ['--click', NEW_SCENE_SCRIPT, '--wait', '5', '--click', OUTLINER, '--key', 'ctrl+z', '--key', 'ctrl+s',
                           '--wait', '5'])
check('undo after scene New script clears the scene script', not scene_script(project),
      str(scene_script(project)) + ' | ' + out[-200:])

# New script for an object whose Script component has no file: game_logic.lua
# is taken, so the new file is numbered, and typing goes into it.
project = project_copy()
scene = os.path.join(project, 'courtyard.scene')
text = read(scene)
assert '"lua": "scripts/game_logic.lua"' in text
open(scene, 'w', encoding='utf-8').write(text.replace('"lua": "scripts/game_logic.lua"', '"lua": ""'))
out = editor_run(project, ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '5', '--text', MARK, '--key', 'ctrl+s',
                           '--click', OUTLINER, '--key', 'ctrl+s', '--wait', '5'])
lua = read(os.path.join(project, 'scripts', 'game_logic_2.lua'))
check('object New script writes scripts/game_logic_2.lua and opens it',
      lua is not None and lua.startswith(MARK) and 'function onCreate(self)' in lua, repr((lua or '')[:60]) + ' | ' + out[-200:])
check('game_logic.lua is left alone', read(os.path.join(project, 'scripts', 'game_logic.lua')) == original)
check('object New script sets the Script component', '"lua": "scripts/game_logic_2.lua"' in read(scene), out[-200:])

# --- Find and replace ---
TYPED = 'zzq = zzq + zzq '
FIND = ['--text', TYPED, '--key', 'ctrl+h', '--text', 'zzq', '--key', 'tab', '--text', 'yy']

lua, out = run_args(FIND + ['--key', 'ctrl+alt+enter', '--click', CODE, '--key', 'ctrl+s'])
check('Ctrl+Alt+Enter in Replace replaces every match', (lua or '').startswith('yy = yy + yy ') and 'zzq' not in (lua or ''),
      repr((lua or '')[:40]) + ' | ' + out[-200:])

lua, out = run_args(FIND + ['--click', REPLACE_ALL, '--click', CODE, '--key', 'ctrl+s'])
check('the Replace all button replaces every match', (lua or '').startswith('yy = yy + yy ') and 'zzq' not in (lua or ''),
      repr((lua or '')[:40]) + ' | ' + out[-200:])

lua, out = run_args(['--text', 'ZZQ '] + FIND + ['--click', MATCH_CASE, '--click', REPLACE_ALL, '--click', CODE, '--key', 'ctrl+s'])
check('with Match case on, Replace all leaves other cases alone', (lua or '').startswith('ZZQ yy = yy + yy '),
      repr((lua or '')[:40]) + ' | ' + out[-200:])

lua, out = run_args(FIND + ['--key', 'ctrl+alt+enter', '--click', CODE, '--key', 'ctrl+z', '--key', 'ctrl+s'])
check('one Ctrl+Z undoes the whole Replace all', (lua or '') == TYPED + original, repr((lua or '')[:40]) + ' | ' + out[-200:])

lua, out = run_args(FIND + ['--key', 'enter', '--key', 'enter', '--click', CODE, '--key', 'ctrl+s'])
check('Enter in Replace replaces one match at a time', (lua or '').startswith('yy = yy + zzq '), repr((lua or '')[:40]) + ' | ' + out[-200:])

# --- Brackets and quotes ---
lines = [['f(x'], ['s = "hi'], ['g(', 'backspace'], ['h(1)'], ["-- don't"]]
args = ['--key', 'ctrl+end']
for i, line in enumerate(lines):
    if i: args += ['--key', 'escape', '--key', 'end', '--key', 'enter']
    args += ['--text', line[0]] + sum((['--key', k] for k in line[1:]), [])
lua, out = run_args(args + ['--key', 'escape', '--key', 'ctrl+s'])
want = original + 'f(x)\ns = "hi"\ng\nh(1)\n-- don\'t'
check('typed brackets and quotes close themselves, step over and delete in pairs', lua == want,
      repr((lua or '')[len(original):]) + ' | ' + out[-200:])

# --- Changed on disk ---
def changed_run(args):
    """The file's text afterwards (None when it is gone), the editor's output
    and the final screenshot."""
    project = project_copy()
    new = os.path.join(project, 'changed.lua')
    open(new, 'w', encoding='utf-8', newline='\n').write(CHANGED)
    script = os.path.join(project, 'scripts', 'game_logic.lua')
    args = [new + '@' + script if a == '@NEW' else script if a == '@SCRIPT' else a for a in args]
    out = editor_run(project, ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '5'] + args)
    return read(script), out, readpng(os.path.join(project, 'shot.png'))


def bar_shown(shot):
    """A disk bar over the code: its background across the top of the editor."""
    w, rows, bpp = shot
    return all(tuple(rows[84][x * bpp:x * bpp + 3]) == BAR for x in range(300, 1000, 25))


def tab_strip(shot):
    w, rows, bpp = shot
    return [bytes(r[360 * bpp:640 * bpp]) for r in rows[50:76]]

COPY = ['--copy-file', '@NEW', '--wait', '90']
lua, out, shot = changed_run(COPY + ['--text', MARK, '--key', 'ctrl+s'])
check('a script with no edits reloads when its file changes', lua == MARK + CHANGED, repr((lua or '')[:60]) + ' | ' + out[-200:])

lua, out, shot = changed_run(['--text', MARK] + COPY)
check('with edits and no answer, the file on disk is left alone', lua == CHANGED, repr((lua or '')[:60]) + ' | ' + out[-200:])
conflict_bar = bar_shown(shot)
check('with edits, a change on disk brings up the Reload / Keep mine bar', conflict_bar)

lua, out, shot = changed_run(['--text', MARK] + COPY + ['--click', RELOAD, '--key', 'ctrl+s'])
check('with edits, Reload on the bar takes the file from disk', lua == CHANGED, repr((lua or '')[:60]) + ' | ' + out[-200:])

# Keep mine leaves the cursor after the typed text: Q lands there. Clicking
# where the bar should be with no bar up puts the cursor elsewhere.
lua, out, shot = changed_run(['--text', MARK] + COPY + ['--click', KEEP_MINE, '--text', 'Q', '--key', 'ctrl+s'])
check('with edits, Keep mine on the bar keeps them and saves over the file',
      conflict_bar and lua == MARK + 'Q' + original and not bar_shown(shot),
      f'bar came up: {conflict_bar} | ' + repr((lua or '')[:60]) + ' | ' + out[-200:])

# --- Deleted on disk ---
DELETE = ['--delete-file', '@SCRIPT', '--wait', '90']
_, _, plain = changed_run(['--wait', '90'])
lua, out, shot = changed_run(DELETE)
check('a script deleted on disk stays gone with no answer', lua is None, repr((lua or '')[:60]) + ' | ' + out[-200:])
check('a script deleted on disk brings up the Save / Close bar', bar_shown(shot) and not bar_shown(plain))
check('a script deleted on disk is marked on its tab', tab_strip(shot) != tab_strip(plain))

lua, out, shot = changed_run(['--text', MARK] + DELETE + ['--click', SAVE_DELETED, '--wait', '5'])
check('Save on the deleted bar writes the file again, edits and all', lua == MARK + original and not bar_shown(shot),
      repr((lua or '')[:60]) + ' | ' + out[-200:])

lua, out, shot = changed_run(DELETE + ['--click', CLOSE_DELETED, '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
check('Close on the deleted bar closes the tab; nothing writes the file', lua is None and not bar_shown(shot),
      repr((lua or '')[:60]) + ' | ' + out[-200:])

# --- New script on a Trigger component ---
project = project_copy()
scene = os.path.join(project, 'courtyard.scene')
data = json.load(open(scene, encoding='utf-8'))
logic = next(o for o in data['objects'] if o['name'] == 'Game Logic')
logic['components'].append({'type': 'trigger', 'size': [1, 1, 1], 'lua': None})
json.dump(data, open(scene, 'w', encoding='utf-8'), indent=2)
out = editor_run(project, ['--select', 'Game Logic', '--click', NEW_TRIGGER_SCRIPT, '--wait', '5', '--text', MARK, '--key', 'ctrl+s',
                           '--click', OUTLINER, '--key', 'ctrl+s', '--wait', '5'])
lua = read(os.path.join(project, 'scripts', 'game_logic_trigger.lua'))
check('Trigger New script writes scripts/game_logic_trigger.lua and opens it',
      lua is not None and lua.startswith(MARK) and 'function onTriggerEnter(index)' in lua, repr((lua or '')[:60]) + ' | ' + out[-200:])
saved = json.load(open(scene, encoding='utf-8'))
trig = next(c for o in saved['objects'] if o['name'] == 'Game Logic' for c in o['components'] if c['type'] == 'trigger')
check('Trigger New script sets the trigger\'s script', trig.get('lua') == 'scripts/game_logic_trigger.lua', trig)
check('the Script component keeps its own file', read(os.path.join(project, 'scripts', 'game_logic.lua')) == original)

# --- The completion docs card stays off the inspector ---
# Completion far right on the line: the list and its docs card would reach
# past the editor panel onto the inspector, which must look as it does with
# no completion open.
def inspector(args):
    project = project_copy()
    editor_run(project, ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '5', '--key', 'ctrl+end'] + args + ['--wait', '10'])
    w, rows, bpp = readpng(os.path.join(project, 'shot.png'))
    return [bytes(r[1250 * bpp:1590 * bpp]) for r in rows[150:340]]

plain, popup = inspector([]), inspector(['--text', ' ' * 70 + 'Entity.Fi'])
diff = sum(a != b for a, b in zip(plain, popup))
check('the completion list and docs card stay inside the editor panel', diff == 0, f'{diff} inspector rows differ')

print(f'{fails} failures')
sys.exit(1 if fails else 0)
