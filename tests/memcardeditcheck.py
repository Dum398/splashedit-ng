# The scene inspector's Memory card section: Enable saves and the save title
# are edited there and land in the saved scene's settings.memCard, each edit
# one undo step. Drives the editor headless on copies of the courtyard example
# and reads the saved scene.
# usage: python3 memcardeditcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# At 1600x960 with nothing selected the inspector shows the scene. Folding
# Cutscenes and animations, Rendering and Render buffers brings the Memory card
# section into view whether or not the background export has filled in its
# sizes. Controls are clicked by name (--click @scene.mcenable): the editor aims
# at where the frame before drew them, so no pixel positions live here.
FOLDS = ['--click', '@s_sequences', '--wait', '3', '--click', '@s_rendering', '--wait', '3', '--click', '@s_render', '--wait', '5']
ENABLE = '@scene.mcenable'  # the Enable saves switch
TITLE = '@scene.mctitle'  # the Save title field
TYPED = 'MY GAME 1'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args):
    """Runs `args` after folding the two sections above, then Ctrl+S; returns the saved
    scene's settings.memCard ({} when absent) and the editor's output."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project] + FOLDS + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    out = (r.stdout + r.stderr).strip()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in out.splitlines() if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    out = '\n'.join(l for l in out.splitlines() if not l.startswith('control '))
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return scene.get('settings', {}).get('memCard', {}), out


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
assert 'memCard' not in original.get('settings', {}), 'the example already has memory card settings'

# Saving alone writes no memCard, so one below is the clicks' doing.
mc, out = run([])
check('save alone writes no memCard', mc == {}, str(mc) + ' | ' + out[-200:])

mc, out = run(['--click', ENABLE, '--wait', '3'])
check('Enable saves turns saves on', mc.get('enabled') is True, str(mc) + ' | ' + out[-200:])

mc, out = run(['--click', ENABLE, '--wait', '3', '--click', TITLE, '--wait', '3', '--text', TYPED, '--key', 'enter', '--wait', '3'])
check('Enable saves then typing a title saves both', mc.get('enabled') is True and mc.get('title') == TYPED,
      str(mc) + ' | ' + out[-200:])

# Clicking away commits the title too.
mc, out = run(['--click', ENABLE, '--wait', '3', '--click', TITLE, '--wait', '3', '--text', TYPED, '--click', '140,750',
               '--wait', '3'])
check('clicking away commits the title', mc.get('title') == TYPED, str(mc) + ' | ' + out[-200:])

# Escape cancels the typing.
mc, out = run(['--click', ENABLE, '--wait', '3', '--click', TITLE, '--wait', '3', '--text', TYPED, '--key', 'escape', '--wait', '3'])
check('escape keeps the title as it was', mc.get('enabled') is True and 'title' not in mc, str(mc) + ' | ' + out[-200:])

# One undo takes the title back and leaves saves on.
mc, out = run(['--click', ENABLE, '--wait', '3', '--click', TITLE, '--wait', '3', '--text', TYPED, '--key', 'enter', '--wait', '3',
               '--key', 'ctrl+z', '--wait', '3'])
check('ctrl+z takes the title back in one step', mc.get('enabled') is True and 'title' not in mc, str(mc) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
