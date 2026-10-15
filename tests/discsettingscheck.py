# The scene inspector's Disc section, driven through the GUI on copies of the
# courtyard: type a volume label (typed letters are upper-cased and anything
# but A-Z 0-9 _ dropped as they are typed), pick the built-in psxsplash
# licence from the Licence dropdown, and open projects whose disc.json names
# a licence file of the wrong size or one that is not there, which show a
# warning line under the dropdown. Every change is saved to disc.json at once
# and is not a scene edit. The editor prints the settings ("disc: volume
# MYGAME", "disc: volume default", "disc: licence none|psxsplash|<file
# name>", "disc: licence error <message>") whenever they change. Controls are
# clicked by name (--click @disc.volume, --click @disc.licence/psxsplash).
# usage: python3 discsettingscheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# Folding the sections above it brings the Disc section (open from the
# start) into view at 1600x960.
FOLDS = ['--click', '@s_sequences', '--wait', '3', '--click', '@s_rendering', '--wait', '3', '--click', '@s_loading', '--wait', '3',
         '--click', '@s_vram', '--wait', '5']


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args, files=None):
    """Runs the editor on a fresh courtyard copy (the scene inspector shows),
    with `files` ({name: bytes}) written into it first, folds the sections
    above Disc, then `args`. Returns the printed disc: lines, all output
    lines, disc.json's contents (None when there is none), the named
    controls and the screenshot."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    for name, data in (files or {}).items():
        with open(os.path.join(project, name), 'wb') as fh:
            fh.write(data)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + FOLDS + args + ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    path = os.path.join(project, 'disc.json')
    data = json.load(open(path)) if os.path.exists(path) else None
    controls = {}
    for l in lines:
        if l.startswith('control ') and ' at ' in l:
            name, at = l[len('control '):].split(' at ')
            a, b = at.split(' ')
            controls[name] = tuple(float(v) for v in a.split(',') + b.split(','))
    return [l for l in lines if l.startswith('disc:')], lines, data, controls, shot


def click(name):
    return ['--click', '@' + name, '--wait', '3']


# The PNG reader vramlayoutcheck.py uses (no third-party modules).
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


def warn_pixels(shot, rect):
    """How many pixels in `rect` of the shot are the warning orange's hue."""
    _, h, rows = png_rgb(shot)
    x0, y0, x1, y1 = (int(v) for v in rect)
    n = 0
    for y in range(max(y0, 0), min(y1, h)):
        row = rows[y]
        for x in range(x0, x1):
            r, g, b = row[3 * x:3 * x + 3]
            n += r > 120 and r > g + 25 and g > b + 25
    return n


# Untouched: the defaults, nothing written, no warning.
discs, lines, data, controls, _ = run([])
check('the defaults are printed once', discs == ['disc: volume default', 'disc: licence none'], discs)
check('nothing touched writes no disc.json', data is None, data)
check('the section shows its controls', all(n in controls for n in ('s_disc', 'disc.volume', 'disc.licence')), sorted(controls))
check('no warning line without a licence', 'disc.error' not in controls, controls.get('disc.error'))

# Typing: "my game" lands as MYGAME (upper-cased, the space dropped as typed).
discs, lines, data, controls, _ = run(click('disc.volume') + ['--text', 'my game', '--key', 'enter', '--wait', '3'])
check('the typed volume is upper-cased and filtered in disc.json', (data or {}).get('volume') == 'MYGAME', data)
check('the volume change is printed', discs[:2] == ['disc: volume default', 'disc: licence none'] and
      'disc: volume MYGAME' in discs, discs)
check('disc.json is no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])

# The psxsplash licence from the dropdown.
discs, lines, data, controls, _ = run(click('disc.licence') + click('disc.licence/psxsplash'))
check('psxsplash is saved in disc.json', data == {'volume': '', 'licence': 'psxsplash'}, data)
check('the licence change is printed', discs == ['disc: volume default', 'disc: licence none', 'disc: licence psxsplash'], discs)
check('the built-in licence shows no warning', 'disc.error' not in controls, controls.get('disc.error'))

# A licence file of 1000 bytes, named in disc.json before the editor starts.
bad = {'bad.dat': bytes(1000), 'disc.json': json.dumps({'volume': '', 'licence': 'bad.dat'}).encode()}
discs, lines, data, controls, shot = run([], bad)
errs = [l for l in discs if l.startswith('disc: licence error ')]
check('the file licence is printed by name', 'disc: licence bad.dat' in discs, discs)
check('the bad size is printed as an error naming 1000', len(errs) == 1 and '1000 bytes' in errs[0], discs)
check('the warning line is drawn', 'disc.error' in controls, sorted(controls))
if 'disc.error' in controls:
    n = warn_pixels(shot, controls['disc.error'])
    check('the warning line is warning-coloured', n > 40, n)
check('opening it leaves disc.json as it was', data == {'volume': '', 'licence': 'bad.dat'}, data)

# A licence file that is not there.
gone = {'disc.json': json.dumps({'volume': 'X', 'licence': 'gone.dat'}).encode()}
discs, lines, data, controls, shot = run([], gone)
errs = [l for l in discs if l.startswith('disc: licence error ')]
check('a missing licence file is printed as an error', len(errs) == 1 and 'Cannot read' in errs[0] and 'gone.dat' in errs[0], discs)
check('the warning line is drawn for it', 'disc.error' in controls, sorted(controls))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
