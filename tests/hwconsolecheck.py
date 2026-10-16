# The Run on hardware output popup's toolbar, driven through the GUI: the
# filter, Copy, Clear, and a new run dropping the clear point. The console is
# a fake Unirom on a localhost socket (SPLASHEDIT_PORT=tcp:127.0.0.1:N): it
# ignores the psxmon PING, answers SEXE, takes the upload, then prints 20
# "tick" lines and 3 "Lua error" ones; the first run then waits for a flag
# file before printing "late 1", the second prints 5 "more" lines. The editor
# adds two lines of its own (uploading, running), so the first run shows 25
# lines before the late one and the second 30, past the first's clear point
# (26): only dropping that clear point shows the second run whole. While the
# popup is open the editor prints "console: hw shown N of M" whenever it
# changes, and "console: hw copied N lines" / "console: hw cleared" per click.
# usage: python3 hwconsolecheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import os, shutil, socket, struct, subprocess, sys, tempfile, threading, time

editor = os.path.abspath(sys.argv[1])
shot = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
engine = os.path.join(work, 'psxsplash.ps-exe')
with open(engine, 'wb') as fh:
    fh.write(b'PS-X EXE' + bytes(0x800 - 8) + bytes(range(100)))
flag = os.path.join(work, 'late.flag')
flagFrom = os.path.join(work, 'flag.src')
with open(flagFrom, 'wb') as fh:
    fh.write(b'go\n')

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind(('127.0.0.1', 0))
server.listen(2)
port = server.getsockname()[1]
runs = []  # one entry per session: 'ok' or why it went wrong


def recv(conn, n):
    out = b''
    while len(out) < n:
        b = conn.recv(n - len(out))
        if not b:
            raise EOFError('host closed the link')
        out += b
    return out


def session(conn, first):
    conn.settimeout(20)
    tail = b''
    while not tail.endswith(b'SEXE'):
        tail = (tail + recv(conn, 1))[-4:]
    conn.sendall(b'SEXE' + b'OKAY')
    header = recv(conn, 0x800)
    if header[:8] != b'PS-X EXE':
        return 'SEXE header is not the EXE header'
    _entry, _dest, size, _check = struct.unpack('<4I', recv(conn, 16))
    recv(conn, size)
    time.sleep(0.3)
    conn.sendall(b''.join(b'tick %d\n' % i for i in range(1, 21)) +
                 b''.join(b'Lua error: boom %s\n' % k for k in (b'a', b'b', b'c')))
    if first:
        while not os.path.exists(flag):
            time.sleep(0.1)
        conn.sendall(b'late 1\n')
    else:
        conn.sendall(b''.join(b'more %d\n' % i for i in range(1, 6)))
    conn.settimeout(None)
    while conn.recv(4096):  # hold the link until the editor stops the run
        pass
    return 'ok'


def console():
    for first in (True, False):
        conn, _ = server.accept()
        try:
            runs.append(session(conn, first))
        except Exception as e:  # recorded, checked below
            runs.append('session %d: %s' % (len(runs) + 1, e))
        finally:
            conn.close()


threading.Thread(target=console, daemon=True).start()
ENV = dict(os.environ, SPLASHEDIT_PSXSPLASH=engine, SPLASHEDIT_PORT='tcp:127.0.0.1:%d' % port)


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def after(lines, want, start):
    """Index of the first `want` in `lines` at or after `start`, or -1."""
    for i in range(start, len(lines)):
        if lines[i] == want:
            return i
    return -1


project = os.path.join(tempfile.mkdtemp(), 'courtyard')
shutil.copytree(example, project)
mid = [] if not shot else ['--shot', shot]
args = ['--wait', '10', '--key', 'ctrl+f5', '--wait', '400',
        '--click', '@hwstatus', '--wait', '10',
        '--click', '@hw.filter', '--wait', '3', '--text', 'ErRoR', '--wait', '10'] + mid + \
       ['--click', '@hw.copy', '--wait', '5',
        '--click', '@hw.filter', '--wait', '3', '--key', 'ctrl+a', '--wait', '3', '--key', 'backspace', '--wait', '5',
        '--click', '@hw.clear', '--wait', '10',
        '--copy-file', flagFrom + '@' + flag, '--wait', '60',
        # Clear past the late line (26 lines), then stop and start a second
        # run: its 30 lines pass the old clear point, and all must show.
        '--click', '@hw.clear', '--wait', '10',
        '--key', 'ctrl+f5', '--wait', '30', '--key', 'ctrl+f5', '--wait', '400']
cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
lines = (r.stdout + r.stderr).strip().splitlines()
missing = [l for l in lines if 'no such control drawn' in l]
check('every control the script names was drawn', not missing, missing)
check('the fake console served two runs', runs == ['ok', 'ok'], runs)
con = [l for l in lines if l.startswith('console: ')]

i = after(con, 'console: hw shown 25 of 25', 0)
check('the popup shows all 25 lines', i >= 0, con)
j = after(con, 'console: hw shown 3 of 25', max(i, 0))
check('"ErRoR" keeps the 3 error lines, whatever their case', j > i >= 0, con)
k = after(con, 'console: hw copied 3 lines', max(j, 0))
check('Copy copies the 3 lines shown', k > j >= 0, con)
k2 = after(con, 'console: hw shown 25 of 25', max(k, 0))
check('an empty filter shows all 25 again', k2 > k >= 0, con)
c = after(con, 'console: hw cleared', max(k2, 0))
z = after(con, 'console: hw shown 0 of 0', max(c, 0))
check('Clear clears, at once', c > k2 >= 0 and z == c + 1, con)
late = after(con, 'console: hw shown 1 of 1', max(z, 0))
check('a line printed after Clear shows', late > z >= 0, con)
c2 = after(con, 'console: hw cleared', max(late, 0))
z2 = after(con, 'console: hw shown 0 of 0', max(c2, 0))
check('a second Clear hides the late line, at once', c2 > late >= 0 and z2 == c2 + 1, con)
again = after(con, 'console: hw shown 30 of 30', max(z2, 0))
check('a new run shows all its lines, the old clear point gone', again > z2 >= 0, con)
check('nothing was copied but once', len([l for l in con if 'copied' in l]) == 1, con)
if shot:
    check('the screenshot was written', os.path.exists(shot), shot)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
