# Lua compiled on the host runs on a real NOPARSER psxsplash. The bundled engines
# have no Lua compiler, so they only run the bytecode the editor writes; if the
# psxlua the editor compiles with and the one the engine runs drift apart,
# luacheck.py (fixed expected files) stays green while every game breaks.
# This builds a copy of the courtyard, as Play does (splashpack-cli build, Lua
# stored compiled), whose script works out a token at run time with integer,
# division and modulo, power, FixedPoint, string, closure-with-upvalue, table
# iteration and loop code, and prints it from onCreate. It boots the engine in
# pcsx-redux and checks the token is in the log, exactly, and that the engine
# printed no "parser not loaded" and no Lua error.
# usage: python3 luabootcheck.py <splashpack-cli> <psxsplash.ps-exe> <pcsx-redux> <bios> [frame]
#   (needs a display: xvfb-run -a; the engine must be a NOPARSER build)
import os, shutil, subprocess, sys, tempfile

cli, engine, redux, bios = (os.path.abspath(a) for a in sys.argv[1:5])
frame = sys.argv[5] if len(sys.argv) > 5 else '900'
here = os.path.dirname(os.path.abspath(__file__))
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


SCRIPT = r'''-- tests/luabootcheck.py: every value below is checked on the engine.
local function counter(n)
  return function(step) n = n + step return n end
end
local tick = counter(7)
tick(5)
local up = tick(-3)
local sq = 0
for i = 1, 10 do sq = sq + i * i end
local seven = up - 2
local div, mod = -seven / 2, -seven % 3
local pw = 2 ^ (seven + 3)
local keys = 0
for k, v in pairs({alpha = 1, be = 2, c = 3}) do keys = keys + #k * v end
local seq = 0
for i, v in ipairs({10, 20, 30}) do seq = seq + i * v end
local evens, j = 0, 0
while j < 9 do
  j = j + 1
  if j % 2 == 0 then evens = evens + j end
end
local half = FixedPoint.new(seven) / 2
local words = {}
for w in string.gmatch("psx lua pin", "%a+") do words[#words + 1] = w:upper() end
local token = string.format("LUABOOT %d %d %d %d %d %d %d %d %d %s", up, sq, div, mod, pw,
  keys, seq, evens, FixedPoint.raw(half), table.concat(words, "-")) .. "!" .. #words

function onCreate(self)
  print(token)
end
'''
# psxlua numbers are 32-bit integers with C division and modulo: -7 / 2 is -3, -7 % 3 is -1.
# 7 as FixedPoint is 7 << 12; halved, raw 14336. FixedPoint's metatable has no __index, so
# its methods are called as FixedPoint.raw(x), not x:raw().
TOKEN = 'LUABOOT 9 385 -3 -1 1024 12 140 20 14336 PSX-LUA-PIN!3'

work = tempfile.mkdtemp()
try:
    project = os.path.join(work, 'courtyard')
    shutil.copytree(os.path.join(here, '..', 'examples', 'courtyard'), project)
    with open(os.path.join(project, 'scripts', 'game_logic.lua'), 'w', newline='\n') as fh:
        fh.write(SCRIPT)
    out = os.path.join(work, 'out')
    r = subprocess.run([cli, 'build', os.path.join(project, 'courtyard.scene'), '-o', out],
                       capture_output=True, text=True, timeout=300)
    check('splashpack-cli build', r.returncode == 0 and os.path.isfile(os.path.join(out, 'scene_0.splashpack')),
          r.stdout + r.stderr)
    if fails:
        sys.exit(1)

    env = dict(os.environ, BOOT_OUT=out, BOOT_FRAME=frame, LIBGL_ALWAYS_SOFTWARE='1')
    try:
        r = subprocess.run([redux, '-testmode', '-stdout', '-lua_stdout', '-safe', '-bios', bios,
                            '-loadexe', engine, '-run', '-pcdrv', '-pcdrvbase', out,
                            '-dofile', os.path.join(here, 'boot', 'boot.lua')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           env=env, timeout=600)
        log = r.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b'').decode('utf-8', 'replace')
    lines = [l.strip() for l in log.splitlines()]
    # print() writes each line whole to the BIOS tty, so the token is a line of its own.
    tokens = [l for l in lines if l.startswith('LUABOOT ')]
    errors = [l for l in lines if 'Lua error' in l or 'Lua script' in l or 'parser not loaded' in l]
    for l in tokens:
        print('  engine: ' + l)
    check('pcsx-redux ran to frame %s' % frame, any(l.startswith('SHOT ') for l in lines), log[-3000:])
    check('the script printed ' + TOKEN, TOKEN in tokens, 'printed %s; engine errors %s' % (tokens, errors[:5]))
    for bad in ('parser not loaded', 'Lua error', 'Error loading Lua script', 'Error executing Lua script',
                'Error registering Lua script'):
        hits = [l for l in lines if bad in l]
        check('no "%s" in the engine log' % bad, not hits, hits[:5])
finally:
    shutil.rmtree(work, ignore_errors=True)

print('luabootcheck: %d failure(s)' % fails)
sys.exit(1 if fails else 0)
