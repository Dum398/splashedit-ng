# Play with 2 linked consoles, on a real pcsx-redux: a copy of the courtyard
# whose object script calls Net.Connect() and, once linked, Net.Send()s an
# event every second, and whose scene script logs each one it gets
# (onNetEvent). Play (F5) with players=2 starts console 1 (redux's SIO1 server),
# then console 2 (its client) once console 1's link port is up. Both views
# must show frames, and their status lines must reach "Linked" with exactly
# one host, slots 0 and 1, and the hint that the scene has no Network id. A
# click on console 2 gives it the keyboard, and Capture frame then records
# console 2, into its own directory; Stop (F5) ends the session.
#
# Needs SPLASHEDIT_REDUX (a pcsx-redux dev build with -shmdisplay and SIO1)
# and SPLASHEDIT_PSXSPLASH (a development psxsplash with Net.*), and
# SPLASHEDIT_BIOS if that redux has none; without them it says so and passes.
# Not in CI: CI's checks never get a real pcsx-redux.
# usage: python3 linkplaycheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import glob, json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0

redux, engine = os.environ.get('SPLASHEDIT_REDUX', ''), os.environ.get('SPLASHEDIT_PSXSPLASH', '')
if not (os.path.isfile(redux) and os.path.isfile(engine)):
    print('skip linkplaycheck: set SPLASHEDIT_REDUX and SPLASHEDIT_PSXSPLASH to a real pcsx-redux and psxsplash')
    sys.exit(0)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


work = tempfile.mkdtemp()
project = os.path.join(work, 'courtyard')
shutil.copytree(example, project)
with open(os.path.join(project, 'scripts', 'game_logic.lua'), 'w', newline='\n') as fh:
    fh.write('local frames, seconds = 0, 0\n'
             'function onCreate(self)\n'
             '    Net.Connect()\n'
             'end\n'
             'function onUpdate(self, dt)\n'
             '    frames = frames + 1\n'
             '    if frames < 60 then return end\n'
             '    frames = 0\n'
             '    seconds = seconds + 1\n'
             '    if Net.State() == 2 then\n'
             '        Net.Send(7, Net.LocalSlot() * 1000 + seconds)\n'
             '    end\n'
             'end\n')
with open(os.path.join(project, 'scripts', 'net_scene.lua'), 'w', newline='\n') as fh:
    fh.write('function onNetEvent(id, arg)\n'
             '    Debug.Log("linkcheck: got " .. id .. " arg " .. arg)\n'
             'end\n')
scene = os.path.join(project, 'courtyard.scene')
with open(scene) as fh:
    doc = json.load(fh)
doc['settings']['script'] = 'scripts/net_scene.lua'
doc['settings']['networkId'] = ''
with open(scene, 'w') as fh:
    json.dump(doc, fh, indent=2)
cfg = os.path.join(work, 'play.cfg')
with open(cfg, 'w') as fh:
    fh.write('players=2\n')
# Its own temp directory, so the Play folder (and the consoles' pcsx.json) are this run's.
tmp = os.path.join(work, 'tmp')
os.makedirs(tmp)
ENV = dict(os.environ, TMPDIR=tmp, TMP=tmp, TEMP=tmp)

final = (shots + '-focus.png') if shots else os.path.join(work, 'end.png')
args = [editor, '--size', '1600x960', '--project', project, '--scene', scene, '--play-settings', cfg,
        '--wait', '30', '--key', 'f5', '--wait-play', '--wait', '900']
if shots:
    args += ['--shot', shots + '-linked.png']
args += ['--click', '@game.console2', '--wait', '30', '--shot', final + '.tmp.png', '--click', '@game.capture',
         '--wait', '300', '--key', 'f5', '--wait', '60',
         '--screenshot', os.path.join(work, 'stopped.png')]
r = subprocess.run(args, capture_output=True, text=True, timeout=600, env=ENV)
out = r.stdout
if os.path.exists(final + '.tmp.png'):
    os.replace(final + '.tmp.png', final)
check('editor ran', r.returncode == 0, r.stderr[-800:])
check('Play boots 2 consoles', 'play: console 1 started, link port' in out and 'play: console 2 started, link port' in out,
      [l for l in out.splitlines() if l.startswith('play:')])

lines = {1: [], 2: []}
for line in out.splitlines():
    m = re.match(r'game: console ([12]) (.*)$', line)
    if m:
        lines[int(m.group(1))].append(m.group(2))
for c in (1, 2):
    check('console %d shows frames' % c, any(l.startswith('frames') for l in lines[c]), lines[c][-3:])
linked = {}
for c in (1, 2):
    for l in lines[c]:
        m = re.search(r'Linked · slot (\d+) · (host|not host) · (\d+) players', l)
        if m:
            linked[c] = (int(m.group(1)), m.group(2) == 'host', int(m.group(3)))
    check('console %d links' % c, c in linked, lines[c][-3:])
if len(linked) == 2:
    check('exactly one host', linked[1][1] != linked[2][1], linked)
    check('slots 0 and 1', sorted([linked[1][0], linked[2][0]]) == [0, 1], linked)
    check('2 players each', linked[1][2] == 2 and linked[2][2] == 2, linked)
    host = 1 if linked[1][1] else 2
    check('host has slot 0', linked[host][0] == 0, linked)
check('no Network id hint', all('no Network id: set one in Scene > Network' in l for c in (1, 2) for l in lines[c] if 'Linked' in l), lines[1][-1:])
check('a click gives console 2 the keyboard', any(' focused |' in l for l in lines[2]) and
      not any(' focused |' in l for l in lines[1][-1:]), (lines[1][-1:], lines[2][-1:]))

cap = [l for l in out.splitlines() if l.startswith('frame: ')]
check('capture records the focused console', any(l.startswith('frame: captured from console 2 ') and
                                                 l.rstrip('/').endswith('console2') for l in cap), cap)

# Each console's own pcsx.json: server and client on one port.
play = os.path.join(tmp, 'splashedit-play')
try:
    with open(os.path.join(play, 'console1', 'pcsx.json')) as fh:
        a = json.load(fh)['emulator']['Debug']
    with open(os.path.join(play, 'console2', 'pcsx.json')) as fh:
        b = json.load(fh)['emulator']['Debug']
    check('server and client on one port', a['SIO1Server'] and not a['SIO1Client'] and b['SIO1Client'] and
          not b['SIO1Server'] and b['SIO1Clienthost'] == '127.0.0.1' and a['SIO1ServerPort'] == b['SIO1ClientPort'],
          (a, b))
except (OSError, KeyError, ValueError) as e:
    check('server and client on one port', False, e)
status = [l for l in out.splitlines() if l.startswith('playstatus: ')]
check('stopping Play stops both', status and not status[-1].startswith('playstatus: pcsx-redux running'), status[-2:])

if fails:
    print('\n'.join(out.splitlines()[-40:]))
print('linkplaycheck: %d failure(s)' % fails)
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if fails else 0)
