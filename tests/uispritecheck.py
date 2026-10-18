# HUD sprites: a UI Image backed by a cell of a scene sprite sheet. Exports
# sprite/sprite.scene with a canvas 'hud' holding a sheet-backed image on each
# sheet ('icon': hero cell 6, 'spark': fx cell 5) and a plain image ('plain'),
# then reads the UI table back the way psxsplash's uisystem.cpp does: 48-byte
# element records, type data [0]tpX [1]tpY [2..3]clutX [4..5]clutY [6]u0 [7]v0
# [8]u1 [9]v1 (last texel) [10]bpp [11]cellW [12]cellH [13]cols [14]baseU
# [15]baseV. The sheet-backed images must share their sheet's texture (page,
# CLUT and base u0/v0 as the sprite table has them, one VRAM copy), point at
# their cell, give back that cell from (u0-baseU)/cellW and (v0-baseV)/cellH
# the way the loader recovers it, and the texels the GPU fetches there through
# the .vram file must be that cell of the source PNG; so must those after
# UI.SetFrame's arithmetic for another cell. The plain image keeps bytes
# 11..15 zero (UI.SetFrame/GetFrame ignore it). Also: colour 0.5 is 128, the
# --stats 'ui image' lines, and the two errors (no such sheet, cell past the
# sheet).
# usage: python3 uispritecheck.py <splashpack-cli>
import json, os, struct, subprocess, sys, tempfile, zlib

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
fixture = os.path.join(here, 'sprite')
crate = os.path.join(here, '..', 'examples', 'courtyard', 'textures', 'crate.png')
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
base['canvases'] = [{'name': 'hud', 'visible': True, 'sortOrder': 0, 'defaultFont': None, 'elements': [
    {'type': 'image', 'name': 'icon', 'visible': True, 'rect': [10, 20, 16, 16], 'anchorMin': [0, 0], 'anchorMax': [0, 0],
     'color': [0.5, 0.5, 0.5], 'texture': None, 'bitDepth': 8, 'cutout': True, 'sheet': 'hero', 'cell': 6},
    {'type': 'image', 'name': 'spark', 'visible': True, 'rect': [40, 20, 12, 8], 'anchorMin': [0, 0], 'anchorMax': [0, 0],
     'color': [0.5, 0.5, 0.5], 'texture': None, 'bitDepth': 8, 'cutout': True, 'sheet': 'fx', 'cell': 5},
    {'type': 'image', 'name': 'plain', 'visible': True, 'rect': [100, 20, 32, 32], 'anchorMin': [0, 0], 'anchorMax': [0, 0],
     'color': [0.5, 0.5, 0.5], 'texture': crate, 'bitDepth': 8, 'cutout': True}]}]


def export(edit=lambda s: None, name='s'):
    """Writes the edited scene into tmp with absolute fixture paths and exports
    it; returns (rc, output, splashpack bytes, vram bytes)."""
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
    pack = open(out, 'rb').read() if os.path.exists(out) and r.returncode == 0 else b''
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


def texel(mem, img, u, v):
    """The 15-bit colour word the GPU fetches at (u, v) of the image's page and CLUT."""
    px, py = img['tpx'] * 64, img['tpy'] * 256
    if img['bpp'] == 0:
        idx = (mem[py + v][px + u // 4] >> ((u % 4) * 4)) & 15
        return mem[img['cluty']][img['clutx'] * 16 + idx]
    if img['bpp'] == 1:
        idx = (mem[py + v][px + u // 2] >> ((u % 2) * 8)) & 255
        return mem[img['cluty']][img['clutx'] * 16 + idx]
    return mem[py + v][px + u]


def sheets_of(pack):
    """The sprite table's sheet records by name."""
    off, count = struct.unpack_from('<I', pack, 128)[0], struct.unpack_from('<H', pack, 132)[0]
    out = {}
    for i in range(count):
        n, tpx, tpy, u0, v0, cx, cy, cw, ch, cols, rows, bpp = struct.unpack_from('<IBBBBHHBBBBB', pack, off + i * 20)
        out[cstr(pack, n)] = dict(tpx=tpx, tpy=tpy, u0=u0, v0=v0, clutx=cx, cluty=cy, cw=cw, ch=ch, cols=cols, rows=rows, bpp=bpp)
    return out


def elements_of(pack):
    """The UI elements by name: colour and the 16 type-data bytes, decoded as uisystem.cpp does."""
    ncanvas = struct.unpack_from('<H', pack, 92)[0]
    nfont = pack[94]
    table = struct.unpack_from('<I', pack, 96)[0]
    p = table + nfont * 112
    out = {}
    for ci in range(ncanvas):
        dofs, _nl, _so, count, _fl, _no = struct.unpack_from('<IBBBBI', pack, p + ci * 12)
        for ei in range(count):
            q = dofs + ei * 48
            td = pack[q + 24:q + 40]
            tpx, tpy, cx, cy, u0, v0, u1, v1, bpp, cw, ch, cols, bu, bv = struct.unpack_from('<BBHHBBBBBBBBBB', td)
            out[cstr(pack, struct.unpack_from('<I', pack, q + 4)[0])] = dict(
                type=pack[q], color=tuple(pack[q + 20:q + 23]), td=td, tpx=tpx, tpy=tpy, clutx=cx, cluty=cy, u0=u0, v0=v0,
                u1=u1, v1=v1, bpp=bpp, cw=cw, ch=ch, cols=cols, bu=bu, bv=bv)
    return out


def frame_of(e):
    """UISystem::loadFromSplashpack: the authored cell recovered from the UVs, or -1 when not sheet-backed (getFrame)."""
    if not e['cw'] or not e['ch']:
        return -1
    cols = e['cols'] or 1
    return ((e['v0'] - e['bv']) // e['ch']) * cols + (e['u0'] - e['bu']) // e['cw']


def set_frame(e, f):
    """UISystem::setFrame on a copy."""
    e = dict(e)
    cols = e['cols'] or 1
    e['u0'] = (e['bu'] + (f % cols) * e['cw']) & 255
    e['v0'] = (e['bv'] + (f // cols) * e['ch']) & 255
    e['u1'], e['v1'] = (e['u0'] + e['cw'] - 1) & 255, (e['v0'] + e['ch'] - 1) & 255
    return e


def word_of(px):
    """What a source pixel becomes at worst: transparent 0, else 5-bit channels."""
    r, g, b, a = px
    return None if a < 128 else (r >> 3, g >> 3, b >> 3)


def cell_matches(mem, e, src, cell, cw, ch, cols):
    """Every texel of the element's UVs (u0..u1, v0..v1) equals that cell of the source PNG within 1 step per channel."""
    _w, _h, rows = src
    if (e['u1'] - e['u0'] + 1, e['v1'] - e['v0'] + 1) != (cw, ch):
        return 'size %dx%d' % (e['u1'] - e['u0'] + 1, e['v1'] - e['v0'] + 1)
    sx, sy = (cell % cols) * cw, (cell // cols) * ch
    for y in range(ch):
        for x in range(cw):
            w = texel(mem, e, e['u0'] + x, e['v0'] + y)
            want = word_of(rows[sy + y][sx + x])
            if want is None:
                if w != 0: return 'texel %d,%d should be transparent, is %04x' % (x, y, w)
                continue
            got = (w & 31, (w >> 5) & 31, (w >> 10) & 31)
            if w == 0 or any(abs(a - b) > 1 for a, b in zip(got, want)):
                return 'texel %d,%d is %s, the PNG %s' % (x, y, got, want)
    return ''


rc, out, pack, vram = export()
check('export with sheet-backed images succeeds', rc == 0 and pack, out[-400:])
if not pack:
    print('%d of %d checks failed' % (fails, checks))
    sys.exit(1)
sheets, els, mem = sheets_of(pack), elements_of(pack), read_vram(vram)
srcs = {n: png(os.path.join(fixture, n + '.png')) for n in ('hero', 'fx')}
check('the hud canvas has its three images', sorted(els) == ['icon', 'plain', 'spark'] and all(e['type'] == 0 for e in els.values()),
      sorted(els))
for name, sheet, cell in (('icon', 'hero', 6), ('spark', 'fx', 5)):
    e, s = els[name], sheets[sheet]
    check(name + ': page, CLUT and bit depth are the sheet\'s',
          (e['tpx'], e['tpy'], e['clutx'], e['cluty'], e['bpp']) == (s['tpx'], s['tpy'], s['clutx'], s['cluty'], s['bpp']), (e, s))
    check(name + ': the grid is the sheet\'s, based at its u0/v0',
          (e['cw'], e['ch'], e['cols'], e['bu'], e['bv']) == (s['cw'], s['ch'], s['cols'], s['u0'], s['v0']), (e, s))
    want_uv = (s['u0'] + (cell % s['cols']) * s['cw'], s['v0'] + (cell // s['cols']) * s['ch'])
    check(name + ': u0/v0 is cell %d, u1/v1 its last texel' % cell,
          (e['u0'], e['v0'], e['u1'], e['v1']) == (want_uv[0], want_uv[1], want_uv[0] + s['cw'] - 1, want_uv[1] + s['ch'] - 1), e)
    check(name + ': the loader recovers cell %d (UI.GetFrame)' % cell, frame_of(e) == cell, frame_of(e))
    why = cell_matches(mem, e, srcs[sheet], cell, s['cw'], s['ch'], s['cols'])
    check(name + ': the GPU fetches cell %d of %s.png' % (cell, sheet), not why, why)
    other = 1 if cell != 1 else 2
    moved = set_frame(e, other)
    why = cell_matches(mem, moved, srcs[sheet], other, s['cw'], s['ch'], s['cols'])
    check(name + ': after UI.SetFrame %d the GPU fetches that cell' % other, not why and frame_of(moved) == other, why)
    check(name + ': colour 0.5 is 128 (the texture as drawn)', e['color'] == (128, 128, 128), e['color'])
p = els['plain']
check('plain image: bytes 11..15 zero (not sheet-backed: UI.GetFrame -1)', p['td'][11:16] == b'\0' * 5 and frame_of(p) == -1,
      list(p['td']))
check('plain image: its whole 8 bpp texture', p['bpp'] == 1 and (p['u1'] - p['u0'] + 1, p['v1'] - p['v0'] + 1) == png(crate)[:2], p)

tex = [l for l in out.splitlines() if l.startswith('texture ')]
check('each sheet texture is in VRAM once', sum('hero.png' in l for l in tex) == 1 and sum('fx.png' in l for l in tex) == 1, tex)
ui = [l for l in out.splitlines() if l.startswith('ui image ')]
check('--stats lists each image with its sheet and cell',
      ui == ['ui image sheet=hero cell=6 hud/icon', 'ui image sheet=fx cell=5 hud/spark', 'ui image sheet=- cell=0 hud/plain'], ui)
check('the sheets stay in the sprite table', sorted(sheets) == ['fx', 'hero'], sorted(sheets))


def with_icon(sheet, cell):
    def edit(s):
        s['canvases'][0]['elements'][0]['sheet'] = sheet
        s['canvases'][0]['elements'][0]['cell'] = cell
    return edit


rc, out, _p, _v = export(with_icon('nope', 0), 'nosheet')
check('a sheet the scene does not have fails the export',
      rc != 0 and "canvas 'hud', element 'icon': no sprite sheet named 'nope'" in out, out[-300:])
rc, out, _p, _v = export(with_icon('hero', 8), 'pastend')
check('a cell past the sheet fails the export',
      rc != 0 and "canvas 'hud', element 'icon': cell 8 is outside sprite sheet 'hero', which has 8 cells" in out, out[-300:])
rc, out, _p, _v = export(with_icon('hero', 7), 'last')
check('the last cell exports', rc == 0, out[-300:])

print('%d of %d checks failed' % (fails, checks) if fails else 'uispritecheck: all %d checks passed' % checks)
sys.exit(1 if fails else 0)
