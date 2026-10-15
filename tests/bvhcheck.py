# The BVH preview in the editor, driven through the GUI: the Scene
# inspector's BVH section (s_bvh, closed until clicked) shows the live
# export's tree, its "Show in viewport" toggle (bvh.show) draws the boxes in
# the scene view, and its Depth field (bvh.depth) limits them to the levels
# down to that depth: 0 leaves only the root box. Under scripted runs the
# editor prints "bvh: nodes N leaves L depth D" once the scene has exported
# and "bvh: showing depth K boxes B" or "bvh: hidden" as the viewport draws
# it; the scene view is shot hidden and shown and must differ inside it.
# usage: python3 bvhcheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# The Scene inspector's sections above BVH start open; folding them brings BVH into the panel.
FOLDS = []
for s in ('s_scenescript', 's_sequences', 's_rendering', 's_loading', 's_build', 's_vram', 's_disc', 's_render'):
    FOLDS += ['--click', '@' + s, '--wait', '3']
AWAY = ['--move', '760,940', '--wait', '10']


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


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


def run(name, args):
    """Runs the editor on a fresh copy of the courtyard with the BVH section
    open, then `args`; returns the screenshot, the printed bvh lines and the
    named controls' rects."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    path = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '20', '--wait-export'] + FOLDS + click('s_bvh') + args + \
        AWAY + ['--screenshot', path]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    check(name + ': the BVH section shows its controls', all(n in rects for n in ('s_bvh', 'bvh.show', 'bvh.depth', 'vp.view')),
          sorted(k for k in rects if 'bvh' in k or k.startswith('vp.')))
    return path, [l for l in lines if l.startswith('bvh: ')], rects


def tree(lines):
    m = next((re.match(r'bvh: nodes (\d+) leaves (\d+) depth (\d+)$', l) for l in lines if l.startswith('bvh: nodes')), None)
    return m and tuple(int(v) for v in m.groups())


def showing(lines):
    return [tuple(int(v) for v in m.groups()) for m in (re.match(r'bvh: showing depth (\d+) boxes (\d+)$', l) for l in lines) if m]


hidden, lines, rects = run('hidden', [])
t = tree(lines)
check('the tree is reported once the scene exports', t is not None and t[0] > 1 and t[1] == (t[0] + 1) // 2 and t[2] > 0, lines)
check('the boxes start hidden', 'bvh: hidden' in lines and not showing(lines), lines)

shown, lines, _ = run('shown', click('bvh.show'))
t2 = tree(lines)
s = showing(lines)
check('Show in viewport draws every level', t2 is not None and s[-1:] == [(t2[2], t2[0])], (t2, lines))

_, lines, _ = run('depth0', click('bvh.show') + typed('bvh.depth', '0'))
s = showing(lines)
check('Depth 0 leaves only the root box', s[-1:] == [(0, 1)] and len(s) >= 2 and s[0][1] > 1, lines)

_, lines, _ = run('depth0off', click('bvh.show') + typed('bvh.depth', '0') + click('bvh.show'))
check('turning it off hides the boxes again', lines[-1:] == ['bvh: hidden'], lines)

if keep: shutil.copyfile(shown, keep)
w, h, a = png_rgb(hidden)
_, _, b = png_rgb(shown)
view = rects.get('vp.view', (0, 0, 0, 0))
changed = 0
for y in range(max(0, view[1]), min(h, view[3])):
    ra, rb = a[y], b[y]
    if ra == rb: continue
    for x in range(max(0, view[0]), min(w, view[2])):
        if ra[3 * x:3 * x + 3] != rb[3 * x:3 * x + 3]: changed += 1
print('viewport pixels changed:', changed)
check('the boxes change the scene view', changed > 2000, changed)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
