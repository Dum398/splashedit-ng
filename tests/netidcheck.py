# The scene inspector's Network section, driven through the GUI on copies of
# the courtyard: the Network id field starts empty (the engine derives an id
# at run time), typing an id locks it and shows its hash, Ctrl+Z puts it
# back, and Ctrl+S saves it so splashpack-cli writes the hash into the
# splashpack header (sceneHash, read with tests/splashdiff.py). The hash is
# Unity's: FNV-1a over the low byte of each UTF-16 unit. The editor prints
# "network: derived" or "network: locked <id> hash 0x<8 hex digits>" when it
# changes.
# usage: python3 netidcheck.py <splashedit> <splashpack-cli> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2])
keep = sys.argv[3] if len(sys.argv) > 3 else None
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import splashdiff

example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def unity_hash(s):
    """Unity's PSXSpriteExporter.HashSceneId."""
    if not s:
        return 0
    b = s.encode('utf-16-le')
    h = 2166136261
    for i in range(0, len(b), 2):
        h = ((h ^ b[i]) * 16777619) & 0xffffffff
    return h or 1


def copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    return project


def run(project, args):
    """Runs the editor on `project` (the scene inspector shows), scrolls the
    inspector to its end, then `args`. Returns the network: lines, all
    output lines, the named controls and the screenshot path."""
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5', '--wheel', '1400,600,-60', '--wait', '3'] + args + \
        ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].split(' at ')
            a, b = at.split(' ')
            controls[name] = tuple(float(v) for v in a.split(',') + b.split(','))
    return [l for l in lines if l.startswith('network:')], lines, controls, shot


def export(project):
    out = os.path.join(project, 'out.splashpack')
    r = subprocess.run([cli, 'export', os.path.join(project, 'courtyard.scene'), '-o', out], capture_output=True, text=True)
    check('splashpack-cli exports the scene', r.returncode == 0, r.stdout + r.stderr)
    return splashdiff.Pack(out).h['sceneHash'] if r.returncode == 0 else None


def typed(text):
    return ['--click', '@scene.netid', '--wait', '3', '--text', text, '--wait', '2', '--key', 'enter', '--wait', '3']


lobby = 'network: locked lobby hash 0x%08X' % unity_hash('lobby')

# Untouched: derived, the field is in view, and the header says 0.
p = copy()
net, lines, controls, _ = run(p, [])
check('an untouched scene prints network: derived', net == ['network: derived'], net)
nr = controls.get('scene.netid')
check('the Network id field is drawn inside the window', nr is not None and 0 <= nr[1] and nr[3] <= 960, nr)
check('an untouched scene exports sceneHash 0', export(p) == 0)

# Typed and saved: locked, the scene file keeps it, the header has Unity's hash.
p = copy()
net, lines, _, shot = run(p, typed('lobby') + ['--key', 'ctrl+s', '--wait', '5'])
check('typing an id prints its Unity hash', net[-1:] == [lobby], net)
saved = json.dumps(json.load(open(os.path.join(p, 'courtyard.scene'))))
check('Ctrl+S saves networkId into the scene file', '"networkId": "lobby"' in saved)
got = export(p)
check('the export writes the hash into the header', got == unity_hash('lobby'), None if got is None else hex(got))
if keep:
    shutil.copy(shot, keep)

# A non-ASCII id hashes as Unity's C# string does.
p = copy()
net, lines, _, _ = run(p, typed('été') + ['--key', 'ctrl+s', '--wait', '5'])
want = 'network: locked été hash 0x%08X' % unity_hash('été')
check('a non-ASCII id prints the UTF-16 hash', net[-1:] == [want], net)
got = export(p)
check('a non-ASCII id exports the UTF-16 hash', got == unity_hash('été'), None if got is None else hex(got))

# Undo puts the derived id back.
p = copy()
net, lines, _, _ = run(p, typed('lobby') + ['--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z after typing goes back to derived', net == ['network: derived', lobby, 'network: derived'], net)

print('FAILS=%d' % fails)
sys.exit(1 if fails else 0)
