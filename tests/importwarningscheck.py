# Import warnings: a model whose import gives several warnings lists all of
# them in the inspector's Import settings, each with its fix, and the status
# bar counts them instead of showing the first. Drops tests/skinimport/rig.gltf
# (blended skin weights and cubic-spline keys: two warnings) on the viewport of
# a copy of the courtyard example, then reads what the editor reports drawing
# and the screenshot.
# usage: python3 importwarningscheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
rig = os.path.join(here, 'skinimport', 'rig.gltf')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
WARN = (0xf0, 0xa4, 0x3a)  # color::warn


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


project = os.path.join(tempfile.mkdtemp(), 'courtyard')
shutil.copytree(example, project)
shot = os.path.join(project, 'shot.png')
cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5', '--drop', rig + '@800,400', '--wait', '20',
       '--screenshot', shot]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
out = (r.stdout + r.stderr).strip()
if keep and os.path.exists(shot): shutil.copyfile(shot, keep)

status = next((l[len('status: '):] for l in out.splitlines() if l.startswith('status: ')), '')
listed = [l.strip()[len('import warning: '):] for l in out.splitlines() if l.strip().startswith('import warning: ')]
check('the model imports', status.startswith('Imported rig.gltf'), out[-400:])
check('the status bar counts the warnings and points at the inspector',
      status.endswith('. 2 import warnings, see the inspector'), status)
check('the status bar does not show only the first warning', 'Note:' not in status, status)
check('the inspector lists both warnings', len(listed) == 2, listed)
check('one is the blended skin weights', any('the heaviest joint wins' in w for w in listed), listed)
check('one is the cubic-spline keys', any('cubic-spline keys' in w for w in listed), listed)

# The list is drawn: warning-coloured marks (a bullet per warning and the
# heading's icon) in the inspector below the Import settings rows.
w, h, rows = png_rgb(shot)
marks = 0
for y in range(590, min(h, 900)):
    row = rows[y]
    for x in range(1256, min(w, 1590)):
        px = row[x * 3:x * 3 + 3]
        marks += all(abs(px[i] - WARN[i]) <= 24 for i in range(3))
check('warning marks are drawn in the inspector', marks >= 30, f'{marks} warning-coloured pixels')

print(f'{fails} failures')
sys.exit(1 if fails else 0)
