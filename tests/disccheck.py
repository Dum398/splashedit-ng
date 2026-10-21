# Build a disc image of examples/courtyard and read it back the way a PS1
# does: raw Mode 2 Form 1 sectors (sync, BCD address, subheader twice, EDC,
# P/Q parity), then the ISO 9660 tree, then the files psxsplash's CD-ROM
# loader opens (SCENE0/SCENE_0.{SPK,VRM,SPU};1); then a disc of two scenes,
# the second under SCENE1/SCENE_1.*; then a disc with the psxsplash licence
# and --volume, the same from disc.json, and a licence of a bad size.
# usage: python3 disccheck.py <splashpack-cli>
import json, os, shutil, struct, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
scene = os.path.join(here, '..', 'examples', 'courtyard', 'courtyard.scene')
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


# Stand-in engines: a PS-X EXE header and the file-name format each loader
# carries (psxsplash src/fileloader.cpp).
def engine(path, marker):
    body = b'PS-X EXE' + bytes(2040) + b'code' * 300 + marker + b'\0' + bytes(100)
    open(path, 'wb').write(body)
    return body


cd_exe = engine(os.path.join(tmp, 'cd.ps-exe'), b'SCENE%d/SCENE_%d.SPK;1')
engine(os.path.join(tmp, 'pc.ps-exe'), b'scene_%d.splashpack')

binpath = os.path.join(tmp, 'game.bin')
r = subprocess.run([cli, 'disc', scene, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', binpath],
                   capture_output=True, text=True)
check('disc exits 0', r.returncode == 0, r.stderr)
r2 = subprocess.run([cli, 'disc', scene, '--engine', os.path.join(tmp, 'pc.ps-exe'), '-o',
                     os.path.join(tmp, 'pc.bin')], capture_output=True, text=True)
check('PCdrv engine refused', r2.returncode != 0 and 'PCdrv' in r2.stderr and
      not os.path.exists(os.path.join(tmp, 'pc.bin')), (r2.returncode, r2.stderr))
if r.returncode:
    sys.exit(1)

raw = open(binpath, 'rb').read()
check('whole raw sectors', len(raw) % 2352 == 0, len(raw))
n = len(raw) // 2352
cue = open(os.path.join(tmp, 'game.cue')).read()
check('cue names the bin as MODE2/2352', 'FILE "game.bin" BINARY' in cue and 'TRACK 01 MODE2/2352' in cue and
      'INDEX 01 00:00:00' in cue, cue)


# EDC: CRC-32 polynomial 0xD8018001, reflected, bit at a time.
def edc(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xD8018001 if c & 1 else 0)
    return c


# ECC: Reed-Solomon over GF(2^8) mod x^8+x^4+x^3+x^2+1 (ECMA-130 annex A),
# written from the generator: parity bytes p0, p1 make sum(v_i) = 0 and
# sum(a^(n-1-i) v_i) = 0 over each codeword of length n.
EXP = [0] * 512
LOG = [0] * 256
x = 1
for i in range(255):
    EXP[i] = x
    LOG[x] = i
    x <<= 1
    if x & 0x100:
        x ^= 0x11D
for i in range(255, 512):
    EXP[i] = EXP[i - 255]


def mul(a, b):
    return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


def div(a, b):
    return 0 if a == 0 else EXP[(LOG[a] - LOG[b]) % 255]


def parity(vals):
    # vals are the n-2 data symbols at powers n-1 .. 2; solve for the two
    # symbols at powers 1 and 0.
    m = len(vals) + 2
    s0 = s1 = 0
    for i, v in enumerate(vals):
        s0 ^= v
        s1 ^= mul(v, EXP[m - 1 - i])
    # p1*a + p0 = s1, p1 + p0 = s0
    p1 = div(s1 ^ s0, EXP[1] ^ 1)
    return p1, s0 ^ p1


def ecc(sec):
    buf = bytearray(sec[12:12 + 2340])
    buf[0:4] = b'\0\0\0\0'  # Mode 2: the address is not covered
    p = bytearray(172)
    for col in range(86):  # P: 43 word columns x 2 byte planes, 24 rows
        lo = col & 1
        w = col >> 1
        vals = [buf[2 * (w + 43 * row) + lo] for row in range(24)]
        p[col], p[col + 86] = parity(vals)
    buf += p
    q = bytearray(104)
    for d in range(52):  # Q: 26 diagonals x 2 planes, 43 symbols
        lo = d & 1
        w = d >> 1
        vals = [buf[2 * ((44 * k + 43 * w) % 1118) + lo] for k in range(43)]
        q[d], q[d + 52] = parity(vals)
    return bytes(p + q)


def bcd(v):
    return (v // 10) * 16 + v % 10


bad_sync = bad_addr = bad_sub = bad_edc = bad_ecc = 0
for lba in range(n):
    s = raw[lba * 2352:(lba + 1) * 2352]
    a = lba + 150
    bad_sync += s[0:12] != b'\0' + b'\xff' * 10 + b'\0'
    bad_addr += s[12:16] != bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 2])
    bad_sub += s[16:20] != s[20:24]
    bad_edc += struct.unpack_from('<I', s, 0x818)[0] != edc(s[16:0x818])
    if lba in (0, 16, 17, 18, 20, 21, 22, n // 2, n - 1):  # the pure-Python ECC is slow
        bad_ecc += s[0x81C:] != ecc(s)
check('sync', bad_sync == 0, bad_sync)
check('BCD address = lba + 150, mode 2', bad_addr == 0, bad_addr)
check('subheader copies agree', bad_sub == 0, bad_sub)
check('EDC on every sector', bad_edc == 0, bad_edc)
check('P/Q parity on sampled sectors', bad_ecc == 0, bad_ecc)

user = b''.join(raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(n))


def sector(i):
    return user[i * 2048:(i + 1) * 2048]


pvd = sector(16)
check('PVD', pvd[0:6] == b'\x01CD001' and pvd[8:19] == b'PLAYSTATION', pvd[0:20])
check('terminator', sector(17)[0:6] == b'\xffCD001')
check('volume size = sectors in the image', struct.unpack_from('<I', pvd, 80)[0] == n)
check('volume name from the scene file', pvd[40:49] == b'COURTYARD', pvd[40:72])


def records(lba, size):
    out = {}
    data = user[lba * 2048:lba * 2048 + size]
    at = 0
    while at < len(data):
        ln = data[at]
        if ln == 0:
            at = (at // 2048 + 1) * 2048
            continue
        rec = data[at:at + ln]
        name = rec[33:33 + rec[32]]
        out[name] = (struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0], rec[25] & 2,
                     at // 2048, struct.unpack_from('>I', rec, 6)[0])
        at += ln
    return out


root_lba, root_size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
root = records(root_lba, root_size)
check('root fits one sector', root_size == 2048, root_size)
check('SYSTEM.CNF in the first root sector', root.get(b'SYSTEM.CNF;1', (0, 0, 0, 1))[3] == 0, root.keys())
cnf = root[b'SYSTEM.CNF;1']
cnf_text = user[cnf[0] * 2048:cnf[0] * 2048 + cnf[1]]
check('SYSTEM.CNF boots PSX.EXE', cnf_text.startswith(b'BOOT = cdrom:\\PSX.EXE;1'), cnf_text)
exe = root[b'PSX.EXE;1']
check('PSX.EXE is the engine', user[exe[0] * 2048:exe[0] * 2048 + exe[1]] == cd_exe)
check('both-endian extents agree', all(v[0] == v[4] for v in root.values()))
names = [k for k in root if k not in (b'\0', b'\1')]
check('root sorted by name', names == sorted(names), names)

ref = os.path.join(tmp, 'ref.splashpack')
r3 = subprocess.run([cli, 'export', scene, '-o', ref, '--lua-bytecode'], capture_output=True, text=True)
check('reference export', r3.returncode == 0, r3.stderr)
s0 = root.get(b'SCENE0')
check('SCENE0 is a directory', s0 is not None and s0[2] == 2, root.keys())
if s0:
    sub = records(s0[0], s0[1])
    check('.. is the root', sub[b'\1'][0] == root_lba)
    for name, ext in ((b'SCENE_0.SPK;1', '.splashpack'), (b'SCENE_0.VRM;1', '.vram'), (b'SCENE_0.SPU;1', '.spu')):
        e = sub.get(name)
        want = open(ref[:-len('.splashpack')] + ext, 'rb').read()
        got = e and user[e[0] * 2048:e[0] * 2048 + e[1]]
        check(name.decode() + ' = export with Lua bytecode', got == want, e)
        if e:
            last = e[0] + max(1, (e[1] + 2047) // 2048) - 1
            sm = raw[last * 2352 + 18]
            check(name.decode() + ' last sector ends record and file', sm == 0x89, hex(sm))

# Path table: root then SCENE0, both pointing at their directory sectors.
lt = struct.unpack_from('<I', pvd, 140)[0]
pt = sector(lt)
check('path table root', struct.unpack_from('<I', pt, 2)[0] == root_lba)
check('path table SCENE0', s0 and pt[18:24] == b'SCENE0' and struct.unpack_from('<I', pt, 12)[0] == s0[0], pt[:24])

# Two scenes: each lands under SCENE<i>/SCENE_<i>, in the order given, as one
# build of the two (splashpack-cli build) writes them: scene 1 as its own
# export, scene 0 as its own but for the render buffer sizes, raised to the
# larger of the two. The second is the courtyard with fog on and a loading
# screen, so the two packs differ (and scene 1 needs the bigger bump allocator).
project = os.path.dirname(scene)
cave = os.path.join(tmp, 'cave.scene')
doc = json.load(open(scene, encoding='utf-8'))
doc['settings'].setdefault('fog', {})['enabled'] = True
# and a loading screen, which lands as SCENE1/SCENE_1.LDG.
doc.setdefault('canvases', []).append({'name': 'Loading', 'elements': [
    {'type': 'box', 'name': 'bg', 'rect': [0, 0, 320, 240], 'color': [0.1, 0.2, 0.5]},
    {'type': 'progress', 'name': 'loading', 'rect': [40, 200, 240, 8], 'color': [1, 1, 1],
     'background': [0.2, 0.2, 0.2], 'value': 0}]})
doc['settings']['loadingCanvas'] = 'Loading'
json.dump(doc, open(cave, 'w', encoding='utf-8'))
ref1 = os.path.join(tmp, 'ref1.splashpack')
r5 = subprocess.run([cli, 'export', cave, '--project', project, '-o', ref1, '--lua-bytecode'], capture_output=True, text=True)
check('reference export of the second scene', r5.returncode == 0, r5.stderr)
# What the disc holds is one build of both scenes (splashpack-cli build, the
# same splash::exportBuild): psxsplash allocates its render buffers once, from
# scene 0, so scene 0 carries the largest of the build.
built = os.path.join(tmp, 'built')
r6 = subprocess.run([cli, 'build', scene, cave, '--project', project, '-o', built, '--lua-bytecode'], capture_output=True, text=True)
check('reference build of both scenes', r6.returncode == 0, r6.stderr)
# The bundled engines have no Lua parser: build stores Lua compiled with or without the flag.
plain = os.path.join(tmp, 'built-plain')
r7 = subprocess.run([cli, 'build', scene, cave, '--project', project, '-o', plain], capture_output=True, text=True)
same = r7.returncode == 0 and all(
    open(os.path.join(plain, 'scene_%d.splashpack' % i), 'rb').read() ==
    open(os.path.join(built, 'scene_%d.splashpack' % i), 'rb').read() for i in (0, 1))
check('build without --lua-bytecode stores the same compiled Lua',
      same and b'\x1bLua' in open(os.path.join(plain, 'scene_0.splashpack'), 'rb').read(), r7.stderr)
two = os.path.join(tmp, 'two.bin')
r4 = subprocess.run([cli, 'disc', scene, cave, '--project', project, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', two],
                    capture_output=True, text=True)
check('two-scene disc exits 0', r4.returncode == 0, r4.stderr)
if r4.returncode == 0:
    raw = open(two, 'rb').read()
    n = len(raw) // 2352
    check('two-scene disc: whole raw sectors, EDC on every sector',
          len(raw) % 2352 == 0 and all(struct.unpack_from('<I', raw, i * 2352 + 0x818)[0] ==
                                       edc(raw[i * 2352 + 16:i * 2352 + 0x818]) for i in range(n)))
    user = b''.join(raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(n))
    pvd = sector(16)
    root_lba = struct.unpack_from('<I', pvd, 158)[0]
    root = records(root_lba, struct.unpack_from('<I', pvd, 166)[0])
    check('volume name from the first scene', pvd[40:49] == b'COURTYARD', pvd[40:72])
    packs = {}
    for i, base in ((0, os.path.join(built, 'scene_0.splashpack')), (1, os.path.join(built, 'scene_1.splashpack'))):
        d = root.get(b'SCENE%d' % i)
        check('SCENE%d is a directory' % i, d is not None and d[2] == 2, root.keys())
        if not d:
            continue
        sub = records(d[0], d[1])
        for ext3, ext in (('SPK', '.splashpack'), ('VRM', '.vram'), ('SPU', '.spu')):
            name = b'SCENE_%d.%s;1' % (i, ext3.encode())
            e = sub.get(name)
            got = e and user[e[0] * 2048:e[0] * 2048 + e[1]]
            want = open(base[:-len('.splashpack')] + ext, 'rb').read()
            check('SCENE%d/%s = the build\'s scene %d' % (i, name.decode(), i), got == want, e)
            if ext3 == 'SPK':
                packs[i] = got
                # Magic and version, as scene 0's pack has them.
                check('SCENE%d/%s has the splashpack header' % (i, name.decode()), bool(got) and got[:2] == b'SP' and
                      got[:4] == open(ref, 'rb').read()[:4], got and got[:4])
    check('the two scenes are different packs', packs.get(0) and packs.get(1) and packs[0] != packs[1])
    # Scene 0 is its own export but for the header's ordering table and bump
    # allocator (bytes 148..155), which are the larger of the two scenes' own.
    lone = [open(f, 'rb').read() for f in (ref, ref1)]
    sizes = [struct.unpack_from('<II', p, 148) for p in lone]
    most = tuple(max(a, b) for a, b in zip(*sizes))
    p0 = packs.get(0) or b''
    check('SCENE0 is scene 0 alone outside the render buffer fields', len(p0) == len(lone[0]) and
          p0[:148] == lone[0][:148] and p0[156:] == lone[0][156:], [i for i in range(min(len(p0), len(lone[0])))
                                                                    if p0[i] != lone[0][i]][:12])
    check('SCENE0 carries the build\'s largest ordering table and bump allocator',
          len(p0) >= 156 and struct.unpack_from('<II', p0, 148) == most, (p0[148:156] if p0 else None, sizes))
    check('SCENE1 is scene 1 alone', packs.get(1) == lone[1])
    ldg = root.get(b'SCENE1') and records(root[b'SCENE1'][0], root[b'SCENE1'][1]).get(b'SCENE_1.LDG;1')
    want = open(ref1[:-len('.splashpack')] + '.loading', 'rb').read() if os.path.exists(
        ref1[:-len('.splashpack')] + '.loading') else None
    got = ldg and user[ldg[0] * 2048:ldg[0] * 2048 + ldg[1]]
    check('SCENE1/SCENE_1.LDG = loader pack of scene 1', bool(want) and got == want and got[:4] == b'LP\x02\x00',
          (ldg, got and got[:4]))
    check('no LDG at the root', not any(k.endswith(b'.LDG;1') for k in root), root.keys())
    s0dir = root.get(b'SCENE0')
    check('no SCENE0/SCENE_0.LDG', s0dir and b'SCENE_0.LDG;1' not in records(s0dir[0], s0dir[1]))
    check('no SCENE2', b'SCENE2' not in root, root.keys())

# A licensed disc with its own volume name: the built-in psxsplash licence at
# LBA 0-11 (Form 1, its "Created with psxsplash" text at LBA 4), LBA 12-15
# blank Form 2 (subheader 00 00 20 00 twice, EDC over 16..2347 at 2348).
lic = os.path.join(tmp, 'lic.bin')
r6 = subprocess.run([cli, 'disc', scene, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', lic,
                     '--licence', 'psxsplash', '--volume', 'MyGame'], capture_output=True, text=True)
check('licensed disc exits 0', r6.returncode == 0, r6.stderr)
if r6.returncode == 0:
    raw = open(lic, 'rb').read()
    sec = lambda i: raw[i * 2352:(i + 1) * 2352]
    check('PVD volume id from --volume', sec(16)[24 + 40:24 + 72] == b'MYGAME'.ljust(32), sec(16)[24 + 40:24 + 72])
    check('LBA 4 carries the licence text', b'Created wit' in sec(4)[24:24 + 64] and
          b'psxsplash' in sec(4)[24:24 + 64], sec(4)[24:24 + 64])
    check('LBA 0-11 Form 1 data, EDC and parity', all(
        sec(i)[16:24] == b'\0\0\x08\0\0\0\x08\0' and
        struct.unpack_from('<I', sec(i), 0x818)[0] == edc(sec(i)[16:0x818]) for i in range(12)) and
        sec(4)[0x81C:] == ecc(sec(4)), [sec(i)[16:24] for i in range(12)])
    check('LBA 12-15 blank Form 2 with EDC at 2348', all(
        sec(i)[16:24] == b'\0\0\x20\0\0\0\x20\0' and sec(i)[24:2348] == bytes(2324) and
        struct.unpack_from('<I', sec(i), 2348)[0] == edc(sec(i)[16:2348]) for i in range(12, 16)),
        [sec(i)[16:24] for i in range(12, 16)])
    check('the rest as without a licence', sec(17) == open(binpath, 'rb').read()[17 * 2352:18 * 2352])

# disc.json in the project gives the volume and the licence; --volume and
# --licence none override it.
proj = os.path.join(tmp, 'proj')
shutil.copytree(os.path.dirname(scene), proj)
json.dump({'volume': 'from json', 'licence': 'psxsplash'}, open(os.path.join(proj, 'disc.json'), 'w'))
pj = os.path.join(tmp, 'pj.bin')
r7 = subprocess.run([cli, 'disc', scene, '--project', proj, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', pj],
                    capture_output=True, text=True)
check('disc.json disc exits 0', r7.returncode == 0, r7.stderr)
if r7.returncode == 0:
    raw = open(pj, 'rb').read()
    check('disc.json volume and licence', raw[16 * 2352 + 24 + 40:16 * 2352 + 24 + 72] == b'FROMJSON'.ljust(32) and
          b'Created wit' in raw[4 * 2352 + 24:4 * 2352 + 88], raw[16 * 2352 + 64:16 * 2352 + 96])
r8 = subprocess.run([cli, 'disc', scene, '--project', proj, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', pj,
                     '--licence', 'none', '--volume', 'other'], capture_output=True, text=True)
if r8.returncode == 0:
    raw = open(pj, 'rb').read()
    check('--licence none and --volume override disc.json',
          raw[16 * 2352 + 24 + 40:16 * 2352 + 24 + 72] == b'OTHER'.ljust(32) and
          raw[:16 * 2352] == open(binpath, 'rb').read()[:16 * 2352], raw[16 * 2352 + 64:16 * 2352 + 96])
else:
    check('--licence none and --volume override disc.json', False, r8.stderr)

# A licence of the wrong size fails, names the size, and writes nothing.
short = os.path.join(tmp, 'short.dat')
open(short, 'wb').write(bytes(1000))
badbin = os.path.join(tmp, 'bad.bin')
r9 = subprocess.run([cli, 'disc', scene, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', badbin,
                     '--licence', short], capture_output=True, text=True)
check('bad-size licence refused', r9.returncode != 0 and '1000' in r9.stderr and not os.path.exists(badbin),
      (r9.returncode, r9.stderr))

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
