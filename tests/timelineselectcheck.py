# Timeline selections and events: drag a box over empty lane space to select
# keys on several lanes (Ctrl adds a second box), drag one selected key to
# move them all, Delete them all, each one undo step; copy and paste keys at
# the scrub head; and the audio lane: add an event from the clip menu, drag
# it, delete it, undo each. A key drag or a box held past the lanes' right or
# left edge scrolls the view along. Each run saves with Ctrl+S and the check
# reads the saved scene.
# usage: python3 timelineselectcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


# Layout at 1600x960 with the timeline open (as timelinecheck.py): frame f
# on the ruler at x 532 + f * 4.4267 (150 frames fitted), lane i centred at
# y 700 + 40 i; the audio lane comes after the three tracks.
def fx(frame):
    return round(532 + frame * (1224 - 532 - 28) / 150)


def lane(i):
    return 700 + 40 * i


RULER_Y = 668
AUDIO_Y = lane(3)
# Double-clicking the audio lane at (1100, AUDIO_Y) opens the clip menu
# upwards from the mouse: bell, then gong.
ADD_AT = f'1100,{AUDIO_Y}'
MENU_GONG = '1120,806'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def key(frame, value):
    return {'frame': frame, 'value': value}


def project_copy(far=False):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    # Two audio clips for the audio lane's menu.
    for name, clip in (('Bell', 'bell'), ('Gong', 'gong')):
        s['objects'].append({'name': name, 'active': True,
                             'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                             'components': [{'type': 'audio', 'clip': None, 'clipName': clip, 'sampleRate': 22050,
                                             'loop': False, 'defaultVolume': 100, 'trimLeadingSilence': False}],
                             'children': []})
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 150, 'tracks': [
        {'type': 'objectPosition', 'target': 'Crate', 'keyframes': [key(0, [0, 0, 0]), key(45, [0, 1.5, 0]), key(90, [0, 0, 0])]},
        {'type': 'cameraPosition', 'target': '', 'keyframes': [key(0, [0, 3, -10]), key(60, [4, 3, -6]), key(150, [6, 2, 0])]},
        {'type': 'objectRotation', 'target': 'Crate', 'keyframes': [key(30, [0, 0, 0]), key(120, [0, 90, 0])]}],
        'audioEvents': [{'frame': 20, 'clip': 'bell', 'volume': 100, 'pan': 64},
                        {'frame': 75, 'clip': 'gong', 'volume': 90, 'pan': 30}]}]
    if far:  # a rotation key past what the fitted view shows
        s['cutscenes'][0]['tracks'][2]['keyframes'].append(key(400, [0, 0, 0]))
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(args, far=False):
    """Runs the editor on a fresh copy with the timeline open on intro, then
    `args` and Ctrl+S; returns the saved cutscene and the editor's output."""
    project = project_copy(far)
    cmd = [editor, '--size', '1600x960', '--project', project, '--timeline', 'intro'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return scene['cutscenes'][0], (r.stdout + r.stderr).strip()


def frames(cs):
    return [[k['frame'] for k in t['keyframes']] for t in cs['tracks']]


def events(cs):
    return [(a['frame'], a['clip']) for a in cs['audioEvents']]


START = [[0, 45, 90], [0, 60, 150], [30, 120]]
START_AUDIO = [(20, 'bell'), (75, 'gong')]


def box(f0, y0, f1, y1):
    return ['--drag', f'{fx(f0)},{y0},{fx(f1)},{y1}', '--wait', '3']


# A box from empty space over frames 40..65 of the first two lanes takes the
# key at 45 and the one at 60; dragging the one at 45 by ten frames moves both.
BOX_TWO = box(40, lane(0) - 15, 65, lane(1) + 15)
GROUP_MOVE = BOX_TWO + ['--drag', f'{fx(45)},{lane(0)},{fx(55) + 1},{lane(0)}', '--wait', '3']
cs, out = run(GROUP_MOVE)
check('a box selects keys on two lanes and one drag moves them all',
      frames(cs) == [[0, 55, 90], [0, 70, 150], [30, 120]], str(frames(cs)) + ' | ' + out[-200:])
check('the group move keeps the keys\' values', cs['tracks'][0]['keyframes'][1]['value'] == [0, 1.5, 0] and
      cs['tracks'][1]['keyframes'][1]['value'] == [4, 3, -6], cs['tracks'][:2])
cs, out = run(GROUP_MOVE + ['--key', 'ctrl+z', '--wait', '3'])
check('one Ctrl+Z takes the whole group move back', frames(cs) == START, str(frames(cs)) + ' | ' + out[-200:])

# A box alone moves nothing.
cs, out = run(BOX_TWO)
check('box selecting alone changes nothing', frames(cs) == START and events(cs) == START_AUDIO, str(frames(cs)) + ' | ' + out[-200:])

# Ctrl adds a second box (the key at 120 on the third lane); Delete removes all three.
GROUP_DELETE = BOX_TWO + ['--keydown', 'ctrl', '--wait', '2'] + box(115, lane(2) - 15, 125, lane(2) + 15) + [
    '--keyup', 'ctrl', '--wait', '2', '--key', 'delete', '--wait', '3']
cs, out = run(GROUP_DELETE)
check('Ctrl adds a second box and Delete removes every selected key',
      frames(cs) == [[0, 90], [0, 150], [30]], str(frames(cs)) + ' | ' + out[-200:])
cs, out = run(GROUP_DELETE + ['--key', 'ctrl+z', '--wait', '3'])
check('one Ctrl+Z brings every deleted key back', frames(cs) == START, str(frames(cs)) + ' | ' + out[-200:])

# A box over the third lane and the audio lane takes a key and an event; they move together.
cs, out = run(box(15, lane(2) - 15, 35, AUDIO_Y + 15) + ['--drag', f'{fx(30)},{lane(2)},{fx(40) + 1},{lane(2)}', '--wait', '3'])
check('a box over a key and an audio event moves both',
      frames(cs)[2] == [40, 120] and events(cs) == [(30, 'bell'), (75, 'gong')], f'{frames(cs)} {events(cs)} | ' + out[-200:])

# Copy the key at 45 and paste it at the scrub head (frame 100).
cs, out = run(['--click', f'{fx(45)},{lane(0)}', '--wait', '3', '--key', 'ctrl+c', '--wait', '2', '--click', f'{fx(100)},{RULER_Y}',
               '--wait', '3', '--key', 'ctrl+v', '--wait', '3'])
check('Ctrl+C then Ctrl+V pastes the key at the scrub head', frames(cs)[0] == [0, 45, 90, 100] and
      cs['tracks'][0]['keyframes'][3]['value'] == [0, 1.5, 0], str(cs['tracks'][0]) + ' | ' + out[-200:])

# Audio lane: double-click it for the clip menu, pick gong: an event at the scrub head.
ADD = ['--click', f'{fx(100)},{RULER_Y}', '--wait', '3', '--dblclick', ADD_AT, '--wait', '3', '--click', MENU_GONG, '--wait', '3']
cs, out = run(ADD)
check('the audio lane\'s menu adds an event of the picked clip at the scrub head',
      events(cs) == START_AUDIO + [(100, 'gong')] and cs['audioEvents'][2]['volume'] == 100, f'{events(cs)} | ' + out[-200:])
cs, out = run(ADD + ['--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z takes the added audio event back', events(cs) == START_AUDIO, f'{events(cs)} | ' + out[-200:])

# Drag the bell event from 20 to 35.
MOVE = ['--drag', f'{fx(20)},{AUDIO_Y},{fx(35) + 1},{AUDIO_Y}', '--wait', '3']
cs, out = run(MOVE)
check('dragging an audio event moves it', events(cs) == [(35, 'bell'), (75, 'gong')] and cs['audioEvents'][1]['pan'] == 30,
      f'{events(cs)} | ' + out[-200:])
cs, out = run(MOVE + ['--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z puts the dragged audio event back', events(cs) == START_AUDIO, f'{events(cs)} | ' + out[-200:])

# Click the gong event, Delete.
DELETE = ['--click', f'{fx(75)},{AUDIO_Y}', '--wait', '3', '--key', 'delete', '--wait', '3']
cs, out = run(DELETE)
check('Delete removes the selected audio event', events(cs) == [(20, 'bell')] and frames(cs) == START, f'{events(cs)} | ' + out[-200:])
cs, out = run(DELETE + ['--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z brings the deleted audio event back', events(cs) == START_AUDIO, f'{events(cs)} | ' + out[-200:])

# Auto-scroll. The lanes end at x 1236 (the inspector is right of them) and
# start at 520 (the labels are left). Scripted frames are 1/60 s apart, so a
# hold of 120 frames is 2 s of scrolling at 240 px/s and up.
PAST_RIGHT, PAST_LEFT = 1300, 500


def hold(x0, y0, x1, y1, frames=120):
    return ['--mousedown', f'{x0},{y0}', '--wait', '3', '--move', f'{x1},{y1}', '--wait', str(frames), '--mouseup', '0', '--wait', '3']


# Drag the key at 90 past the right edge and hold: the view scrolls and the
# key goes with it, well past frame 174 where the edge alone would leave it.
cs, out = run(hold(fx(90), lane(0), PAST_RIGHT, lane(0)))
check('a key held past the right edge scrolls the view and moves on with it',
      len(frames(cs)[0]) == 3 and frames(cs)[0][:2] == [0, 45] and frames(cs)[0][2] > 300, str(frames(cs)) + ' | ' + out[-200:])
# A box held past the right edge reaches the key at 400 the fitted view does not show; Delete removes it.
cs, out = run(hold(fx(130), lane(2) - 15, PAST_RIGHT, lane(2) + 15) + ['--key', 'delete', '--wait', '3'], far=True)
check('a box held past the right edge scrolls the view and selects what it reaches',
      frames(cs) == START, str(frames(cs)) + ' | ' + out[-200:])
# Shift+wheel pans the view 27 frames on; the key at 120 dragged past the
# left edge and held follows the view back to frame 0 (the edge alone: 20).
PAN = ['--keydown', 'shift', '--wait', '2', '--wheel', f'900,{lane(1)},-3', '--wait', '3', '--keyup', 'shift', '--wait', '2']
cs, out = run(PAN + hold(round(fx(120) - 3 * 40), lane(2), PAST_LEFT, lane(2)))
check('a key held past the left edge scrolls the view back and moves on with it',
      frames(cs)[2] == [0, 30], str(frames(cs)) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
