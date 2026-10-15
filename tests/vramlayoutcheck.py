# The scene inspector's VRAM layout section, driven through the GUI on copies
# of the courtyard: Add area (a 64 x 64 area at the first free spot, 320,0),
# type a new width into its W field, drag on the VRAM map to draw a second
# area (snapped to 8 words and lines), remove the first. Every change is saved
# to vram.json at once and is not a scene edit. The editor prints the areas
# ("vram: x,y,w,h | x,y,w,h", "vram: none") whenever they change. The map is
# drawn from the live export's VRAM map, so the area left must show red on it
# in the screenshot (the live export packs with vram.json). Controls are
# clicked by name (--click @vram.add, --drag @vram.map,dx0,dy0,dx1,dy1).
# usage: python3 vramlayoutcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# Scrolls the scene inspector so the whole section is on screen.
SCROLL = ['--wheel', '1400,500,-6', '--wait', '5']


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args):
    """Runs the editor on a fresh courtyard copy (the scene inspector shows),
    scrolls to the section, then `args`, and lets the live export settle.
    Returns the printed vram: lines, the other output lines, vram.json's
    contents (None when there is none), the named controls and the shot."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + SCROLL + args + [
        '--wait', '90', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    path = os.path.join(project, 'vram.json')
    data = json.load(open(path)) if os.path.exists(path) else None
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].split(' at ')
            a, b = at.split(' ')
            controls[name] = tuple(float(v) for v in a.split(',') + b.split(','))
    return [l for l in lines if l.startswith('vram:')], lines, data, controls, shot


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def type_into(name, text):
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


# The PNG reader gizmocheck.py uses (no third-party modules).
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


def area(s):
    return tuple(int(v) for v in s.split(','))


vrams, lines, data, controls, _ = run([])
check('no areas to start with', vrams == ['vram: none'] and data is None, (vrams, data))
check('the map is drawn', 'vram.map' in controls, sorted(controls))
mx0, my0, mx1, my1 = controls.get('vram.map', (0, 0, 1024, 512))
scale = (mx1 - mx0) / 1024.0
cx, cy = (mx0 + mx1) / 2, (my0 + my1) / 2


def offset(vx, vy):
    """VRAM word/line -> offset from the map's centre, in pixels."""
    return mx0 + vx * scale - cx, my0 + vy * scale - cy


# A drag from 512,320 to 640,416 in VRAM.
(dx0, dy0), (dx1, dy1) = offset(512, 320), offset(640, 416)
drag = ['--drag', '@vram.map,%.1f,%.1f,%.1f,%.1f' % (dx0, dy0, dx1, dy1), '--wait', '5']

vrams, lines, data, controls, shot = run(click('vram.add') + type_into('vram.w.0', '128') + drag + click('vram.remove.0'))
check('add, type, drag, remove: one line per change', len(vrams) == 5 and vrams[:3] ==
      ['vram: none', 'vram: 320,0,64,64', 'vram: 320,0,128,64'], vrams)
drawn = None
if len(vrams) == 5 and vrams[3].startswith('vram: 320,0,128,64 | '):
    drawn = area(vrams[3].split(' | ')[1])
    check('the dragged area is snapped to 8 and near where the mouse went',
          all(v % 8 == 0 for v in drawn) and abs(drawn[0] - 512) <= 8 and abs(drawn[1] - 320) <= 8 and
          abs(drawn[2] - 128) <= 16 and abs(drawn[3] - 96) <= 16, drawn)
    check('remove leaves the dragged area', vrams[4] == 'vram: %d,%d,%d,%d' % drawn, vrams[4])
else:
    check('the drag adds a second area', False, vrams)
want = [dict(zip('xywh', drawn))] if drawn else None
check('vram.json holds what is left', data == {'prohibited': want}, data)
check('vram.json is no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])

# The map shows the area red: the live export packed with vram.json.
_, _, rows = png_rgb(shot)


def reddish(vx, vy):
    px, py = int(mx0 + vx * scale), int(my0 + vy * scale)
    r, g, b = rows[py][3 * px:3 * px + 3]
    return r > g + 40 and r > b + 30, (r, g, b)


if drawn:
    x, y, w, h = drawn
    inside = [reddish(x + w * fx, y + h * fy) for fx in (0.3, 0.5, 0.7) for fy in (0.3, 0.5, 0.7)]
    check('the map shows the area red', all(ok for ok, _ in inside), [c for _, c in inside])
gone = reddish(352, 32)
check('the removed area is no longer red', not gone[0], gone[1])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
