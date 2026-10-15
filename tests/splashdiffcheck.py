# Check that splashdiff.py reads the current splashpack version (and v23) and compares
# the room block: courtyard copied with two Rooms and a Portal added, exported
# twice and compared, then byte-patched copies and a scene edit that must show
# up as room/portal differences.
# usage: python3 splashdiffcheck.py <splashpack-cli>
import json, os, re, shutil, struct, subprocess, sys, tempfile

cli = sys.argv[1]
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


court = os.path.join(tmp, 'courtyard')
shutil.copytree(os.path.join(here, '..', 'examples', 'courtyard'), court)
base = json.load(open(os.path.join(court, 'courtyard.scene')))


def node(name, pos, comp):
    return {'name': name, 'active': True,
            'transform': {'position': pos, 'rotation': [0.0, 0.0, 0.0, 1.0], 'scale': [1.0, 1.0, 1.0]},
            'components': [comp], 'children': []}


def with_rooms(west_size=(8.0, 6.0, 12.0)):
    s = json.loads(json.dumps(base))
    s['objects'] += [
        node('Room West', [-4.0, 2.5, 1.5], {'type': 'room', 'size': list(west_size), 'offset': [0.0, 0.0, 0.0]}),
        node('Room East', [4.0, 2.5, 1.5], {'type': 'room', 'size': [8.0, 6.0, 12.0], 'offset': [0.0, 0.0, 0.0]}),
        node('Doorway', [0.0, 1.5, 1.5],
             {'type': 'portal', 'roomA': 'Room West', 'roomB': 'Room East', 'width': 2.0, 'height': 3.0}),
    ]
    return s


def export(scene, name, stats=False):
    path = os.path.join(court, name + '.scene')
    with open(path, 'w') as f:
        json.dump(scene, f)
    out = os.path.join(tmp, name + '.splashpack')
    r = subprocess.run([cli, 'export', path, '-o', out] + (['--stats'] if stats else []),
                       capture_output=True, text=True)
    return r, out


def diff(a, b):
    r = subprocess.run([sys.executable, os.path.join(here, 'splashdiff.py'), a, b, '-v'],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def patched(src, name, offset, fmt, change):
    """Copy src (and its .vram) to name with the value at offset changed."""
    with open(src, 'rb') as f:
        d = bytearray(f.read())
    v, = struct.unpack_from('<' + fmt, d, offset)
    struct.pack_into('<' + fmt, d, offset, change(v))
    out = os.path.join(tmp, name + '.splashpack')
    with open(out, 'wb') as f:
        f.write(bytes(d))
    shutil.copyfile(os.path.splitext(src)[0] + '.vram', os.path.splitext(out)[0] + '.vram')
    return out


def lines(text, pattern):
    return [x for x in text.splitlines() if re.match(pattern, x)]


# (a) two exports of the same room scene compare equal, and the rooms are real
ra, a = export(with_rooms(), 'rooms_a', stats=True)
rb, b = export(with_rooms(), 'rooms_b')
check('room scene exports', ra.returncode == 0 and rb.returncode == 0, ra.stderr + rb.stderr)
refs = [int(m) for m in re.findall(r'^room tri_refs=(\d+) .*Room (?:West|East)$', ra.stdout, re.M)]
check('both rooms hold triangles (--stats)', len(refs) == 2 and all(refs), ra.stdout)
rc, out = diff(a, b)
check('(a) same scene twice: rc 0, EQUAL', rc == 0 and 'EQUAL' in out, out)
check('(a) summary names the rooms and portals', '3 rooms, 1 portals' in out, out)
check('(a) rooms are compared, not skipped', 'NOT COMPARED: roomCount' not in out, out)

try:
    pk = splashdiff.Pack(a)
except Exception as ex:  # the patch arms need the decoded header
    check('room scene decodes', False, ex)
    print('%d/%d checks passed' % (checks - fails, checks))
    sys.exit(1)
h = pk.h
base_off = pk.roomBlockOffset
check('room block decoded', base_off is not None and len(pk.rooms) == 3 and len(pk.portals) == 1,
      (base_off, len(pk.rooms), len(pk.portals)))
portals_off = base_off + 36 * h['roomCount']
refs_off = portals_off + 40 * h['portalCount']

# (b) one RoomData box int of room 0
p = patched(a, 'patch_box', base_off + 0, 'i', lambda v: v + 64)
rc, out = diff(a, p)
got = lines(out, r'room 0\.box: ')
check('(b) room 0 box patched: rc 1 and a room 0 line', rc == 1 and got, out)
print('     ' + '\n     '.join(got))

# (c) one PortalData center component (after roomA, roomB)
p = patched(a, 'patch_portal', portals_off + 4, 'i', lambda v: v + 64)
rc, out = diff(a, p)
got = lines(out, r'portal 0\.center: ')
check('(c) portal 0 center patched: rc 1 and a portal 0 line', rc == 1 and got, out)
print('     ' + '\n     '.join(got))

# (d) the triangleIndex of room 0's first tri ref
p = patched(a, 'patch_ref', refs_off + 4 * pk.rooms[0]['firstTriRef'] + 2, 'H', lambda v: v ^ 1)
rc, out = diff(a, p)
got = lines(out, r'room 0 tri refs differ')
check('(d) room 0 tri ref patched: rc 1 and a room tri refs line', rc == 1 and got, out)
print('     ' + '\n     '.join(got))

# (e) the scene with one Room's box changed
re_, e = export(with_rooms(west_size=(7.0, 6.0, 12.0)), 'rooms_e')
check('(e) edited scene exports', re_.returncode == 0, re_.stderr)
rc, out = diff(a, e)
got = lines(out, r'room \d')
check('(e) Room West resized: rc 1 and a room line', rc == 1 and got, out)
print('     ' + '\n     '.join(got[:4]))

# (f) a scene without rooms still compares equal and says nothing about rooms
rf1, f1 = export(json.loads(json.dumps(base)), 'plain_a')
rf2, f2 = export(json.loads(json.dumps(base)), 'plain_b')
rc, out = diff(f1, f2)
check('(f) courtyard without rooms: rc 0, EQUAL, no room line',
      rf1.returncode == 0 and rf2.returncode == 0 and rc == 0 and 'EQUAL' in out and 'room' not in out, out)

# (g) a Unity 2.4.0 v23 pack (psxsplash tests/boot/fixture/scene_0) decodes and
# compares equal with itself; against a v27 export the version is a difference
v23 = os.path.join(here, 'splashdiff', 'v23.splashpack')
rc, out = diff(v23, v23)
check('(g) v23 fixture vs itself: rc 0, EQUAL', rc == 0 and 'EQUAL' in out, out)
print('     ' + out.splitlines()[-1])
rc, out = diff(v23, f1)
got = lines(out, r'header\.version: 23 != 27$')
check('(g) v23 vs v27: rc 1 (not refused) and a header.version line', rc == 1 and got, out)
print('     ' + '\n     '.join(got))

shutil.rmtree(tmp, ignore_errors=True)
print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
