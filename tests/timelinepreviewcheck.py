# Timeline preview: while the Timeline panel is open on a cutscene, the Scene
# view draws the tracked objects (and the camera) posed at the scrub head,
# without editing the document. Scrubbing to where the crate has risen must
# change the Scene view's pixels; the saved scene must stay byte for byte what
# the same save gives without the timeline; and a gizmo drag on a posed
# object must not write the previewed pose into the scene (while the same drag
# at frame 0, where the pose is the scene's own, does move it).
# usage: python3 timelinepreviewcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')

# Layout at 1600x960 with the timeline open (as timelinecheck.py): frame f
# on the ruler at x 532 + f * 4.4267, the Scene view above the panel.
def fx(frame):
    return round(532 + frame * (1224 - 532 - 28) / 150)

RULER_Y = 668
SCENE_VIEW = (290, 50, 1230, 595)
# The Crate's move gizmo at frame 0 (the scene's own pose): a point on its X arrow. The Scene
# view starts under the tab strip (Scene, UI), which shifts its centre down by 19 px.
GIZMO_X = (845, 387)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def key(frame, value, interp=None):
    k = {'frame': frame, 'value': value}
    if interp:
        k['interp'] = interp
    return k


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    # The Crate sits at (0, 0.5, 1) turned -15 degrees; frame 0 is that pose,
    # frame 75 lifts it to y 3 and turns it to 30.
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 150, 'audioEvents': [], 'tracks': [
        {'type': 'objectPosition', 'target': 'Crate', 'keyframes': [key(0, [0, 0.5, 1]), key(75, [0, 3, 1], 'easeInOut'),
                                                                    key(150, [0, 0.5, 1])]},
        {'type': 'objectRotation', 'target': 'Crate', 'keyframes': [key(0, [0, -15, 0]), key(75, [0, 30, 0])]},
        {'type': 'cameraPosition', 'target': '', 'keyframes': [key(0, [0, 3, -10]), key(150, [6, 2, -2])]},
        {'type': 'cameraRotation', 'target': '', 'keyframes': [key(0, [10, 0, 0]), key(150, [10, -60, 0])]},
    ]}]
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(args, save=True):
    """Runs the editor on a fresh copy with `args` (then Ctrl+S when `save`);
    returns the scene file's bytes, the screenshot path and the output."""
    project = project_copy()
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project] + args
    if save:
        cmd += ['--key', 'ctrl+s', '--wait', '10']
    cmd += ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    data = open(os.path.join(project, 'courtyard.scene'), 'rb').read()
    return data, shot, (r.stdout + r.stderr).strip()


def crate(data):
    s = json.loads(data)
    return next(c for o in s['objects'] if o['name'] == 'Crates' for c in o['children'] if c['name'] == 'Crate')


def read_png(path, rows=None):
    """RGB rows of an 8-bit RGB/RGBA PNG (no interlace), the first `rows` only."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and not interlace, (depth, ctype, interlace)
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out, prev = [], bytearray(stride)
    for y in range(min(h, rows or h)):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line) if bpp == 3 else bytes(b for i, b in enumerate(line) if i % 4 != 3))
        prev = line
    return w, out


def differing(a, b, box):
    """Pixels inside `box` (x0, y0, x1, y1) whose RGB differs by more than 24 in sum."""
    x0, y0, x1, y1 = box
    _, ra = read_png(a, y1)
    _, rb = read_png(b, y1)
    n = 0
    for y in range(y0, y1):
        la, lb = ra[y], rb[y]
        for x in range(x0, x1):
            i = 3 * x
            if abs(la[i] - lb[i]) + abs(la[i + 1] - lb[i + 1]) + abs(la[i + 2] - lb[i + 2]) > 24:
                n += 1
    return n


def scrub(frame):
    return ['--timeline', 'intro', '--click', f'{fx(frame)},{RULER_Y}', '--wait', '3']


# What a save writes with no timeline at all: the reference bytes.
base, _, out = run([])
check('the reference save wrote the scene', b'"Crate"' in base, out[-200:])

# Scrubbing poses the Scene view: frame 75 differs from frame 0 where the crate rose.
untouched = open(os.path.join(project_copy(), 'courtyard.scene'), 'rb').read()
d0, shot0, out0 = run(scrub(0), save=False)
d75, shot75, out75 = run(scrub(75), save=False)
check('scrubbing leaves the scene file alone', d0 == untouched and d75 == untouched)
n = differing(shot0, shot75, SCENE_VIEW)
check('the Scene view draws the pose at the scrub head', n > 1500, f'{n} pixels differ | ' + out75[-200:])
# Same scrub twice: the view is stable (the difference above is the pose).
_, shot75b, _ = run(scrub(75), save=False)
n2 = differing(shot75, shot75b, SCENE_VIEW)
check('the same scrub draws the same view', n2 < 200, f'{n2} pixels differ')

# No frame late: the click presses on frame 6 (actions start at 5), and that
# frame's Scene view already draws the pose at the new scrub head.
# The nav overlay appears when the background export lands, which is not
# this frame, so both shots of this comparison hide the overlays.
_, shot6, out6 = run(['--no-overlays', '--timeline', 'intro', '--click', f'{fx(75)},{RULER_Y}', '--shot-at', '6'], save=False)
_, shot75n, _ = run(['--no-overlays'] + scrub(75), save=False)
n3 = differing(shot6, shot75n, SCENE_VIEW)
check('the Scene view draws the scrub on the frame it happens', n3 < 200, f'{n3} pixels differ | ' + out6[-200:])

# Scrub, play a little, save: byte for byte the reference.
saved, _, out = run(scrub(75) + ['--click', f'{fx(30)},{RULER_Y}', '--wait', '3'])
check('a save while previewing writes the scene unchanged', saved == base, out[-200:])

# The gizmo: at frame 0 the pose is the scene's own, so the move gizmo is
# there and dragging its X arrow moves the crate (the drag reaches it).
drag = ['--select', 'Crate', '--key', 'w', '--wait', '2']
grab = ['--drag', f'{GIZMO_X[0]},{GIZMO_X[1]},{GIZMO_X[0] + 60},{GIZMO_X[1]}', '--wait', '3']
saved, _, out = run(drag + scrub(0) + grab)
moved = crate(saved)['transform']['position']
check('at the scene\'s own pose the gizmo drags the crate', abs(moved[0]) > 0.05 and abs(moved[1] - 0.5) < 1e-4,
      str(moved) + ' | ' + out[-200:])

# At frame 75 the crate is posed: the same drag, and one where the posed
# crate is drawn, write nothing.
saved, _, out = run(drag + scrub(75) + grab)
check('a drag where the gizmo was writes nothing while posed', saved == base,
      str(crate(saved)['transform']) + ' | ' + out[-200:])
saved, _, out = run(drag + scrub(75) + ['--drag', '808,262,868,262', '--wait', '3', '--drag', '808,262,808,200', '--wait', '3'])
check('a drag on the posed crate writes nothing', saved == base, str(crate(saved)['transform']) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
