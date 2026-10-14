# Timeline's bottom row, driven through the GUI: with a key selected, type its
# frame and a value, pick its curve, flip an on/off key and delete it (and a
# two-key selection); with a skin clip event selected, add one from the
# lane's + menu, type its frame, change its clip (a red one whose clip is
# gone to one Arm has), loop it and delete it; with an audio event selected,
# its frame, clip, volume and pan. Each run saves with Ctrl+S and the check
# reads the saved scene; the edits that matter most are undone with one Ctrl+Z.
# Controls are clicked by name (--click @timeline.kv1): the editor aims at
# where the frame before drew them, so no pixel positions live here.
# usage: python3 timelinekeyrowcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def key(frame, value, interp=None):
    k = {'frame': frame, 'value': value}
    if interp:
        k['interp'] = interp
    return k


# Lanes: 0 the Crate's position, 1 its on/off, then audio, then skin clips.
TRACKS = [{'type': 'objectPosition', 'target': 'Crate', 'keyframes': [key(0, [0, 0.5, 1]), key(75, [0, 3, 1])]},
          {'type': 'objectActive', 'target': 'Crate', 'keyframes': [key(0, [1, 0, 0]), key(60, [0, 0, 0])]}]
AUDIO = [{'frame': 20, 'clip': 'bell', 'volume': 100, 'pan': 64}]
# Arm plays raise at 30; the event at 90 wants a clip Arm does not have (drawn red).
SKIN = [{'frame': 30, 'object': 'Arm', 'clip': 'raise', 'loop': False},
        {'frame': 90, 'object': 'Arm', 'clip': 'wave', 'loop': False}]


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    # tests/skin's arm with its clips bend and raise.
    for f in ('arm.mesh', 'bend.anim', 'raise.anim'):
        shutil.copy(os.path.join(here, 'skin', f), project)
    s['objects'].append(json.load(open(os.path.join(here, 'skin', 'skin.scene'), encoding='utf-8'))['objects'][0])
    # Two audio clips for the audio lane.
    for name, clip in (('Bell', 'bell'), ('Gong', 'gong')):
        s['objects'].append({'name': name, 'active': True,
                             'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                             'components': [{'type': 'audio', 'clip': None, 'clipName': clip, 'sampleRate': 22050,
                                             'loop': False, 'defaultVolume': 100, 'trimLeadingSilence': False}],
                             'children': []})
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 150, 'tracks': TRACKS, 'audioEvents': AUDIO, 'skinEvents': SKIN}]
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


mid = None  # with SAVE_MID in the script: the cutscene as saved at that point


def run(args):
    """Runs the editor on a fresh copy with the timeline open on intro, then
    `args` and Ctrl+S; returns the saved cutscene and the editor's output
    (without its list of named controls)."""
    global mid
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--timeline', 'intro'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    mid = json.load(open(snap, encoding='utf-8'))['cutscenes'][0] if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8'))['cutscenes'][0], out


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    """Double-click a number field, replace its text, Enter."""
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


def menu(item):
    """Hover a menu's row: its submenu opens (and the row's name goes, an open menu's row is not named)."""
    return ['--move', '@' + item, '--wait', '5']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
# Saves and keeps a copy of the scene as it is then (in `mid`), so an undo arm sees the edit happen first.
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']


def k1(cs):
    return cs['tracks'][0]['keyframes'][1]


def skin(cs):
    return [(e['frame'], e['object'], e['clip'], e['loop']) for e in cs.get('skinEvents', [])]


START_SKIN = [(e['frame'], e['object'], e['clip'], e['loop']) for e in SKIN]

# (a) The key at 75: type 2.25 into its Y. One Ctrl+Z puts 3 back.
VALUE = click('timeline.key.0.1') + typed('timeline.kv1', '2.25')
cs, out = run(VALUE)
check('the key row\'s Y field sets the selected key\'s value', k1(cs)['value'] == [0, 2.25, 1] and k1(cs)['frame'] == 75,
      str(k1(cs)) + ' | ' + out[-200:])
cs, out = run(VALUE + SAVE_MID + UNDO)
check('one Ctrl+Z takes the typed value back', mid and k1(mid)['value'] == [0, 2.25, 1] and k1(cs)['value'] == [0, 3, 1],
      f'{mid and k1(mid)} then {k1(cs)} | ' + out[-200:])
cs, out = run(click('timeline.key.0.1') + typed('timeline.kv0', '-1.5') + typed('timeline.kv2', '4'))
check('the X and Z fields set theirs', k1(cs)['value'] == [-1.5, 3, 4], str(k1(cs)) + ' | ' + out[-200:])

# (b) Its curve: Step, then In-out; one Ctrl+Z goes back to Step.
CURVE = click('timeline.key.0.1') + click('kinterp/Step')
cs, out = run(CURVE)
check('the CURVE picker sets the key\'s curve to Step', k1(cs).get('interp') == 'step' and k1(cs)['value'] == [0, 3, 1],
      str(k1(cs)) + ' | ' + out[-200:])
cs, out = run(CURVE + click('kinterp/In-out') + SAVE_MID + UNDO)
check('In-out then one Ctrl+Z leaves Step', mid and k1(mid).get('interp') == 'easeInOut' and k1(cs).get('interp') == 'step',
      f'{mid and k1(mid)} then {k1(cs)} | ' + out[-200:])
cs, out = run(click('timeline.key.0.1') + click('kinterp/In') + click('kinterp/Out'))
check('the picker\'s Out lands as easeOut', k1(cs).get('interp') == 'easeOut', str(k1(cs)) + ' | ' + out[-200:])

# (d) The rest of the key row: FRAME moves the key, the on/off toggle, delete.
cs, out = run(click('timeline.key.0.1') + typed('timeline.kframe', '100'))
check('the FRAME field moves the key', [k['frame'] for k in cs['tracks'][0]['keyframes']] == [0, 100] and
      cs['tracks'][0]['keyframes'][1]['value'] == [0, 3, 1], str(cs['tracks'][0]) + ' | ' + out[-200:])
cs, out = run(click('timeline.key.1.0') + click('timeline.kon'))
check('the on/off toggle turns an on key off', cs['tracks'][1]['keyframes'][0]['value'][0] == 0, str(cs['tracks'][1]) + ' | ' + out[-200:])
cs, out = run(click('timeline.key.1.1') + click('timeline.kon'))
check('and an off key on', cs['tracks'][1]['keyframes'][1]['value'][0] == 1, str(cs['tracks'][1]) + ' | ' + out[-200:])
cs, out = run(click('timeline.key.0.1') + click('timeline.delkey'))
check('the row\'s x deletes the key', [k['frame'] for k in cs['tracks'][0]['keyframes']] == [0], str(cs['tracks'][0]) + ' | ' + out[-200:])
cs, out = run(click('timeline.key.0.1') + ['--keydown', 'ctrl', '--wait', '2'] + click('timeline.key.1.1') +
              ['--keyup', 'ctrl', '--wait', '2'] + click('timeline.delkeys'))
check('with two keys selected the row\'s x deletes both',
      [[k['frame'] for k in t['keyframes']] for t in cs['tracks']] == [[0], [0]], str(cs['tracks']) + ' | ' + out[-200:])

# (c) Skin clip events. The lane's + adds one at the scrub head (0): Arm's bend.
ADD = click('timeline.addskin') + menu('timeline.addskin/Arm') + click('timeline.addskin/Arm/bend')
cs, out = run(ADD)
check('the skin lane\'s + menu adds an event of the picked clip', sorted(skin(cs)) == sorted(START_SKIN + [(0, 'Arm', 'bend', False)]),
      f'{skin(cs)} | ' + out[-200:])
# The new event stays selected: FRAME moves it to 60.
cs, out = run(ADD + typed('timeline.evframe', '60'))
check('the event row\'s FRAME moves the new skin event', sorted(skin(cs)) == sorted(START_SKIN + [(60, 'Arm', 'bend', False)]),
      f'{skin(cs)} | ' + out[-200:])
# The red event at 90 (clip wave, gone): CLIP > Arm > bend fixes it; one Ctrl+Z puts wave back.
FIX = click('timeline.skin.1') + click('timeline.evclip') + menu('timeline.evclip/Arm') + click('timeline.evclip/Arm/bend')
cs, out = run(FIX)
check('the CLIP picker points the missing-clip event at a clip Arm has',
      skin(cs) == [START_SKIN[0], (90, 'Arm', 'bend', False)], f'{skin(cs)} | ' + out[-200:])
cs, out = run(FIX + SAVE_MID + UNDO)
check('one Ctrl+Z puts the old clip back', mid and skin(mid)[1] == (90, 'Arm', 'bend', False) and skin(cs) == START_SKIN,
      f'{mid and skin(mid)} then {skin(cs)} | ' + out[-200:])
cs, out = run(click('timeline.skin.0') + click('timeline.evloop'))
check('the LOOP toggle loops the skin event', skin(cs) == [(30, 'Arm', 'raise', True), START_SKIN[1]], f'{skin(cs)} | ' + out[-200:])
cs, out = run(click('timeline.skin.0') + click('timeline.delev'))
check('the event row\'s x deletes the skin event', skin(cs) == [START_SKIN[1]], f'{skin(cs)} | ' + out[-200:])


# Audio events: frame, clip, volume and pan from the same row.
def audio(cs):
    return [(a['frame'], a['clip'], a['volume'], a['pan']) for a in cs['audioEvents']]


cs, out = run(click('timeline.audio.0') + typed('timeline.evframe', '45') + click('timeline.evclip') + click('timeline.evclip/gong') +
              typed('timeline.evvol', '50') + typed('timeline.evpan', '10'))
check('the audio event row sets frame, clip, volume and pan', audio(cs) == [(45, 'gong', 50, 10)], f'{audio(cs)} | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
