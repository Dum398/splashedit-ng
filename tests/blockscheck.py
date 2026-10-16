# Block scripts: `splashpack-cli blocks` turns each blocks/*.blocks into
# exactly blocks/expected/<name>.lua, every expected file compiles with
# `splashpack-cli luac`, bad.blocks fails naming the block, and a scene whose
# script is door.blocks exports byte for byte like one pointing at the
# expected door.lua.
# usage: python3 -P blockscheck.py <splashpack-cli>
import glob, json, os, shutil, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
blocks = os.path.join(here, 'blocks')
tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(*args):
    # text mode reads Windows' \r\n as \n
    r = subprocess.run([cli, *args], capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, r.stdout, r.stderr.strip()


fixtures = sorted(f for f in glob.glob(os.path.join(blocks, '*.blocks')) if os.path.basename(f) != 'bad.blocks')
check('fixtures present', len(fixtures) >= 4, fixtures)
for src in fixtures:
    name = os.path.splitext(os.path.basename(src))[0]
    expected = os.path.join(blocks, 'expected', name + '.lua')
    rc, out, err = run('blocks', src)
    # text mode: a Windows checkout may have \r\n line ends
    want = open(expected, encoding='utf-8').read() if os.path.exists(expected) else None
    if rc != 0 or want is None or out != want:
        first = next((i + 1 for i, (a, b) in enumerate(zip(out.split('\n'), (want or '').split('\n'))) if a != b), None)
        check(name + ' generates expected/' + name + '.lua', False,
              err or ('no expected file' if want is None else 'first difference at line %s' % first))
    else:
        check(name + ' generates expected/' + name + '.lua', True)

for exp in sorted(glob.glob(os.path.join(blocks, 'expected', '*.lua'))):
    name = os.path.basename(exp)
    rc, _, err = run('luac', exp, '-o', os.path.join(tmp, name + 'c'))
    check('expected/' + name + ' compiles', rc == 0, err)

rc, out, err = run('blocks', os.path.join(blocks, 'bad.blocks'))
check('bad.blocks fails with the location', rc == 1 and out == '' and
      'bad.blocks: stack 1, block 3 (change_var): undeclared variable "missing"' in err, (rc, err))


def export(script):
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'script': None},
             'objects': [{'name': 'Door', 'active': True,
                          'transform': {'position': [0.0, 0.0, 4.0], 'rotation': [0.0, 0.0, 0.0, 1.0],
                                        'scale': [1.0, 1.0, 1.0]},
                          'components': [{'type': 'script', 'lua': script}], 'children': []}]}
    path = os.path.join(tmp, 'door.scene')
    json.dump(scene, open(path, 'w', encoding='utf-8'))
    out = os.path.join(tmp, script + '.splashpack')
    rc, _, err = run('export', path, '-o', out, '--project', tmp)
    return (open(out, 'rb').read() if rc == 0 and os.path.exists(out) else None), err


shutil.copy(os.path.join(blocks, 'door.blocks'), tmp)
open(os.path.join(tmp, 'door.lua'), 'w', encoding='utf-8', newline='\n').write(
    open(os.path.join(blocks, 'expected', 'door.lua'), encoding='utf-8').read())
from_blocks, err1 = export('door.blocks')
from_lua, err2 = export('door.lua')
check('a door.blocks scene exports', from_blocks is not None, err1)
check('door.blocks exports like door.lua', from_blocks is not None and from_blocks == from_lua,
      (err2, len(from_blocks or b''), len(from_lua or b'')))
open(os.path.join(tmp, 'door.blocks'), 'w').write(open(os.path.join(blocks, 'bad.blocks')).read())
broken, err = export('door.blocks')
check('a bad .blocks script fails the export', broken is None and 'undeclared variable' in err, err)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
