# Triangles psxsplash lost or bent, as Play shows them. A development engine
# prints "psxsplash: render lost ot O skinnear S leaf L clamp C of T tris"
# every 60 frames that lost any. The editor runs a fake pcsx-redux
# (SPLASHEDIT_REDUX, a shell script) that prints a peak line, then two such
# reports in turn until a flag file appears, then nothing; it execs
# emulator_tests (beside the editor) with SPLASHEDIT_FAKE_REDUX set, which
# publishes an empty display block so the Game view opens. While they come,
# the Game view shows the last one in plain words ("game: lost <words>" in a
# scripted run) and the scene's Render buffers section the worst per cause
# ("render: In Play, worst 60 frames: ..."), each cause's maximum over both
# reports; once they stop, the Game view line goes ("game: lost none") and
# the Render buffers caption stays.
# usage: python3 renderlostcheck.py <splashedit> [screenshot prefix]   (needs a display: xvfb-run -a)
import os, shutil, stat, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
display = os.path.join(os.path.dirname(editor), 'emulator_tests')
shots = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
engine = os.path.join(work, 'psxsplash.ps-exe')
with open(engine, 'wb') as fh:
    fh.write(b'PS-X EXE fake')
flag = os.path.join(work, 'quiet.flag')
flagFrom = os.path.join(work, 'flag.src')
with open(flagFrom, 'wb') as fh:
    fh.write(b'go\n')
A = 'psxsplash: render lost ot 5 skinnear 0 leaf 30 clamp 2 of 1500 tris'
B = 'psxsplash: render lost ot 0 skinnear 3 leaf 10 clamp 9 of 1500 tris'
redux = os.path.join(work, 'fake-redux.sh')
with open(redux, 'w', newline='\n') as fh:
    # The reports come from a background loop (at most 3 minutes of them);
    # the exec keeps the pid the editor looks for the display block under.
    fh.write('#!/bin/sh\n'
             'echo "psxsplash: render peak depth 258 of 988, bump 11808 of 51656"\n'
             '(i=0; while [ ! -e "%s" ] && [ $i -lt 300 ]; do echo "%s"; sleep 0.3; echo "%s"; sleep 0.3; i=$((i + 1)); done) &\n'
             'SPLASHEDIT_FAKE_REDUX=1 exec "%s"\n' % (flag, A, B, display))
os.chmod(redux, os.stat(redux).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
ENV = dict(os.environ, SPLASHEDIT_REDUX=redux, SPLASHEDIT_PSXSPLASH=engine)

WORDS_A = '5 triangles past the draw distance, 2 squashed at the screen edge, 30 near-camera pieces drawn rough'
WORDS_B = '3 skinned triangles too close to the camera, 9 squashed at the screen edge, 10 near-camera pieces drawn rough'
WORST = ('render: In Play, worst 60 frames: 5 triangles past the draw distance, 3 skinned too close to the camera, '
         '9 squashed at the screen edge, 30 near-camera pieces drawn rough.')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


project = os.path.join(tempfile.mkdtemp(), 'courtyard')
shutil.copytree(example, project)
game = [] if not shots else ['--shot', shots + '-game.png']
buffers = [] if not shots else ['--shot', shots + '-buffers.png']
args = ['--wait', '10', '--key', 'f5', '--wait', '240'] + game + \
       ['--scroll-to', '@render.lost', '--wait', '20'] + buffers + \
       ['--copy-file', flagFrom + '@' + flag, '--wait', '600']
cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'courtyard.scene')] + args + \
      ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=300)
lines = (r.stdout + r.stderr).strip().splitlines()
missing = [l for l in lines if 'no such control drawn' in l]
check('every control the script names was drawn', not missing, missing)
lost = [l for l in lines if l.startswith('game: lost ')]
shown = [l[len('game: lost '):] for l in lost if l != 'game: lost none']
check('the Game view shows the reports', bool(shown), lost)
check('in plain words, the last report each time', all(s in (WORDS_A, WORDS_B) for s in shown), shown)
check('both reports showed in turn', WORDS_A in shown and WORDS_B in shown, shown)
check('once they stop, the Game view line goes', bool(lost) and lost[-1] == 'game: lost none', lost[-3:])
render = [l for l in lines if l.startswith('render: ')]
check('Render buffers shows the worst per cause', bool(render) and render[-1] == WORST, render[-3:])
check('the screenshot was written', os.path.exists(os.path.join(project, 'shot.png')))
if shots:
    check('the mid-run screenshots were written', os.path.exists(shots + '-game.png') and os.path.exists(shots + '-buffers.png'))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
