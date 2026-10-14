# .blend import through a mock Blender: a Python script standing in for
# blender that takes the arguments the importer passes, finds filepath= in the
# --python-expr and writes there a GLB made here from tests/skinimport/rig.gltf
# (a skinned, animated rig). Checks that importing the .blend writes exactly
# what importing that GLB directly writes, that SPLASHEDIT_BLENDER wins over
# PATH, that a sidecar beside the .blend applies, that Blender failing or
# writing nothing is an error carrying what it printed, and that no Blender at
# all is an error saying what to do.
# The mock runs only where a shebang script can be executed (not on Windows,
# where those arms print SKIP); the no-Blender arms run everywhere.
# usage: python3 blendcheck.py <splashpack-cli>
import ast, base64, json, os, pathlib, re, shutil, stat, struct, subprocess, sys, tempfile

cli = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
fails = checks = 0
windows = os.name == 'nt'


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def skip(name, why):
    print('SKIP ' + name + ': ' + why)


# rig.gltf (one data-URI buffer) as a GLB.
j = json.load(open(os.path.join(here, 'skinimport', 'rig.gltf')))
uri = j['buffers'][0].pop('uri')
data = base64.b64decode(uri.split(',', 1)[1])
js = json.dumps(j).encode()
js += b' ' * (-len(js) % 4)
data += b'\0' * (-len(data) % 4)
glb = os.path.join(tmp, 'rig.glb')
open(glb, 'wb').write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(data)) +
                      struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(data), 0x004E4942) + data)

# The .blend itself is never read by the mock; it only has to exist.
blend = os.path.join(tmp, 'src', 'rig.blend')
os.makedirs(os.path.dirname(blend))
open(blend, 'wb').write(b'BLENDER-v300 not really')

MOCK = r'''#!/usr/bin/env python3
import ast, json, os, re, shutil, sys
args = sys.argv[1:]
log = os.environ.get('MOCK_BLENDER_LOG')
if log:
    json.dump(args, open(log, 'w'))
mode = os.environ.get('MOCK_BLENDER_MODE', 'ok')
if mode == 'fail':
    print('Read blend: ' + args[1])
    sys.stderr.write('Traceback (most recent call last):\nRuntimeError: mock exporter exploded 4711\n')
    sys.exit(1)
expr = args[args.index('--python-expr') + 1]
m = re.search(r"filepath='((?:[^'\\]|\\.)*)'", expr)
out = ast.literal_eval(m.group(0)[len('filepath='):])
if mode == 'ok':
    shutil.copyfile(os.environ['MOCK_BLENDER_GLB'], out)
'''
WRONG = r'''#!/usr/bin/env python3
import sys
sys.stderr.write('this is the Blender on PATH, not SPLASHEDIT_BLENDER\n')
sys.exit(3)
'''


def write_mock(path, body=MOCK):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'w').write(body)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


bindir = os.path.join(tmp, 'bin')
write_mock(os.path.join(bindir, 'blender'))
emptydir = os.path.join(tmp, 'empty')
os.makedirs(emptydir)
argslog = os.path.join(tmp, 'args.json')


def env(path, blender=None, **extra):
    e = {k: v for k, v in os.environ.items() if k != 'SPLASHEDIT_BLENDER'}
    e['PATH'] = path
    if blender is not None:
        e['SPLASHEDIT_BLENDER'] = blender
    e['MOCK_BLENDER_GLB'] = glb
    e['MOCK_BLENDER_LOG'] = argslog
    e.update(extra)
    return e


def run(model, proj, environ, *flags):
    return subprocess.run([cli, 'import', model, '--project', proj, *flags], capture_output=True, text=True, env=environ)


def tree(proj):  # every file under models/, relative path -> bytes
    out, root = {}, os.path.join(proj, 'models')
    for d, _, files in os.walk(root):
        for f in files:
            out[pathlib.Path(os.path.relpath(os.path.join(d, f), root)).as_posix()] = open(os.path.join(d, f), 'rb').read()
    return out


def mock_path():
    return bindir + os.pathsep + os.environ.get('PATH', '')


def temps():
    return set(n for n in os.listdir(tempfile.gettempdir()) if n.startswith('splashedit-blend-'))


direct = os.path.join(tmp, 'direct')
r = run(glb, direct, env(os.environ.get('PATH', '')))
check('the GLB imports directly', r.returncode == 0 and 'rig.mesh' in tree(direct), r.stderr)
want = tree(direct)

if windows:
    for n in ('blend through blender on PATH', 'SPLASHEDIT_BLENDER over PATH', 'sidecar beside the .blend',
              'Blender failing', 'Blender writing nothing'):
        skip(n, 'the mock Blender is a shebang script')
else:
    # Blender found on PATH, SPLASHEDIT_BLENDER unset.
    before = temps()
    proj = os.path.join(tmp, 'viapath')
    r = run(blend, proj, env(mock_path()))
    check('.blend through blender on PATH exits 0', r.returncode == 0, r.stdout + r.stderr)
    got = tree(proj)
    check('.blend writes the same files as the GLB', sorted(got) == sorted(want), (sorted(got), sorted(want)))
    check('.blend writes the same bytes as the GLB (mesh, skeleton, clips)',
          len(want) > 1 and all(got.get(k) == v for k, v in want.items()), [k for k, v in want.items() if got.get(k) != v])
    args = json.load(open(argslog))
    check('Blender runs in the background on the .blend, then the export script',
          args[:2] == ['--background', os.path.abspath(blend)] and args[-2] == '--python-expr', args)
    expr = args[-1]
    check('the script exports a GLB with modifiers applied, skins and animations',
          all(s in expr for s in ("bpy.ops.export_scene.gltf(", "export_format='GLB'", 'export_apply=True',
                                  'export_animations=True', 'export_skins=True')), expr)
    out = ast.literal_eval(re.search(r"filepath=('(?:[^'\\]|\\.)*')", expr).group(1))
    check('the export goes to a temporary folder that is removed after',
          not out.startswith(os.path.dirname(blend)) and not os.path.exists(os.path.dirname(out)) and temps() <= before, out)

    # SPLASHEDIT_BLENDER wins over a (failing) blender on PATH.
    wrongbin = os.path.join(tmp, 'wrongbin')
    write_mock(os.path.join(wrongbin, 'blender'), WRONG)
    named = os.path.join(tmp, 'elsewhere', 'my blender 4.2')
    write_mock(named)
    proj = os.path.join(tmp, 'viaenv')
    r = run(blend, proj, env(wrongbin + os.pathsep + os.environ.get('PATH', ''), named))
    check('SPLASHEDIT_BLENDER over PATH: exits 0 with the same files', r.returncode == 0 and tree(proj) == want,
          r.stdout + r.stderr)

    # A sidecar beside the .blend sets the settings, as for any model.
    scaled = os.path.join(tmp, 'scaled')
    r = run(glb, scaled, env(os.environ.get('PATH', '')), '--scale', '3')
    json.dump({'scale': 3}, open(blend + '.import.json', 'w'))
    proj = os.path.join(tmp, 'sidecar')
    r = run(blend, proj, env(mock_path()))
    os.remove(blend + '.import.json')
    check('sidecar beside the .blend applies (scale 3)', r.returncode == 0 and 'settings scale 3 ' in r.stdout and
          tree(proj) == tree(scaled) and tree(proj) != want, r.stdout + r.stderr)

    # Blender failing: its stderr comes back in the error.
    proj = os.path.join(tmp, 'failed')
    r = run(blend, proj, env(mock_path(), MOCK_BLENDER_MODE='fail'))
    check('Blender failing: exits 1 with the exit code and its stderr',
          r.returncode == 1 and 'rig.blend: Blender could not export it as glTF (exit code 1)' in r.stderr and
          'mock exporter exploded 4711' in r.stderr, r.stderr)
    check('Blender failing writes nothing', not os.path.exists(os.path.join(proj, 'models', 'rig.mesh')))

    # Blender exiting 0 without writing the file.
    r = run(blend, os.path.join(tmp, 'silent'), env(mock_path(), MOCK_BLENDER_MODE='silent'))
    check('Blender writing nothing: exits 1 saying so', r.returncode == 1 and
          'rig.blend: Blender finished without writing the glTF export' in r.stderr, r.stderr)

# No Blender anywhere: PATH without one, SPLASHEDIT_BLENDER unset.
installed = [p for p in ('/Applications/Blender.app/Contents/MacOS/Blender',) if os.path.exists(p)]
pf = os.path.join(os.environ.get('ProgramFiles', 'C:/Program Files'), 'Blender Foundation')
if windows and os.path.isdir(pf):
    installed.append(pf)
if installed:
    skip('no Blender', 'one is installed at ' + installed[0])
else:
    r = run(blend, os.path.join(tmp, 'none'), env(emptydir))
    check('no Blender: exits 1 saying to install it, set SPLASHEDIT_BLENDER or export glTF',
          r.returncode == 1 and 'rig.blend: importing a .blend file needs Blender, which was not found' in r.stderr and
          'Install Blender' in r.stderr and 'set SPLASHEDIT_BLENDER' in r.stderr and 'as glTF (.glb)' in r.stderr,
          r.stderr)
missing = os.path.join(tmp, 'no', 'such', 'blender')
r = run(blend, os.path.join(tmp, 'missing'), env(emptydir, missing))
check('SPLASHEDIT_BLENDER naming no file: exits 1 naming it',
      r.returncode == 1 and 'SPLASHEDIT_BLENDER is set to ' + missing + ', which is not a file' in r.stderr, r.stderr)

shutil.rmtree(tmp, ignore_errors=True)
print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
