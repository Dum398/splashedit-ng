# Export skin/skin.scene plus variations and read the skinned-mesh table back
# the way psxsplash's splashpack.cpp does. Checks the bone per triangle vertex,
# frame layout per clip, and that each baked matrix moves the exported
# vertices to where the clip puts them (computed here independently, in the
# scene's Y-up space, then converted like a vertex).
# usage: python3 skincheck.py <splashpack-cli>
import json, math, os, struct, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


base = json.load(open(os.path.join(here, 'skin', 'skin.scene')))
GTE = base['settings']['gteScaling']


def export(edit=lambda s: None, files=None, stats=False):
    d = tempfile.mkdtemp(dir=tmp)
    for n in ('arm.mesh', 'bend.anim', 'raise.anim', 'play.lua'):
        open(os.path.join(d, n), 'w').write(open(os.path.join(here, 'skin', n)).read())
    for n, content in (files or {}).items():
        json.dump(content, open(os.path.join(d, n), 'w'))
    s = json.loads(json.dumps(base))
    edit(s)
    json.dump(s, open(os.path.join(d, 's.scene'), 'w'))
    out = os.path.join(d, 's.splashpack')
    r = subprocess.run([cli, 'export', os.path.join(d, 's.scene'), '-o', out] + (['--stats'] if stats else []),
                       capture_output=True, text=True)
    return r, (open(out, 'rb').read() if r.returncode == 0 else b'')


def read(d):
    version, nlua, ngo = struct.unpack_from('<HHH', d, 2)
    header = 156 if version >= 25 else 148 if version >= 24 else 144
    gos = []
    for i in range(ngo):
        o = header + nlua * 8 + i * 92
        mesh, = struct.unpack_from('<I', d, o)
        poly, = struct.unpack_from('<H', d, o + 52)
        flags, = struct.unpack_from('<I', d, o + 56)
        tris = [[struct.unpack_from('<hhh', d, mesh + t * 52 + k * 6) for k in range(3)] for t in range(poly)]
        gos.append(dict(flags=flags, tris=tris))
    n, = struct.unpack_from('<H', d, 112)
    table, = struct.unpack_from('<I', d, 116)
    skins = []
    for i in range(n):
        doff, nlen, _, _, _, noff = struct.unpack_from('<IBBBBI', d, table + i * 12)
        go, bones, nclip = struct.unpack_from('<HBB', d, doff)
        p = doff + 4
        poly = len(gos[go]['tris'])
        idx = list(d[p:p + poly * 3])
        p = (p + poly * 3 + 3) & ~3
        clips = []
        for c in range(nclip):
            start = p
            cl = d[p]
            name = d[p + 1:p + 1 + cl].decode()
            p += 1 + cl + 1
            flags, fps = d[p], d[p + 1]
            p += 2
            p = (p + 1) & ~1
            fc, = struct.unpack_from('<H', d, p)
            p += 2
            frames = [[struct.unpack_from('<12h', d, p + (f * bones + b) * 24) for b in range(bones)] for f in range(fc)]
            p += fc * bones * 24
            clips.append(dict(name=name, loop=flags & 1, fps=fps, frames=frames, span=p - start))
        bind, joints = [], []
        if version >= 26:
            p = (p + 1) & ~1
            bind = [struct.unpack_from('<3h', d, p + b * 6) for b in range(bones)]
            p += bones * 6
            for b in range(bones):
                jl = d[p]
                joints.append(d[p + 1:p + 1 + jl].decode())
                p += 1 + jl + 1
            check('joint names end where the object name starts', p == noff, (p, noff))
        objname = d[noff:d.index(b'\0', noff)].decode()
        skins.append(dict(go=go, bones=bones, idx=idx, clips=clips, name=objname, nlen=nlen, bind=bind,
                          joints=joints, version=version, doff=doff,
                          end=noff + len(objname.encode()) + 1))
    return gos, skins


def apply(m, v):
    # Engine: rotation 4.12 times a 4.12 vertex, plus the translation.
    return tuple(sum(m[r * 3 + c] * v[c] for c in range(3)) / 4096 + m[9 + r] for r in range(3))


def zrot(deg, p):
    a = math.radians(deg)
    return (p[0] * math.cos(a) - p[1] * math.sin(a), p[0] * math.sin(a) + p[1] * math.cos(a), p[2])


def psx(p, scale=(1, 1, 1)):
    return (p[0] * scale[0] / GTE * 4096, -p[1] * scale[1] / GTE * 4096, p[2] * scale[2] / GTE * 4096)


def close(a, b, tol=2.0):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


# ---- base scene
r, d = export()
check('base export succeeds', r.returncode == 0, r.stderr)
gos, skins = read(d)
check('one skinned mesh', len(skins) == 1, len(skins))
sk = skins[0]
check('skinned object flag 0x10 + active', gos[0]['flags'] == 0x11, hex(gos[0]['flags']))
check('object name and bone/clip counts', (sk['name'], sk['go'], sk['bones'], len(sk['clips'])) == ('Arm', 0, 2, 2),
      (sk['name'], sk['go'], sk['bones'], len(sk['clips'])))

tris = gos[0]['tris']
# make_arm.py writes the shoulder box's 12 triangles, then the elbow box's 12;
# the exporter keeps triangle order. (Position alone cannot tell them apart:
# both boxes have a face at y = 1.)
want = [0] * 36 + [1] * 36
check('bone per vertex follows the box it belongs to', len(tris) == 24 and sk['idx'] == want, sk['idx'])

bend, raise_ = sk['clips']
check('clip names and flags', (bend['name'], bend['loop'], bend['fps'], raise_['name'], raise_['loop']) ==
      ('bend', 1, 15, 'raise', 0))
# 1.0 s loop at 15 fps: 15 frames over [0, 1). 2.4.0 wrote 16, the last equal to the first.
check('looping clip stores no duplicate end frame', len(bend['frames']) == 15, len(bend['frames']))
check('looping clip frames 0 and last differ', bend['frames'][0] != bend['frames'][-1])
# 0.5 s one-shot: frames at i/15 up to the end pose.
check('one-shot frame count', len(raise_['frames']) == 9, len(raise_['frames']))
ident = (4096, 0, 0, 0, 4096, 0, 0, 0, 4096, 0, 0, 0)
check('rest pose is identity', all(b == ident for b in bend['frames'][0]), bend['frames'][0])

top = [v for t in tris for v in t if -v[1] / 4096 * GTE > 1.99]  # upper box top vertices, packed
check('found the upper box top vertices', len(top) >= 4, len(top))


def elbow_pose(deg, p):
    q = zrot(deg, (p[0], p[1] - 1, p[2]))
    return (q[0], q[1] + 1, q[2])


ok = True
for f, frame in enumerate(bend['frames']):
    t = f / 15
    deg = 90 * (t / 0.5 if t <= 0.5 else (1 - t) / 0.5)
    for v in top:
        scene = (v[0] / 4096 * GTE, -v[1] / 4096 * GTE, v[2] / 4096 * GTE)
        if not close(apply(frame[1], v), psx(elbow_pose(deg, scene))):
            ok = False
check('bend: every frame moves the top vertices to the slerped elbow angle', ok)
end = raise_['frames'][-1]
ok = all(close(apply(end[1], v), psx(zrot(-45, (v[0] / 4096 * GTE, -v[1] / 4096 * GTE, v[2] / 4096 * GTE)))) for v in top)
check('raise: last frame is the end pose, child follows the parent joint', ok)
check('raise: frame 7 is t=7/15', all(close(apply(raise_['frames'][7][0], v),
                                              psx(zrot(-45 * (7 / 15) / 0.5, (v[0] / 4096 * GTE, -v[1] / 4096 * GTE, v[2] / 4096 * GTE))))
                                        for v in top))

# ---- non-uniform scale: matrices are conjugated by the scale the vertices carry
S = (2.0, 0.5, 1.0)
r, d = export(lambda s: s['objects'][0]['transform'].__setitem__('scale', list(S)))
gos2, skins2 = read(d)
top2 = [v for t in gos2[0]['tris'] for v in t if -v[1] / 4096 * GTE / S[1] > 1.99]
frame = skins2[0]['clips'][0]['frames'][7]
deg = 90 * (7 / 15) / 0.5
ok = len(top2) >= 4
for v in top2:
    scene = (v[0] / 4096 * GTE / S[0], -v[1] / 4096 * GTE / S[1], v[2] / 4096 * GTE / S[2])
    ok = ok and close(apply(frame[1], v), psx(elbow_pose(deg, scene), S))
check('non-uniform scale: bent vertices stay on the scaled arm', ok)
check('non-uniform scale: bind positions carry the scale', close(skins2[0]['bind'][1], psx((0, 1, 0), S)),
      skins2[0]['bind'])

# ---- v26 tail: joint bind positions in the vertices' space, then joint names
check('version 27', sk['version'] == 27, sk['version'])
check('joint names', sk['joints'] == ['shoulder', 'elbow'], sk['joints'])
check('bind positions: shoulder at the origin, elbow at y = 1',
      len(sk['bind']) == 2 and close(sk['bind'][0], psx((0, 0, 0))) and close(sk['bind'][1], psx((0, 1, 0))),
      sk['bind'])

# ---- no skin component: nothing written, flag clear (control for the reads above)
r, d = export(lambda s: s['objects'][0]['components'].pop(1))
gos3, skins3 = read(d)
check('without a skin component: no table, flag clear', r.returncode == 0 and skins3 == [] and gos3[0]['flags'] == 1,
      (r.stderr, len(skins3), gos3 and hex(gos3[0]['flags'])))


# ---- errors instead of output the engine would mishandle
def err(name, edit, needle, files=None):
    r, _ = export(edit, files)
    check(name, r.returncode != 0 and needle in r.stderr, r.stderr.strip()[:200])


err('no clips is an error', lambda s: s['objects'][0]['components'][1].__setitem__('clips', []), 'never drawn')
bad_joint = json.load(open(os.path.join(here, 'skin', 'bend.anim')))
bad_joint['channels'][0]['joint'] = 'wrist'
err('unknown joint is an error', lambda s: s['objects'][0]['components'][1].__setitem__('clips', ['x.anim']),
    "no joint named 'wrist'", {'x.anim': bad_joint})
long_name = json.load(open(os.path.join(here, 'skin', 'bend.anim')))
long_name['name'] = 'a' * 25
err('clip name over 24 characters is an error',
    lambda s: s['objects'][0]['components'][1].__setitem__('clips', ['x.anim']), '1..24', {'x.anim': long_name})
dup = json.load(open(os.path.join(here, 'skin', 'bend.anim')))
err('two clips with one name is an error',
    lambda s: s['objects'][0]['components'][1].__setitem__('clips', ['bend.anim', 'x.anim']), 'two clips', {'x.anim': dup})
plain = json.load(open(os.path.join(here, 'skin', 'arm.mesh')))
del plain['skin']
err('mesh without a skeleton is an error',
    lambda s: s['objects'][0]['components'][0].__setitem__('mesh', 'x.mesh'), 'no skeleton', {'x.mesh': plain})
err('skin without a mesh is an error', lambda s: s['objects'][0]['components'].pop(0), 'needs a mesh')
err('fps 31 is an error', lambda s: s['objects'][0]['components'][1].__setitem__('fps', 31), 'fps')


# ---- --stats skin lines, against the table read back from the same pack
r, d = export(stats=True)
check('--stats export succeeds', r.returncode == 0, r.stderr)
gos, skins = read(d)
lines = r.stdout.splitlines()
skin_lines = [l for l in lines if l.startswith('skin ')]
clip_lines = [l for l in skin_lines if l.startswith('skin clip ')]
mesh_lines = [l for l in skin_lines if not l.startswith('skin clip ')]
want_mesh, want_clip, total = [], [], 0
for sk in skins:
    block = sk['end'] - sk['doff']
    bl = [l for l in mesh_lines if l.endswith(' ' + sk['name'])]
    check('one skin line for ' + sk['name'], len(bl) == 1, mesh_lines)
    if bl:
        f = dict(kv.split('=') for kv in bl[0].split()[1:5])
        check('skin line counts match the table', (int(f['bones']), int(f['triangles']), int(f['clips'])) ==
              (sk['bones'], len(gos[sk['go']]['tris']), len(sk['clips'])), bl[0])
        # 12-byte table entry + the block; up to 3 bytes of align4 padding sit before doff.
        b = int(f['bytes'])
        check('skin bytes cover the entry and its data block', 12 + block <= b <= 12 + block + 3, (b, 12 + block))
        total += b
    for c in sk['clips']:
        want_clip.append('skin clip fps=%d frames=%d bytes=%d loop=%d %s/%s' % (c['fps'], len(c['frames']), c['span'],
                                                                                 c['loop'], sk['name'], c['name']))
check('one clip line per baked clip, with fps, frames, bytes and the loop flag', clip_lines == want_clip, (clip_lines, want_clip))
check('skins total line', ('skins=%d bytes=%d' % (len(skins), total)) in lines, [l for l in lines if l.startswith('skins=')])
r, d = export(lambda s: s['objects'][0]['components'].pop(1), stats=True)
check('a scene without skins prints no skin lines', r.returncode == 0 and
      not [l for l in r.stdout.splitlines() if l.startswith('skin')], r.stdout[-300:])

print(f'{checks - fails}/{checks} passed')
sys.exit(1 if fails else 0)
