# Blob shadows in the editor. The third-person template with Player Body
# selected: Shadow starts Off (no shadow lines, no blob). Picking Blob from the
# mesh inspector's Shadow menu sizes the blob from the body's footprint (its
# 0.6 m width: `shadow: blob size 0.6 darkness 60 Player Body`), shows the Size
# and Darkness rows, and draws the blob in the scene view on the nav floor
# under the body (`shadow preview: Player Body floor R<k> height 0.00 radius
# 0.60 darkness 153`). Typing 0.75 into Size takes (`size 0.75`, radius 0.75,
# "shadowSize": 0.75 saved). The blob's screen box is
# vp.shadow.Player Body darker than the same pixels before). The scene saved
# with Ctrl+S carries "shadow": "blob", and splashpack-cli exports it as a v28
# pack with one shadow record for the body.
# usage: python3 shadowcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep_shot = sys.argv[2] if len(sys.argv) > 2 else None
cli = os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
here = os.path.dirname(os.path.abspath(__file__))
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def read_png(path):
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


def brightness(path, box):
    """Mean of R+G+B over the box (x0, y0, x1, y1) of a screenshot."""
    _, _, rows = read_png(path)
    x0, y0, x1, y1 = box
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            s += sum(rows[y][3 * x:3 * x + 3])
            n += 1
    return s / max(n, 1)


root = tempfile.mkdtemp()
shutil.copytree(os.path.join(here, '..', 'templates', 'third-person'), os.path.join(root, 'p'))
proj = os.path.join(root, 'p')
scene = os.path.join(proj, 'third_person.scene')
before = os.path.join(root, 'before.png')
shot = keep_shot or os.path.join(root, 'after.png')
W = lambda n: ['--wait', str(n)]
cmd = [editor, '--size', '1600x960', '--project', proj, '--scene', scene, '--select', 'Player Body'] + W(30) + \
      ['--wait-export', '--move', '800,400'] + W(3) + ['--key', 'f'] + W(20) + ['--scroll-to', '@obj.shadow'] + W(5) + \
      ['--shot', before, '--click', '@obj.shadow'] + W(5) + ['--click', '@obj.shadow/Blob'] + W(10) + \
      ['--dblclick', '@obj.shadowsize'] + W(3) + ['--key', 'ctrl+a', '--text', '0.75', '--key', 'enter'] + W(5) + \
      ['--key', 'ctrl+s'] + W(10) + ['--wait-export'] + W(20) + ['--move', '800,400'] + W(5) + ['--screenshot', shot]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=600)
lines = (r.stdout + r.stderr).splitlines()
rects = {}
for l in lines:
    m = re.match(r'control (\S+(?: \S+)*) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
    if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
out = [l for l in lines if not l.startswith('control ')]

check('the editor ran to its screenshot', r.returncode == 0 and os.path.exists(shot), (r.returncode, out[-10:]))
inspector = [l for l in out if l.startswith('shadow: ')]
check('Blob takes its size from the footprint, then the typed size',
      inspector == ['shadow: blob size 0.6 darkness 60 Player Body', 'shadow: blob size 0.75 darkness 60 Player Body'],
      inspector)
check('Size and Darkness rows show for a blob', 'obj.shadowsize' in rects and 'obj.shadowdark' in rects,
      [k for k in rects if k.startswith('obj.shadow')])
preview = [l for l in out if l.startswith('shadow preview: ')]
pm = [re.match(r'shadow preview: Player Body floor R(\d+) height (\S+) radius (\S+) darkness (\d+)$', l) for l in preview]
check('the scene view previews it on the nav floor, whole, at each size',
      len(pm) == 2 and all(pm) and [float(x.group(3)) for x in pm] == [0.6, 0.75] and
      all(float(x.group(2)) == 0 and x.group(4) == '153' for x in pm), preview)
box = rects.get('vp.shadow.Player Body')
view = rects.get('vp.view')
check('the blob is drawn inside the scene view',
      bool(box and view and box[0] >= view[0] and box[1] >= view[1] and box[2] <= view[2] and box[3] <= view[3] and
           box[2] - box[0] > 10), (box, view))
if box and os.path.exists(before) and os.path.exists(shot):
    # The blob's screen box, darker than before it was there (it fades to
    # nothing at the rim, so on average by about a tenth).
    b0, b1 = brightness(before, box), brightness(shot, box)
    check('the floor under the body is darker with the blob', b1 < b0 * 0.95, (b0, b1))
saved = json.load(open(scene))
body = next(o for o in saved['objects'] if o['name'] == 'Player Body')
mesh = next(c for c in body['components'] if c['type'] == 'mesh')
check('the saved scene has shadow blob, the typed size and no default darkness',
      mesh.get('shadow') == 'blob' and abs(mesh.get('shadowSize', 0) - 0.75) < 1e-6 and 'shadowDarkness' not in mesh, mesh)
pack = os.path.join(root, 'out.splashpack')
e = subprocess.run([cli, 'export', scene, '-o', pack], capture_output=True, text=True)
d = open(pack, 'rb').read() if e.returncode == 0 else b''
if len(d) >= 160:
    version, table = struct.unpack_from('<H', d, 2)[0], struct.unpack_from('<I', d, 156)[0]
    count = struct.unpack_from('<H', d, table)[0] if table else 0
    rec = struct.unpack_from('<HBBHH', d, table + 4) if count else None
    check('the export is v28 with one blob record', version == 28 and count == 1 and rec and rec[1] == 1 and rec[2] == 153 and rec[3] == round(0.75 / 100 * 4096),
          (version, table, count, rec))
else:
    check('the export is v28 with one blob record', False, e.stdout + e.stderr)
print('shadowcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
