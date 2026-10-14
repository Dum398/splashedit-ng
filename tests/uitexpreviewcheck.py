# The UI tab's inspector shows an Image element's texture the way the mesh
# inspector does: Source next to what the console will show, at the element's
# bit depth and cutout. Opens a copy of tests/ui/ui.scene whose image 'crate'
# is a generated 32 x 32 RGBA texture (a transparent left quarter, a colour
# gradient elsewhere) at 16 bpp with cutout on, selects it on the canvas and
# reads the preview the editor prints with --ui ("uicanvas: texpreview ...":
# size, bit depth, cutout, PS1 colours and the panes' screen rectangles) and
# its pixels in the screenshots. Picking 4 bpp in the Bit depth menu must
# re-quantize the PS1 pane (and leave the Source pane alone); turning Cutout
# off must change the transparent quarter of the PS1 pane. Needs a display
# (xvfb-run -a).
# usage: python3 uitexpreviewcheck.py <splashedit>
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
TEX = 32
CLEAR = TEX // 4  # columns 0..7 have alpha 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def writepng(path, w, h, rgba):
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def readpng(path):
    """8-bit RGB or RGBA PNG: width, height, bytes per pixel, rows."""
    d = open(path, 'rb').read()
    p, idat = 8, b''
    while p < len(d):
        n, = struct.unpack_from('>I', d, p)
        t = d[p + 4:p + 8]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', d, p + 8)
            assert depth == 8 and ctype in (2, 6), (depth, ctype)
            bpp = 3 if ctype == 2 else 4
        if t == b'IDAT':
            idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride): line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride): line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a, b, c = line[i - bpp] if i >= bpp else 0, prev[i], prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def project():
    """tests/ui/ui.scene with 'crate' on the generated texture, 16 bpp, cutout on."""
    d = os.path.join(tempfile.mkdtemp(), 'ui')
    os.makedirs(os.path.join(d, 'fonts'))
    os.makedirs(os.path.join(d, 'textures'))
    s = json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))
    for f in s['fonts']:
        shutil.copy(os.path.join(here, 'ui', f['source']), os.path.join(d, 'fonts'))
        f['source'] = 'fonts/' + os.path.basename(f['source'])
    rgba = []
    for y in range(TEX):
        for x in range(TEX):
            rgba += [255, 0, 255, 0] if x < CLEAR else [x * 8, y * 8, 255 - (x + y) * 4, 255]
    writepng(os.path.join(d, 'textures', 'grad.png'), TEX, TEX, rgba)
    for e in s['canvases'][0]['elements']:
        if e['name'] == 'crate':
            e.update(texture='textures/grad.png', bitDepth=16, cutout=True)
    json.dump(s, open(os.path.join(d, 'ui.scene'), 'w'), indent=1)
    return d


def run(args, shots=()):
    """Runs the editor on a fresh project with the UI tab open, then Ctrl+S;
    returns the saved crate, the output and the project directory."""
    d = project()
    cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '5', '--screenshot', os.path.join(d, 'end.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    scene = json.load(open(os.path.join(d, 'ui.scene'), encoding='utf-8'))
    crate = [e for e in scene['canvases'][0]['elements'] if e['name'] == 'crate'][0]
    return crate, out, d


PREVIEW = re.compile(r'uicanvas: texpreview (\d+)x(\d+) (\d+) bpp cutout (\d) ps1 (\d+) dE ([\d.]+) '
                     r'source (\d+),(\d+) ps1 (\d+) size (\d+)x(\d+)')


def previews(out):
    return [dict(w=int(m[0]), h=int(m[1]), bpp=int(m[2]), cutout=int(m[3]), colours=int(m[4]), x=int(m[6]), y=int(m[7]),
                 px=int(m[8]), dw=int(m[9]), dh=int(m[10])) for m in PREVIEW.findall(out)]


def region(shot, x0, y0, x1, y1):
    """The pixels of screenshot rows y0..y1 and columns x0..x1 (exclusive), as RGB tuples."""
    w, h, bpp, rows = shot
    return [tuple(rows[y][x * bpp:x * bpp + 3]) for y in range(y0, y1) for x in range(x0, x1)]


def differ(a, b):
    return sum(1 for p, q in zip(a, b) if max(abs(p[i] - q[i]) for i in range(3)) > 8)


# Where the canvas is, and the crate on it.
_, out, _ = run([])
m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out)
check('--ui opens the UI tab and reports its layout', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
X, Y, S = map(float, m.groups())
CRATE = '%d,%d' % (round(X + 280.5 * S), round(Y + 110.5 * S))  # inside the crate (248..312 x 80..144)

# A box selected: no texture, no preview.
_, out, _ = run(['--click', '%d,%d' % (round(X + 68.5 * S), round(Y + 85.5 * S)), '--wait', '30'])
check('a progress bar shows no texture preview', 'uicanvas: inspector element hp' in out and not previews(out), out[-300:])

# The image selected: the preview shows its texture at its bit depth and cutout.
crate, out, d = run(['--click', CRATE, '--wait', '40'])
pv = previews(out)
check('selecting the image shows the preview', 'uicanvas: inspector element crate' in out and len(pv) >= 1, out[-400:])
if not pv:
    print(f'{fails} failures')
    sys.exit(1)
p0 = pv[-1]
check('the preview is the element\'s texture at 16 bpp with cutout', (p0['w'], p0['h'], p0['bpp'], p0['cutout']) == (TEX, TEX, 16, 1), p0)
check('the panes are side by side, inside the inspector', p0['x'] > 1240 and p0['px'] > p0['x'] + p0['dw'] and p0['dw'] > 60 and
      p0['px'] + p0['dw'] < 1600, p0)

# The inspector rows under the preview: Bit depth, then Cutout (32 px rows,
# after the two lines of numbers under the panes).
BPP_Y = p0['y'] + p0['dh'] + 64
CUT_Y = BPP_Y + 32
BPP_MENU = '1460,%d' % BPP_Y
BPP_4 = '1400,%d' % (BPP_Y + 33)  # the menu's first item
CUT = '%d,%d' % (1371, CUT_Y)
shots = [os.path.join(tempfile.mkdtemp(), n) for n in ('a.png', 'b.png')]
crate, out, d = run(['--click', CRATE, '--wait', '40', '--shot', shots[0], '--click', BPP_MENU, '--wait', '5', '--click', BPP_4,
                     '--wait', '40', '--shot', shots[1], '--click', CUT, '--wait', '40'])
pv = previews(out)
check('the run shows the preview', len(pv) >= 3, out[-400:])
check('4 bpp and cutout off are saved', crate.get('bitDepth') == 4 and crate.get('cutout') is False, crate)
at16 = [p for p in pv if p['bpp'] == 16 and p['cutout'] == 1]
at4 = [p for p in pv if p['bpp'] == 4 and p['cutout'] == 1]
off = [p for p in pv if p['bpp'] == 4 and p['cutout'] == 0]
check('picking 4 bpp re-quantizes the preview', at16 and at4 and at4[-1]['colours'] <= 16 < at16[-1]['colours'],
      [(p['bpp'], p['cutout'], p['colours']) for p in pv])
check('turning cutout off redraws the preview', bool(off), [(p['bpp'], p['cutout']) for p in pv])

a, b, c = readpng(shots[0]), readpng(shots[1]), readpng(os.path.join(d, 'end.png'))
x, y, px, dw, dh = p0['x'], p0['y'], p0['px'], p0['dw'], p0['dh']
inset = 3
src = (x + inset, y + inset, x + dw - inset, y + dh - inset)
ps1 = (px + inset, y + inset, px + dw - inset, y + dh - inset)
opaque = (px + dw // 4 + inset, y + inset, px + dw - inset, y + dh - inset)  # the gradient
clear = (px + inset, y + inset, px + dw * CLEAR // TEX - inset, y + dh - inset)  # the transparent quarter
ra = region(a, *ps1)
check('the PS1 pane shows the gradient', len(set(ra)) > 32, len(set(ra)))
check('the Source pane shows the gradient', len(set(region(a, *src))) > 32, len(set(region(a, *src))))
n = differ(region(a, *opaque), region(b, *opaque))
check('at 4 bpp the PS1 pane changes', n > len(region(a, *opaque)) // 4, f'{n} of {len(region(a, *opaque))} pixels differ')
n = differ(region(a, *src), region(b, *src))
check('the Source pane stays as it was', n == 0, f'{n} pixels differ')
n = differ(region(b, *clear), region(c, *clear))
check('cutout off changes the transparent texels in the PS1 pane', n > len(region(b, *clear)) // 2,
      f'{n} of {len(region(b, *clear))} pixels differ')

print(f'{fails} failures')
sys.exit(1 if fails else 0)
