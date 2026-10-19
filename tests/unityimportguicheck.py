# File > Import Unity Project... in the editor, on the small Unity project in
# fixtures/unity-mini: the dialog's controls are drawn, the scene list reads
# the project (Mini on, Empty off: it has no PSXSceneExporter), Import stays
# off while the new project's folder is not empty, a scene switched on and
# moved up comes first in scenes.list, and an import opens the new project
# and shows the report (the camera listed). The default import is given a CD
# track (Add a CD track, its file typed), which lands in disc.json. Then the
# report is closed and the imported scene is screenshotted. Controls are
# clicked by name.
# usage: python3 unityimportguicheck.py <splashedit> [screenshot dir]
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
mini = os.path.join(here, 'fixtures', 'unity-mini')
example = os.path.join(here, '..', 'examples', 'courtyard')
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else tempfile.mkdtemp()
os.makedirs(shots, exist_ok=True)
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args, final):
    """Runs the editor on a fresh courtyard copy, then `args`; returns the output lines."""
    work = tempfile.mkdtemp()
    project = os.path.join(work, 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + ['--wait', '5', '--screenshot', final]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return lines, project


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(field, text):
    return click(field) + ['--text', text, '--wait', '3']


def controls(lines):
    return {l.split(' ')[1] for l in lines if l.startswith('control ')}


def imported(lines):
    return [l for l in lines if l.startswith('unityimport: imported') or l.startswith('unityimport: failed')]


OPEN = click('menu.file') + click('menu.file/Import Unity Project...')
AWAY = ['--move', '800,120', '--wait', '3']

# The form, ending on it: its controls (a CD track row added), the scene list
# read, Import off while the new project's folder (here the courtyard example)
# is not empty.
work = tempfile.mkdtemp()
lines, project = run(OPEN + typed('unity.source', mini) + typed('unity.dest', example) + click('unity.cdadd') + click('unity.import') + AWAY,
                     os.path.join(shots, 'unityimport-refused.png'))
have = controls(lines)
for c in ['unity.source', 'unity.sourcebrowse', 'unity.dest', 'unity.destbrowse', 'unity.scenes', 'unity.scene.Mini',
          'unity.scene.Empty', 'unity.up.Mini', 'unity.down.Mini', 'unity.up.Empty', 'unity.down.Empty', 'unity.status',
          'unity.import', 'unity.cancel', 'unity.cdadd', 'unity.cdtrack.2', 'unity.cdbrowse.2', 'unity.cdremove.2']:
    check('the dialog draws ' + c, c in have, sorted(h for h in have if h.startswith('unity')))
check('no import while the folder is not empty', imported(lines) == [], imported(lines))
check('the dialog is still open', 'popups open: yes' in lines)

# A scene switched on and moved first, then imported.
dest = os.path.join(work, 'both')
lines, project = run(OPEN + typed('unity.source', mini) + typed('unity.dest', dest) + click('unity.scene.Empty') +
                     click('unity.up.Empty') + AWAY + ['--shot', os.path.join(shots, 'unityimport-dialog.png'), '--wait', '2'] +
                     click('unity.import') + ['--wait', '20'], os.path.join(work, 'both.png'))
have = controls(lines)
check('the report page draws its list and Close', {'unity.report', 'unity.close'} <= have, sorted(h for h in have if h.startswith('unity')))
got = imported(lines)
check('one import ran', len(got) == 1 and got[0].startswith('unityimport: imported scenes=2'), got)
check('the courtyard project was left alone', sorted(os.listdir(example)) == sorted(os.listdir(project)))
lst = os.path.join(dest, 'scenes.list')
check('the scene moved up comes first', os.path.exists(lst) and open(lst).read().split() == ['Empty.scene', 'Mini.scene'],
      open(lst).read() if os.path.exists(lst) else 'no scenes.list')
check('the report lists the camera', 'unityimport: report [left out] Mini / Main Camera: Camera, AudioListener (no PS1 equivalent)' in lines,
      [l for l in lines if l.startswith('unityimport: report')])
check('the new project opened', any(l.startswith('status: Imported 2 scenes from Unity') for l in lines),
      [l for l in lines if l.startswith('status:')])

# The default choice (Mini alone) and a CD track: the report, then closed, the imported scene.
work = tempfile.mkdtemp()
dest = os.path.join(work, 'mini')
wav = os.path.join(mini, 'Assets', 'Audio', 'beep.wav')
lines, _ = run(OPEN + typed('unity.source', mini) + typed('unity.dest', dest) + click('unity.cdadd') + typed('unity.cdtrack.2', wav) +
               click('unity.import') + ['--wait', '20'] + AWAY +
               ['--shot', os.path.join(shots, 'unityimport-report.png'), '--wait', '2'] + click('unity.close') +
               click('tree.Cube') + ['--key', 'f', '--wait', '5'], os.path.join(shots, 'unityimport-result.png'))
got = imported(lines)
check('the default import brings Mini alone', got == ['unityimport: imported scenes=1 objects=14 meshes=4 scripts=2 textures=1 skipped=2'], got)
disc = os.path.join(dest, 'disc.json')
check('the CD track is in disc.json', os.path.exists(disc) and json.load(open(disc)).get('tracks') == ['audio/beep.wav'],
      open(disc).read() if os.path.exists(disc) else 'no disc.json')
check('the report is closed at the end', 'popups open: no' in lines, [l for l in lines if l.startswith('popups')])
check('the imported scene is open', any(l.startswith('status: Imported 1 scene from Unity') for l in lines),
      [l for l in lines if l.startswith('status:')])
check('the imported objects are in the tree', {'tree.Parent', 'tree.Cube', 'tree.Quad', 'tree.RenamedLamp'} <= controls(lines),
      sorted(c for c in controls(lines) if c.startswith('tree.')))

# The imported scene's UI tab draws the canvases the game starts with: hud, not
# the loading screen's canvas (hud_loading), which psxsplash draws alone while
# the scene loads; picked, the loading canvas is drawn alone.
def drawn(lines):
    return [l for l in lines if l.startswith('uicanvas: drawn')]
r = subprocess.run([editor, '--size', '1600x960', '--project', dest, '--scene', os.path.join(dest, 'Mini.scene'), '--ui', '--wait', '5',
                    '--screenshot', os.path.join(shots, 'unityimport-ui.png')], capture_output=True, text=True, env=ENV, timeout=240)
got = drawn((r.stdout + r.stderr).splitlines())
check('the UI tab leaves the loading canvas out of the start screen', got[-1:] == ['uicanvas: drawn hud'], got)
r = subprocess.run([editor, '--size', '1600x960', '--project', dest, '--scene', os.path.join(dest, 'Mini.scene'), '--ui', '--wait', '5'] +
                   click('ui.canvas') + click('ui.canvas/hud_loading') + ['--screenshot', os.path.join(shots, 'unityimport-ui-loading.png')],
                   capture_output=True, text=True, env=ENV, timeout=240)
lines = (r.stdout + r.stderr).splitlines()
got = drawn(lines)
check('picked, the loading canvas is drawn alone', got[-1:] == ['uicanvas: drawn hud_loading'], (got, [l for l in lines if 'no such control' in l]))
print('screenshots in ' + shots)
sys.exit(1 if fails else 0)
