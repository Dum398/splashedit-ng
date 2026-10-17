# The scene inspector's Disc section "Music (CD-DA)" sub-list, driven through
# the GUI on copies of the courtyard. Each step adds, reorders or removes a
# track and checks both the scripted lines the editor prints
# ("disc music: <n> tracks", "disc music: track <N> <path> <m:ss|missing|
# unreadable>") and disc.json on disk after the step. A track whose file is
# not there shows a "missing" row. A screenshot of the scrolled-to Disc
# section is saved (optional 2nd arg sets its path).
# usage: python3 discmusiccheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shot_out = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fmt = os.path.join(here, 'audio', 'formats')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# Folding the sections above Disc brings the Disc section and its short music
# sub-list fully into view at 1600x960, so no scrolling is needed here.
FOLDS = ['--click', '@s_sequences', '--wait', '3', '--click', '@s_rendering', '--wait', '3', '--click', '@s_loading', '--wait', '3',
         '--click', '@s_vram', '--wait', '5']
TONE_WAV = open(os.path.join(fmt, 'tone.wav'), 'rb').read()
TONE_FLAC = open(os.path.join(fmt, 'tone.flac'), 'rb').read()


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args, files=None, keep_shot=False):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    # The two fixture tones live in the project so the Add dropdown lists them.
    open(os.path.join(project, 'tone.wav'), 'wb').write(TONE_WAV)
    open(os.path.join(project, 'tone.flac'), 'wb').write(TONE_FLAC)
    for name, data in (files or {}).items():
        open(os.path.join(project, name), 'wb').write(data)
    shot = os.path.join(project, 'shot.png')
    cmd = ([editor, '--size', '1600x960', '--project', project, '--wait', '5'] + FOLDS + args +
           ['--wait', '5', '--screenshot', shot])
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    path = os.path.join(project, 'disc.json')
    data = json.load(open(path)) if os.path.exists(path) else None
    controls = {l[len('control '):].split(' at ')[0] for l in lines if l.startswith('control ') and ' at ' in l}
    music = [l for l in lines if l.startswith('disc music:')]
    if keep_shot and shot_out and os.path.exists(shot):
        shutil.copyfile(shot, shot_out)
    return music, lines, data, controls


def disc_json(tracks):
    return {'disc.json': json.dumps({'volume': '', 'licence': '', 'tracks': tracks}).encode()}


def click(name):
    return ['--click', '@' + name, '--wait', '3']


# Add one track through the dropdown.
music, lines, data, controls = run(click('disc.addtrack') + click('disc.addtrack/tone.wav'), keep_shot=True)
check('the music controls are drawn', 'disc.addtrack' in controls, sorted(controls))
check('adding one track writes it to disc.json', (data or {}).get('tracks') == ['tone.wav'], data)
check('one-track scripted lines', 'disc music: 1 tracks' in music and 'disc music: track 2 tone.wav 0:00' in music, music)

# Add a second to a list that already has one.
music, lines, data, controls = run(click('disc.addtrack') + click('disc.addtrack/tone.flac'), disc_json(['tone.wav']))
check('adding a second track appends it', (data or {}).get('tracks') == ['tone.wav', 'tone.flac'], data)
check('two-track scripted lines', 'disc music: 2 tracks' in music and
      'disc music: track 2 tone.wav 0:00' in music and 'disc music: track 3 tone.flac 0:00' in music, music)
check('both track rows are drawn', 'disc.track0' in controls and 'disc.track1' in controls, sorted(controls))

# Move the second track up: the list reorders on disk.
music, lines, data, controls = run(click('disc.track1.up'), disc_json(['tone.wav', 'tone.flac']))
check('the up button reorders disc.json', (data or {}).get('tracks') == ['tone.flac', 'tone.wav'], data)
check('reordered scripted lines', 'disc music: track 2 tone.flac 0:00' in music and
      'disc music: track 3 tone.wav 0:00' in music, music)

# A disabled button at the ends does nothing: track 0's up is disabled.
music, lines, data, controls = run(click('disc.track0.up'), disc_json(['tone.wav', 'tone.flac']))
check('up on the first track changes nothing', (data or {}).get('tracks') == ['tone.wav', 'tone.flac'], data)

# Remove the first track.
music, lines, data, controls = run(click('disc.track0.remove'), disc_json(['tone.flac', 'tone.wav']))
check('the remove button drops the track', (data or {}).get('tracks') == ['tone.wav'], data)
check('one-track scripted lines after remove', 'disc music: 1 tracks' in music and
      'disc music: track 2 tone.wav 0:00' in music, music)

# A track whose file is gone shows a "missing" row.
music, lines, data, controls = run([], disc_json(['gone.wav']))
check('a missing track file shows a missing row', 'disc music: track 2 gone.wav missing' in music, music)
check('the missing track row is drawn', 'disc.track0' in controls, sorted(controls))

if shot_out:
    check('a screenshot was saved', os.path.exists(shot_out), shot_out)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
