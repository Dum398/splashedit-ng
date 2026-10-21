# Play with 3 consoles through the editor's relay, on a real pcsx-redux: a
# copy of the courtyard whose object script calls Net.Connect() and, once
# connected, Net.Send()s an event every second, and whose scene script logs
# each one it gets (onNetEvent). Play (F5) with players=3 starts the relay,
# then console 1, then each next console once the relay has accepted the one
# before it (every console is redux's SIO1 client to the relay). All three
# views must show frames, their status lines must reach "Relay slot S ·
# Linked" with slots 1, 2 and 3 in console order, console 1 (the room's
# authority) host and the others not, 3 players each;
# console 2 must log events console 1 and 3 sent (the relay fanned them out);
# and the Network view must list Hellos, HelloAcks and Events from slot 1 to
# slots 2 and 3. Stop (F5) ends every console and the relay.
#
# Needs SPLASHEDIT_REDUX (a pcsx-redux dev build with -shmdisplay and SIO1)
# and SPLASHEDIT_PSXSPLASH (a development psxsplash with Net.*), and
# SPLASHEDIT_BIOS if that redux has none; without them it says so and passes.
# Not in CI: CI's checks never get a real pcsx-redux. Runs 3 pcsx-redux at once.
# usage: python3 relayplaycheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
N = 3

redux, engine = os.environ.get('SPLASHEDIT_REDUX', ''), os.environ.get('SPLASHEDIT_PSXSPLASH', '')
if not (os.path.isfile(redux) and os.path.isfile(engine)):
    print('skip relayplaycheck: set SPLASHEDIT_REDUX and SPLASHEDIT_PSXSPLASH to a real pcsx-redux and psxsplash')
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
             '    Debug.Log("relaycheck: got " .. id .. " arg " .. arg)\n'
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
    fh.write('players=%d\n' % N)
tmp = os.path.join(work, 'tmp')
os.makedirs(tmp)
ENV = dict(os.environ, TMPDIR=tmp, TMP=tmp, TEMP=tmp)

net = (shots + '-network.png') if shots else os.path.join(work, 'network.png')
args = [editor, '--size', '1600x960', '--project', project, '--scene', scene, '--play-settings', cfg,
        '--wait', '30', '--key', 'f5', '--wait-play', '--wait', '1500']
if shots:
    args += ['--shot', shots + '-grid.png']
# Console 2's output, filtered to the events it got; then the Network view.
args += ['--click', '@game.console2', '--wait', '20', '--click', '@playstatus', '--wait', '20',
         '--click', '@play.filter', '--text', 'relaycheck: got', '--wait', '30',
         '--click', '@play.tab.network', '--wait', '60', '--shot', net,
         '--click', '@net.type.Snapshot', '--wait', '20']
if shots:
    args += ['--shot', shots + '-network-nosnap.png']
args += ['--key', 'escape', '--wait', '10', '--key', 'f5', '--wait', '60',
         '--screenshot', os.path.join(work, 'stopped.png')]
r = subprocess.run(args, capture_output=True, text=True, timeout=900, env=ENV)
out = r.stdout
check('editor ran', r.returncode == 0, r.stderr[-800:])
play = [l for l in out.splitlines() if l.startswith('play:')]
check('the relay listens first', any(l.startswith('play: relay listening, port ') for l in play), play)
started = [int(m.group(1)) for l in play for m in [re.match(r'play: console (\d+) started, relay port \d+', l)] if m]
check('Play boots 3 consoles in order', started == [1, 2, 3], play)

lines = {c: [] for c in range(1, N + 1)}
for line in out.splitlines():
    m = re.match(r'game: console (\d+) (.*)$', line)
    if m and int(m.group(1)) in lines:
        lines[int(m.group(1))].append(m.group(2))
for c in lines:
    check('console %d shows frames' % c, any(l.startswith('frames') for l in lines[c]), lines[c][-3:])
linked = {}
for c in lines:
    for l in lines[c]:
        m = re.search(r'Relay slot (\d+) · Linked · slot (\d+) · (host|not host) · (\d+) players', l)
        if m:
            linked[c] = (int(m.group(1)), int(m.group(2)), m.group(3) == 'host', int(m.group(4)))
    check('console %d reaches Connected through the relay' % c, c in linked, lines[c][-3:])
if len(linked) == N:
    check('slots 1, 2, 3 in console order', [linked[c][0] for c in (1, 2, 3)] == [1, 2, 3], linked)
    check('the engine agrees with the relay', all(linked[c][0] == linked[c][1] for c in linked), linked)
    check('console 1 is host, the others are not', [linked[c][2] for c in (1, 2, 3)] == [True, False, False], linked)
    check('3 players each', all(linked[c][3] == N for c in linked), linked)

shown = [l for l in out.splitlines() if l.startswith('console: play shown ')]
got = [int(m.group(1)) for l in shown for m in [re.match(r'console: play shown (\d+) of', l)] if m]
check('console 2 got the others\' events (Net.Send crossed)', got and got[-1] > 0, shown[-3:])

conns = [l for l in out.splitlines() if l.startswith('net: connections')]
check('the Network view maps consoles to slots', 'net: connections 1:1 2:2 3:3' in conns, conns[-3:])
types = [l for l in out.splitlines() if l.startswith('net: types')]
last = types[-1] if types else ''
counts = dict((k, int(v)) for k, v in re.findall(r' (\w+) (\d+)', last))
check('the Network view lists the handshake and the events',
      counts.get('Hello', 0) >= N and counts.get('HelloAck', 0) >= N and counts.get('Event', 0) > 0 and
      counts.get('EventAck', 0) > 0 and counts.get('Snapshot', 0) > 0, last)
# Each join told the others: at least 1 + 2 + 3 HelloAcks for 3 joins.
check('every member hears of each join', counts.get('HelloAck', 0) >= 6, last)
lastrows = [l for l in out.splitlines() if l.startswith('net: last')]
check('an Event from a slot goes to the other two',
      any(re.search(r'Event slot (\d) > slots (\d), (\d)', l) for l in lastrows), lastrows[-3:])
check('hiding Snapshots hides them', any(l.startswith('net: shown') for l in out.splitlines()) and
      not any('Snapshot' in l for l in lastrows[-1:]), lastrows[-1:])
try:
    with open(os.path.join(tmp, 'splashedit-play', 'console3', 'pcsx.json')) as fh:
        d = json.load(fh)['emulator']['Debug']
    check('console 3 is a client of the relay', d['SIO1Client'] and not d['SIO1Server'] and d['SIO1Clienthost'] == '127.0.0.1', d)
except (OSError, KeyError, ValueError) as e:
    check('console 3 is a client of the relay', False, e)
status = [l for l in out.splitlines() if l.startswith('playstatus: ')]
check('the status bar says 3 via the relay', any('3 linked consoles via the relay' in l for l in status), status[-3:])
check('stopping Play stops them all', status and not status[-1].startswith('playstatus: pcsx-redux running'), status[-2:])
if shots and os.path.exists(net):
    print('network screenshot: ' + net)

if fails:
    print('\n'.join(out.splitlines()[-60:]))
print('relayplaycheck: %d failure(s)' % fails)
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if fails else 0)
