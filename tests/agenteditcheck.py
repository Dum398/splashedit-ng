# The Agent component in the editor, driven through the GUI on a copy of the
# courtyard example with an empty object 'Guard' (at -1, 0, 1, turned 45
# degrees): "Agent" comes from the Add component picker, Move speed
# (obj.agspeed) is typed, waypoints are added with "At object" (agaddobj) and
# "At view centre" (agaddview), one coordinate is typed (obj.agwp1.x), they
# are reordered (obj.agwp1.up) and deleted (obj.agwp0.del, then Ctrl+Z), and
# a waypoint marker is dragged in the viewport (vp.agwp1). Each run saves
# with Ctrl+S and the check reads the saved scene. Under scripted runs the
# editor prints
#   agent: <name> enabled on speed S ... waypoints N clips c0,..,c7 bytes B warnings K
# as it changes. The viewport draws the selected agent's vision cone, hearing
# square and waypoints in pink and blue only while the overlay toggle
# (vp.gizmos) is on.
# usage: python3 agenteditcheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
PINK = (0xff, 0x5f, 0xa0)


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
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append({'name': 'Guard', 'active': True, 'transform': {'position': [-1, 0, 1],
                         'rotation': [0, 0.38268343, 0, 0.92387953], 'scale': [1, 1, 1]}, 'components': [], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


last_project = [None]
guard_pos = [None]


def run(name, args):
    """Runs the editor on a fresh fixture with Guard selected and its first
    export done, then `args`, then saves and shoots after the export lands;
    returns the saved scene's Guard agent, the agent lines, the named
    controls' rects and the screenshot."""
    project = fixture()
    last_project[0] = project
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Guard'] + W(20) + ['--wait-export'] + args + \
        ['--key', 'ctrl+s'] + W(10) + ['--wait-export'] + W(5) + ['--move', '760,940'] + W(10) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=280)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    s = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    g = next((o for o in s['objects'] if o.get('name') == 'Guard'), {})
    guard_pos[0] = g.get('transform', {}).get('position')
    agent = next((c for c in g.get('components', []) if c.get('type') == 'agent'), None)
    return agent, [l for l in lines if l.startswith('agent: ')], rects, shot


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


ADD = ['--click', '@obj.addcomp'] + W(3) + ['--text', 'agent npc'] + W(2) + ['--key', 'enter'] + W(5)
WPS = ['--click', '@agaddobj'] + W(4) + ['--click', '@agaddview'] + W(4)
EDIT = ADD + typed('obj.agspeed', '3.5') + WPS + typed('obj.agwp1.x', '2')


def near(p, c, tol=40):
    return all(abs(p[i] - c[i]) <= tol for i in range(3))


def inside(rect, panel):
    return panel[0] <= rect[0] and rect[2] <= panel[2] and panel[1] <= rect[1] and rect[3] <= panel[3]


# 1. Added from the picker: the defaults, no waypoints.
agent, out, rects, _ = run('added', ADD)
check('the picker adds an agent with the defaults',
      agent == {'type': 'agent', 'startEnabled': True, 'moveSpeed': 2, 'stopDistance': 0.5, 'vision': True, 'visionRange': 8,
                'fov': 90, 'visionRegionDepth': 2, 'hearingRange': 0, 'alertTimeout': 90, 'waypoints': [], 'stateClips': {}}, agent)
check('the line reports the defaults and 28 bytes',
      out[-1:] == ['agent: Guard enabled on speed 2 stop 0.5 vision on range 8 fov 90 depth 2 hearing 0 alert 90 waypoints 0 '
                   'clips -1,-1,-1,-1,-1,-1,-1,-1 bytes 28 warnings 0'], out)
panel = rects.get('obj.panel', (0, 0, 0, 0))
names = ('s_agent', 'obj.agspeed', 'obj.agstop', 'obj.agrange', 'obj.agfov', 'obj.agdepth', 'obj.aghear', 'obj.agalert',
         'agaddobj', 'agaddview', 'obj.agclips', 'obj.agcost', 'obj.aglua')
check('the section, its fields, buttons and captions are drawn', all(n in rects for n in names), [n for n in names if n not in rects])
outside = [n for n in names if n in rects and n != 's_agent' and rects[n][2] > rects['s_agent'][2]]
check('nothing runs past the section on the right', not outside, [(n, rects[n]) for n in outside])

# 2. Speed typed, two waypoints added, one coordinate typed.
agent, out, rects, shot = run('edited', EDIT)
wps = (agent or {}).get('waypoints', [])
check('the typed move speed is saved', agent and agent['moveSpeed'] == 3.5, agent)
check('"At object" adds the object\'s world position', wps[:1] == [[-1, 0, 1]], wps)
check('"At view centre" adds a second waypoint whose typed x is 2', len(wps) == 2 and wps[1][0] == 2, wps)
check('the line has 2 waypoints and 52 bytes', out and ' waypoints 2 ' in out[-1] and out[-1].endswith('bytes 52 warnings 0'), out)
check('the waypoint rows and their buttons are drawn inside the panel',
      all(n in rects and inside(rects[n], panel) for n in ('obj.agwp0.x', 'obj.agwp1.z', 'obj.agwp1.up', 'obj.agwp1.del')),
      {n: rects.get(n) for n in ('obj.agwp0.x', 'obj.agwp1.z', 'obj.agwp1.up', 'obj.agwp1.del')})
check('both waypoint markers are drawn in the viewport', all(n in rects for n in ('vp.agwp0', 'vp.agwp1')),
      [k for k in rects if k.startswith('vp.')])
if keep: shutil.copyfile(shot, keep)

# 3. Reorder, delete and undo.
agent, out, rects, _ = run('reorder', EDIT + ['--click', '@obj.agwp1.up'] + W(4))
r = (agent or {}).get('waypoints', [])
check('Up swaps the two waypoints', len(r) == 2 and r[1] == [-1, 0, 1] and r[0][0] == 2, r)
agent, out, rects, _ = run('delete', EDIT + ['--click', '@obj.agwp0.del'] + W(4))
d = (agent or {}).get('waypoints', [])
check('Delete removes the first waypoint', len(d) == 1 and d[0][0] == 2, d)
agent, out, rects, _ = run('undo', EDIT + ['--click', '@obj.agwp0.del'] + W(4) + ['--key', 'ctrl+z'] + W(4))
u = (agent or {}).get('waypoints', [])
check('Ctrl+Z brings it back', u == wps, (u, wps))

# 4. Dragging the second marker 120 px right moves it on its own height; the
# first sits on the move gizmo's centre and its dot still takes the drag.
agent, out, rects, _ = run('drag', EDIT + ['--drag', '@vp.agwp1,0,0,120,0'] + W(6))
g = (agent or {}).get('waypoints', [])
check('the drag moves waypoint 2 in x/z and keeps its y', len(g) == 2 and g[0] == [-1, 0, 1] and
      g[1][1] == wps[1][1] and abs(g[1][0] - wps[1][0]) + abs(g[1][2] - wps[1][2]) > 0.3, (g, wps))
agent, out, rects, _ = run('drag0', EDIT + ['--drag', '@vp.agwp0,0,0,0,80'] + W(6))
g0 = (agent or {}).get('waypoints', [])
check('waypoint 1, over the gizmo centre, drags instead of the object', len(g0) == 2 and g0[0] != [-1, 0, 1] and
      g0[0][1] == 0 and guard_pos[0] == [-1, 0, 1], (g0, guard_pos[0]))

# 5. The overlay: pink cone/waypoints with it on, gone with it off.
_, _, rects2, off = run('off', EDIT + ['--click', '@vp.gizmos'] + W(5))
w, h, a = png_rgb(shot)
_, _, b = png_rgb(off)
view = rects2.get('vp.view', (0, 0, 0, 0))
top = rects2.get('vp.gizmos', (0, 0, 0, view[1]))[3] + 6
on_px = off_px = 0
for y in range(top, min(h, view[3])):
    for x in range(max(0, view[0]), min(w, view[2])):
        on_px += near(a[y][3 * x:3 * x + 3], PINK)
        off_px += near(b[y][3 * x:3 * x + 3], PINK)
print('pink pixels: on %d off %d' % (on_px, off_px))
check('the agent overlay is drawn in pink with the overlay on', on_px - off_px > 300, (on_px, off_px))
check('no waypoint markers are named with the overlay off', 'vp.agwp0' not in rects2, [k for k in rects2 if k.startswith('vp.ag')])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
