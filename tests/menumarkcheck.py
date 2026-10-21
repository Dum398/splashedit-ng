# Title-bar menu marks, driven through the GUI: an on/off item that is on
# shows a check mark, and the picked option of a group (tool, look, console
# count) shows a dot. The editor names each mark <menu>/<label>/check or
# <menu>/<label>/dot while the menu is open, and the screenshot is read
# inside each mark for the accent colour, so a named mark that drew nothing
# fails too.
# usage: python3 menumarkcheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, re, shutil, subprocess, sys, tempfile, zlib, struct

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
settings = os.path.join(work, 'prefs', 'play.cfg')
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux')
ACCENT = (0x8b, 0x7b, 0xff)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def readpng(path):
    """RGB rows of an 8-bit RGB or RGBA PNG, without third-party modules."""
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
    raw = zlib.decompress(idat)
    rows, prev, stride = [], bytearray(w * bpp), w * bpp
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return rows


def accentPixels(rows, rect):
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(max(0, y0), min(len(rows), y1 + 1)):
        for x in range(max(0, x0), min(len(rows[0]), x1 + 1)):
            p = rows[y][x]
            if all(abs(p[i] - ACCENT[i]) < 40 for i in range(3)):
                n += 1
    return n


def run(args):
    """Runs the editor on a fresh courtyard copy, then `args`, and takes a
    screenshot with the last menu still open. Returns the named marks
    {name: rect} and the screenshot's rows."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--play-settings', settings, '--wait', '5'] + args + \
        ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    marks = {}
    for l in lines:
        m = re.match(r'control (menu\..+/(?:check|dot)) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m:
            marks[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    rows = readpng(shot) if os.path.exists(shot) else []
    return marks, rows


def drawn(marks, rows, name):
    return name in marks and rows and accentPixels(rows, marks[name]) >= 6


TOOLS = ('Hand', 'Move', 'Rotate', 'Scale')

# View: one tool and one look carry a dot; Project and Snap (on by
# default) carry check marks.
marks, rows = run(['--click', '@menu.view', '--wait', '3'])
tools = [k for k in marks if k.split('/')[1] in TOOLS]
check('exactly one tool is marked, with a dot', len(tools) == 1 and tools[0].endswith('/dot') and drawn(marks, rows, tools[0]), marks)
check('PS1 Look, the picked look, shows a dot', drawn(marks, rows, 'menu.view/PS1 Look/dot'), marks)
check('no option of a group shows a check mark',
      not any(k.endswith('/check') and k.split('/')[1] in TOOLS + ('PS1 Look', 'Clean Look') for k in marks), marks)
check('Snap on shows a check mark', drawn(marks, rows, 'menu.view/Snap/check'), marks)
check('Snap on shows no dot', 'menu.view/Snap/dot' not in marks, marks)

# Snap off: no mark at all; picking Rotate moves the dot to it.
marks, rows = run(['--click', '@menu.view', '--wait', '3', '--click', '@menu.view/Snap', '--wait', '3', '--click', '@menu.view',
                   '--wait', '3', '--click', '@menu.view/Rotate', '--wait', '3', '--click', '@menu.view', '--wait', '3'])
check('Snap off shows no mark', not any(k.startswith('menu.view/Snap/') for k in marks), marks)
check('Rotate, once picked, shows the only tool dot',
      drawn(marks, rows, 'menu.view/Rotate/dot') and [k for k in marks if k.split('/')[1] in TOOLS] == ['menu.view/Rotate/dot'], marks)

# Players: the picked console count shows a dot. Build: Boot Logo in Test
# Runs, an on/off item, is a check mark when marked.
marks, rows = run(['--click', '@menu.build', '--wait', '3', '--click', '@menu.build/Players: 1 player...', '--wait', '3'])
check('1 Console, the picked count, shows a dot', drawn(marks, rows, 'menu.players/1 Console/dot'), marks)
marks, rows = run(['--click', '@menu.build', '--wait', '3'])
logo = [k for k in marks if k.startswith('menu.build/Boot Logo in Test Runs/')]
check('Boot Logo in Test Runs is a check mark', logo == ['menu.build/Boot Logo in Test Runs/check'] and drawn(marks, rows, logo[0]), logo)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
