# Timeline edits beyond keys: rename a cutscene (in the scene inspector and
# from the timeline's sequence picker) with a taken name refused, delete one
# and undo it, point a track at another object from its lane menu, and the
# Scene view previewing light and UV offset tracks, and a skin clip event whose
# clip is gone drawn red in its lane beside a good one. Each run saves with
# Ctrl+S and the check reads the saved scene; the previews compare Scene
# view pixels at two scrub frames and must leave the scene file alone.
# usage: python3 timelineeditcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile, zlib

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')

# Layout at 1600x960 with the timeline open (as timelinecheck.py): frame f
# on the ruler at x 532 + f * 4.4267, lanes 40 px from y 680.
def fx(frame):
    return round(532 + frame * (1224 - 532 - 28) / 150)

RULER_Y = 668
SCENE_VIEW = (290, 50, 1230, 595)
INSPECTOR_NAME = '1301,301'      # the scene inspector's first sequence name ("intro")
INSPECTOR_DELETE = '1452,309'    # its delete button
PICKER = '560,630'               # the timeline's sequence dropdown (right of the Project | Timeline tabs)
PICKER_RENAME = '540,581'        # its Rename... (the popup opens upwards)
LANE0_MENU = '478,691'           # the first lane's ... button
DRIVE_INSTEAD = '345,761'        # the lane menu's Drive instead
DRIVE_CRATE2 = '435,894'         # Crate (2) in its submenu


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def key(frame, value):
    return {'frame': frame, 'value': value}


def project_copy(tracks=None, skin=None):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    if skin is not None:
        # tests/skin's arm, with its clips bend and raise, and `skin` as intro's skin events.
        for f in ('arm.mesh', 'bend.anim', 'raise.anim'):
            shutil.copy(os.path.join(here, 'skin', f), project)
        s['objects'].append(json.load(open(os.path.join(here, 'skin', 'skin.scene'), encoding='utf-8'))['objects'][0])
    for o in s['objects']:
        if o['name'] == 'Torch Light':
            o['components'][0]['runtime'] = True  # light tracks drive runtime point lights
    if tracks is None:
        tracks = [{'type': 'objectPosition', 'target': 'Crate', 'keyframes': [key(0, [0, 0.5, 1]), key(75, [0, 3, 1])]},
                  {'type': 'cameraPosition', 'target': '', 'keyframes': [key(0, [0, 3, -10])]}]
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 150, 'audioEvents': [], 'tracks': tracks},
                      {'name': 'outro', 'durationFrames': 90, 'audioEvents': [], 'tracks': []}]
    if skin is not None:
        s['cutscenes'][0]['skinEvents'] = skin
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


LOGIC = os.path.join('scripts', 'game_logic.lua')  # Game Logic's script
script_after = None  # with `lua`: the script as the run left it


def run(args, tracks=None, save=True, lua=None, skin=None):
    """Runs the editor on a fresh copy with `args` (then Ctrl+S when `save`);
    returns the scene file's bytes, the screenshot path and the output. With
    `lua`, Game Logic's script gets it appended first and script_after its
    text after the run."""
    global script_after
    project = project_copy(tracks, skin)
    if lua is not None:
        with open(os.path.join(project, LOGIC), 'a', encoding='utf-8', newline='') as f:
            f.write(lua)
    shot = os.path.join(project, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', project] + args
    if save:
        cmd += ['--key', 'ctrl+s', '--wait', '10']
    cmd += ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    data = open(os.path.join(project, 'courtyard.scene'), 'rb').read()
    if lua is not None:
        script_after = open(os.path.join(project, LOGIC), encoding='utf-8', newline='').read()
    return data, shot, (r.stdout + r.stderr).strip()


def names(data):
    return [c['name'] for c in json.loads(data).get('cutscenes', [])]


def read_png(path, rows=None):
    """RGB rows of an 8-bit RGB/RGBA PNG (no interlace), the first `rows` only."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, [], 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and not interlace, (depth, ctype, interlace)
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out, prev = [], bytearray(stride)
    for y in range(min(h, rows or h)):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line) if bpp == 3 else bytes(b for i, b in enumerate(line) if i % 4 != 3))
        prev = line
    return w, out


def differing(a, b, box):
    """Pixels inside `box` (x0, y0, x1, y1) whose RGB differs by more than 24 in sum."""
    x0, y0, x1, y1 = box
    _, ra = read_png(a, y1)
    _, rb = read_png(b, y1)
    n = 0
    for y in range(y0, y1):
        la, lb = ra[y], rb[y]
        for x in range(x0, x1):
            i = 3 * x
            if abs(la[i] - lb[i]) + abs(la[i + 1] - lb[i + 1]) + abs(la[i + 2] - lb[i + 2]) > 24:
                n += 1
    return n


def rename_in_inspector(name):
    return ['--dblclick', INSPECTOR_NAME, '--wait', '3', '--key', 'ctrl+a', '--text', name, '--key', 'enter', '--wait', '3']


# Rename in the scene inspector: double-click the name, type, Enter.
saved, _, out = run(rename_in_inspector('opening'))
check('the inspector renames a cutscene', names(saved) == ['opening', 'outro'], str(names(saved)) + ' | ' + out[-200:])
# One undo step: Ctrl+Z puts the old name back.
saved, _, out = run(rename_in_inspector('opening') + ['--key', 'ctrl+z', '--wait', '3'])
check('undo takes the rename back', names(saved) == ['intro', 'outro'], str(names(saved)) + ' | ' + out[-200:])
# A name the other cutscene has is refused; so is one past the runtime's 24 characters.
saved, _, out = run(rename_in_inspector('outro'))
check('a taken name is refused', names(saved) == ['intro', 'outro'], str(names(saved)) + ' | ' + out[-200:])
saved, _, out = run(rename_in_inspector('x' * 25))
check('a name past 24 characters is refused', names(saved) == ['intro', 'outro'], str(names(saved)) + ' | ' + out[-200:])

# Scripts play it by name: the rename rewrites Cutscene.Play("intro") in the
# scene's scripts on disk (not "outro", not Animation.Play), and the same
# Ctrl+Z puts both back.
PLAYS = '\nCutscene.Play("intro")\nCutscene.Play("outro")\nAnimation.Play("intro")\n'
saved, _, out = run(rename_in_inspector('opening'), lua=PLAYS)
check('the rename updates the scripts that play it', names(saved) == ['opening', 'outro'] and
      script_after.endswith('\nCutscene.Play("opening")\nCutscene.Play("outro")\nAnimation.Play("intro")\n'),
      str(names(saved)) + ' | ' + script_after[-120:] + ' | ' + out[-200:])
saved, _, out = run(rename_in_inspector('opening') + ['--key', 'ctrl+z', '--wait', '3'], lua=PLAYS)
check('undo puts the scripts back with the name', names(saved) == ['intro', 'outro'] and script_after.endswith(PLAYS),
      str(names(saved)) + ' | ' + script_after[-120:] + ' | ' + out[-200:])

# Rename from the timeline's sequence picker.
saved, _, out = run(['--timeline', 'intro', '--click', PICKER, '--wait', '3', '--click', PICKER_RENAME, '--wait', '3', '--key', 'ctrl+a',
                     '--text', 'opening', '--key', 'enter', '--wait', '3'])
check('the timeline\'s picker renames the open cutscene', names(saved) == ['opening', 'outro'], str(names(saved)) + ' | ' + out[-200:])

# Delete from the inspector, then undo.
saved, _, out = run(['--click', INSPECTOR_DELETE, '--wait', '3'])
check('the inspector deletes a cutscene', names(saved) == ['outro'], str(names(saved)) + ' | ' + out[-200:])
saved, _, out = run(['--click', INSPECTOR_DELETE, '--wait', '3', '--key', 'ctrl+z', '--wait', '3'])
check('undo brings the deleted cutscene back in place', names(saved) == ['intro', 'outro'] and
      json.loads(saved)['cutscenes'][0]['tracks'][0]['target'] == 'Crate', str(names(saved)) + ' | ' + out[-200:])

# Retarget the first lane (Crate: Position) to Crate (2) from its menu.
retarget = ['--timeline', 'intro', '--click', LANE0_MENU, '--wait', '3', '--move', DRIVE_INSTEAD, '--wait', '3', '--move',
            f'{int(DRIVE_INSTEAD.split(",")[0]) + 5},{DRIVE_INSTEAD.split(",")[1]}', '--wait', '3', '--click', DRIVE_CRATE2, '--wait', '3']
saved, _, out = run(retarget)
t = json.loads(saved)['cutscenes'][0]['tracks'][0]
check('the lane menu points a track at another object, keys kept',
      t['target'] == 'Crate (2)' and [k['frame'] for k in t['keyframes']] == [0, 75], str(t)[:200] + ' | ' + out[-200:])
saved, _, out = run(retarget + ['--key', 'ctrl+z', '--wait', '3'])
check('undo points it back', json.loads(saved)['cutscenes'][0]['tracks'][0]['target'] == 'Crate', out[-200:])


# Previews: frame 0 against frame 75, the scene file untouched.
def scrub(frame):
    return ['--timeline', 'intro', '--click', f'{fx(frame)},{RULER_Y}', '--wait', '3']


for what, tracks, least in (
        ('a light track', [{'type': 'lightColor', 'target': 'Torch Light', 'keyframes': [key(0, [1, 0.59, 0.25]), key(75, [0.1, 0.3, 1])]},
                           {'type': 'lightIntensity', 'target': 'Torch Light', 'keyframes': [key(0, [1.05, 0, 0]), key(75, [4, 0, 0])]}], 1500),
        ('a UV offset track', [{'type': 'objectUVOffset', 'target': 'Crate', 'keyframes': [key(0, [0, 0, 0]), key(75, [32, 32, 0])]}], 150)):
    untouched = open(os.path.join(project_copy(tracks), 'courtyard.scene'), 'rb').read()
    d0, shot0, _ = run(scrub(0), tracks, save=False)
    d75, shot75, out75 = run(scrub(75), tracks, save=False)
    n = differing(shot0, shot75, SCENE_VIEW)
    check(f'the Scene view previews {what}', n > least, f'{n} pixels differ | ' + out75[-200:])
    check(f'previewing {what} leaves the scene file alone', d0 == untouched and d75 == untouched)

# The skin lane (after the two tracks and the audio lane, y 800..840): the
# event at 30 plays Arm's raise, the one at 90 a clip Arm does not have. Under
# each tab, its stem and foot take the event's colour: blue, and red (missing).
def stem_colours(path, frame, y0=822, y1=838):
    _, rows = read_png(path, y1)
    red = blue = 0
    for y in range(y0, y1):
        for x in range(fx(frame) - 3, fx(frame) + 4):
            r, g, b = rows[y][3 * x:3 * x + 3]
            red += r > b + 60 and r > g + 60
            blue += b > r + 60
    return red, blue


_, shot, out = run(['--timeline', 'intro', '--wait', '3'], save=False,
                   skin=[{'frame': 30, 'object': 'Arm', 'clip': 'raise', 'loop': False},
                         {'frame': 90, 'object': 'Arm', 'clip': 'wave', 'loop': False}])
good, missing = stem_colours(shot, 30), stem_colours(shot, 90)
check('a skin event whose clip is gone draws red, a good one does not', good[1] >= 8 and good[0] == 0 and missing[0] >= 8 and
      missing[1] == 0, f'good (red, blue) {good}, missing {missing} | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
