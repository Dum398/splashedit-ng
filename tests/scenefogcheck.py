# The scene inspector's Rendering section, driven through the GUI: turn fog
# on, type its density, type a GTE scaling, pick Interior, save with Ctrl+S
# and read the saved scene's settings; then the undo arms, each edit one
# Ctrl+Z. Controls are clicked by name (--click @scene.fog): the editor aims at
# where the frame before drew them, so no pixel positions live here.
# usage: python3 scenefogcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


mid = None  # with SAVE_MID in the script: the settings as saved at that point
rects, shot = {}, None  # the last run's named controls and screenshot


def run(args, edit=None):
    """Runs the editor on a fresh courtyard copy (nothing selected, so the
    inspector shows the scene), then `args` and Ctrl+S; returns the saved
    scene's settings and the editor's output (without its list of controls).
    `edit` changes the copied scene's JSON before the editor opens it."""
    global mid, rects, shot
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    if edit:
        d = json.load(open(scene, encoding='utf-8'))
        edit(d)
        json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    mid = json.load(open(snap, encoding='utf-8'))['settings'] if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    shot = os.path.join(project, 'shot.png')
    return json.load(open(scene, encoding='utf-8'))['settings'], out


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    """Double-click a number field, replace its text, Enter."""
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']


def summary(s):
    f = s.get('fog', {})
    return (f.get('enabled'), f.get('density'), s.get('gteScaling'), s.get('sceneType'))


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))['settings']
START = (False, 5, 100.0, 'exterior')
assert summary(original) == START, 'the example\'s settings changed: ' + str(summary(original))

s, out = run([])
check('save alone keeps the example\'s settings', summary(s) == START, f'{summary(s)} | ' + out[-200:])

s, out = run(click('scene.fog'))
check('the Fog switch turns fog on', summary(s) == (True, 5, 100.0, 'exterior'), f'{summary(s)} | ' + out[-200:])

ALL = click('scene.fog') + typed('scene.fogdensity', '7') + typed('scene.gte', '50') + click('scene.scenetype') + \
    click('scenetype/Interior')
s, out = run(ALL)
check('fog, density, GTE scaling and scene type all land in the saved scene', summary(s) == (True, 7, 50.0, 'interior'),
      f'{summary(s)} | ' + out[-200:])
check('the inspector reports the new values', 'scene: fog 1 density 7 colour #808099 gte 50 type interior' in out, out[-300:])

s, out = run(click('scene.fog') + typed('scene.fogdensity', '42'))
check('density is clamped to 10', summary(s)[1] == 10, f'{summary(s)} | ' + out[-200:])

# Undo arms: each edit goes back in one Ctrl+Z, leaving the ones before it.
s, out = run(ALL + SAVE_MID + UNDO)
check('one Ctrl+Z puts Exterior back and keeps the rest',
      mid and summary(mid) == (True, 7, 50.0, 'interior') and summary(s) == (True, 7, 50.0, 'exterior'),
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(click('scene.fog') + typed('scene.fogdensity', '7') + SAVE_MID + UNDO)
check('one Ctrl+Z takes the typed density back', mid and summary(mid)[1] == 7 and summary(s) == (True, 5, 100.0, 'exterior'),
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(click('scene.fog') + SAVE_MID + UNDO)
check('one Ctrl+Z turns fog back off', mid and summary(mid)[0] is True and summary(s) == START,
      f'{mid and summary(mid)} then {summary(s)} | ' + out[-200:])

# The background colour: psxsplash clears the screen to it every frame, fog
# on or off, so the perspective view shows it with fog off too (not the
# editor sky). A colour no sky or courtyard texture comes near.
def png(path):
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


GREEN = (0x20, 0xc0, 0x40)


def green_share(fog_on):
    """Share of the viewport's pixels within 12 of GREEN, the scene's
    background colour, with fog on or off."""
    def edit(d):
        d['settings']['fog'].update({'enabled': fog_on, 'color': [GREEN[0] / 255, GREEN[1] / 255, GREEN[2] / 255]})
    _, out = run(['--no-overlays'], edit)
    v = rects.get('vp.view')
    if not v: return None, out
    _, _, rows = png(shot)
    n = hit = 0
    for y in range(v[1] + 4, v[3] - 4, 4):
        r = rows[y]
        for x in range(v[0] + 4, v[2] - 4, 4):
            n += 1
            hit += all(abs(r[3 * x + i] - GREEN[i]) <= 12 for i in range(3))
    return hit / max(n, 1), out


share, out = green_share(False)
check('fog off: the viewport clears to the background colour', share is not None and share > 0.2, (share, out[-200:]))
share, out = green_share(True)
check('fog on: the viewport clears to the background colour', share is not None and share > 0.2, (share, out[-200:]))
check('the inspector reports the background colour', 'colour #20C040' in out, out[-300:])

# The culling caption follows the Room objects the exporter writes.
s, out = run([])
check('no Room: the caption says frustum culling', 'culling frustum' in out and 'scene.culling' in rects,
      (sorted(k for k in rects if k.startswith('scene.')), out[-200:]))


def rooms(n, off=False):
    def edit(d):
        for o in d['objects'][:n]:
            o['components'].append({'type': 'room', 'size': [4, 4, 4], 'offset': [0, 0, 0]})
            o['active'] = not off
    return edit


s, out = run([], rooms(2))
check('two Rooms: the caption says rooms and portals, 2 rooms', 'culling rooms 2' in out, out[-200:])
s, out = run([], rooms(1, off=True))
check('a switched-off Room is not counted', 'culling frustum' in out, out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
