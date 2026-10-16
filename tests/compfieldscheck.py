# The object inspector's component fields that only the scene file used to
# set, driven through the GUI: Interactable's button, Repeatable, Cooldown,
# Show prompt and prompt canvas; Player's sprint speed, gravity, step height,
# max slope and nav cell size/height (and Navigation's, which reach the
# bake through splashpack-cli); Audio's name, sample rate, Loop and Trim silence; Mesh's
# Smooth normals; a spot Light's inner angle. Each run selects an object,
# edits, saves with Ctrl+S and reads the saved scene; the undo arms take one
# edit back with one Ctrl+Z. Controls are clicked by name (--click
# @obj.irepeat): the editor aims at where the frame before drew them, so no
# pixel positions live here.
# usage: python3 compfieldscheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, struct, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def obj(name, components):
    return {'name': name, 'active': True, 'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
            'components': components, 'children': []}


def project_copy():
    """The courtyard plus what it lacks: a Lever (Interactable), a Bell
    (Audio), two UI canvases for the prompt, and the Torch Light as a spot."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append(obj('Lever', [{'type': 'interactable', 'radius': 2, 'button': 14, 'repeatable': True, 'cooldownFrames': 30,
                                       'showPrompt': False, 'promptCanvas': '', 'lineOfSight': False, 'facingAngle': 90}]))
    s['objects'].append(obj('Bell', [{'type': 'audio', 'clip': None, 'clipName': 'bell', 'sampleRate': 22050, 'loop': False,
                                      'defaultVolume': 100, 'trimLeadingSilence': False}]))
    s['objects'].append(obj('Nav Settings', [{'type': 'navigation', 'agentHeight': 1.6, 'agentRadius': 0.3}]))
    s['canvases'] = [{'name': 'Hud', 'elements': []}, {'name': 'Hint', 'elements': []}]
    for o in s['objects']:
        if o['name'] == 'Torch Light':
            for c in o['components']:
                if c['type'] == 'light':
                    c.update(kind='spot', spotAngle=40, innerSpotAngle=0)
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


lines_all = []
mid = None  # with SAVE_MID in the script: the scene as saved at that point


def run(select, args):
    """Runs the editor on a fresh copy with `select` selected, then `args`
    and Ctrl+S; returns the saved scene and the editor's output (without its
    list of named controls)."""
    global mid
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    mid = json.load(open(snap, encoding='utf-8')) if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check('every control the script names was drawn', not missing, missing)
    global lines_all
    lines_all = lines
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')), out


def comp(scene, name, kind):
    if scene is None:
        return None
    for o in scene['objects']:
        if o['name'] == name:
            for c in o['components']:
                if c['type'] == kind:
                    return c
        for ch in o.get('children', []):
            if ch['name'] == name:
                for c in ch['components']:
                    if c['type'] == kind:
                        return c
    return None


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    """Double-click a number field, replace its text, Enter."""
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
# Saves and keeps a copy of the scene as it is then (in `mid`), so an undo arm sees the edit happen first.
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']


# Interactable. Start: Cross (14), repeatable, 30 frames, no prompt.
def inter(c):
    return c and (c['button'], c['repeatable'], c['cooldownFrames'], c['showPrompt'], c['promptCanvas'])


START_I = (14, True, 30, False, '')
s, out = run('Lever', [])
check('save alone keeps the Lever as it was', inter(comp(s, 'Lever', 'interactable')) == START_I,
      f"{inter(comp(s, 'Lever', 'interactable'))} | " + out[-200:])

INTER = click('obj.ibutton') + click('ibutton/Triangle') + typed('obj.icooldown', '90') + click('obj.iprompt') + \
    click('obj.icanvas') + click('icanvas/Hint') + click('obj.irepeat')
s, out = run('Lever', INTER)
check('button, cooldown, prompt, prompt canvas and repeatable land in the saved scene',
      inter(comp(s, 'Lever', 'interactable')) == (12, False, 90, True, 'Hint'), f"{inter(comp(s, 'Lever', 'interactable'))} | " + out[-200:])
s, out = run('Lever', INTER + SAVE_MID + UNDO)
check('one Ctrl+Z turns Repeatable back on and keeps the rest',
      inter(comp(mid, 'Lever', 'interactable')) == (12, False, 90, True, 'Hint') and
      inter(comp(s, 'Lever', 'interactable')) == (12, True, 90, True, 'Hint'),
      f"{inter(comp(mid, 'Lever', 'interactable'))} then {inter(comp(s, 'Lever', 'interactable'))} | " + out[-200:])
s, out = run('Lever', typed('obj.icooldown', '99999') + click('obj.iprompt') + click('obj.icanvas') + click('icanvas/Hint') +
             click('obj.icanvas') + click('icanvas/None'))
check('cooldown is clamped to 65535 and None clears the prompt canvas',
      inter(comp(s, 'Lever', 'interactable')) == (14, True, 65535, True, ''), f"{inter(comp(s, 'Lever', 'interactable'))} | " + out[-200:])


# Player: sprint speed, gravity, step height, max slope.
def player(c):
    return c and (c['sprintSpeed'], c['gravity'], c['maxStepHeight'], c['walkableSlopeAngle'])


PLAYER = typed('obj.psprint', '6.5') + typed('obj.pgravity', '12') + typed('obj.pstep', '0.5') + typed('obj.pslope', '30')
s, out = run('Player Start', PLAYER)
check('sprint speed, gravity, step height and max slope land in the saved scene',
      player(comp(s, 'Player Start', 'player')) == (6.5, 12, 0.5, 30), f"{player(comp(s, 'Player Start', 'player'))} | " + out[-200:])
s, out = run('Player Start', PLAYER + SAVE_MID + UNDO + typed('obj.pgravity', '-3'))
check('one Ctrl+Z puts the max slope back; gravity is clamped to 0',
      player(comp(mid, 'Player Start', 'player')) == (6.5, 12, 0.5, 30) and
      player(comp(s, 'Player Start', 'player')) == (6.5, 0, 0.5, 46),
      f"{player(comp(mid, 'Player Start', 'player'))} then {player(comp(s, 'Player Start', 'player'))} | " + out[-200:])


# Nav voxel size, on the Player and on a Navigation component (which the bake
# takes whole when the scene has one). The caption beside the rows gives the
# bake's voxel counts, in float32 like the editor and core/navregion.cpp.
def f32(v):
    return struct.unpack('f', struct.pack('f', v))[0]


def voxels(who, height, radius, step, cs, ch):
    import math
    h, r, st, cs, ch = map(f32, (height, radius, step, cs, ch))
    return f'{who} voxels: height {math.ceil(h / ch)} radius {math.ceil(r / cs)} climb {math.floor(st / ch)}'


def cells(c):
    return c and (c['maxStepHeight'], c['walkableSlopeAngle'], c['navCellSize'], c['navCellHeight'])


cli = os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli' + ('.exe' if editor.endswith('.exe') else ''))


def asked(scene_json):
    """Exports a saved scene with splashpack-cli and returns the cell size the
    nav bake was asked for, off its --stats line."""
    d = tempfile.mkdtemp()
    project = os.path.join(d, 'courtyard')
    shutil.copytree(example, project)
    path = os.path.join(project, 'courtyard.scene')
    json.dump(scene_json, open(path, 'w', encoding='utf-8'), indent=2)
    r = subprocess.run([cli, 'export', path, '-o', os.path.join(d, 'out.splashpack'), '--stats'], capture_output=True, text=True,
                       timeout=300)
    for l in r.stdout.splitlines():
        if l.startswith('nav regions=') and '(asked ' in l:
            return l.split('(asked ')[1].split(')')[0]
    return 'no nav line: ' + (r.stdout + r.stderr)[-200:]


s, out = run('Player Start', [])
check('the Player shows its voxel counts at open', voxels('player', 1.8, 0.5, 0.35, 0.05, 0.025) in out, out[-300:])
check('the voxel caption is drawn', 'control obj.pvoxels at ' in '\n'.join(lines_all), '')
s, out = run('Player Start', ['--dblclick', '@obj.pcellh', '--wait', '3', '--key', 'enter', '--wait', '3'])
check('Enter on the Cell height field keeps 0.025 (shown to 3 decimals)',
      cells(comp(s, 'Player Start', 'player')) == (0.35, 46, 0.05, 0.025), f"{cells(comp(s, 'Player Start', 'player'))} | " + out[-200:])
PCELL = typed('obj.pstep', '0.4') + typed('obj.pcell', '0.1') + typed('obj.pcellh', '0.05')
s, out = run('Player Start', PCELL)
check('step height, cell size and cell height land in the saved Player',
      cells(comp(s, 'Player Start', 'player')) == (0.4, 46, 0.1, 0.05), f"{cells(comp(s, 'Player Start', 'player'))} | " + out[-200:])
check('the voxel counts follow the cells', voxels('player', 1.8, 0.5, 0.4, 0.1, 0.05) in out, out[-300:])
s, out = run('Player Start', PCELL + SAVE_MID + UNDO + typed('obj.pcell', '0'))
check('one Ctrl+Z puts the cell height back; cell size is clamped to 0.01',
      cells(comp(mid, 'Player Start', 'player')) == (0.4, 46, 0.1, 0.05) and
      cells(comp(s, 'Player Start', 'player')) == (0.4, 46, 0.01, 0.025),
      f"{cells(comp(mid, 'Player Start', 'player'))} then {cells(comp(s, 'Player Start', 'player'))} | " + out[-200:])
NCELL = typed('obj.nstep', '0.3') + typed('obj.nslope', '35') + typed('obj.ncell', '0.2') + typed('obj.ncellh', '0.04')
s, out = run('Nav Settings', NCELL)
check('Navigation step height, max slope, cell size and cell height land in the saved scene',
      cells(comp(s, 'Nav Settings', 'navigation')) == (0.3, 35, 0.2, 0.04), f"{cells(comp(s, 'Nav Settings', 'navigation'))} | " + out[-200:])
check('the Player keeps its own settings', cells(comp(s, 'Player Start', 'player')) == (0.35, 46, 0.05, 0.025),
      str(cells(comp(s, 'Player Start', 'player'))))
check('the Navigation voxel counts use its agent', voxels('navigation', 1.6, 0.3, 0.3, 0.2, 0.04) in out, out[-300:])
check('the bake is asked for the Navigation cell size', asked(s) == '0.2', asked(s))
s['objects'] = [o for o in s['objects'] if o['name'] != 'Nav Settings']
for o in s['objects']:
    for c in o['components']:
        if c['type'] == 'player':
            c['navCellSize'] = 0.08
check('without a Navigation component the bake is asked for the Player cell size', asked(s) == '0.08', asked(s))


# Audio: name, sample rate, loop, trim silence.
def audio(c):
    return c and (c['clipName'], c['sampleRate'], c['loop'], c['trimLeadingSilence'], c['defaultVolume'])


NAME = ['--click', '@obj.aname', '--wait', '3', '--key', 'ctrl+a', '--text', 'chime', '--key', 'enter', '--wait', '3']
AUDIO = NAME + typed('obj.arate', '11025') + click('obj.aloop') + click('obj.atrim')
s, out = run('Bell', AUDIO)
check('name, sample rate, loop and trim silence land in the saved scene',
      audio(comp(s, 'Bell', 'audio')) == ('chime', 11025, True, True, 100), f"{audio(comp(s, 'Bell', 'audio'))} | " + out[-200:])
s, out = run('Bell', NAME + SAVE_MID + UNDO + typed('obj.arate', '100'))
check('one Ctrl+Z takes the typed name back; sample rate is clamped to 4000',
      audio(comp(mid, 'Bell', 'audio'))[0] == 'chime' and audio(comp(s, 'Bell', 'audio')) == ('bell', 4000, False, False, 100),
      f"{audio(comp(mid, 'Bell', 'audio'))} then {audio(comp(s, 'Bell', 'audio'))} | " + out[-200:])

# Mesh: Smooth normals (on by default) and a spot light's inner angle, clamped to the spot angle.
s, out = run('Crate', click('obj.smooth'))
check('Smooth normals turns off', comp(s, 'Crate', 'mesh')['smoothNormals'] is False, str(comp(s, 'Crate', 'mesh')) + ' | ' + out[-200:])
s, out = run('Torch Light', typed('obj.linner', '25'))
check('the spot light\'s inner angle lands in the saved scene', comp(s, 'Torch Light', 'light')['innerSpotAngle'] == 25,
      str(comp(s, 'Torch Light', 'light')) + ' | ' + out[-200:])
s, out = run('Torch Light', typed('obj.linner', '90'))
check('the inner angle is clamped to the spot angle', comp(s, 'Torch Light', 'light')['innerSpotAngle'] == 40,
      str(comp(s, 'Torch Light', 'light')) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
