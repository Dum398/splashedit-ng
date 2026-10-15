# The scene viewport draws what the game reads but never renders: Trigger
# boxes, Interactable reach rings and the last export's nav regions, and the
# viewport's box toggle (vp.gizmos) hides them. A copy of the courtyard gets
# a Trigger and an Interactable; the same view is shot with the toggle on
# and off, and each overlay is counted by the pixels the "on" shot moved
# towards its colour.
# usage: python3 gizmocheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
COLOURS = {'trigger': (0xe0, 0x8c, 0xff), 'interact': (0xff, 0xb8, 0x6b), 'nav': (0x9f, 0xd3, 0x6b)}


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def png_rgb(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGBA or RGB PNG."""
    data = open(path, 'rb').read()
    pos, idat, w = 8, b'', 0
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


project = os.path.join(tempfile.mkdtemp(), 'courtyard')
shutil.copytree(example, project)
scene = os.path.join(project, 'courtyard.scene')
s = json.load(open(scene, encoding='utf-8'))
s['objects'].append({'name': 'Gate', 'active': True, 'transform': {'position': [-2.5, 1, -2], 'rotation': [0, 0, 0, 1],
                     'scale': [1, 1, 1]}, 'components': [{'type': 'trigger', 'size': [2, 2, 2], 'lua': None}], 'children': []})
s['objects'].append({'name': 'Lever', 'active': True, 'transform': {'position': [3, 0.2, -1], 'rotation': [0, 0, 0, 1],
                     'scale': [1, 1, 1]}, 'components': [
                         {'type': 'interactable', 'radius': 1.5, 'button': 14, 'repeatable': True, 'cooldownFrames': 30,
                          'showPrompt': False, 'promptCanvas': '', 'lineOfSight': False, 'facingAngle': 90}], 'children': []})
json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)


def shot(name, args):
    path = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '30'] + args + ['--wait', '10', '--screenshot', path]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (vp\.\w+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    check(name + ': the viewport and its overlay toggle are drawn', 'vp.view' in rects and 'vp.gizmos' in rects, lines[-5:])
    return path, rects


# The mouse leaves the toolbar in both runs, so the toggle's tooltip is in neither shot.
AWAY = ['--move', '760,940', '--wait', '20']
on, rects = shot('on', AWAY)
off, _ = shot('off', ['--click', '@vp.gizmos'] + AWAY)
if keep: shutil.copyfile(on, keep)
w, h, a = png_rgb(on)
_, _, b = png_rgb(off)
# Inside the viewport, below its toolbars; the rest of the window is not counted.
view = rects.get('vp.view', (0, 0, 0, 0))
top = rects.get('vp.gizmos', (0, 0, 0, view[1]))[3] + 6


def towards(pa, pb, c):
    """How well the change from `pb` to `pa` points at colour `c`: an
    overlay drawn at alpha t moves each pixel by t * (c - pb), so the cosine
    between the change and (c - pb) is near 1 for that overlay's colour."""
    d = [pa[i] - pb[i] for i in range(3)]
    e = [c[i] - pb[i] for i in range(3)]
    nd = sum(v * v for v in d) ** 0.5
    ne = sum(v * v for v in e) ** 0.5
    if nd < 24 or ne == 0: return 0
    return sum(d[i] * e[i] for i in range(3)) / (nd * ne)


counts = dict.fromkeys(COLOURS, 0)
for y in range(top, min(h, view[3])):
    ra, rb = a[y], b[y]
    if ra == rb: continue
    for x in range(max(0, view[0]), min(w, view[2])):
        pa, pb = ra[3 * x:3 * x + 3], rb[3 * x:3 * x + 3]
        if pa == pb: continue
        cos = {k: towards(pa, pb, c) for k, c in COLOURS.items()}
        k = max(cos, key=cos.get)
        if cos[k] > 0.97: counts[k] += 1
print('overlay pixels:', counts)
check('the Trigger box is drawn', counts['trigger'] > 200, counts)
check('the Interactable reach ring is drawn', counts['interact'] > 200, counts)
check('the nav regions from the export are drawn', counts['nav'] > 2000, counts)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
