# Script editor and Play: a build reads scripts from disk, so F5 (Play) and
# Ctrl+F5 (Run on hardware) pressed while the script editor has the keyboard
# must write its unsaved tabs first. Then New script: in the scene inspector
# and on an object's empty Script component it writes scripts/<name>.lua,
# sets it (one undo step) and opens it. Drives the editor headless on copies of
# the courtyard example.
# usage: python3 scripteditorcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
MARK = 'zz_typed_marker = 1 '
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" / "New script" button at 1600x960
NEW_SCENE_SCRIPT = '1428,221'  # the scene inspector's "New script"
OUTLINER = '140,750'  # empty Scene panel: takes the keyboard from the script editor
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


def read(path):
    return open(path, encoding='utf-8').read() if os.path.exists(path) else None


def run(keys):
    """Opens game_logic.lua in the script editor, types MARK, presses `keys`;
    returns the file's text afterwards and the editor's output."""
    project = project_copy()
    args = ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '5', '--text', MARK]
    for k in keys:
        args += ['--key', k]
    out = editor_run(project, args)
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

print(f'{fails} failures')
sys.exit(1 if fails else 0)
