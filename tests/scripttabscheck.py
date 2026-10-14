# The centre panel's tab strip (Scene, UI, then one tab per open script),
# driven through the GUI: open a script, then click back to Scene; go to UI
# and back to Scene; go from the script to UI. The editor prints
# "scripttabs: active <tab>" each time a click brings a tab to the front.
# Tabs are clicked by name (--click @tab.scene, @tab.ui, @tab.0).
# usage: python3 scripttabscheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" button at 1600x960 (as in scripteditorcheck.py)
OPEN = ['--select', 'Game Logic', '--click', EDIT_SCRIPT, '--wait', '10']


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args):
    """Runs the editor on a fresh courtyard copy, then `args`; returns the
    tabs the clicks brought to the front, in order, and the named controls."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + [
        '--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    active = [l.split(' ', 2)[2] for l in lines if l.startswith('scripttabs: active ')]
    controls = [l.split()[1] for l in lines if l.startswith('control ')]
    return active, controls


# Positive control: a script tab exists once Edit script is clicked.
active, controls = run(OPEN)
check('Edit script opens a script tab', 'tab.0' in controls, controls)

active, _ = run(OPEN + ['--click', '@tab.scene', '--wait', '10'])
check('Scene comes back to the front after a script tab', active == ['Scene'], active)

active, _ = run(['--click', '@tab.ui', '--wait', '10', '--click', '@tab.scene', '--wait', '10'])
check('Scene comes back to the front after the UI tab', active == ['UI', 'Scene'], active)

active, _ = run(OPEN + ['--click', '@tab.ui', '--wait', '10', '--click', '@tab.0', '--wait', '10', '--click', '@tab.scene',
                        '--wait', '10'])
check('UI, the script and Scene each come to the front in turn', active == ['UI', 'game_logic.lua', 'Scene'], active)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
