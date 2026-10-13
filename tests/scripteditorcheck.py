# Script editor and Play: a build reads scripts from disk, so F5 (Play) and
# Ctrl+F5 (Run on hardware) pressed while the script editor has the keyboard
# must write its unsaved tabs first. Drives the editor headless on a copy of
# the courtyard example: Edit script in the inspector, type, press the key.
# usage: python3 scripteditorcheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
MARK = 'zz_typed_marker = 1 '
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" button at 1600x960


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(keys):
    """Opens game_logic.lua in the script editor, types MARK, presses `keys`;
    returns the file's text afterwards and the editor's output."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    env = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Game Logic', '--click', EDIT_SCRIPT,
           '--wait', '5', '--text', MARK]
    for k in keys:
        cmd += ['--key', k]
    cmd += ['--wait', '30', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120)
    lua = open(os.path.join(project, 'scripts', 'game_logic.lua'), encoding='utf-8').read()
    return lua, (r.stdout + r.stderr).strip()


original = open(os.path.join(example, 'scripts', 'game_logic.lua'), encoding='utf-8').read()

# Without a key nothing saves, so a saved file below is the key's doing.
lua, out = run([])
check('typing alone leaves the file on disk as it was', lua == original, lua[:80] + ' | ' + out[-200:])

lua, out = run(['f5'])
check('F5 in the script editor saves the typed text before Play', lua.startswith(MARK), repr(lua[:60]) + ' | ' + out[-200:])

lua, out = run(['ctrl+f5'])
check('Ctrl+F5 in the script editor saves the typed text before Run on hardware', lua.startswith(MARK),
      repr(lua[:60]) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
