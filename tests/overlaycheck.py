# Build > Performance Overlay and Room Debug Overlay, driven through the GUI:
# each picks the psxsplash build Play and Run on hardware boot, the one beside
# the plain build with that overlay compiled in (psxsplash-perf.ps-exe,
# -rooms, -perf-rooms). The editor points at fake builds in a temp folder
# through SPLASHEDIT_PSXSPLASH and keeps Play's tools in a temp file
# (--play-settings), so the choice must be there on a second launch. The
# editor prints "play: overlays <perf|rooms|perf,rooms|none> engine <file>"
# at the start and whenever either changes. Menu items are clicked by name
# (--click @menu.build, --click "@menu.build/Room Debug Overlay").
# usage: python3 overlaycheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
engines = os.path.join(work, 'engine')
os.makedirs(engines)
for name in ('psxsplash.ps-exe', 'psxsplash-perf.ps-exe', 'psxsplash-rooms.ps-exe', 'psxsplash-perf-rooms.ps-exe'):
    with open(os.path.join(engines, name), 'wb') as fh:
        fh.write(b'PS-X EXE fake')
settings = os.path.join(work, 'prefs', 'play.cfg')
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux',
           SPLASHEDIT_PSXSPLASH=os.path.join(engines, 'psxsplash.ps-exe'))


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def toggle(label):
    return ['--click', '@menu.build', '--wait', '3', '--click', '@menu.build/' + label, '--wait', '3']


def run(args):
    """Runs the editor on a fresh courtyard copy with Play's settings in
    `settings`, then `args`. Returns the printed play: lines and all output."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--play-settings', settings, '--wait', '5'] + args + \
        ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return [l for l in lines if l.startswith('play: overlays')], lines


def at(lines, i):
    return lines[i] if i < len(lines) else None


# Perf on, rooms on, perf off: one line each, naming the build that boots.
plays, lines = run(toggle('Performance Overlay') + toggle('Room Debug Overlay') + toggle('Performance Overlay'))
check('it starts with no overlay and the plain build', at(plays, 0) == 'play: overlays none engine psxsplash.ps-exe', plays)
check('Performance Overlay picks psxsplash-perf', at(plays, 1) == 'play: overlays perf engine psxsplash-perf.ps-exe', plays)
check('Room Debug Overlay on top picks psxsplash-perf-rooms',
      at(plays, 2) == 'play: overlays perf,rooms engine psxsplash-perf-rooms.ps-exe', plays)
check('Performance Overlay off again leaves psxsplash-rooms', at(plays, 3) == 'play: overlays rooms engine psxsplash-rooms.ps-exe',
      plays)
check('no menu is left open', 'popups open: no' in lines, [l for l in lines if l.startswith('popups open')])
check('the overlays are no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])

# Saved with Play's tools at once.
saved = open(settings).read().splitlines() if os.path.exists(settings) else None
check('the settings file holds the overlays', saved is not None and 'overlay_perf=0' in saved and 'overlay_rooms=1' in saved,
      saved)

# A second launch on the same settings starts where the first left off.
plays, lines = run([])
check('a second launch keeps Room Debug Overlay', plays == ['play: overlays rooms engine psxsplash-rooms.ps-exe'], plays)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
