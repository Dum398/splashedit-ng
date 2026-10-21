# Play with 4 consoles through the editor's relay, with fake pcsx-redux
# processes: the Players list, the Game view's grid, each console's relay
# slot, and the Network view. Each fake (SPLASHEDIT_REDUX, a Python script)
# reads its console's pcsx.json, connects to the relay as redux's SIO1 client
# does (protobuf, one byte per message after a FlowControl), says Hello,
# prints the link reporter's line from every HelloAck it gets, sends an Event
# a second, then execs emulator_tests with SPLASHEDIT_FAKE_REDUX set (its
# display block opens the Game view) while a child keeps talking. Console k
# waits (4 - k) * 0.6 s before connecting, so consoles started all at once
# would reach the relay in the wrong order: only Play's one-at-a-time start
# gives console k slot k.
#
# Checks: Players > 4 Consoles is saved; Play starts the relay, then the four
# consoles in order; the grid shows four consoles, each "Relay slot k ·
# Linked · ... 4 players" (every member hears of every join); a click gives
# console 3 the keyboard; the Network view maps consoles to slots, counts the
# handshake and the Events, filters by slot and type, and Pause holds it;
# Stop (F5) ends the session.
# usage: python3 relayviewcheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import os, re, shutil, stat, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
display = os.path.join(os.path.dirname(editor), 'emulator_tests')
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
N = 4

if os.name == 'nt':
    print('skip relayviewcheck: the fake pcsx-redux forks')
    sys.exit(0)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


work = tempfile.mkdtemp()
engine = os.path.join(work, 'psxsplash.ps-exe')
with open(engine, 'wb') as fh:
    fh.write(b'PS-X EXE fake')
FAKE = r'''#!/usr/bin/env python3
import json, os, re, socket, struct, sys, time
args = sys.argv[1:]
portable = args[args.index('-portable') + 1] if '-portable' in args else ''
k = int(re.search(r'console(\d+)$', portable.rstrip('/')).group(1)) if portable else 1


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(t, payload, seq, slot=0xFF):
    body = struct.pack('<BBBBHH', t, slot, 0, 0, seq, len(payload)) + payload
    return b'\xa5' + body + struct.pack('<H', crc16(body))


def wire(data):  # as redux sends: one DataTransfer per byte
    return b''.join(bytes([5, 0x0A, 3, 0x0A, 1, b]) for b in data)


def talk(sock):
    sock.sendall(bytes([6, 0x12, 4, 0x08, 1, 0x10, 1]))
    hello = struct.pack('<IHHII', 0x4C4E5350, 1, 0, 0xFA4E0001, 1000 + k)
    sock.sendall(wire(frame(1, hello, 1)))
    sock.settimeout(0.1)
    buf, psnl, slot, seq, start, parent = b'', b'', 0xFF, 2, time.time(), os.getppid()
    nextEvent = time.time() + 1
    while time.time() - start < 100 and os.getppid() == parent:
        try:
            got = sock.recv(4096)
            if not got:
                return
            buf += got
        except socket.timeout:
            pass
        while buf and len(buf) >= 1 + buf[0]:
            msg, buf = buf[1:1 + buf[0]], buf[1 + buf[0]:]
            if msg[:1] == b'\x0a':  # SIOPayload{1: DataTransfer{1: data}} as the relay sends it
                n = msg[3]
                psnl += msg[4:4 + n] if msg[2:3] == b'\x0a' else b''
        while True:
            at = psnl.find(b'\xa5')
            if at < 0 or len(psnl) - at < 9:
                break
            t, src, fl, rs, sq, ln = struct.unpack_from('<BBBBHH', psnl, at + 1)
            if len(psnl) - at < 11 + ln:
                break
            payload = psnl[at + 9:at + 9 + ln]
            psnl = psnl[at + 11 + ln:]
            if t == 2 and ln == 16:
                _, _, slot, host, _, players = struct.unpack_from('<IHBBIB', payload)
                print('splashedit-link: state 2 host %d slot %d players %d' % (int(slot == host), slot, players), flush=True)
            elif t == 4:
                sock.sendall(wire(frame(5, b'', sq, slot)))
        if slot != 0xFF and time.time() >= nextEvent:
            nextEvent += 1
            sock.sendall(wire(frame(4, struct.pack('<ii', 7, slot * 1000 + seq), seq, slot)))
            seq += 1


cfg = json.load(open(os.path.join(portable, 'pcsx.json'))) if portable else {}
dbg = cfg.get('emulator', {}).get('Debug', {})
if dbg.get('SIO1Client'):
    time.sleep((4 - k) * 0.6)
    sock = socket.create_connection((dbg['SIO1Clienthost'], dbg['SIO1ClientPort']))
    if os.fork() == 0:
        try:
            talk(sock)
        finally:
            os._exit(0)
    sock.close()
os.environ['SPLASHEDIT_FAKE_REDUX'] = '1'
os.execv(DISPLAY, [DISPLAY])
'''.replace('DISPLAY', repr(display))
redux = os.path.join(work, 'fake-redux.py')
with open(redux, 'w', newline='\n') as fh:
    fh.write(FAKE)
os.chmod(redux, os.stat(redux).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
tmp = os.path.join(work, 'tmp')
os.makedirs(tmp)
ENV = dict(os.environ, SPLASHEDIT_REDUX=redux, SPLASHEDIT_PSXSPLASH=engine, TMPDIR=tmp, TMP=tmp, TEMP=tmp)
ENV.pop('SPLASHEDIT_BIOS', None)

project = os.path.join(work, 'courtyard')
shutil.copytree(example, project)
cfg = os.path.join(work, 'play.cfg')
with open(cfg, 'w') as fh:
    fh.write('players=1\n')


def shot(name):
    return ['--shot', shots + '-' + name + '.png'] if shots else []


args = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'courtyard.scene'),
        '--play-settings', cfg, '--wait', '10', '--click', '@players', '--wait', '10'] + shot('menu') + \
       ['--click', '@menu.players/4 Consoles', '--wait', '10', '--key', 'f5', '--wait-play', '--wait', '400'] + shot('grid') + \
       ['--click', '@game.console3', '--wait', '10', '--click', '@playstatus', '--wait', '10',
        '--click', '@play.tab.network', '--wait', '60'] + shot('network') + \
       ['--click', '@net.slot.2', '--wait', '10', '--click', '@net.type.Snapshot', '--wait', '10',
        '--click', '@net.pause', '--wait', '120'] + shot('filtered') + \
       ['--click', '@net.pause', '--wait', '10', '--key', 'escape', '--wait', '10', '--key', 'f5', '--wait', '30',
        '--screenshot', os.path.join(work, 'end.png')]
r = subprocess.run(args, capture_output=True, text=True, env=ENV, timeout=300)
out = r.stdout
lines = out.splitlines()
check('editor ran', r.returncode == 0, r.stderr[-800:])
missing = [l for l in (r.stdout + r.stderr).splitlines() if 'no such control drawn' in l]
check('every control the script names was drawn', not missing, missing)
check('Players > 4 Consoles picks 4, through the relay', 'play: players 4 relay' in lines, [l for l in lines if l.startswith('play: players')])
with open(cfg) as fh:
    saved = fh.read()
check('and is saved', 'players=4\n' in saved and 'relay=0\n' in saved, saved)
play = [l for l in lines if l.startswith('play:')]
listen = [i for i, l in enumerate(lines) if l.startswith('play: relay listening, port ')]
first = [i for i, l in enumerate(lines) if l.startswith('play: console 1 started')]
check('the relay listens before console 1 starts', listen and first and listen[0] < first[0], play)
started = [int(m.group(1)) for l in play for m in [re.match(r'play: console (\d+) started, relay port \d+', l)] if m]
check('Play starts the 4 consoles one at a time, in order', started == [1, 2, 3, 4], play)

status = {}
focused = {}
for line in lines:
    m = re.match(r'game: console (\d+) \w+( focused)? \| (.*)$', line)
    if m:
        status.setdefault(int(m.group(1)), []).append(m.group(3))
        focused[int(m.group(1))] = bool(m.group(2))
check('the grid shows 4 consoles', sorted(status) == [1, 2, 3, 4], sorted(status))
for c in range(1, N + 1):
    want = 'Relay slot %d · Linked · slot %d · %s · 4 players' % (c, c, 'host' if c == 1 else 'not host')
    check('console %d: relay slot %d, 4 players' % (c, c), any(l.startswith(want) for l in status.get(c, [])), status.get(c, [])[-2:])
check('a click gives console 3 the keyboard', focused.get(3) and not focused.get(1), focused)
check('the output popup has Output and Network', 'play: output tab network' in lines, [l for l in lines if l.startswith('play: output tab')])

conns = [l for l in lines if l.startswith('net: connections')]
check('the Network view maps consoles to slots', 'net: connections 1:1 2:2 3:3 4:4' in conns, conns[-3:])
types = [l for l in lines if l.startswith('net: types')]
counts = dict((k, int(v)) for k, v in re.findall(r' (\w+) (\d+)', types[-1])) if types else {}
check('it counts the handshake', counts.get('Hello', 0) == N and counts.get('HelloAck', 0) >= 1 + 2 + 3 + 4, counts)
check('and the Events and their acks', counts.get('Event', 0) > 0 and counts.get('EventAck', 0) > 0, counts)
views = [l for l in lines if l.startswith('net: shown ')]
check('a console chip filters by its slot', any(' slot 2' in l for l in views), views[-3:])
last = [l for l in lines if l.startswith('net: last')]
slot2 = [l for l in last if l != 'net: last']
check('filtered rows all touch slot 2, no Snapshots',
      slot2 and all(all('slot 2' in row or 'slots 2' in row or ', 2' in row for row in l.split(' | ')[1:]) and 'Snapshot' not in l
                    for l in slot2[-1:]), slot2[-2:])
paused = [i for i, l in enumerate(lines) if l == 'net: paused']
resumed = [i for i, l in enumerate(lines) if l == 'net: resumed']
held = [l for l in lines[paused[0]:resumed[0]] if l.startswith('net: shown ')] if paused and resumed else []
check('Pause holds the list still', paused and resumed and len(held) == 1 and held[0].endswith(' paused'), held)
end = [l for l in lines if l.startswith('playstatus: ')]
check('the status bar says 4 via the relay', any('4 linked consoles via the relay' in l for l in end), end[-3:])
check('Stop stops every console', end and not end[-1].startswith('playstatus: pcsx-redux running'), end[-2:])

if fails:
    print('\n'.join(lines[-50:]))
print('relayviewcheck: %d failure(s)' % fails)
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if fails else 0)
