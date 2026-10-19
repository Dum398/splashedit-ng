# psxsplash allocates its ordering table and bump allocator once, from the
# first scene it loads (scene 0, Renderer::Configure), and aborts on a later
# scene whose splashpack asks for more. `splashpack-cli build` (the same
# splash::exportBuild Play, Run on hardware and the disc export use) must
# therefore write scene 0 with the per-field largest of the build. Two
# scenes: scene 0 with automatic sizes (an empty scene: small), scene 1 asking
# for a 1024-slot ordering table and a bump allocator smaller than scene 0's
# own. Checked in the exported headers: scene 0 carries 1024 and keeps its own
# larger bump, scene 1 is as it was, the build prints the render buffers line;
# the pair swapped raises only the bump (per field); a single scene, and a
# build whose scene 0 is already the largest, are written as they were.
# usage: python3 renderbufferscheck.py <splashpack-cli>
import json, os, subprocess, sys, tempfile

cli = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import splashdiff  # noqa: E402

tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def scene(name, **settings):
    path = os.path.join(tmp, name + '.scene')
    json.dump({'format': 'splashedit-ng/scene', 'version': 1, 'settings': settings, 'objects': []}, open(path, 'w'))
    return path


def build(out, *scenes):
    r = subprocess.run([cli, 'build', *scenes, '-o', os.path.join(tmp, out), '--project', tmp], capture_output=True, text=True,
                       timeout=300)
    return r.returncode, r.stdout, r.stderr


def sizes(out, i):
    h = splashdiff.Pack(os.path.join(tmp, out, 'scene_%d.splashpack' % i)).h
    return h['orderingTableSize'], h['bumpAllocatorSize']


small = scene('small')
big = scene('big', orderingTableSize=1024, bumpAllocatorSize=4000)
rc, out, err = build('alone', small)
check('scene 0 alone builds', rc == 0, err)
ot0, bump0 = sizes('alone', 0) if rc == 0 else (0, 0)
check('alone it is small: below scene 1\'s ordering table, above its bump', 0 < ot0 < 1024 and bump0 > 4000, (ot0, bump0))
check('a single scene prints no render buffers line', 'render buffers' not in out, out)

rc, out, err = build('pair', small, big)
check('the two-scene build succeeds', rc == 0, err)
check('scene 0 carries scene 1\'s ordering table and keeps its own larger bump', sizes('pair', 0) == (1024, bump0),
      (sizes('pair', 0), bump0))
check('scene 1 is written as it asks', sizes('pair', 1) == (1024, 4000), sizes('pair', 1))
check('the build says it raised scene 0', 'render buffers: scene 0 raised to ordering_table=1024 bump=%d (alone %d, %d)' % (bump0, ot0, bump0)
      in out, out)

rc, out, err = build('swapped', big, small)
check('per field: scene 0 keeps its ordering table, takes scene 1\'s larger bump', rc == 0 and sizes('swapped', 0) == (1024, bump0),
      (err, sizes('swapped', 0) if rc == 0 else None))
check('the later scene keeps its own sizes', rc == 0 and sizes('swapped', 1) == (ot0, bump0), sizes('swapped', 1) if rc == 0 else None)

largest = scene('largest', orderingTableSize=1024, bumpAllocatorSize=bump0 + 1000)
rc, out, err = build('ordered', largest, small)
check('nothing raised when scene 0 is already the largest', rc == 0 and sizes('ordered', 0) == (1024, bump0 + 1000) and
      'render buffers' not in out, (err, out))

print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
