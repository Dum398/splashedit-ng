# Rooms and portals in the editor, driven through the GUI: Room and Portal
# come from the Add component picker; a Portal's Room A and Room B are
# picked from dropdowns of the scene's Room objects and land in the saved
# scene (one Ctrl+Z takes Room B back); a Room shows what the live export
# made of it ("N triangles, M cells, K portals"); the viewport draws the room
# boxes and the portal's opening and arrow, and the box toggle (vp.gizmos)
# hides them. Controls are clicked by name (--click @obj.pa); the overlay
# pixels are classified by colour the way gizmocheck.py does.
# usage: python3 roomscheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
COLOURS = {'room': (0x40, 0xe0, 0xd0), 'portal': (0xff, 0xe1, 0x4d), 'nav': (0x9f, 0xd3, 0x6b)}


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



def obj(name, pos, components, rot=(0, 0, 0, 1)):
    return {'name': name, 'active': True, 'transform': {'position': list(pos), 'rotation': list(rot), 'scale': [1, 1, 1]},
            'components': components, 'children': []}


def room(size):
    return {'type': 'room', 'size': list(size), 'offset': [0, 0, 0]}


def project_copy(linked):
    """The courtyard split in two Rooms, West and East, with a Door portal
    between them facing +X (unlinked unless `linked`), and two bare objects,
    Hall and Gap, to add components to."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append(obj('West', (-3.6, 2, 0.5), [room((7, 4.4, 12))]))
    s['objects'].append(obj('East', (3.6, 2, 0.5), [room((7, 4.4, 12))]))
    s['objects'].append(obj('Door', (0, 1.5, -1.5), [{'type': 'portal', 'roomA': 'West' if linked else '',
                                                      'roomB': 'East' if linked else '', 'width': 2, 'height': 3}],
                            (0, 0.70710678, 0, 0.70710678)))
    s['objects'].append(obj('Hall', (0, 0, 8), []))
    s['objects'].append(obj('Gap', (0, 0, 9), []))
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


mid = None


def run(select, args, linked=False, env=None):
    """Runs the editor on a fresh copy with `select` selected, then `args`
    and Ctrl+S; returns the saved scene and the editor's output lines
    (without its list of named controls)."""
    global mid
    project = project_copy(linked)
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=180)
    mid = json.load(open(snap, encoding='utf-8')) if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check('every control the script names was drawn', not missing, missing)
    return json.load(open(scene, encoding='utf-8')), [l for l in lines if not l.startswith('control ')]


def comp(scene, name, kind):
    for o in (scene or {}).get('objects', []):
        if o['name'] == name:
            return next((c for c in o['components'] if c['type'] == kind), None)
    return None


def portal(scene):
    c = comp(scene, 'Door', 'portal')
    return c and (c['roomA'], c['roomB'], c['width'], c['height'])


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


def add_component(query):
    return click('obj.addcomp') + ['--text', query, '--wait', '2', '--key', 'enter', '--wait', '3']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']

# 1. The Add component picker offers both kinds.
s, out = run('Hall', add_component('room') + click('tree.Gap') + add_component('portal'))
check('Room is added to Hall from the picker', comp(s, 'Hall', 'room') == {'type': 'room', 'size': [1, 1, 1], 'offset': [0, 0, 0]},
      comp(s, 'Hall', 'room'))
check('Portal is added to Gap from the picker', comp(s, 'Gap', 'portal') is not None and comp(s, 'Hall', 'portal') is None,
      (comp(s, 'Gap', 'portal'), comp(s, 'Hall', 'portal')))

# 2. Room A and B from the dropdowns, the opening's size typed; saved, and the printed state follows.
PICK = click('obj.pa') + click('pa/West') + click('obj.pb') + click('pb/East') + typed('obj.pw', '2.5')
s, out = run('Door', PICK)
check('Room A, Room B and width land in the saved scene', portal(s) == ('West', 'East', 2.5, 3), f'{portal(s)} | {out[-6:]}')
printed = [l for l in out if l.startswith('portal: Door')]
check('the unlinked portal reports both missing rooms first', printed[:1] == ['portal: Door a= b= 2x3 problems 2'], printed)
check('the portal line follows the picks', printed[-1:] == ['portal: Door a=West b=East 2.5x3'], printed)
s, out = run('Door', PICK + SAVE_MID + UNDO)
check('one Ctrl+Z takes the typed width back and keeps Room A and B',
      portal(mid) == ('West', 'East', 2.5, 3) and portal(s) == ('West', 'East', 2, 3), f'{portal(mid)} then {portal(s)}')
s, out = run('Door', click('obj.pa') + click('pa/East') + click('obj.pb') + click('pb/East'))
printed = [l for l in out if l.startswith('portal: Door')]
check('the same room on both sides is flagged as the exporter flags it',
      portal(s) == ('East', 'East', 2, 3) and printed[-1:] == ['portal: Door a=East b=East 2x3 problems 1'], (portal(s), printed))

# 3. Once the portal is linked the export builds the rooms; West shows its share.
# The export is stretched so even a Release build shows the placeholder first; the export that
# started before the picks (portal unlinked, so it fails) finishes before the linked one runs;
# --wait-export holds until the result matches the scene, whatever the frame rate.
s, out = run('Door', PICK + click('tree.West') + ['--wait-export'], env={'SPLASHEDIT_EXPORT_DELAY_MS': '1000'})
rooms = [l for l in out if l.startswith('room: West')]
check('the room line is there before the export lands', rooms[:1] in (['room: West Not exported yet'],
                                                                      ['room: West Export failed: see problems']), rooms)
m = re.match(r'room: West (\d+) triangles?, (\d+) cells?, (\d+) portals?$', rooms[-1] if rooms else '')
check('West shows its triangles, cells and the one portal', m and int(m.group(1)) > 0 and int(m.group(2)) >= 1 and m.group(3) == '1',
      rooms)


# 4. The viewport draws the rooms and the portal; the box toggle hides them.
def shot(name, args):
    project = shot.project
    path = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '30'] + args + ['--wait', '10', '--screenshot', path]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    check(name + ': every control the script names was drawn', not [l for l in lines if 'no such control drawn' in l])
    rects = {}
    for l in lines:
        mm = re.match(r'control (vp\.\w+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if mm: rects[mm.group(1)] = tuple(int(v) for v in mm.groups()[1:])
    check(name + ': the viewport and its overlay toggle are drawn', 'vp.view' in rects and 'vp.gizmos' in rects, lines[-5:])
    return path, rects


shot.project = project_copy(True)
AWAY = ['--move', '760,940', '--wait', '20']
on, rects = shot('on', AWAY)
off, _ = shot('off', ['--click', '@vp.gizmos'] + AWAY)
if keep: shutil.copyfile(on, keep)
w, h, a = png_rgb(on)
_, _, b = png_rgb(off)
view = rects.get('vp.view', (0, 0, 0, 0))
top = rects.get('vp.gizmos', (0, 0, 0, view[1]))[3] + 6
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
check('the Room boxes are drawn', counts['room'] > 400, counts)
check('the Portal opening and its arrow are drawn', counts['portal'] > 300, counts)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
