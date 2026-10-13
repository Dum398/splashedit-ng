# Export scenes with memory card settings and read the table back the way
# psxsplash's splashpack.cpp does: header u32 memcardTableOffset at byte 120
# (v21+, 0 = none) pointing at the 464-byte SPLASHPACKMemcard record
# {char region[2] @0x00; char product[10] @0x02; char title[32] @0x0C;
#  u8 iconFrameCount @0x2C; u8 pad @0x2D; u16 pad @0x2E; u16 clut[16] @0x30;
#  u8 iconPixels[3][128] @0x50}. Icons are decoded as the BIOS draws them:
# 16x16 4bpp, top row first, low nibble = left pixel, colour = clut[index].
# Also: a scene round trip of settings.memCard, and the over-limit checks.
# usage: python3 memcardcheck.py <splashpack-cli>
import json, os, struct, subprocess, sys, tempfile, zlib

cli = sys.argv[1]
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def png(path, rows):
    # rows: top row first, each a list of (r, g, b, a) 0..255
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return path


RED, BLUE, WHITE = (255, 0, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)
GREEN, BLACK, YELLOW, CLEAR = (0, 255, 0, 255), (0, 0, 0, 255), (255, 255, 0, 255), (0, 0, 0, 0)
# Frame 0: white top row (row order), then red left half / blue right half
# (nibble order). Frame 1: green with a black bottom row. Frame 2: yellow,
# a colour frame 0 does not have (shared palette), with a transparent column.
frame0 = [[WHITE] * 16] + [[RED] * 8 + [BLUE] * 8 for _ in range(15)]
frame1 = [[GREEN] * 16 for _ in range(15)] + [[BLACK] * 16]
frame2 = [[CLEAR] + [YELLOW] * 15 for _ in range(16)]
icons = [png(os.path.join(tmp, 'icon%d.png' % i), f) for i, f in enumerate((frame0, frame1, frame2))]
big = png(os.path.join(tmp, 'big.png'), [[RED] * 32 for _ in range(32)])


def scene(mc=None):
    s = {'format': 'splashedit-ng/scene', 'version': 1, 'settings': {}, 'objects': []}
    if mc is not None:
        s['settings']['memCard'] = mc
    return s


def export(s):
    path = os.path.join(tmp, 'm.scene')
    json.dump(s, open(path, 'w'))
    out = os.path.join(tmp, 'm.splashpack')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([cli, 'export', path, '-o', out], capture_output=True, text=True)
    return r, (open(out, 'rb').read() if r.returncode == 0 else b'')


def table(d):
    version, = struct.unpack_from('<H', d, 2)
    off, = struct.unpack_from('<I', d, 120)
    if off == 0:
        return version, off, None
    # Field by field at the offsets SPLASHPACKMemcard's comments give.
    t = dict(region=d[off:off + 2], product=d[off + 2:off + 12], title=d[off + 12:off + 44],
             frames=d[off + 0x2C], pad0=d[off + 0x2D], pad1=struct.unpack_from('<H', d, off + 0x2E)[0],
             clut=list(struct.unpack_from('<16H', d, off + 0x30)),
             pixels=[d[off + 0x50 + f * 128:off + 0x50 + (f + 1) * 128] for f in range(3)],
             end=off + 464)
    return version, off, t


def need(t, what):
    # Without a table nothing below can be read: stop here, as a failure.
    if t is None:
        check(what + ': memcardTableOffset is 0, no table to read', False)
        print('%d checks, %d failed' % (checks, fails))
        sys.exit(1)
    return t


def decode(t, f):
    # 16 rows, top first, of clut words.
    px = t['pixels'][f]
    return [[t['clut'][(px[y * 8 + x // 2] >> (4 * (x & 1))) & 0xF] for x in range(16)] for y in range(16)]


def to5(c):
    return int(c / 255 * 31 + 0.5)


def matches(word, rgba):
    if rgba[3] == 0:
        return word == 0
    return word != 0 and (word & 31, (word >> 5) & 31, (word >> 10) & 31) == tuple(to5(c) for c in rgba[:3])


def frame_ok(t, f, rows):
    got = decode(t, f)
    bad = [(x, y, hex(got[y][x]), rows[y][x]) for y in range(16) for x in range(16) if not matches(got[y][x], rows[y][x])]
    return not bad, bad[:4]


# ---- export arm: three frames, every field set
mc = {'enabled': True, 'region': 'BE', 'product': 'SLES-01234', 'title': 'MEMCARD CHECK SAVE', 'icons': icons}
r, d = export(scene(mc))
check('export', r.returncode == 0, r.stderr)
version, off, t = table(d)
check('version >= 21 (loader gate)', version >= 21, version)
check('memcardTableOffset set and 4-aligned', off != 0 and off % 4 == 0, off)
check('464-byte record inside the file', t and t['end'] <= len(d), (off, len(d)))
need(t, 'three frames')
check('region "BE" at +0x00', t['region'] == b'BE', t['region'])
check('product "SLES-01234" at +0x02', t['product'] == b'SLES-01234', t['product'])
check('title at +0x0C, zero padded to 32', t['title'] == b'MEMCARD CHECK SAVE'.ljust(32, b'\0'), t['title'])
check('iconFrameCount 3 at +0x2C', t['frames'] == 3, t['frames'])
check('pad bytes 0', t['pad0'] == 0 and t['pad1'] == 0, (t['pad0'], t['pad1']))
check('transparent texels use clut word 0', 0 in t['clut'], t['clut'])
for f, rows in enumerate((frame0, frame1, frame2)):
    ok, bad = frame_ok(t, f, rows)
    check('frame %d decodes to its image with the shared clut' % f, ok, bad)
check('no warnings for a complete config', 'warning' not in r.stderr, r.stderr)

# ---- one frame: count 1, unused frames zero
r, d = export(scene(dict(mc, icons=icons[:1])))
t1 = need(table(d)[2], 'one icon')
check('one icon: frameCount 1', r.returncode == 0 and t1['frames'] == 1, (r.stderr, t1 and t1['frames']))
check('one icon: frame 0 decodes', frame_ok(t1, 0, frame0)[0], frame_ok(t1, 0, frame0)[1])
check('one icon: frames 1 and 2 are zero', t1['pixels'][1] == bytes(128) and t1['pixels'][2] == bytes(128))
check('one icon, no transparency: no clut word 0 needed', 0 not in t1['clut'][:3], t1['clut'])

# ---- defaults: enabled only, no icon
r, d = export(scene({'enabled': True}))
t2 = need(table(d)[2], 'defaults')
check('defaults: BA / SLUS-00000 / PSXSPLASH SAVE',
      t2 and (t2['region'], t2['product'], t2['title']) == (b'BA', b'SLUS-00000', b'PSXSPLASH SAVE'.ljust(32, b'\0')),
      t2)
check('no icon: frameCount 1 (loader clamps to 1..3), blank clut and pixels',
      t2['frames'] == 1 and t2['clut'] == [0] * 16 and all(p == bytes(128) for p in t2['pixels']), t2)
check('no icon: warning says to add a 16x16 image', 'add a 16x16 image' in r.stderr, r.stderr)

# ---- off: offset 0, and the same bytes as a scene with no memCard at all
r0, d0 = export(scene())
_, off0, _ = table(d0)
check('no memCard: offset 0', r0.returncode == 0 and off0 == 0, off0)
r1, d1 = export(scene(dict(mc, enabled=False, title='X' * 40)))
check('disabled: offset 0, nothing validated, same bytes as none', r1.returncode == 0 and table(d1)[1] == 0 and d1 == d0,
      r1.stderr)

# ---- exactly 32 characters fills the title with no terminator (the loader adds it)
r, d = export(scene({'enabled': True, 'title': 'T' * 32, 'icons': icons[:1]}))
check('32-character title fills the field', r.returncode == 0 and need(table(d)[2], '32 characters')['title'] == b'T' * 32,
      r.stderr)

# ---- over-limit and bad values: refused with a message saying what to do
for name, bad, words in (
        ('title 33 characters', {'title': 'T' * 33}, ('title', '32', 'shorten')),
        ('product 11 characters', {'product': 'SLUS-000000'}, ('product', '10', 'shorten')),
        ('empty product', {'product': ''}, ('product', 'SLUS-00000')),
        ('region of 1 letter', {'region': 'B'}, ('region', 'BA', 'BE', 'BI')),
        ('non-ASCII title', {'title': 'Savé'}, ('title', 'ASCII')),
        ('4 icon frames', {'icons': icons + icons[:1]}, ('icon', '3')),
        ('32x32 icon', {'icons': [big]}, ('16x16', '32x32', 'resize')),
        ('missing icon', {'icons': [os.path.join(tmp, 'nope.png')]}, ('icon', 'nope.png'))):
    rb, _ = export(scene(dict(mc, **bad)))
    check('%s is refused' % name, rb.returncode != 0 and all(w in rb.stderr for w in words), (rb.returncode, rb.stderr))
rw, _ = export(scene(dict(mc, region='BX', title='SAVE - 1')))
check('unknown region and "-" in title only warn',
      rw.returncode == 0 and "region 'BX'" in rw.stderr and "'?'" in rw.stderr, (rw.returncode, rw.stderr))

# ---- scene round trip
def resave(s):
    src, dst = os.path.join(tmp, 'rs.scene'), os.path.join(tmp, 'rs2.scene')
    json.dump(s, open(src, 'w'))
    rr = subprocess.run([cli, 'resave', src, dst], capture_output=True, text=True)
    return rr, (json.load(open(dst)) if rr.returncode == 0 else {})


full = dict(mc, futureKey={'kept': 1})
rr, saved = resave(scene(full))
check('resave keeps every memCard field and unknown keys', saved.get('settings', {}).get('memCard') == full,
      (rr.stderr, saved.get('settings')))
rr, saved = resave(scene({'enabled': True, 'region': 'BA', 'product': 'SLUS-00000', 'title': 'PSXSPLASH SAVE', 'icons': []}))
check('defaults are omitted', saved.get('settings', {}).get('memCard') == {'enabled': True}, saved.get('settings'))
rr, saved = resave(scene({'enabled': False}))
check('an all-default memCard is not written', 'memCard' not in saved.get('settings', {}), saved.get('settings'))
rr, saved = resave(scene())
check('a scene without memCard stays without', 'memCard' not in saved.get('settings', {}), saved.get('settings'))

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
