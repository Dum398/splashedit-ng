# Clips from another model file onto a skinned model (#237), through
# `splashpack-cli import-clips`, the editor's Add clips from model:
#  - the key oracle: Nod/Big read from skinimport/rig_nod.<ext> (the armature
#    alone, with an extra joint Tail) onto rig_host.<ext> (the rig with Wave
#    only) is the .anim importing rig.<ext> (both clips) writes, byte for byte
#    and channel for channel, for glTF and FBX;
#  - the Tail channel is left out with one warning giving the count, and a file
#    with other joint names (rig_alien) is refused;
#  - the host's import settings (up axis, scale) convert the added clips as
#    they convert the host's own, and a clip name taken by the host is numbered;
#  - names kept in the host's sidecar (clipNames) are what the import writes;
#  - an exported splashpack carries those names;
#  - a scene saved before clipSources existed loads and saves byte for byte.
# usage: python3 multiclipcheck.py <splashpack-cli>
import json, os, shutil, struct, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
rigdir = os.path.join(here, 'skinimport')
sys.path.insert(0, rigdir)
import make_rig

tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(*args):
    return subprocess.run([cli] + list(args), capture_output=True, text=True)


def project(name, model=None, sidecar=None):
    """A project with skinimport/<model> copied into models/ (and a sidecar), imported."""
    p = os.path.join(tmp, name)
    os.makedirs(os.path.join(p, 'models'))
    if model:
        dst = os.path.join(p, 'models', model)
        shutil.copyfile(os.path.join(rigdir, model), dst)
        if sidecar is not None: json.dump(sidecar, open(dst + '.import.json', 'w'))
        r = run('import', dst, '--project', p)
        check('%s: %s imports' % (name, model), r.returncode == 0, r.stderr)
    return p


def anim(p, rel):
    return json.load(open(os.path.join(p, rel), encoding='utf-8'))


def same_channels(a, b, tol=1e-6):
    """Channel for channel within tol; the first difference, or None."""
    if a['length'] != b['length'] or a['loop'] != b['loop']: return 'length/loop %s %s' % ((a['length'], a['loop']), (b['length'], b['loop']))
    if len(a['channels']) != len(b['channels']): return 'channel count %d %d' % (len(a['channels']), len(b['channels']))
    for x, y in zip(a['channels'], b['channels']):
        for k in ('joint', 'property', 'interpolation'):
            if x[k] != y[k]: return '%s %s %s' % (k, x[k], y[k])
        for k in ('times', 'values'):
            if len(x[k]) != len(y[k]) or any(abs(u - v) > tol for u, v in zip(x[k], y[k])):
                return '%s %s %s: %s vs %s' % (x['joint'], x['property'], k, x[k][:8], y[k][:8])
    return None


# 0. The fixtures are what make_rig.py writes.
for n, opts in make_rig.EXTRA.items():
    check('skinimport/rig_%s.gltf and .fbx are what make_rig.py writes' % n,
          open(os.path.join(rigdir, 'rig_%s.gltf' % n)).read() == make_rig.gltf(**opts) + '\n' and
          open(os.path.join(rigdir, 'rig_%s.fbx' % n)).read() == make_rig.fbx(**opts))

# 1. The key oracle, per format.
for ext in ('gltf', 'fbx'):
    combined = project('combined_' + ext, 'rig.' + ext)
    host = project('host_' + ext, 'rig_host.' + ext)
    check(ext + ': the host has Wave alone', sorted(os.listdir(os.path.join(host, 'models', 'rig_host'))) == ['Body.mesh', 'Wave.anim'],
          os.listdir(os.path.join(host, 'models', 'rig_host')))
    r = run('import-clips', os.path.join(rigdir, 'rig_nod.' + ext), '--project', host, '--model', 'models/rig_host.' + ext)
    rel = 'models/rig_host/clips/rig_nod_%s/Nod_Big.anim' % ext
    check(ext + ': import-clips writes ' + rel, r.returncode == 0 and ('clip %s take Nod/Big' % rel) in r.stdout.splitlines(),
          r.stdout + r.stderr)
    if r.returncode != 0: continue
    a = open(os.path.join(combined, 'models/rig/Nod_Big.anim'), 'rb').read()
    b = open(os.path.join(host, rel), 'rb').read()
    check(ext + ': Nod/Big from the separate file is byte for byte the one from the combined file', a == b, (len(a), len(b)))
    diff = same_channels(json.loads(a), json.loads(b))
    check(ext + ': and channel for channel within 1e-6', diff is None, diff)
    check(ext + ': and has the same name', json.loads(a)['name'] == json.loads(b)['name'] == 'Nod_Big', (json.loads(a)['name'], json.loads(b)['name']))
    warned = [l for l in r.stdout.splitlines() if 'not a joint' in l]
    check(ext + ': Tail, a joint the host lacks, is left out with one warning giving the count',
          'dropped 1' in r.stdout.splitlines() and len(warned) == 1 and warned[0].startswith('warning: 1 animated node is not a joint') and
          "'Tail'" in warned[0] and 'Tail' not in [c['joint'] for c in json.loads(b)['channels']], r.stdout)
    r = run('import-clips', os.path.join(rigdir, 'rig_alien.' + ext), '--project', host, '--model', 'models/rig_host.' + ext)
    check(ext + ': a file sharing no joint with the host is refused, nothing written',
          r.returncode == 1 and 'rig_alien.%s shares no joint with the skeleton' % ext in r.stderr and
          not os.path.exists(os.path.join(host, 'models/rig_host/clips/rig_alien_' + ext)), r.stderr)

# 2. The host's settings convert the added clips: up Z and scale 2 on both
# sides; the whole rig.gltf added onto the host, its Wave numbered past the
# host's own Wave.
settings = {'up': 'z', 'scale': 2}
combined = project('combined_z', 'rig.gltf', settings)
host = project('host_z', 'rig_host.gltf', settings)
r = run('import-clips', os.path.join(rigdir, 'rig.gltf'), '--project', host, '--model', 'models/rig_host.gltf')
check('rig.gltf onto the Z-up, scale 2 host', r.returncode == 0, r.stderr)
if r.returncode == 0:
    wa = anim(combined, 'models/rig/Wave.anim')
    wb = anim(host, 'models/rig_host/clips/rig_gltf/Wave.anim')
    check('Wave added from rig.gltf is converted as the host converts its own', same_channels(wa, wb) is None, same_channels(wa, wb))
    check('and is named Wave_2: the host has a Wave', (wa['name'], wb['name']) == ('Wave', 'Wave_2'), (wa['name'], wb['name']))
    check('Nod_Big likewise, byte for byte', open(os.path.join(combined, 'models/rig/Nod_Big.anim'), 'rb').read() ==
          open(os.path.join(host, 'models/rig_host/clips/rig_gltf/Nod_Big.anim'), 'rb').read())
    plain = anim(project('combined_y', 'rig.gltf'), 'models/rig/Wave.anim')
    check('(the settings change the keys: without them Wave differs)', same_channels(plain, wb) is not None)

# 3. Names kept in the host's sidecar are what the import writes, the file
# names staying; the export carries them.
names = {'clipNames': {'Wave': 'walk', 'clips/rig_nod_gltf/Nod_Big': 'nod'}}
host = project('named', 'rig_host.gltf', names)
r = run('import-clips', os.path.join(rigdir, 'rig_nod.gltf'), '--project', host, '--model', 'models/rig_host.gltf')
check('import-clips onto the named host', r.returncode == 0, r.stderr)
check('the host clip takes its name from the sidecar, the file keeps the take',
      anim(host, 'models/rig_host/Wave.anim')['name'] == 'walk')
check('so does the added clip', r.returncode == 0 and anim(host, 'models/rig_host/clips/rig_nod_gltf/Nod_Big.anim')['name'] == 'nod')
r = run('import', os.path.join(host, 'models', 'rig_host.gltf'), '--project', host)
check('and importing the host again keeps the name', r.returncode == 0 and anim(host, 'models/rig_host/Wave.anim')['name'] == 'walk')
scene = json.load(open(os.path.join(here, 'skinimport', 'skin_saved.scene'), encoding='utf-8'))
obj = scene['objects'][0]
obj['components'][0]['mesh'] = 'models/rig_host.mesh'
obj['components'][1]['clips'] = ['models/rig_host/Wave.anim', 'models/rig_host/clips/rig_nod_gltf/Nod_Big.anim']
obj['components'][1]['clipSources'] = ['models/rig_nod.gltf']
scene['settings'].pop('script', None)
json.dump(scene, open(os.path.join(host, 's.scene'), 'w'))
out = os.path.join(host, 's.splashpack')
r = run('export', os.path.join(host, 's.scene'), '-o', out)
check('the scene with the renamed clips exports', r.returncode == 0, r.stderr[-400:])


def skin_clip_names(d):
    """Clip names per skinned mesh, read the way psxsplash's splashpack.cpp does (see skincheck.py)."""
    version, nlua, ngo = struct.unpack_from('<HHH', d, 2)
    header = 156 if version >= 25 else 148 if version >= 24 else 144
    polys = []
    for i in range(ngo):
        o = header + nlua * 8 + i * 92
        polys.append(struct.unpack_from('<H', d, o + 52)[0])
    n, = struct.unpack_from('<H', d, 112)
    table, = struct.unpack_from('<I', d, 116)
    skins = []
    for i in range(n):
        doff, = struct.unpack_from('<I', d, table + i * 12)
        go, bones, nclip = struct.unpack_from('<HBB', d, doff)
        p = (doff + 4 + polys[go] * 3 + 3) & ~3
        clips = []
        for c in range(nclip):
            cl = d[p]
            clips.append(d[p + 1:p + 1 + cl].decode())
            p += 1 + cl + 1 + 2
            p = (p + 1) & ~1
            fc, = struct.unpack_from('<H', d, p)
            p += 2 + fc * bones * 24
        skins.append(clips)
    return skins


if r.returncode == 0:
    got = skin_clip_names(open(out, 'rb').read())
    check('the splashpack carries the names walk and nod', got == [['walk', 'nod']], got)

# 4. A scene saved before clipSources existed (skinimport/skin_saved.scene,
# written by the editor at 37ae5d1) loads and saves byte for byte; a skin's
# clipSources survive load and save.
old = os.path.join(here, 'skinimport', 'skin_saved.scene')
re_ = os.path.join(tmp, 'old.scene')
r = run('resave', old, re_)
check('an old skin scene saves byte for byte', r.returncode == 0 and open(old, 'rb').read() == open(re_, 'rb').read(), r.stderr)
check('with no clipSources key', r.returncode == 0 and 'clipSources' not in open(re_).read())
r = run('resave', os.path.join(host, 's.scene'), re_)
check('clipSources survive a load and save',
      r.returncode == 0 and json.load(open(re_))['objects'][0]['components'][1].get('clipSources') == ['models/rig_nod.gltf'], r.stderr)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
