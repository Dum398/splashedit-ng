# Timeline panel: scrub the ruler, double-click a lane to key it at the scrub
# head, drag a key, Delete a selected key (and not the selected object),
# undo, and open the panel from the scene inspector's Edit in timeline. Each
# run saves with Ctrl+S and the check reads the saved scene. Drives the editor
# headless on copies of the courtyard example with a cutscene added.
# usage: python3 timelinecheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')

# Layout at 1600x960: the panel is 320 px tall under the Scene, lanes 40 px
# from y 680, frame f at x 532 + f * 4.4267 (150 frames fitted).
def fx(frame):
    return round(532 + frame * (1224 - 532 - 28) / 150)

def lane(i):
    return 700 + 40 * i

RULER_Y = 668
EDIT_IN_TIMELINE = '1522,311'  # the scene inspector's first "Edit in timeline"


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def key(frame, value, interp=None):
    k = {'frame': frame, 'value': value}
    if interp:
        k['interp'] = interp
    return k


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['cutscenes'] = [{'name': 'intro', 'durationFrames': 150, 'audioEvents': [], 'tracks': [
        {'type': 'cameraPosition', 'target': '', 'keyframes': [key(0, [0, 3, -10]), key(60, [4, 3, -6], 'easeInOut'), key(150, [6, 2, 0])]},
        {'type': 'cameraRotation', 'target': '', 'keyframes': [key(0, [10, 0, 0]), key(150, [10, -60, 0])]},
        {'type': 'objectPosition', 'target': 'Crate', 'keyframes': [key(0, [0, 0, 0]), key(45, [0, 1.5, 0], 'easeOut'), key(90, [0, 0, 0])]},
    ]}]
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(args):
    """Runs the editor on a fresh copy with `args`, then Ctrl+S; returns the
    saved scene and the editor's output."""
    project = project_copy()
    cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--key', 'ctrl+s', '--wait', '10', '--screenshot',
                                                                         os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return scene, (r.stdout + r.stderr).strip()


def frames(scene, track):
    return [k['frame'] for k in scene['cutscenes'][0]['tracks'][track]['keyframes']]


def names(objs):
    out = []
    for o in objs:
        out.append(o['name'])
        out += names(o.get('children', []))
    return out


# Scrub to 38, double-click the camera rotation lane: a key at 38 with the
# value the track had there (linear 0 -> -60 over 150 frames: -15.2).
scene, out = run(['--timeline', 'intro', '--click', f'{fx(38)},{RULER_Y}', '--wait', '3', '--dblclick', f'{fx(100)},{lane(1)}',
                  '--wait', '3'])
check('double-click a lane keys it at the scrub head', frames(scene, 1) == [0, 38, 150], str(frames(scene, 1)) + ' | ' + out[-200:])
k = next((k for k in scene['cutscenes'][0]['tracks'][1]['keyframes'] if k['frame'] == 38), None)
check('the new key takes the sampled value', k is not None and abs(k['value'][1] + 15.2) < 0.01 and abs(k['value'][0] - 10) < 0.01,
      k)
check('the other lanes are untouched', frames(scene, 0) == [0, 60, 150] and frames(scene, 2) == [0, 45, 90], scene['cutscenes'][0])

# Nothing but the scripted save: the cutscene is as it was.
scene, out = run(['--timeline', 'intro', '--click', f'{fx(38)},{RULER_Y}', '--wait', '3'])
check('scrubbing alone adds no key', frames(scene, 1) == [0, 150], str(frames(scene, 1)) + ' | ' + out[-200:])

# Drag the key at 45 on the Crate lane to 60 (snapped to whole frames).
scene, out = run(['--timeline', 'intro', '--drag', f'{fx(45)},{lane(2)},{fx(60) + 1},{lane(2)}', '--wait', '3'])
check('dragging a key moves it to the frame under the mouse', frames(scene, 2) == [0, 60, 90], str(frames(scene, 2)) + ' | ' + out[-200:])
check('the moved key keeps its value and curve', scene['cutscenes'][0]['tracks'][2]['keyframes'][1].get('interp') == 'easeOut' and
      scene['cutscenes'][0]['tracks'][2]['keyframes'][1]['value'] == [0, 1.5, 0], scene['cutscenes'][0]['tracks'][2])

# Delete removes the selected key while the timeline has focus, and leaves
# the selected object alone.
scene, out = run(['--select', 'Crate', '--timeline', 'intro', '--click', f'{fx(45)},{lane(2)}', '--wait', '3', '--key', 'delete',
                  '--wait', '3'])
check('Delete removes the selected key', frames(scene, 2) == [0, 90], str(frames(scene, 2)) + ' | ' + out[-200:])
check('Delete in the timeline keeps the selected object', 'Crate' in names(scene['objects']), names(scene['objects']))

# Undo takes the added key back.
scene, out = run(['--timeline', 'intro', '--click', f'{fx(38)},{RULER_Y}', '--wait', '3', '--dblclick', f'{fx(100)},{lane(1)}',
                  '--wait', '3', '--key', 'ctrl+z', '--wait', '3'])
check('Ctrl+Z takes the added key back', frames(scene, 1) == [0, 150], str(frames(scene, 1)) + ' | ' + out[-200:])

# The scene inspector's Edit in timeline opens the panel on the cutscene.
scene, out = run(['--click', EDIT_IN_TIMELINE, '--wait', '5', '--click', f'{fx(38)},{RULER_Y}', '--wait', '3', '--dblclick',
                  f'{fx(100)},{lane(1)}', '--wait', '3'])
check('Edit in timeline opens the cutscene in the panel', frames(scene, 1) == [0, 38, 150], str(frames(scene, 1)) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
