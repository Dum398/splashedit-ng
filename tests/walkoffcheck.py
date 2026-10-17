# Nav walk-off zones in the editor, driven through the GUI: "Nav walk-off
# zone" comes from the Add component picker onto an empty object at the
# courtyard floor's front edge, its Size (obj.wsize.x/.y/.z) is typed, and
# the section's caption (obj.wstats) says how many nav edges the live export
# opened, or warns when the box opens none. Under scripted runs the editor
# prints
#   walkoff: <name> edges N        (or "walkoff: <name> no export")
# as it changes. The default box (1 x 2 x 0.5) floats above the floor and
# opens nothing; 20 x 4 x 1 straddles the edge and opens some; 20 x 1 x 1
# floats again. The viewport draws the box in orange while the overlay
# toggle (vp.gizmos) is on.
# usage: python3 walkoffcheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
ORANGE = (0xff, 0x6a, 0x00)
NAV = (0x9f, 0xd3, 0x6b)
WARN = 'Opens no nav edges: place the box across the edge of a walkable floor.'


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


def fixture():
    """The courtyard with an empty object 'Ledge' over the floor's open front
    edge (the floor runs z -9..6 at y ~0), 1.5 m up."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append({'name': 'Ledge', 'active': True, 'transform': {'position': [0, 1.5, -8.5], 'rotation': [0, 0, 0, 1],
                         'scale': [1, 1, 1]}, 'components': [], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(name, args):
    """Runs the editor on a fresh fixture with Ledge selected and its first
    export done, then `args`, then saves; returns the saved scene, the
    walkoff lines, the named controls' rects and the screenshot."""
    project = fixture()
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Ledge'] + W(20) + ['--wait-export'] + args + \
        ['--key', 'ctrl+s'] + W(10) + ['--move', '760,940'] + W(10) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=280)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    s = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return s, [l for l in lines if l.startswith('walkoff: ')], rects, shot


def walkoff(scene):
    o = next((o for o in scene['objects'] if o.get('name') == 'Ledge'), {})
    return next((c for c in o.get('components', []) if c.get('type') == 'walkoff'), None)


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


ADD = ['--click', '@obj.addcomp'] + W(3) + ['--text', 'walk-off zone'] + W(2) + ['--key', 'enter'] + W(5) + ['--wait-export'] + W(5)
HIT = typed('obj.wsize.x', '20') + typed('obj.wsize.y', '4') + typed('obj.wsize.z', '1') + ['--wait-export'] + W(5)
MISS = typed('obj.wsize.y', '1') + ['--wait-export'] + W(5)
EDGES = re.compile(r'walkoff: Ledge edges (\d+)$')


def counts(out):
    return [int(m.group(1)) for m in map(EDGES.match, out) if m]


# 1. Added from the picker with Unity's default size: above the floor, no edges, warned.
s, out, rects, _ = run('added', ADD)
check('the picker adds a walk-off zone with the default size', walkoff(s) == {'type': 'walkoff', 'size': [1, 2, 0.5]}, walkoff(s))
check('the default box (above the floor) reports edges 0', out[-1:] == ['walkoff: Ledge edges 0'], out)
check('the section and its size fields are drawn', all(n in rects for n in ('s_walkoff', 'obj.wsize.x', 'obj.wsize.y', 'obj.wsize.z')),
      sorted(k for k in rects if 'walkoff' in k or 'wsize' in k))
panel, ws = rects.get('s_walkoff', (0, 0, 0, 0)), rects.get('obj.wstats', (0, 0, 1e9, 0))
check('the no-edges warning is drawn inside the section', 'obj.wstats' in rects and panel[0] <= ws[0] and ws[2] <= panel[2],
      (ws, panel))

# 2. Across the front edge: some edges open; the caption and the box show.
s, out, rects, hit = run('hit', ADD + HIT)
n = counts(out)
check('the line starts at edges 0 (positive control)', n[:1] == [0], out)
check('the box across the edge opens nav edges', n[-1:] and n[-1] > 0, out)
check('the saved scene has the typed size', walkoff(s) == {'type': 'walkoff', 'size': [20, 4, 1]}, walkoff(s))
check('the caption row is drawn', 'obj.wstats' in rects, sorted(k for k in rects if k.startswith('obj.w')))
if keep: shutil.copyfile(hit, keep)

# 3. Lowered height: it floats again, back to 0 and the warning.
s, out, rects, _ = run('miss', ADD + HIT + MISS)
n = counts(out)
check('the box goes from some edges back to 0', len(n) >= 3 and n[0] == 0 and max(n) > 0 and n[-1] == 0, out)
check('the saved scene has height 1', walkoff(s) == {'type': 'walkoff', 'size': [20, 1, 1]}, walkoff(s))
ws = rects.get('obj.wstats', (0, 0, 1e9, 0))
check('the warning is back inside the section', panel[0] <= ws[0] and ws[2] <= rects.get('s_walkoff', (0, 0, 0, 0))[2], (ws, rects.get('s_walkoff')))

# 4. The overlay: the box (selected, so full orange) and the walk-off edges
# show with the overlay toggle on and are gone with it off.
_, _, rects, off = run('off', ADD + HIT + ['--click', '@vp.gizmos'] + W(5))
w, h, a = png_rgb(hit)
_, _, b = png_rgb(off)
view = rects.get('vp.view', (0, 0, 0, 0))
top = rects.get('vp.gizmos', (0, 0, 0, view[1]))[3] + 6


def near(p, c, tol=48):
    return all(abs(p[i] - c[i]) <= tol for i in range(3))


orange_on = orange_off = 0
for y in range(top, min(h, view[3])):
    for x in range(max(0, view[0]), min(w, view[2])):
        orange_on += near(a[y][3 * x:3 * x + 3], ORANGE)
        orange_off += near(b[y][3 * x:3 * x + 3], ORANGE)
print('orange pixels: on %d off %d' % (orange_on, orange_off))
check('the walk-off box is drawn in orange with the overlay on', orange_on - orange_off > 150, (orange_on, orange_off))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
