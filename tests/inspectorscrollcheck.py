# The object inspector scrolls when an object's components make it taller
# than the panel. Two objects on a copy of the courtyard get a spot Light,
# an Interactable and an Audio clip; Add component (obj.addcomp) starts
# below the panel, the wheel over the inspector brings it into view and a
# click on it opens the picker, and selecting the other object, as tall,
# starts at the top. Rects come from the editor's list of named
# controls, as the last frame drew them.
# usage: python3 inspectorscrollcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
H = 960


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    comps = [{'type': 'light', 'kind': 'spot', 'color': [255, 150, 64], 'intensity': 1, 'range': 9, 'spotAngle': 40, 'innerSpotAngle': 0},
             {'type': 'interactable', 'radius': 2, 'button': 14, 'repeatable': True, 'cooldownFrames': 30, 'showPrompt': False,
              'promptCanvas': '', 'lineOfSight': False, 'facingAngle': 90},
             {'type': 'audio', 'clip': None, 'clipName': 'bell', 'sampleRate': 22050, 'loop': False, 'defaultVolume': 100,
              'trimLeadingSilence': False}]
    for o in s['objects']:
        if o['name'] == 'Torch Light':
            for c in o['components']:
                if c['type'] == 'light':
                    comps[0] = dict(c, kind='spot', spotAngle=40, innerSpotAngle=0)
    for name in ('Crowded', 'Crowded 2'):
        s['objects'].append({'name': name, 'active': True, 'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1],
                             'scale': [1, 1, 1]}, 'components': [dict(c) for c in comps], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(args):
    """Runs the editor with Crowded selected, then `args`; returns the
    output lines and obj.addcomp's rect at the end (None if never drawn)."""
    project = project_copy()
    cmd = [editor, '--size', '1600x%d' % H, '--project', project, '--select', 'Crowded', '--wait', '5'] + args + [
        '--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check('every control the script names was drawn', not missing, missing)
    rect = None
    for l in lines:
        m = re.match(r'control obj\.addcomp at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m:
            rect = tuple(int(v) for v in m.groups())
    return lines, rect


WHEEL = ['--wheel', '1420,500,-10', '--wait', '3']

lines, rect = run([])
check('Add component starts below the panel (the object overflows it)', rect is not None and rect[3] > H - 40, rect)

lines, rect = run(WHEEL + ['--click', '@obj.addcomp', '--wait', '3'])
check('the wheel brings Add component into view', rect is not None and rect[1] > 80 and rect[3] < H - 40, rect)
check('a click on it then opens the picker', 'popups open: yes' in lines, [l for l in lines if l.startswith('popups')])

# Crowded 2 is as tall as Crowded, so a scroll carried over would leave its Add component in view.
lines, rect = run(WHEEL + ['--click', '@tree.Crowded 2', '--wait', '3'])
check('inspecting another object starts at the top', rect is not None and rect[3] > H - 40, rect)

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
