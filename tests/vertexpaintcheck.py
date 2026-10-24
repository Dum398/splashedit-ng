# Vertex colour painting (#199), driven through the GUI on a copy of the
# courtyard. Inspector: Lighting > Painted starts the Floor's painted layer
# from its baked colours (paint: lines, the Clear paint row). Scene view,
# Paint tool (tool.paint or B, settings paint.*): a left drag over the
# selected Floor paints white with no falloff; the saved layer changes
# exactly within the brush radius of the stroke (from the vpaint: stroke
# line) and nowhere else, and one Ctrl+Z puts the Floor back to Baked with
# no layer. Ctrl+click samples the colour under the cursor without changing
# the selection; Fill with Floor and Back Wall selected sets every vertex of
# both to it, one undo step. Clear paint drops the layer.
# usage: python3 vertexpaintcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives viewport.png (a painted stroke under the
# brush circle) and inspector.png (Lighting: Painted, Clear paint).
import json, math, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


W = lambda n: ['--wait', str(n)]
mid = None  # with SAVE_MID in the script: the scene as saved at that point


def find(objs, name):
    for o in objs:
        if o['name'] == name: return o
        c = find(o.get('children', []), name)
        if c: return c
    return None


def mesh_of(scene, name):
    o = find(scene['objects'], name) if scene else None
    return next((c for c in o.get('components', []) if c.get('type') == 'mesh'), None) if o else None


def layer(scene, name):
    """The object's painted layer as a list of 'rrggbb', or None without one."""
    m = mesh_of(scene, name)
    s = m.get('paintColors') if m else None
    return [s[i:i + 6].lower() for i in range(0, len(s), 6)] if s is not None else None


def run(name, args, select='Floor'):
    """A fresh copy with `select` selected, then `args` and Ctrl+S; returns
    the saved scene, the output lines, the named controls' rects and the
    screenshot."""
    global mid
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select] + W(20) + args + \
        ['--key', 'ctrl+s'] + W(10) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    mid = json.load(open(snap, encoding='utf-8')) if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return json.load(open(scene, encoding='utf-8')), lines, rects, shot


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


def last(lines, prefix):
    v = None
    for l in lines:
        if l.startswith(prefix): v = l
    return v


SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)
UNDO = ['--key', 'ctrl+z'] + W(4)
ORIG = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
ORIG_FLOOR = mesh_of(ORIG, 'Floor')

# The Floor's vertices in world space: its mesh plus its position (no rotation, scale 1, parent at the origin).
fm = json.load(open(os.path.join(example, 'meshes', 'floor.mesh'), encoding='utf-8'))
fo = find(ORIG['objects'], 'Floor')
pos = fo['transform']['position']
check('the Floor sits unrotated and unscaled (the world positions below rely on it)',
      fo['transform']['rotation'] == [0, 0, 0, 1] and fo['transform']['scale'] == [1, 1, 1], fo['transform'])
P = fm['positions']
FLOOR = [(P[i] + pos[0], P[i + 1] + pos[1], P[i + 2] + pos[2]) for i in range(0, len(P), 3)]


def seg_dist(p, a, b):
    ab = [b[k] - a[k] for k in range(3)]
    ap = [p[k] - a[k] for k in range(3)]
    den = sum(v * v for v in ab)
    t = 0 if den == 0 else max(0.0, min(1.0, sum(ap[k] * ab[k] for k in range(3)) / den))
    return math.dist(p, [a[k] + ab[k] * t for k in range(3)])


# 1. Lighting > Painted in the inspector: the layer starts from the baked colours.
s, lines, rects, shot = run('menu', ['--click', '@obj.lighting'] + W(4) + ['--click', '@lighting/Painted'] + W(6))
L0 = layer(s, 'Floor')
check('Painted from the menu sets the mode', (mesh_of(s, 'Floor') or {}).get('vertexColors') == 'painted', mesh_of(s, 'Floor'))
check('and starts a layer of one colour per Floor vertex', L0 is not None and len(L0) == len(FLOOR), L0 and len(L0))
check('taken from the baked lighting (not one flat colour)', L0 is not None and len(set(L0)) > 10, L0 and len(set(L0)))
check('the inspector reports it', last(lines, 'paint: Floor') == 'paint: Floor Painted layer %d' % len(FLOOR), last(lines, 'paint: Floor'))
check('a painted mesh shows the Clear paint row', 'obj.clearpaint' in rects, sorted(k for k in rects if k.startswith('obj.'))[:10])
check('the other meshes have no layer', all(layer(s, n) is None for n in ('Back Wall', 'Side Wall', 'Platform', 'Crate')))
if keep and os.path.exists(shot): shutil.copy(shot, os.path.join(keep, 'inspector.png'))

# 2. A stroke with the Paint tool: white, no falloff, radius 1.5 m; then one Ctrl+Z.
STROKE = ['--click', '@tool.paint'] + W(4) + typed('paint.radius', '1.5') + typed('paint.falloff', '0') + \
    ['--drag', '@vp.view,-300,200,200,170'] + W(6)
s, lines, rects, shot = run('stroke', STROKE + SAVE_MID + UNDO)
print('stroke:', last(lines, 'vpaint: stroke'))
check('the brush settings took the typed values', last(lines, 'vpaint: brush') == 'vpaint: brush #FFFFFF radius 1.500 strength 1.000 falloff 0.000',
      last(lines, 'vpaint: brush'))
m = re.match(r'vpaint: stroke steps (\d+) objects (\d+) from (\S+) to (\S+) radius (\S+)$', last(lines, 'vpaint: stroke') or '')
check('the drag made one stroke on the Floor that changed colours', m is not None and int(m.group(1)) > 0 and m.group(2) == '1',
      last(lines, 'vpaint: stroke'))
L1 = layer(mid, 'Floor')
check('the stroke switched the Floor to Painted', (mesh_of(mid, 'Floor') or {}).get('vertexColors') == 'painted', mesh_of(mid, 'Floor'))
check('with a full layer', L1 is not None and len(L1) == len(FLOOR), L1 and len(L1))
if m and L1 and L0 and len(L1) == len(FLOOR) == len(L0):
    a, b, rad = [float(v) for v in m.group(3).split(',')], [float(v) for v in m.group(4).split(',')], float(m.group(5))
    inside = [i for i, p in enumerate(FLOOR) if seg_dist(p, a, b) < rad - 0.05]
    outside = [i for i, p in enumerate(FLOOR) if seg_dist(p, a, b) > rad + 0.05]
    changed = [i for i in range(len(FLOOR)) if L1[i] != L0[i]]
    print('inside %d, outside %d, changed %d' % (len(inside), len(outside), len(changed)))
    check('vertices within the radius of the stroke are white', inside and all(L1[i] == 'ffffff' for i in inside),
          [(i, L1[i]) for i in inside if L1[i] != 'ffffff'][:5])
    check('vertices beyond it keep their baked colour', len(outside) > len(FLOOR) // 2 and all(L1[i] == L0[i] for i in outside),
          [(i, L0[i], L1[i]) for i in outside if L1[i] != L0[i]][:5])
    check('only vertices near the stroke changed', changed and all(seg_dist(FLOOR[i], a, b) <= rad + 0.05 for i in changed),
          [(i, round(seg_dist(FLOOR[i], a, b), 3)) for i in changed if seg_dist(FLOOR[i], a, b) > rad + 0.05][:5])
else:
    check('stroke layer comparable with the menu one', False, (m and m.groups(), L1 and len(L1), L0 and len(L0)))
check('only the Floor was painted', all(layer(mid, n) is None for n in ('Back Wall', 'Side Wall', 'Platform', 'Crate')))
check('one Ctrl+Z puts the Floor back to Baked with no layer', mesh_of(s, 'Floor') is not None and layer(s, 'Floor') is None and
      mesh_of(s, 'Floor').get('vertexColors') == ORIG_FLOOR['vertexColors'], mesh_of(s, 'Floor') and
      {k: v for k, v in mesh_of(s, 'Floor').items() if k != 'paintColors'})
check('and nothing else in the scene keeps a painted layer', 'paintColors' not in json.dumps(s))

# The same stroke in red, kept hovering, for the picture.
if keep:
    _, _, _, shot = run('picture', ['--key', 'b'] + W(4) + ['--click', '@paint.color'] + W(4) + ['--click', '@paint.colorpick,85,-95'] +
                        W(4) + ['--key', 'escape'] + W(3) + typed('paint.radius', '1.5') + ['--drag', '@vp.view,-300,200,200,170'] + W(6) +
                        ['--move', '@vp.view,0,60'] + W(6))
    if os.path.exists(shot): shutil.copy(shot, os.path.join(keep, 'viewport.png'))

# 3. Ctrl+click samples; Fill paints every vertex of the two selected meshes, one undo step.
SAMPLE = ['--keydown', 'ctrl'] + W(2) + ['--click', '@tree.Back Wall'] + W(3) + ['--keyup', 'ctrl'] + W(3) + \
    ['--key', 'b'] + W(4) + ['--keydown', 'ctrl'] + W(2) + ['--click', '@vp.view,-50,200'] + W(4) + ['--keyup', 'ctrl'] + W(3)
s, lines, rects, shot = run('fill', SAMPLE + ['--click', '@paint.fill'] + W(5) + SAVE_MID + UNDO)
sm = re.match(r'vpaint: sampled #([0-9A-F]{6}) on (.+) at (\S+)$', last(lines, 'vpaint: sampled') or '')
print('sample:', last(lines, 'vpaint: sampled'), '| fill:', last(lines, 'vpaint: fill'))
check('Ctrl+click on the Floor samples a colour there', sm is not None and sm.group(2) == 'Floor', last(lines, 'vpaint: sampled'))
col = sm.group(1).lower() if sm else None
check('the sampled colour becomes the brush colour', sm is not None and (last(lines, 'vpaint: brush') or '').startswith('vpaint: brush #' + sm.group(1)),
      last(lines, 'vpaint: brush'))
if sm and L0:
    # Interpolated across the triangle under the cursor: inside the range of the baked colours around it.
    at = [float(v) for v in sm.group(3).split(',')]
    near = [L0[i] for i, p in enumerate(FLOOR) if math.dist(p, at) < 3.7]
    ok = bool(near) and all(min(int(c[k:k + 2], 16) for c in near) <= int(col[k:k + 2], 16) <= max(int(c[k:k + 2], 16) for c in near)
                            for k in (0, 2, 4))
    check('the sample lies among the baked colours around the point', ok, (col, sorted(set(near))[:8]))
check('Ctrl+click in the Scene view kept both selected and painted nothing',
      (last(lines, 'vpaint: fill') or '').startswith('vpaint: fill 2 '), last(lines, 'vpaint: fill'))
for n in ('Floor', 'Back Wall'):
    lay = layer(mid, n)
    check('Fill set every vertex of the %s to the sample' % n, lay is not None and len(lay) > 10 and set(lay) == {col},
          lay and sorted(set(lay))[:4])
    check('and made it Painted', (mesh_of(mid, n) or {}).get('vertexColors') == 'painted', mesh_of(mid, n) and mesh_of(mid, n).get('vertexColors'))
check('the unselected meshes were not filled', all(layer(mid, n) is None for n in ('Side Wall', 'Platform', 'Crate')))
check('one Ctrl+Z takes the Fill off both', 'paintColors' not in json.dumps(s) and
      all((mesh_of(s, n) or {}).get('vertexColors') == 'baked' for n in ('Floor', 'Back Wall')))

# 4. Clear paint in the inspector drops the layer and goes back to Baked.
s, lines, rects, shot = run('clear', ['--key', 'b'] + W(4) + ['--click', '@paint.fill'] + W(5) + SAVE_MID + ['--click', '@obj.clearpaint'] + W(5))
check('Fill made a layer', layer(mid, 'Floor') is not None and len(layer(mid, 'Floor')) == len(FLOOR))
check('Clear paint removes it and the Floor is Baked again', layer(s, 'Floor') is None and
      (mesh_of(s, 'Floor') or {}).get('vertexColors') == 'baked', mesh_of(s, 'Floor') and mesh_of(s, 'Floor').get('vertexColors'))
check('the Clear paint row is gone with the layer', last(lines, 'paint: Floor') == 'paint: Floor Baked vertex layer 0', last(lines, 'paint: Floor'))

print('FAILS=%d' % fails)
sys.exit(1 if fails else 0)
