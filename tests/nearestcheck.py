# The editor draws PS1 pixels with each texture's own filter (GL_NEAREST),
# not the ImGui OpenGL3 backend's linear sampler, which on GL 3.3+ is bound
# over every texture and smeared magnified texels into gradients.
#
# 1. A copy of tests/ui/ui.scene whose image 'crate' is a generated 8 x 8
#    two-colour checker (16 bpp, no cutout) is opened on the UI tab and the
#    crate selected; the inspector's texture preview shows it magnified
#    (position and size from "uicanvas: texpreview ..."). Inside both panes
#    there must be only the checker's two colours: linear filtering puts a
#    ramp of in-between colours at every texel edge.
# 2. tests/quantpreview/quantpreview.scene in the 3D viewport (control
#    vp.view): the PS1 render is upscaled a non-integer ~2.6x, so with
#    nearest every horizontal run of one colour is at least 2 px long; with
#    linear most colour changes pass through 1 px in-between pixels.
#
# Needs a display (xvfb-run -a); run under llvmpipe (GL 4.5) so the backend's
# bound-sampler path is the one exercised.
# usage: python3 nearestcheck.py <splashedit>
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
TEX = 8
A, B = (255, 0, 0), (0, 0, 255)  # exact in 5-bit, neither is PS1 transparent black


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


def region(shot, x0, y0, x1, y1):
    w, h, bpp, rows = shot
    return [tuple(rows[y][x * bpp:x * bpp + 3]) for y in range(y0, y1) for x in range(x0, x1)]


def project():
    """tests/ui/ui.scene with 'crate' on the generated checker, 16 bpp, no cutout."""
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
            rgba += list(A if (x + y) % 2 else B) + [255]
    writepng(os.path.join(d, 'textures', 'checker.png'), TEX, TEX, rgba)
    for e in s['canvases'][0]['elements']:
        if e['name'] == 'crate':
            e.update(texture='textures/checker.png', bitDepth=16, cutout=False)
    json.dump(s, open(os.path.join(d, 'ui.scene'), 'w'), indent=1)
    return d


PREVIEW = re.compile(r'uicanvas: texpreview (\d+)x(\d+) (\d+) bpp cutout (\d) ps1 (\d+) dE ([\d.]+) '
                     r'source (\d+),(\d+) ps1 (\d+) size (\d+)x(\d+)')

# 1. The texture preview.
d = project()
shot = os.path.join(d, 'end.png')
cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5']
r = subprocess.run(cmd + ['--screenshot', shot], capture_output=True, text=True, env=ENV, timeout=180)
out = (r.stdout + r.stderr).strip()
m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out)
check('--ui opens the UI tab and reports its layout', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
X, Y, S = map(float, m.groups())
crate = '%d,%d' % (round(X + 280.5 * S), round(Y + 110.5 * S))
r = subprocess.run(cmd + ['--click', crate, '--wait', '40', '--screenshot', shot], capture_output=True, text=True, env=ENV,
                   timeout=180)
out = (r.stdout + r.stderr).strip()
pv = [m for m in PREVIEW.findall(out)]
check('selecting the checker image shows the preview', len(pv) >= 1, out[-400:])
if not pv:
    print(f'{fails} failures')
    sys.exit(1)
p = pv[-1]
w, h, x, y, px, dw, dh = int(p[0]), int(p[1]), int(p[6]), int(p[7]), int(p[8]), int(p[9]), int(p[10])
check('the preview is the 8 x 8 checker, magnified', (w, h) == (TEX, TEX) and dw >= TEX * 8 and dh >= TEX * 8, p)
img = readpng(shot)
inset = 2  # the pane's border and the 1 px edge the rectangle may round onto
for name, x0 in (('Source', x), ('PS1', px)):
    px_ = region(img, x0 + inset, y + inset, x0 + dw - inset, y + dh - inset)
    cols = set(px_)
    near = sum(1 for c in px_ if max(abs(c[i] - A[i]) for i in range(3)) <= 8 or max(abs(c[i] - B[i]) for i in range(3)) <= 8)
    check(f'the {name} pane shows only the checker\'s two colours (nearest)', len(cols) <= 4 and near == len(px_),
          f'{len(cols)} distinct colours, {len(px_) - near} of {len(px_)} pixels are neither checker colour')

# 2. The 3D viewport's upscale.
qp = os.path.join(here, 'quantpreview')
vshot = os.path.join(tempfile.mkdtemp(), 'vp.png')
r = subprocess.run([editor, '--size', '1600x960', '--project', qp, '--scene', os.path.join(qp, 'quantpreview.scene'),
                    '--wait', '30', '--screenshot', vshot], capture_output=True, text=True, env=ENV, timeout=180)
out = (r.stdout + r.stderr).strip()
vm = re.search(r'^control vp\.view at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', out, re.M)
check('the viewport reports its rectangle', vm is not None, out[-300:])
if vm:
    vx0, vy0, vx1, vy1 = map(int, vm.groups())
    img = readpng(vshot)
    _, _, bpp, rows = img
    # The middle of the viewport: clear of the toolbar, axis gizmo and status badge.
    cx0, cx1 = vx0 + (vx1 - vx0) // 4, vx1 - (vx1 - vx0) // 4
    cy0, cy1 = vy0 + (vy1 - vy0) // 4, vy1 - (vy1 - vy0) // 4
    runs = ones = 0
    for yy in range(cy0, cy1):
        row = rows[yy]
        n, first = 1, True  # the first run is cut by the crop; not counted
        for xx in range(cx0 + 1, cx1):
            if row[xx * bpp:xx * bpp + 3] == row[(xx - 1) * bpp:(xx - 1) * bpp + 3]:
                n += 1
            else:
                if not first:
                    runs += 1
                    ones += n == 1
                n, first = 1, False
    check('the viewport shows something to measure', runs > 200, runs)
    print(f'viewport: {ones} of {runs} runs are 1 px')
    check('the viewport upscale is blocky: no 1 px colour runs (nearest)', runs and ones * 100 <= runs,
          f'{ones} of {runs} horizontal runs are 1 px long')

print(f'{fails} failures')
sys.exit(1 if fails else 0)
