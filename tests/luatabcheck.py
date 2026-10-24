# Tab in the script editor (#262): Tab and Shift+Tab indent and outdent the
# code and the editor keeps the keyboard, so what is typed next lands in the
# script and not in the Scene panel's filter (ImGui's own Tab focus cycling
# would otherwise send the keyboard to the first field in the window). Tab
# with the completion list up takes the completion and the editor keeps the
# keyboard; Tab in the Find bar still moves to Replace. Drives the editor
# headless on copies of the courtyard example.
# usage: python3 luatabcheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
keep_shot = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
MARK = 'zz_mark '
EDIT_SCRIPT = '1410,419'  # Game Logic's "Edit script" button at 1600x960 (as in scripteditorcheck.py)
CODE = '700,500'  # the script editor's text, below a short script
SCENE_PANEL = (6, 274, 75, 880)  # x0, x1, y0, y1: the Scene panel's filter and object rows
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def readpng(path):
    """(width, rows of bytes, bytes per pixel) of an 8-bit RGB or RGBA PNG."""
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR': w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        if kind == b'IDAT': idat += body
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw, rows, prev = zlib.decompress(idat), [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (w * bpp + 1)], bytearray(raw[y * (w * bpp + 1) + 1:(y + 1) * (w * bpp + 1)])
        for i in range(len(line)):
            a, b = line[i - bpp] if i >= bpp else 0, prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, rows, bpp


def read(path):
    return open(path, encoding='utf-8').read() if os.path.exists(path) else None


def run(args):
    """Opens game_logic.lua in the script editor, then the scripted `args`.
    Returns the file's text afterwards, the editor's output, the Scene
    panel's pixels and the screenshot's path."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Game Logic', '--click', EDIT_SCRIPT,
           '--wait', '5'] + args + ['--wait', '10', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    out = (r.stdout + r.stderr).strip()
    x0, x1, y0, y1 = SCENE_PANEL
    panel = None
    if os.path.exists(shot):
        w, rows, bpp = readpng(shot)
        panel = [bytes(row[x0 * bpp:x1 * bpp]) for row in rows[y0:y1]]
    return read(os.path.join(project, 'scripts', 'game_logic.lua')), out, panel, shot


def keys(*names):
    return sum((['--key', k] for k in names), [])


original = read(os.path.join(example, 'scripts', 'game_logic.lua'))

# Positive control: typing and saving with no Tab; its Scene panel is the
# one every run below must show (an empty filter, every object listed).
lua, out, plain, _ = run(['--text', MARK] + keys('ctrl+s'))
check('typing and Ctrl+S save the script', lua == MARK + original, repr((lua or '')[:40]) + ' | ' + out[-200:])

# Tab indents to the next stop (column 8 is one, so a full stop of four);
# zq typed next lands after the indent.
lua, out, panel, shot = run(['--text', MARK] + keys('tab') + ['--text', 'zq'] + keys('ctrl+s'))
if keep_shot and os.path.exists(shot): shutil.copy(shot, keep_shot)
check('Tab indents the code', (lua or '').startswith(MARK + '    '), repr((lua or '')[:40]) + ' | ' + out[-200:])
check('text typed after Tab lands in the script', (lua or '').startswith(MARK + '    zq'), repr((lua or '')[:40]) + ' | ' + out[-200:])
diff = sum(a != b for a, b in zip(plain or [], panel or [])) if plain and panel else -1
check('after Tab the Scene filter stays empty (Scene panel unchanged)', diff == 0, f'{diff} Scene panel rows differ')

# Shift+Tab: two Tabs on the empty last line, one Shift+Tab takes one back.
lua, out, panel, _ = run(keys('ctrl+end', 'tab', 'tab', 'shift+tab') + ['--text', 'zq'] + keys('ctrl+s'))
check('Shift+Tab outdents and the editor keeps the keyboard', lua == original + '    zq',
      repr((lua or '')[len(original):]) + ' | ' + out[-200:])
diff = sum(a != b for a, b in zip(plain or [], panel or [])) if plain and panel else -1
check('after Shift+Tab the Scene filter stays empty (Scene panel unchanged)', diff == 0, f'{diff} Scene panel rows differ')

# Completion: Tab on a local's prefix takes the completion; Q typed next
# lands right after it.
args = keys('ctrl+end') + ['--text', 'function zzf()'] + keys('escape', 'end', 'enter') + ['--text', 'local zzinner = 1'] + \
    keys('escape', 'end', 'enter') + ['--text', 'zzinn'] + keys('tab') + ['--text', 'Q'] + keys('ctrl+s')
lua, out, panel, _ = run(args)
want = original + 'function zzf()\n    local zzinner = 1\n    zzinnerQ'
check('Tab takes the completion and the editor keeps the keyboard', (lua or '').startswith(want),
      repr((lua or '')[len(original):]) + ' | ' + out[-200:])
diff = sum(a != b for a, b in zip(plain or [], panel or [])) if plain and panel else -1
check('after a completion Tab the Scene filter stays empty (Scene panel unchanged)', diff == 0, f'{diff} Scene panel rows differ')

# The Find bar keeps its own Tab: Find -> Replace.
lua, out, _, _ = run(['--text', 'zzq = zzq '] + keys('ctrl+h') + ['--text', 'zzq'] + keys('tab') + ['--text', 'yy'] +
                     keys('ctrl+alt+enter') + ['--click', CODE] + keys('ctrl+s'))
check('Tab in Find still moves to Replace', (lua or '').startswith('yy = yy ') and 'zzq' not in (lua or ''),
      repr((lua or '')[:40]) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
