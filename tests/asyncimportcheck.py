# A glTF dropped in the editor is read and baked on a worker thread: with a
# model large enough to take a few seconds, the editor keeps drawing frames
# while it imports, the status bar shows the import with a Cancel button, and
# once it is built the model is placed (and saved with Ctrl+S). Cancel places
# nothing, adopts nothing and leaves the saved scene as it was, with no
# temporary folder left. Two models dropped one after the other both land, in
# the order dropped, though the second is quicker to import. Drives the editor
# headless on copies of the courtyard example.
# usage: python3 asyncimportcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, math, os, re, shutil, struct, subprocess, sys, tempfile, time

editor = os.path.abspath(sys.argv[1])
shot_to = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def grid_glb(path, n):
    """A wavy n x n grid (2 n^2 triangles) with normals and UVs, as a GLB."""
    pos, nor, uv, idx = bytearray(), bytearray(), bytearray(), bytearray()
    for j in range(n + 1):
        for i in range(n + 1):
            x, z = i / n * 4 - 2, j / n * 4 - 2
            pos += struct.pack('<3f', x, 0.2 * math.sin(x * 5) * math.cos(z * 5), z)
            nor += struct.pack('<3f', 0, 1, 0)
            uv += struct.pack('<2f', i / n, j / n)
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            idx += struct.pack('<6I', a, a + n + 1, a + 1, a + 1, a + n + 1, a + n + 2)
    nv = (n + 1) ** 2
    blob = bytes(pos + nor + uv + idx)
    o1, o2, o3 = len(pos), len(pos) + len(nor), len(pos) + len(nor) + len(uv)
    g = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'name': 'Grid', 'mesh': 0}],
         'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3}]}],
         'buffers': [{'byteLength': len(blob)}],
         'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': len(pos)},
                         {'buffer': 0, 'byteOffset': o1, 'byteLength': len(nor)},
                         {'buffer': 0, 'byteOffset': o2, 'byteLength': len(uv)},
                         {'buffer': 0, 'byteOffset': o3, 'byteLength': len(idx)}],
         'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': nv, 'type': 'VEC3', 'min': [-2, -0.2, -2], 'max': [2, 0.2, 2]},
                       {'bufferView': 1, 'componentType': 5126, 'count': nv, 'type': 'VEC3'},
                       {'bufferView': 2, 'componentType': 5126, 'count': nv, 'type': 'VEC2'},
                       {'bufferView': 3, 'componentType': 5125, 'count': n * n * 6, 'type': 'SCALAR'}]}
    js = json.dumps(g).encode()
    js += b' ' * (-len(js) % 4)
    blob += b'\0' * (-len(blob) % 4)
    open(path, 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(blob)) + struct.pack('<II', len(js), 0x4E4F534A) +
                           js + struct.pack('<II', len(blob), 0x004E4942) + blob)


src = os.path.join(tmp, 'src')
os.makedirs(src)
terrain = os.path.join(src, 'terrain.glb')  # 80000 triangles: seconds to import
rocks = os.path.join(src, 'rocks.glb')      # 2048: a moment
grid_glb(terrain, 200)
grid_glb(rocks, 32)

DROP = '800,420'  # in the Scene view at 1600x960
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def temps():
    return set(n for n in os.listdir(tempfile.gettempdir()) if n.startswith('splashedit-import-'))


def run(name, script):
    """Runs the editor on a fresh copy of the courtyard with `script`. Returns
    (project, output, top-level object names of the saved scene, scene bytes)."""
    project = os.path.join(tmp, name, 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project] + script + ['--screenshot', os.path.join(project, 'end.png')]
    try:
        out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=ENV, timeout=240).stdout.strip()
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        out += '\n(timed out)'
    data = open(os.path.join(project, 'courtyard.scene'), 'rb').read()
    scene = json.loads(data.decode('utf-8'))
    return project, out, [o['name'] for o in scene['objects']], data


def shot_lines(out):
    """Every --shot report: (state, frames drawn while the import ran, imports queued)."""
    return [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r'model import: (\w+), (\d+) frames drawn while it ran, (\d+) queued', out)]


before = temps()
models = lambda p: sorted(os.listdir(os.path.join(p, 'models'))) if os.path.isdir(os.path.join(p, 'models')) else []

# Three frames after the drop the import must still be running and the
# editor drawing; once it is done (--wait-import) it reports how many frames
# it drew meanwhile, and the model is placed and saved.
mid = os.path.join(tmp, 'mid.png')
t0 = time.time()
project, out, names, _ = run('place', ['--drop-now', terrain + '@' + DROP, '--wait', '3', '--shot', mid, '--wait-import',
                                       '--wait', '2', '--shot', os.path.join(tmp, 'after.png'), '--wait', '5', '--key', 'ctrl+s',
                                       '--wait', '10'])
shots = shot_lines(out)
check('both screenshots reported', len(shots) == 2, out[-400:])
if len(shots) == 2:
    check('the import is still running 3 frames after the drop', shots[0][0] == 'running', shots)
    check('the editor drew frames while it imported', shots[1][0] == 'idle' and shots[1][1] >= 10, shots)
    print('     (%d frames drawn while terrain.glb imported)' % shots[1][1])
check('once it is built the model is placed and saved', 'terrain' in names, str(names) + ' | ' + out[-300:])
check('the model is adopted and imported', 'terrain.glb' in models(project) and 'terrain.mesh' in models(project), models(project))
check('the status bar says it was imported', 'status: Imported terrain.glb, 80000 triangles' in out, out[-300:])
check('no temporary folder is left', temps() <= before, temps() - before)

# The scene as a run with no import saves it, to compare Cancel against.
_, _, plain, plainData = run('plain', ['--wait', '5', '--key', 'ctrl+s', '--wait', '10'])

# Cancel: the status bar names the Cancel button's rect in the --shot line.
probe = os.path.join(tmp, 'probe.png')
_, out, _, _ = run('probe', ['--drop-now', terrain + '@' + DROP, '--wait', '3', '--shot', probe, '--wait-import'])
m = re.search(r'cancel button at (\d+),(\d+)', out)
check('the status bar shows a Cancel button while importing', m is not None, out[-300:])
if m:
    project, out, names, data = run('cancel', ['--drop-now', terrain + '@' + DROP, '--wait', '3', '--click-import-cancel', '--wait', '3',
                                               '--shot', os.path.join(tmp, 'cancelled.png'), '--wait-import', '--key', 'ctrl+s',
                                               '--wait', '10'])
    shots = shot_lines(out)
    check('Cancel ends the import at once', bool(shots) and shots[0][0] == 'idle', shots or out[-300:])
    check('Cancel says so in the status bar', 'Import of terrain.glb cancelled' in out, out[-300:])
    check('Cancel places nothing', 'terrain' not in names, str(names))
    check('Cancel leaves the saved scene as it was', data == plainData, '%d vs %d bytes' % (len(data), len(plainData)))
    check('Cancel adopts and writes nothing', not any(n.startswith('terrain') for n in models(project)), models(project))
    check('Cancel leaves no temporary folder', temps() <= before, temps() - before)

# Two drops in a row: the second waits for the first, and both land in the
# order dropped, though the second alone would be done first.
queued = os.path.join(tmp, 'queued.png')
project, out, names, _ = run('two', ['--drop-now', terrain + '@' + DROP, '--drop-now', rocks + '@' + DROP, '--wait', '20', '--shot', queued,
                                     '--wait-import', '--wait', '5', '--key', 'ctrl+s', '--wait', '10'])
shots = shot_lines(out)
check('the second drop waits behind the first', bool(shots) and shots[0][0] == 'running' and shots[0][2] == 2, shots or out[-300:])
if shot_to and os.path.exists(queued):
    shutil.copyfile(queued, shot_to)  # the status bar: Importing terrain.glb... N s (1 more waiting), Cancel
check('both models are placed', 'terrain' in names and 'rocks' in names, str(names))
if 'terrain' in names and 'rocks' in names:
    check('in the order they were dropped', names.index('terrain') < names.index('rocks'), str(names))
check('no temporary folder is left after both', temps() <= before, temps() - before)

print('took %.0f s' % (time.time() - t0))
shutil.rmtree(tmp, ignore_errors=True)
print('%d failures' % fails)
sys.exit(1 if fails else 0)
