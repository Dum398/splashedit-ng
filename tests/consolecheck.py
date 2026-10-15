# The Play output popup's toolbar, driven through the GUI: the filter (case
# does not matter), Copy, Clear, and a new run dropping the clear point. The
# editor runs a fake pcsx-redux (SPLASHEDIT_REDUX, a shell script) that prints
# 20 "tick" lines and 3 "Lua error" ones, then waits for a flag file before
# printing "late 1", so the late line lands where the script wants it. While
# the popup is open the editor prints "console: play shown N of M" (N shown
# of the M lines after the clear point) whenever it changes, and
# "console: play copied N lines" / "console: play cleared" on each click.
# usage: python3 consolecheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import os, shutil, stat, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shot = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
engine = os.path.join(work, 'psxsplash.ps-exe')
with open(engine, 'wb') as fh:
    fh.write(b'PS-X EXE fake')
flag = os.path.join(work, 'late.flag')
flagFrom = os.path.join(work, 'flag.src')
with open(flagFrom, 'wb') as fh:
    fh.write(b'go\n')
redux = os.path.join(work, 'fake-redux.sh')
with open(redux, 'w', newline='\n') as fh:
    fh.write('#!/bin/sh\n'
             'i=1\n'
             'while [ $i -le 20 ]; do echo "tick $i"; i=$((i + 1)); done\n'
             'for k in a b c; do echo "Lua error: boom $k"; done\n'
             'while [ ! -e "%s" ]; do sleep 0.1; done\n'
             'echo "late 1"\n'
             'exec sleep 60\n' % flag)
os.chmod(redux, os.stat(redux).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
ENV = dict(os.environ, SPLASHEDIT_REDUX=redux, SPLASHEDIT_PSXSPLASH=engine)


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


def clearFilter():
    return ['--click', '@play.filter', '--wait', '3', '--key', 'ctrl+a', '--wait', '3', '--key', 'backspace', '--wait', '5']


project = os.path.join(tempfile.mkdtemp(), 'courtyard')
shutil.copytree(example, project)
mid = [] if not shot else ['--shot', shot]
args = ['--wait', '10', '--key', 'f5', '--wait', '240',
        '--click', '@playstatus', '--wait', '10',
        '--click', '@play.filter', '--wait', '3', '--text', 'ErRoR', '--wait', '10'] + mid + \
       ['--click', '@play.copy', '--wait', '5'] + clearFilter() + \
       ['--click', '@play.clear', '--wait', '10',
        '--copy-file', flagFrom + '@' + flag, '--wait', '60',
        # Clear past the late line, then a second run: its 24th line reaches
        # the old clear point, and must still show from its first. The popup
        # stays open through F5 (a click on the status entry would close it).
        '--click', '@play.clear', '--wait', '10',
        '--delete-file', flag, '--key', 'f5', '--wait', '30', '--key', 'f5', '--wait', '240',
        '--copy-file', flagFrom + '@' + flag, '--wait', '60']
cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
lines = (r.stdout + r.stderr).strip().splitlines()
missing = [l for l in lines if 'no such control drawn' in l]
check('every control the script names was drawn', not missing, missing)
con = [l for l in lines if l.startswith('console: ')]

i = after(con, 'console: play shown 23 of 23', 0)
check('the popup shows all 23 lines', i >= 0, con)
j = after(con, 'console: play shown 3 of 23', max(i, 0))
check('"ErRoR" keeps the 3 error lines, whatever their case', j > i >= 0, con)
k = after(con, 'console: play copied 3 lines', max(j, 0))
check('Copy copies the 3 lines shown', k > j >= 0, con)
k2 = after(con, 'console: play shown 23 of 23', max(k, 0))
check('an empty filter shows all 23 again', k2 > k >= 0, con)
c = after(con, 'console: play cleared', max(k2, 0))
check('Clear clears', c > k2 >= 0, con)
z = after(con, 'console: play shown 0 of 0', max(c, 0))
check('after Clear nothing shows, at once', c > k2 >= 0 and z == c + 1, con)
late = after(con, 'console: play shown 1 of 1', max(z, 0))
check('a line printed after Clear shows', late > z >= 0, con)
c2 = after(con, 'console: play cleared', max(late, 0))
z2 = after(con, 'console: play shown 0 of 0', max(c2, 0))
check('a second Clear hides the late line, at once', c2 > late >= 0 and z2 == c2 + 1, con)
again = after(con, 'console: play shown 24 of 24', max(z2, 0))
check('a new run shows all its lines, the old clear point gone', again > z2 >= 0, con)
check('nothing was copied but once', len([l for l in con if 'copied' in l]) == 1, con)
if shot:
    check('the screenshot was written', os.path.exists(shot), shot)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
