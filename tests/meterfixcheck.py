# Budget meters suggest fixes (#194): a click on an amber or red meter lists
# fixes found in the scene, each with what it saves, and Apply makes it as one
# undo step. A fresh project with four 256x256 textures on four quads, all at
# 16 bpp, puts VRAM over 75% (amber or red). The run clicks the VRAM meter,
# waits for the fixes to be measured (--wait-export holds for them), applies
# the first, saves, copies the scene file aside, undoes with Ctrl+Z and saves
# again. Checks: the list has texture fixes with their savings; the VRAM meter
# moved by exactly the saving the row showed; the saved scene has the fix's
# bit depth on the objects the fix named and the undone one has 16 bpp again,
# and the meter is back where it started; the before/after preview converted.
# usage: python3 meterfixcheck.py <splashedit> [screenshot dir]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
shots = sys.argv[2] if len(sys.argv) > 2 else None
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def write_png(path, w, h, px):
    """An 8-bit RGB PNG; px(x, y) -> (r, g, b) 0..255, y = 0 at the top."""
    raw = b''.join(b'\x00' + bytes(c for x in range(w) for c in px(x, y)) for y in range(h))
    chunk = lambda k, d: struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def project():
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, 'meshes'))
    os.makedirs(os.path.join(root, 'textures'))
    # t0 a smooth two-axis gradient (4 bpp visibly bands it), t1..t3 stripes of 8 colours.
    write_png(os.path.join(root, 'textures', 't0.png'), 256, 256, lambda x, y: (x, y, 255 - (x + y) // 2))
    for k in range(1, 4):
        pal = [((i * 37 + k * 50) % 256, (i * 91 + k * 20) % 256, (i * 53 + k * 90) % 256) for i in range(8)]
        write_png(os.path.join(root, 'textures', 't%d.png' % k), 256, 256, lambda x, y, pal=pal: pal[((x + y * k) // 32) % 8])
    mesh = {'format': 'splashedit-ng/mesh', 'version': 1,
            'positions': [-0.5, 0, -0.5, 0.5, 0, -0.5, 0.5, 0, 0.5, -0.5, 0, 0.5], 'normals': [0, 1, 0] * 4,
            'uv': [0, 0, 1, 0, 1, 1, 0, 1], 'submeshes': [[0, 2, 1, 0, 3, 2]]}
    json.dump(mesh, open(os.path.join(root, 'meshes', 'quad.mesh'), 'w'))
    objs = []
    for k in range(4):
        objs.append({'name': 'Quad%d' % k, 'active': True,
                     'transform': {'position': [k * 1.2 - 1.8, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                     'components': [{'type': 'mesh', 'mesh': 'meshes/quad.mesh', 'bitDepth': 16, 'vertexColors': 'flat',
                                     'materials': [{'texture': 'textures/t%d.png' % k, 'color': [1, 1, 1, 1]}]}],
                     'children': []})
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': ''}, 'objects': objs}
    json.dump(scene, open(os.path.join(root, 'quads.scene'), 'w'), indent=1)
    return root


def depths(path):
    """{object name: mesh bitDepth} of a scene file."""
    sc = json.load(open(path))
    out = {}
    for o in sc['objects']:
        for c in o.get('components', []):
            if c.get('type') == 'mesh': out[o['name']] = c.get('bitDepth', 8)
    return out


root = project()
scene = os.path.join(root, 'quads.scene')
applied = os.path.join(root, 'applied.scene')
shot1 = os.path.join(shots, 'meterfix-open.png') if shots else os.path.join(root, 'open.png')
shot2 = os.path.join(shots, 'meterfix-applied.png') if shots else os.path.join(root, 'applied.png')
final = os.path.join(shots, 'meterfix-undone.png') if shots else os.path.join(root, 'undone.png')
WAIT = ['--wait', '3', '--wait-export', '--wait', '5']
cmd = [editor, '--size', '1600x960', '--project', root, '--scene', 'quads.scene', '--wait', '20', '--wait-export', '--wait', '5',
       '--click', '@m_vram'] + WAIT + ['--shot', shot1, '--click', '@meterfix.vram.0.apply'] + WAIT + [
       '--shot', shot2, '--key', 'ctrl+s', '--wait', '5', '--copy-file', scene + '@' + applied, '--key', 'ctrl+z'] + WAIT + [
       '--key', 'ctrl+s', '--wait', '5', '--screenshot', final]
try:
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=400)
    out = r.stdout + r.stderr
except subprocess.TimeoutExpired as e:
    out = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
    print('editor timed out')
lines = out.splitlines()
for l in lines:
    if l.startswith(('meterfix:', 'statusbar: vram')): print('  ' + l)

vram = [(int(m.group(1)), m.group(2)) for m in (re.match(r'statusbar: vram (\d+)/\d+ (good|warn|bad)$', l) for l in lines) if m]
check('the VRAM meter starts amber or red', len(vram) >= 1 and vram[0][1] in ('warn', 'bad'), vram)
rows = [(int(m.group(1)), m.group(2), int(m.group(3)), m.group(4))
        for m in (re.match(r'meterfix: vram (\d+) (.*) saves (\d+) (apply|select)$', l) for l in lines) if m]
check('the VRAM list has texture fixes, each saving something', len(rows) >= 4 and all(r[2] > 0 and r[3] == 'apply' for r in rows),
      rows)
first = [r for r in rows if r[0] == 0]
m0 = re.match(r'(t\d)\.png: 16 \S+ (\d+) bpp$', first[0][1]) if first else None
check('row 0 drops one texture from 16 bpp', m0 is not None, first)
check('the list is biggest saving first', [r[2] for r in rows[:4]] == sorted([r[2] for r in rows[:4]], reverse=True), rows)
appliedLine = [l for l in lines if l.startswith('meterfix: applied ')]
check('Apply on row 0 applied it', len(appliedLine) == 1 and first and appliedLine[0] == 'meterfix: applied vram 0 saves %d' % first[0][2],
      appliedLine)
check('the meter moved by exactly the saving shown, then back on undo',
      len(vram) == 3 and first and vram[0][0] - vram[1][0] == first[0][2] and vram[2][0] == vram[0][0], (vram, first))
preview = [l for l in lines if l.startswith('meterfix: preview ')]
check('the selected fix shows its texture now and after',
      len(preview) >= 1 and m0 is not None and preview[0].startswith('meterfix: preview textures/%s.png 16 bpp' % m0.group(1)) and
      (' -> %s bpp ' % m0.group(2)) in preview[0], preview)
if os.path.exists(applied) and m0:
    d = depths(applied)
    want = {'Quad%d' % k: 16 for k in range(4)}
    want['Quad' + m0.group(1)[1:]] = int(m0.group(2))
    check('the saved scene has the fix on the object it named, and nothing else changed', d == want, (d, want))
else:
    check('the scene was saved after Apply', False, applied)
d = depths(scene)
check('after Ctrl+Z and save every quad is 16 bpp again', d == {'Quad%d' % k: 16 for k in range(4)}, d)

print('meterfixcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
