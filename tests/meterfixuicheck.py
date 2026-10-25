# Budget meters suggest fixes (#194) for UI images and sprite sheets too: a
# HUD image and a sprite sheet show one 256x256 texture at 16 bpp, which the
# packer keeps as one copy, and six quads at 8 bpp fill the rest of VRAM over
# 75% (amber). The run clicks the VRAM meter, waits for the fixes to be
# measured, applies row 0, saves, copies the scene file aside, undoes with
# Ctrl+Z and saves again. Checks: row 0 drops that texture 16 -> 4 bpp and
# saves the most; the VRAM meter moved by exactly the saving the row showed;
# the saved scene has 4 bpp on both the image and the sheet (one fix for the
# one copy) and nothing else changed, the sheet-backed image included; after
# Ctrl+Z both are 16 bpp again and the meter is back where it started.
# usage: python3 meterfixuicheck.py <splashedit> [screenshot dir]   (needs a display: xvfb-run -a)
import json, os, re, struct, subprocess, sys, tempfile, zlib

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
    # hud.png a smooth gradient, shown by the UI image and the sprite sheet;
    # q0..q5 stripes of 8 colours for the quads.
    write_png(os.path.join(root, 'textures', 'hud.png'), 256, 256, lambda x, y: (x, y, 255 - (x + y) // 2))
    for k in range(6):
        pal = [((i * 37 + k * 50) % 256, (i * 91 + k * 20) % 256, (i * 53 + k * 90) % 256) for i in range(8)]
        write_png(os.path.join(root, 'textures', 'q%d.png' % k), 256, 256, lambda x, y, pal=pal: pal[((x + y * (k + 1)) // 32) % 8])
    mesh = {'format': 'splashedit-ng/mesh', 'version': 1,
            'positions': [-0.5, 0, -0.5, 0.5, 0, -0.5, 0.5, 0, 0.5, -0.5, 0, 0.5], 'normals': [0, 1, 0] * 4,
            'uv': [0, 0, 1, 0, 1, 1, 0, 1], 'submeshes': [[0, 2, 1, 0, 3, 2]]}
    json.dump(mesh, open(os.path.join(root, 'meshes', 'quad.mesh'), 'w'))
    objs = []
    for k in range(6):
        objs.append({'name': 'Quad%d' % k, 'active': True,
                     'transform': {'position': [k * 1.2 - 3, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                     'components': [{'type': 'mesh', 'mesh': 'meshes/quad.mesh', 'bitDepth': 8, 'vertexColors': 'flat',
                                     'materials': [{'texture': 'textures/q%d.png' % k, 'color': [1, 1, 1, 1]}]}],
                     'children': []})
    canvas = {'name': 'HUD', 'visible': True, 'sortOrder': 0, 'elements': [
        {'type': 'image', 'name': 'Logo', 'visible': True, 'rect': [8, 8, 64, 64], 'texture': 'textures/hud.png', 'bitDepth': 16,
         'cutout': True},
        # Sheet-backed: drawn from the sheet's texture at the sheet's depth; its own bitDepth stays as it is.
        {'type': 'image', 'name': 'Coin', 'visible': True, 'rect': [80, 8, 32, 32], 'texture': None, 'bitDepth': 8,
         'cutout': True, 'sheet': 'Coins', 'cell': 0}]}
    sheet = {'name': 'Coins', 'texture': 'textures/hud.png', 'bitDepth': 16, 'cellWidth': 32, 'cellHeight': 32, 'anims': []}
    scene = {'format': 'splashedit-ng/scene', 'version': 1,
             'settings': {'gteScaling': 100.0, 'sceneType': 'exterior', 'networkId': ''}, 'objects': objs,
             'spriteSheets': [sheet], 'canvases': [canvas]}
    json.dump(scene, open(os.path.join(root, 'hud.scene'), 'w'), indent=1)
    return root


def depths(path):
    """{name: bitDepth} of a scene file's meshes, UI images and sprite sheets."""
    sc = json.load(open(path))
    out = {}
    for o in sc['objects']:
        for c in o.get('components', []):
            if c.get('type') == 'mesh': out[o['name']] = c.get('bitDepth', 8)
    for cv in sc.get('canvases', []):
        for e in cv.get('elements', []):
            if e.get('type') == 'image': out['image ' + e['name']] = e.get('bitDepth', 8)
    for s in sc.get('spriteSheets', []):
        out['sheet ' + s['name']] = s.get('bitDepth', 4)
    return out


root = project()
scene = os.path.join(root, 'hud.scene')
applied = os.path.join(root, 'applied.scene')
shot1 = os.path.join(shots, 'meterfixui-open.png') if shots else os.path.join(root, 'open.png')
shot2 = os.path.join(shots, 'meterfixui-applied.png') if shots else os.path.join(root, 'applied.png')
final = os.path.join(shots, 'meterfixui-undone.png') if shots else os.path.join(root, 'undone.png')
start = depths(scene)
WAIT = ['--wait', '3', '--wait-export', '--wait', '5']
cmd = [editor, '--size', '1600x960', '--project', root, '--scene', 'hud.scene', '--wait', '20', '--wait-export', '--wait', '5',
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
first = [r for r in rows if r[0] == 0]
check('row 0 drops the UI image and sprite sheet texture 16 -> 4 bpp',
      len(first) >= 1 and re.match(r'hud\.png: 16 \S+ 4 bpp$', first[0][1]) is not None and first[0][3] == 'apply', first)
# The list as first drawn: its rows up to where index 0 comes round again.
listed = rows[:next((i for i in range(1, len(rows)) if rows[i][0] == 0), len(rows))]
check('the list is biggest saving first', len(listed) >= 2 and [r[2] for r in listed] == sorted([r[2] for r in listed], reverse=True),
      listed)
appliedLine = [l for l in lines if l.startswith('meterfix: applied ')]
check('Apply on row 0 applied it', len(appliedLine) == 1 and first and appliedLine[0] == 'meterfix: applied vram 0 saves %d' % first[0][2],
      appliedLine)
check('the meter moved by exactly the saving shown, then back on undo',
      len(vram) == 3 and first and first[0][2] > 0 and vram[0][0] - vram[1][0] == first[0][2] and vram[2][0] == vram[0][0],
      (vram, first))
want = dict(start)
want['image Logo'] = 4
want['sheet Coins'] = 4
if os.path.exists(applied):
    d = depths(applied)
    check('the saved scene has 4 bpp on the image and the sheet, and nothing else changed', d == want, (d, want))
else:
    check('the scene was saved after Apply', False, applied)
d = depths(scene)
check('after Ctrl+Z and save the image and the sheet are 16 bpp again', d == start and d['image Logo'] == 16 and
      d['sheet Coins'] == 16, (d, start))

print('meterfixuicheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
