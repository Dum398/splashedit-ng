# View > Set up external editor (VS Code), driven through the GUI: it writes
# the lua-language-server definitions (psxsplash.d.lua, the file
# tools/gen_lua_api.py commits in src/editor) and a .luarc.json into the
# project, and leaves a .luarc.json the project already has alone. Opening a
# project whose psxsplash.d.lua differs from the editor's rewrites it; a
# project without one gets none. The editor prints "luadefs: wrote
# psxsplash.d.lua, .luarc.json <written|kept>" and "luadefs: rewrote stale
# psxsplash.d.lua". The item is clicked by name
# (--click @menu.view, --click "@menu.view/Set up external editor (VS Code)").
# usage: python3 luadefsguicheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
with open(os.path.join(here, '..', 'src', 'editor', 'psxsplash.d.lua'), 'rb') as fh:
    DEFS = fh.read()
ITEM = 'Set up external editor (VS Code)'
SETUP = ['--click', '@menu.view', '--wait', '3', '--click', '@menu.view/' + ITEM, '--wait', '3']
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project(files=None):
    """A fresh courtyard copy with `files` ({name: bytes}) in its root."""
    p = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, p)
    for name, data in (files or {}).items():
        with open(os.path.join(p, name), 'wb') as fh:
            fh.write(data)
    return p


def run(p, args):
    """Runs the editor on project `p`, then `args`; returns the luadefs: lines."""
    cmd = [editor, '--size', '1600x960', '--project', p, '--wait', '5'] + args + \
        ['--wait', '3', '--screenshot', os.path.join(p, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return [l for l in lines if l.startswith('luadefs:')]


def read(p, name):
    path = os.path.join(p, name)
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as fh:
        return fh.read()


# A project with neither file: the menu item writes both.
p = project()
said = run(p, SETUP)
check('the item says it wrote both', said == ['luadefs: wrote psxsplash.d.lua, .luarc.json written'], said)
check('psxsplash.d.lua is the committed definitions', read(p, 'psxsplash.d.lua') == DEFS,
      None if read(p, 'psxsplash.d.lua') is None else len(read(p, 'psxsplash.d.lua')))
luarc = read(p, '.luarc.json') or b''
check('.luarc.json sets Lua 5.2', b'"runtime.version": "Lua 5.2"' in luarc, luarc[:200])
check('.luarc.json turns math, io and os off',
      all(b'"%s": "disable"' % lib in luarc for lib in (b'math', b'io', b'os', b'package')), luarc[:400])

# The project's own .luarc.json is kept; psxsplash.d.lua is still written.
mine = b'{"runtime.version": "Lua 5.2", "diagnostics.globals": ["mine"]}\n'
p = project({'.luarc.json': mine})
said = run(p, SETUP)
check('the item says it kept .luarc.json', said == ['luadefs: wrote psxsplash.d.lua, .luarc.json kept'], said)
check('the project\'s .luarc.json is not overwritten', read(p, '.luarc.json') == mine, read(p, '.luarc.json'))
check('psxsplash.d.lua is written beside it', read(p, 'psxsplash.d.lua') == DEFS)

# Opened with a stale psxsplash.d.lua: rewritten without a click or a prompt.
p = project({'psxsplash.d.lua': b'---@meta psxsplash\n-- an older editor wrote this\n'})
said = run(p, [])
check('opening says it rewrote the stale file', said == ['luadefs: rewrote stale psxsplash.d.lua'], said)
check('the stale psxsplash.d.lua is the current one again', read(p, 'psxsplash.d.lua') == DEFS)
check('opening writes no .luarc.json', read(p, '.luarc.json') is None)

# Opened with the current one, or none: nothing is written.
p = project({'psxsplash.d.lua': DEFS})
said = run(p, [])
check('a current psxsplash.d.lua is left alone', said == [] and read(p, 'psxsplash.d.lua') == DEFS, said)
p = project()
said = run(p, [])
check('a project without one gets none on open', said == [] and read(p, 'psxsplash.d.lua') is None, said)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
