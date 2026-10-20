# The Frame tab (frame debugger) driven through the GUI on a hand-built GPU
# dump, tests/fixtures/frame/frame.gpd (408 bytes; make_fixture.py writes it):
# --frame opens it, the tab comes to the front with all six primitives drawn;
# clicks on the image (at 4x, so each VRAM pixel is a 4x4 block) pick the
# primitive that last wrote a pixel, including the second half of a command
# split across two GP0 packets; previous / first / a list row step through
# the draw order, and a pick follows the step (at step 1 only the fill has
# drawn). The screenshot shows the frame at 4x with nearest sampling (a
# texel's centre is its exact colour) and the selected primitive outlined.
# usage: python3 framedebugcheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fixture = os.path.join(here, 'fixtures', 'frame', 'frame.gpd')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
ZOOM = 4


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def png_rgb(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGBA or RGB PNG."""
    data = open(path, 'rb').read()
    pos, idat, w, h, bpp = 8, b'', 0, 0, 3
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            bpp = 4 if ctype == 6 else 3
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
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
        rows.append(bytes(line[i] for i in range(stride) if i % bpp < 3))
        prev = line
    return w, h, rows


def pick(x, y):
    """--click on fixture pixel (x, y): the 64x48 image at 4x, offsets from its centre (pixel 32,24's corner)."""
    return ['--click', '@frame.image,%d,%d' % ((x - 32) * ZOOM + ZOOM // 2, (y - 24) * ZOOM + ZOOM // 2), '--wait', '4']


work = tempfile.mkdtemp()
project = os.path.join(work, 'courtyard')
shutil.copytree(example, project)
shot = os.path.join(work, 'frame.png')
args = (['--frame', fixture, '--wait', '10', '--click', '@frame.zoom/%dx' % ZOOM, '--wait', '4'] +
        pick(10, 14) +                                  # the red triangle
        pick(8, 34) +                                   # the 4-bit textured rect: texel (4,4), CLUT entry 8
        pick(30, 30) +                                  # the semi-transparent rect, sent across two packets
        pick(40, 30) +                                  # the line's first point, over that rect
        ['--click', '@frame.prev', '--wait', '4'] +     # step 5: the line undrawn
        ['--click', '@frame.first', '--wait', '4'] +    # step 1: only the fill
        pick(10, 14) +
        ['--click', '@frame.row/4', '--wait', '6'])     # select #4, step 5
cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'courtyard.scene')] + args + [
    '--screenshot', shot]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
lines = (r.stdout + r.stderr).strip().splitlines()
frame = [l for l in lines if l.startswith('frame:')]
rects = {}
for l in lines:
    mm = re.match(r'control (.+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
    if mm: rects[mm.group(1)] = tuple(int(v) for v in mm.groups()[1:])
missing = [l for l in lines if 'no such control drawn' in l]
check('every control the script names was drawn', not missing, missing)
check('the capture loads: 6 primitives, 22 commands, a 64x48 image at 0,0',
      'frame: loaded frame.gpd prims 6 commands 22 frames 1 image 64x48 at 0,0' in frame, frame[:2])
check('frame: prims 6', 'frame: prims 6' in frame, frame[:3])
check('the Frame tab is in front with its controls', 'tab_frame' in rects and 'frame.image' in rects and 'frame.slider' in rects,
      sorted(k for k in rects if k.startswith('frame') or k.startswith('tab'))[:12])
check('it opens on the whole frame, nothing selected', frame[2:4] == ['frame: step 6/6', 'frame: selected none'], frame[2:4])
picks = [l for l in frame if l.startswith('frame: pick')]
want = ['frame: pick 10,14 -> #1 tri-flat', 'frame: pick 8,34 -> #3 rect16-raw', 'frame: pick 30,30 -> #4 rect-semi',
        'frame: pick 40,30 -> #5 line', 'frame: pick 10,14 -> #0 fill']
check('each click picks the primitive that last wrote the pixel, at the step shown', picks == want, picks)
check('a pick selects it with its draw state', 'frame: selected #3 rect16-raw page 64,0 depth 0 clut 0,500' in frame and
      'frame: selected #4 rect-semi blend 1' in frame, [l for l in frame if l.startswith('frame: selected')])
steps = [l for l in frame if l.startswith('frame: step')]
check('previous, first and a list row step through the draw order',
      steps == ['frame: step 6/6', 'frame: step 5/6', 'frame: step 1/6', 'frame: step 5/6'], steps)
check('the last selection is the list row picked', [l for l in frame if l.startswith('frame: selected')][-1:] ==
      ['frame: selected #4 rect-semi blend 1'], frame[-3:])

# The picture: 4x, nearest. Texel centres carry the exact colours (the fill
# (32,40,64) at step 5 is untouched under nothing; the red triangle; the
# texture's CLUT entry 8 at (8,34)); the selected rect has its outline.
if os.path.exists(shot) and 'frame.image' in rects:
    w, h, rows = png_rgb(shot)
    x0, y0 = rects['frame.image'][0], rects['frame.image'][1]

    def at(px, py):
        x, y = x0 + px * ZOOM + ZOOM // 2, y0 + py * ZOOM + ZOOM // 2
        return tuple(rows[y][x * 3:x * 3 + 3])

    def to8(v5): return (v5 << 3) | (v5 >> 2)
    fill = (to8(0x20 >> 3), to8(0x28 >> 3), to8(0x40 >> 3))
    check('a fill pixel shows its exact colour', at(60, 46) == fill, (at(60, 46), fill))
    check('a triangle pixel is pure red', at(20, 4) == (255, 0, 0), at(20, 4))
    e8 = (16 * 8, 255 - 16 * 8, (64 + 32 * 8) & 255)
    check('a texel shows its CLUT colour', at(8, 34) == (to8(e8[0] >> 3), to8(e8[1] >> 3), to8(e8[2] >> 3)), at(8, 34))
    # Nearest: the four screen pixels of one VRAM pixel are equal.
    x, y = x0 + 20 * ZOOM, y0 + 4 * ZOOM
    block = {tuple(rows[y + j][(x + i) * 3:(x + i) * 3 + 3]) for i in range(ZOOM) for j in range(ZOOM)}
    check('each VRAM pixel is a sharp 4x4 block', len(block) == 1, block)
    def golden(p):  # the outline (ffd166), antialiased: bright, green about 0.8 of red, little blue
        return p[0] > 180 and 0.7 * p[0] <= p[1] <= 0.92 * p[0] and p[2] < 0.6 * p[0]
    gold = sum(1 for yy in range(y0 - 4, y0 + 48 * ZOOM + 4) for xx in range(x0 - 4, x0 + 64 * ZOOM + 4)
               if 0 <= yy < h and 0 <= xx < w and golden(rows[yy][xx * 3:xx * 3 + 3]))
    check('the selected primitive is outlined', gold > 2 * (20 + 12) * ZOOM, gold)
else:
    check('screenshot written', False, shot)

if keep and os.path.exists(shot):
    shutil.copy(shot, keep)
shutil.rmtree(work, ignore_errors=True)
print('framedebugcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
