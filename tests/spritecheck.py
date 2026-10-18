# Export sprite/sprite.scene (two sheets, three animations) with splashpack-cli
# and read the sprite table back the way psxsplash's SpriteSystem does: header
# @128 offset, @132 sheet count, @134 animation count; 20-byte sheet records,
# 12-byte animation records, NUL-terminated names at absolute offsets. Every
# frame of every animation is then looked up with the engine's frameToUV
# (u0 + (f % cols) * cellW, v0 + (f / cols) * cellH) in the .vram file's
# texture page through the sheet's CLUT and compared with that cell of the
# source PNG. Also: --stats lines, a scene without sheets, and the errors
# export refuses with.
# usage: python3 spritecheck.py <splashpack-cli>
import json, os, struct, subprocess, sys, tempfile, zlib

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
fixture = os.path.join(here, 'sprite')
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def cstr(d, off):
    return d[off:d.index(b'\0', off)].decode() if off else ''


def png(path):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGBA PNG, row 0 = top."""
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype == 6, 'fixture PNGs are 8-bit RGBA'
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, bpp, stride, rows, prev = zlib.decompress(idat), 4, w * 4, [], bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)])
        prev = line
    return w, h, rows


base = json.load(open(os.path.join(fixture, 'sprite.scene'), encoding='utf-8'))


def export(edit=lambda s: None, name='s'):
    """Writes the edited scene into the fixture directory's copy in tmp (so its
    relative paths hold) and exports it; returns (rc, output, splashpack bytes, vram bytes)."""
    s = json.loads(json.dumps(base))
    edit(s)
    for o in s['objects']:
        for c in o['components']:
            if c['type'] == 'mesh':
                c['mesh'] = os.path.join(fixture, c['mesh'])
                for m in c['materials']:
                    m['texture'] = os.path.join(fixture, m['texture'])
    s['settings']['script'] = os.path.join(fixture, s['settings']['script'])
    for sh in s.get('spriteSheets', []):
        if sh.get('texture'):
            sh['texture'] = os.path.join(fixture, sh['texture'])
    path = os.path.join(tmp, name + '.scene')
    json.dump(s, open(path, 'w', encoding='utf-8'))
    out = os.path.join(tmp, name + '.splashpack')
    for ext in ('.splashpack', '.vram'):
        if os.path.exists(os.path.join(tmp, name + ext)): os.remove(os.path.join(tmp, name + ext))
    r = subprocess.run([cli, 'export', path, '-o', out, '--stats'], capture_output=True, text=True, timeout=120)
    pack = open(out, 'rb').read() if os.path.exists(out) else b''
    vram = open(os.path.join(tmp, name + '.vram'), 'rb').read() if os.path.exists(os.path.join(tmp, name + '.vram')) else b''
    return r.returncode, r.stdout + r.stderr, pack, vram


def read_vram(v):
    """The .vram file uploaded into a 1024x512 word array, as scenemanager does."""
    mem = [[0] * 1024 for _ in range(512)]
    atlases, cluts = struct.unpack_from('<HH', v, 2)
    pos = 8
    for _ in range(atlases):
        x, y, w, h = struct.unpack_from('<4H', v, pos)
        pos += 8
        words = struct.unpack_from('<%dH' % (w * h), v, pos)
        for r in range(h):
            mem[y + r][x:x + w] = words[r * w:(r + 1) * w]
        pos += (w * h * 2 + 3) & ~3
    for _ in range(cluts):
        cx, cy, n, _z = struct.unpack_from('<4H', v, pos)
        pos += 8
        mem[cy][cx * 16:cx * 16 + n] = struct.unpack_from('<%dH' % n, v, pos)
        pos += (n * 2 + 3) & ~3
    return mem


def texel(mem, sheet, u, v):
    """The 15-bit colour word the GPU fetches at (u, v) of the sheet's page and CLUT."""
    px, py = sheet['tpx'] * 64, sheet['tpy'] * 256
    if sheet['bpp'] == 0:
        word = mem[py + v][px + u // 4]
        idx = (word >> ((u % 4) * 4)) & 15
        return mem[sheet['cluty']][sheet['clutx'] * 16 + idx]
    if sheet['bpp'] == 1:
        word = mem[py + v][px + u // 2]
        idx = (word >> ((u % 2) * 8)) & 255
        return mem[sheet['cluty']][sheet['clutx'] * 16 + idx]
    return mem[py + v][px + u]


def parse(pack):
    off, ns, na = struct.unpack_from('<IHH', pack, 128)
    sheets, anims = [], []
    for i in range(ns):
        n, tpx, tpy, u0, v0, cx, cy, cw, ch, cols, rows, bpp, p0, p1 = struct.unpack_from('<I4B2H6BH', pack, off + 20 * i)
        sheets.append(dict(name=cstr(pack, n), tpx=tpx, tpy=tpy, u0=u0, v0=v0, clutx=cx, cluty=cy, cw=cw, ch=ch, cols=cols,
                           rows=rows, bpp=bpp, pad=(p0, p1), noff=n))
    for i in range(na):
        n, sh, first, count, dur, loop, p0, p1 = struct.unpack_from('<I6BH', pack, off + 20 * ns + 12 * i)
        anims.append(dict(name=cstr(pack, n), sheet=sh, first=first, count=count, dur=dur, loop=loop, pad=(p0, p1), noff=n))
    return off, ns, na, sheets, anims


rc, out, pack, vram = export()
check('the fixture exports', rc == 0 and pack, out[-400:])
check('splashpack version 22 or later (sprites are v22)', struct.unpack_from('<H', pack, 2)[0] >= 22)
off, ns, na, sheets, anims = parse(pack)
check('header: table offset set and 4-aligned', off > 0 and off % 4 == 0, off)
check('header: 2 sheets, 3 animations', (ns, na) == (2, 3), (ns, na))
check('sheets in scene order with their names', [s['name'] for s in sheets] == ['hero', 'fx'], sheets)
check('hero: 4 bpp, 16x16 cells, 4x2 grid', [(s['bpp'], s['cw'], s['ch'], s['cols'], s['rows']) for s in sheets][0] == (0, 16, 16, 4, 2),
      sheets[0])
check('fx: 8 bpp, 12x8 cells, 4x2 grid', [(s['bpp'], s['cw'], s['ch'], s['cols'], s['rows']) for s in sheets][1] == (1, 12, 8, 4, 2),
      sheets[1])
check('sheet padding is zero', all(s['pad'] == (0, 0) for s in sheets))
check('animations: names, sheet, first cell, count, vsyncs, loop',
      [(a['name'], a['sheet'], a['first'], a['count'], a['dur'], a['loop']) for a in anims] ==
      [('walk', 0, 0, 4, 8, 1), ('jump', 0, 4, 4, 4, 0), ('spark', 1, 2, 5, 3, 1)], anims)
check('animation padding is zero', all(a['pad'] == (0, 0) for a in anims))
names_start = off + 20 * ns + 12 * na
check('names follow the records, sheets then animations, each NUL-terminated',
      [x['noff'] for x in sheets + anims] == [names_start, names_start + 5, names_start + 8, names_start + 13, names_start + 18] and
      pack[names_start:names_start + 24] == b'hero\0fx\0walk\0jump\0spark\0',
      pack[names_start:names_start + 24])

# Placement: each sheet's page, corner and CLUT agree with --stats' texture lines.
tex = {}
for l in out.splitlines():
    if l.startswith('texture '):
        f = dict(kv.split('=') for kv in l.split()[1:-1])
        tex[os.path.basename(l.split()[-1])] = f
for s, file in zip(sheets, ('hero.png', 'fx.png')):
    t = tex.get(file, {})
    vx, vy = (int(v) for v in t.get('vram', '-1,-1').split(','))
    cx, cy, _n = (int(v) for v in t.get('clut', '-1,-1,0').split(','))
    expander = (4, 2, 1)[s['bpp']]
    check(file + ' is packed with cutout on', t.get('cutout') == '1', t)
    check(file + ': texture page, u0, v0 and CLUT match its VRAM placement',
          (s['tpx'], s['tpy'], s['u0'], s['v0'], s['clutx'], s['cluty']) ==
          (vx // 64, vy // 256, (vx % 64) * expander, vy % 256, cx // 16, cy), (s, t))
    check(file + ': the whole grid lies in one 256x256 page', s['u0'] + s['cols'] * s['cw'] <= 256 and
          s['v0'] + s['rows'] * s['ch'] <= 256, s)

# Every frame of every animation as the engine fetches it, against the PNG.
mem = read_vram(vram)
for a in anims:
    s = sheets[a['sheet']]
    w, h, rows = png(os.path.join(fixture, ('hero.png', 'fx.png')[a['sheet']]))
    bad = []
    for k in range(a['count']):
        f = a['first'] + k
        u, v = s['u0'] + (f % s['cols']) * s['cw'], s['v0'] + (f // s['cols']) * s['ch']
        sx, sy = (f % s['cols']) * s['cw'], (f // s['cols']) * s['ch']
        for y in range(s['ch']):
            for x in range(s['cw']):
                r, g, b, al = rows[sy + y][sx + x]
                word = texel(mem, s, u + x, v + y)
                if al < 128:
                    if word != 0: bad.append((f, x, y, 'hole', hex(word)))
                else:
                    got = ((word & 31) * 255 // 31, ((word >> 5) & 31) * 255 // 31, ((word >> 10) & 31) * 255 // 31)
                    if word == 0 or max(abs(got[0] - r), abs(got[1] - g), abs(got[2] - b)) > 12:
                        bad.append((f, x, y, (r, g, b), got))
    check('animation ' + a['name'] + ': every frame the engine draws is that cell of the PNG', not bad, bad[:4])

# --stats
stat = [l for l in out.splitlines() if l.startswith('sprite')]
check('--stats: sprites line with the table bytes (records + names)',
      'sprites sheets=2 anims=3 bytes=%d' % (20 * 2 + 12 * 3 + 24) in stat, stat)
check('--stats: one line per sheet', 'sprite sheet bpp=4 cell=16x16 grid=4x2 anims=2 hero' in stat and
      'sprite sheet bpp=8 cell=12x8 grid=4x2 anims=1 fx' in stat, stat)

# Without sheets: the header words stay zero and --stats prints no sprite lines.
rc, out0, pack0, _ = export(lambda s: s.pop('spriteSheets'), 'none')
check('a scene without sheets exports', rc == 0 and pack0, out0[-300:])
check('without sheets: offset and counts are zero', struct.unpack_from('<IHH', pack0, 128) == (0, 0, 0),
      struct.unpack_from('<IHH', pack0, 128))
check('without sheets: no sprite lines in --stats', not any(l.startswith('sprite') for l in out0.splitlines()))
check('without sheets: the pack is the sprite table and its textures shorter',
      len(pack0) < len(pack) and not any('hero.png' in l for l in out0.splitlines()))


# What export refuses, each with its message.
def refuses(label, edit, needle):
    rc, o, _p, _v = export(edit, 'bad')
    check('refused: ' + label, rc != 0 and needle in o, o[-300:])


def sheet(**kw):
    d = dict(name='x', texture='fx.png', bitDepth=4, cellWidth=8, cellHeight=8, anims=[])
    d.update(kw)
    return d


refuses('17 sheets', lambda s: s['spriteSheets'].extend(sheet(name='x%d' % i) for i in range(15)), '17 sprite sheets')
refuses('65 animations', lambda s: s['spriteSheets'][1]['anims'].extend(
    dict(name='a%d' % i, firstFrame=0, frameCount=1, frameDuration=1, loop=True) for i in range(62)), '65 sprite animations')
refuses('an empty sheet name', lambda s: s['spriteSheets'][1].update(name=''), 'has no name')
refuses('two sheets with one name', lambda s: s['spriteSheets'][1].update(name='hero'), "two sprite sheets are named 'hero'")
refuses('no texture', lambda s: s['spriteSheets'][1].update(texture=''), "sprite sheet 'fx' has no texture")
refuses('a missing texture', lambda s: s['spriteSheets'][1].update(texture='gone.png'), "sprite sheet 'fx': cannot load")
refuses('a texture not a whole number of cells', lambda s: s['spriteSheets'][0].update(cellWidth=24), 'not a whole number of 24x16')
refuses('a cell over 255', lambda s: s['spriteSheets'][0].update(cellWidth=256), 'each side must be 1..255')
refuses('more than 255 cells', lambda s: s['spriteSheets'][0].update(cellWidth=1, cellHeight=1, anims=[]), '2048 cells')
refuses('an animation past the sheet', lambda s: s['spriteSheets'][1]['anims'][0].update(firstFrame=5, frameCount=4),
        'cells 5..8 are outside the sheet')
refuses('0 frames', lambda s: s['spriteSheets'][1]['anims'][0].update(frameCount=0), '0 frames, it needs at least 1')
refuses('0 vsyncs a frame', lambda s: s['spriteSheets'][1]['anims'][0].update(frameDuration=0), 'it must be 1..255')
refuses('one animation name on two sheets', lambda s: s['spriteSheets'][1]['anims'][0].update(name='walk'),
        "two sprite animations are named 'walk'")

print(f'{checks - fails}/{checks} checks passed')
sys.exit(1 if fails else 0)
