# A .blend dropped in the editor is converted by Blender on a worker thread:
# with a mock Blender that takes a few seconds, the editor keeps drawing
# frames while it runs, the status bar shows the import, and once Blender is
# done the model is placed (and saved with Ctrl+S), as one undo step. Cancel
# in the status bar kills the mock, places nothing and leaves no temporary
# folder. Drives the editor headless on copies of the courtyard example.
# usage: python3 blendasynccheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import atexit, base64, json, os, re, shutil, signal, stat, struct, subprocess, sys, tempfile, time

editor = os.path.abspath(sys.argv[1])
shot_to = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
if os.name == 'nt':
    print('SKIP blendasynccheck: the mock Blender is a shebang script')
    sys.exit(0)
tmp = tempfile.mkdtemp()
atexit.register(shutil.rmtree, tmp, True)  # should anything before the checks' own cleanup fail
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


# skinimport/rig.gltf (one data-URI buffer) as a GLB, for the mock to "export".
j = json.load(open(os.path.join(here, 'skinimport', 'rig.gltf')))
data = base64.b64decode(j['buffers'][0].pop('uri').split(',', 1)[1])
js = json.dumps(j).encode()
js += b' ' * (-len(js) % 4)
data += b'\0' * (-len(data) % 4)
glb = os.path.join(tmp, 'rig.glb')
open(glb, 'wb').write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(data)) +
                      struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(data), 0x004E4942) + data)

blend = os.path.join(tmp, 'src', 'crate.blend')
os.makedirs(os.path.dirname(blend))
open(blend, 'wb').write(b'BLENDER-v300 not really')

# Waits until the check creates MOCK_BLENDER_RELEASE, then writes the GLB
# where the export script asks. Notes its pid. Never released, it waits ten
# minutes, far past every bound here (the editor's 90 s --wait-import, run's
# 180 s), so an import only ends early if the editor killed it; the check
# kills any still running when it ends.
mock = os.path.join(tmp, 'bin', 'blender')
os.makedirs(os.path.dirname(mock))
open(mock, 'w').write(r'''#!/usr/bin/env python3
import ast, os, re, shutil, sys, time
open(os.environ['MOCK_BLENDER_PID'], 'w').write(str(os.getpid()))
until = time.time() + 600
while not os.path.exists(os.environ['MOCK_BLENDER_RELEASE']):
    if time.time() > until:
        sys.exit(3)
    time.sleep(0.05)
expr = sys.argv[sys.argv.index('--python-expr') + 1]
m = re.search(r"filepath='((?:[^'\\]|\\.)*)'", expr)
shutil.copyfile(os.environ['MOCK_BLENDER_GLB'], ast.literal_eval(m.group(0)[len('filepath='):]))
''')
os.chmod(mock, os.stat(mock).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def pidfile(name):
    return os.path.join(tmp, name, 'mock.pid')


def mock_pid(name):
    """The pid the run's mock Blender noted, 0 if it never started."""
    try:
        return int(open(pidfile(name)).read())
    except (OSError, ValueError):
        return 0

DROP = '800,420'  # in the Scene view at 1600x960


def tempdir(name):
    """The run's own TMPDIR: the editor makes its temporary folders with
    std::filesystem::temp_directory_path, which honours TMPDIR, so what is left
    in it was left by that run and not by anything else on the machine."""
    return os.path.join(tmp, name, 'tmp')


def temps(name):
    d = tempdir(name)
    return sorted(n for n in os.listdir(d) if n.startswith(('splashedit-blend-', 'splashedit-import-'))) if os.path.isdir(d) else []


def poll(cond, seconds=10):
    """Whether cond() comes true within `seconds`: under load, a killed process
    takes a while to be reaped."""
    deadline = time.time() + seconds
    while True:
        if cond():
            return True
        if time.time() >= deadline:
            return False
        time.sleep(0.1)


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    try:
        return ') Z ' not in open('/proc/%d/stat' % pid).read()
    except OSError:
        return False


def run(name, script, release_after=None):
    """Runs the editor on a fresh copy of the courtyard with `script`; the mock
    Blender finishes once the file `release_after` exists (at once when None,
    never when ''). Returns (project, output, top-level object names of the
    saved scene)."""
    project = os.path.join(tmp, name, 'courtyard')
    shutil.copytree(example, project)
    release = os.path.join(tmp, name, 'release')
    os.makedirs(tempdir(name))
    if release_after is None:
        open(release, 'w').close()
    env = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe',
               SPLASHEDIT_BLENDER=mock, MOCK_BLENDER_GLB=glb, MOCK_BLENDER_PID=pidfile(name), MOCK_BLENDER_RELEASE=release,
               TMPDIR=tempdir(name))
    cmd = [editor, '--size', '1600x960', '--project', project] + script + ['--screenshot', os.path.join(project, 'end.png')]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    deadline = time.time() + 180
    while p.poll() is None and time.time() < deadline:
        if release_after and os.path.exists(release_after) and not os.path.exists(release):
            time.sleep(0.2)  # let the screenshot finish writing
            open(release, 'w').close()
        time.sleep(0.05)
    if p.poll() is None:
        p.kill()
        print('run %s: the editor was still running after 180 s; killed' % name)
    out = p.communicate()[0].strip()
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return project, out, [o['name'] for o in scene['objects']]


def shot_line(out):
    m = re.search(r'model import: (\w+), (\d+) frames drawn while it ran', out)
    return (m.group(1), int(m.group(2))) if m else (None, 0)


try:
    # Blender finishes only once the screenshot 60 frames after the drop is
    # written: it must find the import running and the editor drawing; then the
    # script waits for the import to finish and saves.
    mid = os.path.join(tmp, 'mid.png')
    project, out, names = run('place', ['--drop-now', blend + '@' + DROP, '--wait', '60', '--shot', mid, '--wait-import',
                                        '--wait', '10', '--key', 'ctrl+s', '--wait', '10'], mid)
    state, frames = shot_line(out)
    check('mid-import screenshot written', os.path.exists(mid), out[-300:])
    check('the import is still running 60 frames after the drop', state == 'running', out[-300:])
    check('the editor drew frames while Blender ran', frames >= 50, (frames, out[-300:]))
    check('once Blender is done the model is placed and saved', 'crate' in names, str(names) + ' | ' + out[-300:])
    check('the .blend is adopted into models/', os.path.exists(os.path.join(project, 'models', 'crate.blend')))
    check('the model is imported', os.path.exists(os.path.join(project, 'models', 'crate.mesh')) or
          os.path.isdir(os.path.join(project, 'models', 'crate')), os.listdir(os.path.join(project, 'models')))
    check('no temporary folder is left', poll(lambda: not temps('place')), temps('place'))
    if shot_to and os.path.exists(mid):
        shutil.copyfile(mid, shot_to)

    # Undo takes the placed model back in one step.
    project, out, names = run('undo', ['--drop-now', blend + '@' + DROP, '--wait-import', '--wait', '10', '--key', 'ctrl+z',
                                       '--wait', '5', '--key', 'ctrl+s', '--wait', '10'])
    check('ctrl+z after the import takes the model back in one step', 'crate' not in names, str(names) + ' | ' + out[-300:])

    # Cancel: the status bar names the Cancel button's rect in the --shot line.
    probe = os.path.join(tmp, 'probe.png')
    _, out, _ = run('probe', ['--drop-now', blend + '@' + DROP, '--wait', '30', '--shot', probe, '--wait-import'], probe)
    m = re.search(r'cancel button at (\d+),(\d+)', out)
    check('the status bar shows a Cancel button while importing', m is not None, out[-300:])
    if m:
        # Clicked where the button is in this run: it moves with the budget meters
        # beside it, which on a busy machine can arrive mid-click, so
        # --click-import-cancel waits for the status bar to settle. The shot comes
        # after --wait-import (at most 90 s): never released, the mock outlives
        # that, so "idle" there means Cancel ended the import while the editor
        # still ran (closing the editor would kill Blender anyway).
        project, out, names = run('cancel', ['--drop-now', blend + '@' + DROP, '--wait', '30', '--click-import-cancel', '--wait', '30',
                                             '--wait-import', '--shot', os.path.join(tmp, 'cancelled.png'), '--key', 'ctrl+s',
                                             '--wait', '10'], '')
        state, _ = shot_line(out)
        check('Cancel ends the import', state == 'idle', out[-300:])
        check('Cancel places nothing', 'crate' not in names, str(names))
        check('Cancel adopts nothing', not os.path.exists(os.path.join(project, 'models', 'crate.blend')))
        pid = mock_pid('cancel')
        # The import only goes idle once Blender's process is reaped; gone for good is polled for.
        check('Cancel kills Blender', pid and state == 'idle' and poll(lambda: not alive(pid)), (pid, state))
        check('Cancel leaves no temporary folder', poll(lambda: not temps('cancel')), temps('cancel'))
finally:
    # Whatever happened above: no mock Blender outlives the check, and its
    # folder (with every run's TMPDIR) goes.
    for name in os.listdir(tmp) if os.path.isdir(tmp) else []:
        pid = mock_pid(name)
        if pid and alive(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
            poll(lambda: not alive(pid))
    shutil.rmtree(tmp, ignore_errors=True)
    if os.path.isdir(tmp):
        print('FAIL the check could not remove ' + tmp)
        fails += 1
print('%d failures' % fails)
sys.exit(1 if fails else 0)
