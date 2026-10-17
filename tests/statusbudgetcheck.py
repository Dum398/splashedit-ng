# The status bar's Triangles meter measures the most triangles that can be in
# view at once against the psxsplash docs' 2000 visible triangles per frame,
# and turns amber then red like the memory meters; when an edit breaks the
# live export the meters stay up from the last good export, marked as such.
# A fresh project with one grid mesh: 1700 triangles reads
# `statusbar: tris 1700/2000 warn`, 2100 reads `... bad`, and VRAM has its line
# too (the control). Then Enable saves is clicked on a scene whose memory card
# product code is empty, which fails the export: the bar says
# `statusbar: export failed, meters from last good` and the tris line stays.
# usage: python3 statusbudgetcheck.py <splashedit> [screenshot dir]   (needs a display: xvfb-run -a)
import json, os, re, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = sys.argv[2] if len(sys.argv) > 2 else None
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# The scene inspector shows with nothing selected; once the export has landed
# the wheel scrolls it to Memory card, its last section (memcardeditcheck.py).
ENABLE_SAVES = ['--wheel', '1420,500,-200', '--wait', '3', '--click', '@scene.mcenable', '--wait', '3']


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project(cols, rows):
    """A temp project: grid.scene with one flat grid of cols x rows quads
    (2 * cols * rows triangles), 10 cm each, and memory card settings that
    are off but have an empty product code, so enabling them fails export."""
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, 'meshes'))
    pos, idx = [], []
    for z in range(rows + 1):
        for x in range(cols + 1):
            pos += [x * 0.1 - cols * 0.05, 0, z * 0.1 - rows * 0.05]
    for z in range(rows):
        for x in range(cols):
            a = z * (cols + 1) + x
            b, c, d = a + 1, a + cols + 2, a + cols + 1
            idx += [a, d, c, a, c, b]
    n = (cols + 1) * (rows + 1)
    mesh = {'format': 'splashedit-ng/mesh', 'version': 1, 'positions': pos, 'normals': [0, 1, 0] * n,
            'submeshes': [idx]}
    json.dump(mesh, open(os.path.join(root, 'meshes', 'grid.mesh'), 'w'))
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': '',
                          'memCard': {'enabled': False, 'product': ''}},
             'objects': [{'name': 'Grid', 'active': True,
                          'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                          'components': [{'type': 'mesh', 'mesh': 'meshes/grid.mesh', 'vertexColors': 'flat',
                                          'materials': [{'color': [0.6, 0.6, 0.6, 1]}]}],
                          'children': []}]}
    json.dump(scene, open(os.path.join(root, 'grid.scene'), 'w'), indent=1)
    return root


def run(root, args, shot):
    """The editor on `root` with `args` after the first export landed;
    returns the statusbar: lines in the order printed."""
    cmd = [editor, '--size', '1600x960', '--project', root, '--wait', '20', '--wait-export', '--wait', '5'] + args + [
        '--screenshot', shot]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        print('editor timed out')
    return [l for l in out.splitlines() if l.startswith('statusbar: ')]


def meter(lines, name):
    """(used, capacity, band) of the meter's lines, in order."""
    got = []
    for l in lines:
        m = re.match(r'statusbar: %s (\d+)/(\d+) (good|warn|bad)$' % name, l)
        if m: got.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return got


def shot(root, name):
    return os.path.join(shots, 'statusbudget-%s.png' % name) if shots else os.path.join(root, name + '.png')


# --- 1700 triangles: amber ---
root = project(34, 25)
lines = run(root, [], shot(root, 'warn'))
check('the export lands', 'statusbar: export ok' in lines, lines)
check('1700 triangles in view is amber', meter(lines, 'tris') == [(1700, 2000, 'warn')], lines)
vram = meter(lines, 'vram')
check('the VRAM meter has its line too', len(vram) == 1 and vram[0][1] == 1024 * 512 * 2 and vram[0][0] > 0, lines)

# --- 2100 triangles: red, then the export fails and the meters stay ---
root = project(35, 30)
lines = run(root, ['--shot', shot(root, 'bad')] + ENABLE_SAVES + ['--wait-export', '--wait', '5'], shot(root, 'failed'))
check('2100 triangles in view is red, and stays through the failed export',
      meter(lines, 'tris') == [(2100, 2000, 'bad')], lines)
exports = [l for l in lines if l.startswith('statusbar: export')]
check('enabling saves with no product code fails the export, meters from the last good one',
      exports == ['statusbar: export ok', 'statusbar: export failed, meters from last good'], exports)
check('the failed export prints no meter lines that differ', len(meter(lines, 'vram')) == 1, lines)

print('statusbudgetcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
