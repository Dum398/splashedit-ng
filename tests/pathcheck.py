# Paths in the editor (splashedit-ng#208), driven through the GUI on a copy of
# the first-person template with a path 'Loop' (3 points in its own space, the
# object at 0, 0.5, 2) and a crate 'Cart' that follows it. The Scene view
# draws the curve (lime) and, with Loop selected, its numbered points
# (vp.pathpt<i>), which drag on their own height. The inspector's Path
# section inserts (obj.pathpt<i>.ins), deletes (.del) and adds points at the
# view centre (pathaddview), switches Closed and Mode, types Speed; Follow
# path picks its path (obj.followpath) and types its start offset. "Path"
# comes from the Add component picker. Each run saves with Ctrl+S and the
# check reads the saved scene. Under scripted runs the editor prints
#   path: <name> points N closed on|off speed S mode once|loop|pingpong autostart on|off length L followers F
#   follow: <name> path P speed S offset O face on|off by agent|animation
# as they change.
# usage: python3 pathcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
template = os.path.join(here, '..', 'templates', 'first-person')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
LIME = (0xc8, 0xf0, 0x6a)
POINTS = [[0, 0, 0], [3, 0, 0], [3, 0, 3]]


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
    project = os.path.join(tempfile.mkdtemp(), 'fp')
    shutil.copytree(template, project)
    scene = os.path.join(project, 'first_person.scene')
    s = json.load(open(scene, encoding='utf-8'))
    crate = next(c for c in s['objects'][0]['children'] if c['name'] == 'Crate')
    mesh = next(c for c in crate['components'] if c['type'] == 'mesh')
    s['objects'].append({'name': 'Loop', 'active': True, 'transform': {'position': [0, 0.5, 2], 'rotation': [0, 0, 0, 1],
                         'scale': [1, 1, 1]}, 'components': [{'type': 'path', 'points': POINTS, 'closed': False, 'speed': 2,
                                                              'mode': 'loop', 'autostart': True}], 'children': []})
    s['objects'].append({'name': 'Cart', 'active': True, 'transform': {'position': [-2, 0.5, 0], 'rotation': [0, 0, 0, 1],
                         'scale': [0.5, 0.5, 0.5]}, 'components': [mesh, {'type': 'followPath', 'path': 'Loop', 'speed': 0,
                                                                          'startOffset': 0, 'faceAlong': True}], 'children': []})
    s['objects'].append({'name': 'Spare', 'active': True, 'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1],
                         'scale': [1, 1, 1]}, 'components': [], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(name, select, args):
    """The editor on a fresh fixture with `select` selected and its first
    export done, then `args`, then Ctrl+S and a screenshot; returns the saved
    scene's objects by name, the path:/follow: lines, the named rects and the shot."""
    project = fixture()
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'first_person.scene'),
           '--select', select] + W(20) + ['--wait-export'] + args + ['--key', 'ctrl+s'] + W(10) + ['--wait-export'] + W(5) + \
        ['--move', '760,940'] + W(10) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=280)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    s = json.load(open(os.path.join(project, 'first_person.scene'), encoding='utf-8'))
    objs = {o['name']: o for o in s['objects']}
    return objs, [l for l in lines if l.startswith(('path: ', 'follow: '))], rects, shot


def comp(objs, name, kind):
    return next((c for c in objs.get(name, {}).get('components', []) if c.get('type') == kind), None)


def typed(name, text):
    return ['--scroll-to', '@' + name] + W(3) + ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text,
                                                                                     '--key', 'enter'] + W(3)


def click(name):
    return (['--scroll-to', '@' + name] + W(3) if '/' not in name else []) + ['--click', '@' + name] + W(4)


def near(p, c, tol=40):
    return all(abs(p[i] - c[i]) <= tol for i in range(3))


# 1. Shown: the line, the points in the viewport, the section, the curve in lime.
objs, out, rects, shot = run('shown', 'Loop', [])
check('the path line reports the saved path and its follower',
      any(re.match(r'path: Loop points 3 closed off speed 2 mode loop autostart on length \S+ followers 1$', l) for l in out), out)
check('the three points are drawn in the viewport', all('vp.pathpt%d' % i in rects for i in range(3)),
      [k for k in rects if k.startswith('vp.path')])
names = ('s_path', 'obj.pathclosed', 'obj.pathspeed', 'obj.pathmode', 'obj.pathauto', 'obj.pathpt0.x', 'obj.pathpt2.del',
         'obj.pathpt2.ins', 'pathaddview', 'obj.pathlen', 'obj.pathlua')
check('the section, its fields and buttons are drawn', all(n in rects for n in names), [n for n in names if n not in rects])
w, h, px = png_rgb(shot)
view = rects.get('vp.view', (0, 0, w, h))
# The point markers are lime too: only pixels more than 16 px from every marker's centre count, so this
# measures the curve between the points.
dots = [((r[0] + r[2]) / 2, (r[1] + r[3]) / 2) for k, r in rects.items() if k.startswith('vp.pathpt')]
lime = sum(near(px[y][3 * x:3 * x + 3], LIME) and all((x - cx) ** 2 + (y - cy) ** 2 > 256 for cx, cy in dots)
           for y in range(max(0, view[1]), min(h, view[3])) for x in range(max(0, view[0]), min(w, view[2])))
print('lime pixels away from the point markers: %d' % lime)
check('the curve is drawn in lime between the points', lime > 120, lime)
pts = [rects['vp.pathpt%d' % i] for i in range(3) if 'vp.pathpt%d' % i in rects]
check('the points sit apart on screen', len(pts) == 3 and len({(p[0] // 4, p[1] // 4) for p in pts}) == 3, pts)

# 2. A drag moves one point on its own height, in the object's space.
objs, out, rects, _ = run('drag', 'Loop', ['--drag', '@vp.pathpt2,0,0,100,0'] + W(6))
p = (comp(objs, 'Loop', 'path') or {}).get('points', [])
check('the drag moves point 3 in x/z, keeps its y and leaves the others',
      len(p) == 3 and p[:2] == POINTS[:2] and p[2][1] == 0 and abs(p[2][0] - 3) + abs(p[2][2] - 3) > 0.3, p)
check('the object itself did not move', objs.get('Loop', {}).get('transform', {}).get('position') == [0, 0.5, 2],
      objs.get('Loop', {}).get('transform'))

# 3. Insert, delete, Closed, Mode, Speed.
EDIT = click('obj.pathpt0.ins') + click('obj.pathpt3.del') + click('obj.pathclosed') + click('obj.pathmode') + \
    click('pathmode/Ping-pong') + typed('obj.pathspeed', '3')
objs, out, rects, _ = run('edit', 'Loop', EDIT)
pc = comp(objs, 'Loop', 'path') or {}
check('insert adds the mid point and delete takes the last',
      pc.get('points') == [[0, 0, 0], [1.5, 0, 0], [3, 0, 0]], pc.get('points'))
check('Closed, Ping-pong and the typed speed are saved',
      pc.get('closed') is True and pc.get('mode') == 'pingPong' and pc.get('speed') == 3, pc)
check('the line follows', out and re.match(r'path: Loop points 3 closed on speed 3 mode pingpong autostart on ', out[-1]), out)
objs, out, rects, _ = run('undo', 'Loop', click('obj.pathpt0.del') + ['--key', 'ctrl+z'] + W(4))
check('Ctrl+Z brings a deleted point back', (comp(objs, 'Loop', 'path') or {}).get('points') == POINTS,
      (comp(objs, 'Loop', 'path') or {}).get('points'))

# 4. At view centre: a fourth point at the object's own height (local y 0).
objs, out, rects, _ = run('addview', 'Loop', click('pathaddview'))
p = (comp(objs, 'Loop', 'path') or {}).get('points', [])
check('"At view centre" adds a fourth point at the object\'s height', len(p) == 4 and p[:3] == POINTS and abs(p[3][1]) < 1e-4, p)

# 5. Follow path: the line, its path picked off and on, the offset typed.
objs, out, rects, _ = run('follow', 'Cart', typed('obj.followoffset', '0.5') + click('obj.followpath') + click('followpath/None'))
fp = comp(objs, 'Cart', 'followPath') or {}
check('the follow line reported the follower by animation',
      any(l == 'follow: Cart path Loop speed 0 offset 0 face on by animation' for l in out), out)
check('the offset is typed and None clears the path', fp.get('startOffset') == 0.5 and fp.get('path') == '', fp)
check('then the line says so', out and out[-1] == 'follow: Cart path - speed 0 offset 0.5 face on by animation', out)
objs, out, rects, _ = run('followpick', 'Cart', click('obj.followpath') + click('followpath/Loop') + click('obj.followface'))
fp = comp(objs, 'Cart', 'followPath') or {}
check('picking Loop again and Face along off are saved', fp.get('path') == 'Loop' and fp.get('faceAlong') is False, fp)

# 6. The picker adds a Path with two points; with nothing following it.
objs, out, rects, _ = run('picker', 'Spare', ['--click', '@obj.addcomp'] + W(3) + ['--text', 'curve route'] + W(2) +
                          ['--key', 'enter'] + W(5))
pc = comp(objs, 'Spare', 'path')
check('the picker adds a path of two points', pc and pc.get('points') == [[-2, 0, 0], [2, 0, 0]] and pc.get('mode') == 'loop', pc)
check('its line has no followers', out and out[-1].startswith('path: Spare points 2 ') and out[-1].endswith(' followers 0'), out)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
