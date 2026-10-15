# Nav region inspection in the editor, driven through the GUI: the Scene
# inspector's Navigation section (s_nav, closed until clicked) gives the live
# export's region and portal counts, toggles for the outlines (nav.show), the
# "R<k>" labels (nav.labels) and the portal edges (nav.portals), and a region
# field (nav.region) with the selected region's details (nav.details).
# Clicking a label (nav.label/<k>) selects that region. Under scripted runs
# the editor prints "nav: regions N portals P" once the scene has exported and
# "nav: region K floor Y room R portals j,j" or "nav: region none" as the
# selection changes.
# Also checked: the section's layout does not move when the export lands, the
# outlines change the scene view, and with labels on a click on the scene
# away from them still selects and deselects objects as before.
# usage: python3 navinspectcheck.py <splashedit> [keep.png]   (needs a display: xvfb-run -a)
import os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# The Scene inspector's sections above Navigation start open; folding them brings it into the panel.
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


def changed(a, b, rect):
    """Pixels that differ between two shots inside rect (x0, y0, x1, y1)."""
    w, h, ra = a
    _, _, rb = b
    n = 0
    for y in range(max(0, rect[1]), min(h, rect[3])):
        if ra[y] == rb[y]: continue
        for x in range(max(0, rect[0]), min(w, rect[2])):
            if ra[y][3 * x:3 * x + 3] != rb[y][3 * x:3 * x + 3]: n += 1
    return n


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def run(name, args, wait_export=True, env=None):
    """Runs the editor on a fresh copy of the courtyard with the Navigation
    section open, then `args`; returns the screenshot, the printed nav
    lines and the named controls' rects."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    path = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '20'] + (['--wait-export'] if wait_export else []) + \
        FOLDS + click('s_nav') + args + AWAY + ['--screenshot', path]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return path, [l for l in lines if l.startswith('nav: ')], rects


SECTION = ('s_nav', 'nav.show', 'nav.labels', 'nav.portals', 'nav.region', 'nav.details', 'vp.view')
REGION = re.compile(r'nav: region (\d+) floor (-?\d+\.\d\d) room (\S+) portals (\S+)$')


def counts(lines):
    m = next((re.match(r'nav: regions (\d+) portals (\d+)$', l) for l in lines if l.startswith('nav: regions')), None)
    return m and (int(m.group(1)), int(m.group(2)))


# Closed by default, nothing selected, outlines drawn.
plain, lines, rects = run('plain', [])
c = counts(lines)
check('the counts are reported once the scene exports', c is not None and c[0] > 1 and c[1] > 0, lines)
check('no region starts selected', 'nav: region none' in lines and not any(REGION.match(l) for l in lines), lines)
check('the Navigation section shows its controls', all(n in rects for n in SECTION), sorted(k for k in rects if 'nav' in k))
check('labels start off', not any(k.startswith('nav.label/') for k in rects), sorted(k for k in rects if 'nav' in k))

# Labels on, then a click on region 0's label selects it.
picked, lines, rects2 = run('picked', click('nav.labels') + click('nav.label/0'))
check('labels are drawn once turned on', sum(k.startswith('nav.label/') for k in rects2) == (c[0] if c else -1),
      sorted(k for k in rects2 if k.startswith('nav.label/'))[:5])
m = next((REGION.match(l) for l in lines if REGION.match(l)), None)
check('clicking R0 selects region 0', m is not None and m.group(1) == '0' and lines[-1] == m.group(0), lines)
if m:
    ns = [int(v) for v in m.group(4).split(',')] if m.group(4) != '-' else []
    check('region 0 reports its room (an island: the courtyard has no Rooms) and neighbours',
          m.group(3).startswith('island-') and ns and all(0 < n < c[0] for n in ns), m.group(0))
check('the details block does not move when a region is picked', rects2.get('nav.details') == rects.get('nav.details'),
      (rects.get('nav.details'), rects2.get('nav.details')))
a, b = png_rgb(plain), png_rgb(picked)
d = changed(a, b, rects2.get('nav.details', (0, 0, 0, 0)))
print('details pixels changed:', d)
check("the selected region's details appear", d > 400, d)

# Typing a region number selects it too.
_, lines, _ = run('typed', ['--dblclick', '@nav.region', '--wait', '3', '--key', 'ctrl+a', '--text', '1', '--key', 'enter',
                            '--wait', '3'])
check('typing 1 in the Region field selects region 1', any(l.startswith('nav: region 1 floor ') for l in lines), lines)

# Before the export lands the caption and details have their space already.
_, lines, early = run('early', [], wait_export=False, env={'SPLASHEDIT_EXPORT_DELAY_MS': '30000'})
check('the early run is before the export', counts(lines) is None, lines)
below = 's_stream'
check('the layout does not move when the export lands',
      all(n in early and n in rects for n in ('nav.region', 'nav.details', below)) and
      all(early[n][1] - early['s_nav'][1] == rects[n][1] - rects['s_nav'][1] for n in ('nav.region', 'nav.details', below)),
      {n: (early.get(n), rects.get(n)) for n in ('s_nav', 'nav.region', 'nav.details', below)})

# Outlines off changes the scene view.
hidden, _, _ = run('hidden', click('nav.show'))
d = changed(a, png_rgb(hidden), rects.get('vp.view', (0, 0, 0, 0)))
print('viewport pixels changed:', d)
check('turning the outlines off changes the scene view', d > 2000, d)

# Contending input: with labels on, a click on the floor away from every label
# still selects an object (the Object inspector replaces the Scene one), a
# click on the empty sky deselects it again, and neither picks a region.
view = rects.get('vp.view', (0, 0, 0, 0))
labels = [r for k, r in rects2.items() if k.startswith('nav.label/')]


def clear(p, margin=30):
    return all(not (r[0] - margin <= p[0] <= r[2] + margin and r[1] - margin <= p[1] <= r[3] + margin) for r in labels)


floor = next((p for p in ((500, 620), (460, 600), (980, 640), (700, 650)) if clear(p)), None)
sky = (view[0] + 60, view[1] + 120)
check('a floor spot away from the labels exists', floor is not None and clear(sky), (floor, labels[:4]))
floor = floor or (500, 620)
pt = lambda p: ['--click', '%d,%d' % p, '--wait', '5']
for on in (False, True):
    tag = 'labels on' if on else 'labels off'
    pre = click('nav.labels') if on else []
    _, lines, rs = run('sel' + str(on), pre + pt(floor))
    check(tag + ': a click on the floor selects an object', any(k.startswith('obj.') for k in rs) and 's_nav' not in rs,
          sorted(k for k in rs if k.startswith(('obj.', 's_')))[:8])
    check(tag + ': and picks no region', not any(REGION.match(l) for l in lines), lines)
    _, lines, rs = run('desel' + str(on), pre + pt(floor) + pt(sky))
    check(tag + ': a click on the sky deselects it', 's_nav' in rs and not any(k.startswith('obj.') for k in rs),
          sorted(k for k in rs if k.startswith(('obj.', 's_')))[:8])

if keep: shutil.copyfile(picked, keep)
print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
