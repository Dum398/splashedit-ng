# Paste offset: Ctrl+C then Ctrl+V puts the copy one grid step (0.25 m) along
# X from the original instead of on top of it, each further paste one step
# more; Ctrl+X then Ctrl+V moves the object, so it stays where it was; Ctrl+Z
# takes a paste back in one step. Drives the editor headless on copies of the
# courtyard example and reads the saved scene.
# usage: python3 pasteoffsetcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
STEP = 0.25


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(select, keys, shot=None):
    """Selects `select`, presses `keys`, then Ctrl+S; returns the saved
    scene's top-level objects and the editor's output."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    args = ['--select', select]
    for k in keys + ['ctrl+s']:
        args += ['--key', k, '--wait', '3']
    cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '10', '--screenshot',
                                                                         shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    out = (r.stdout + r.stderr).strip()
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return scene['objects'], out


def find(objects, name):
    for o in objects:
        if o['name'] == name: return o
        f = find(o.get('children', []), name)
        if f: return f
    return None


def pos(objects, name):
    o = find(objects, name)
    return o['transform']['position'] if o else None


def close(a, b):
    return a is not None and b is not None and all(abs(x - y) < 1e-4 for x, y in zip(a, b))


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))['objects']
crate = pos(original, 'Crate')
group = pos(original, 'Crates')
assert crate and group and not find(original, 'Crate (4)')

# A mesh object: three pastes, three places, none on the original.
objs, out = run('Crate', ['ctrl+c', 'ctrl+v', 'ctrl+v', 'ctrl+v'], keep)
copies = [pos(objs, f'Crate ({n})') for n in (4, 5, 6)]
check('three pastes make three copies', all(copies), str(copies) + ' | ' + out[-200:])
check('the original stays where it was', close(pos(objs, 'Crate'), crate), pos(objs, 'Crate'))
if all(copies):
    want = [[crate[0] + STEP * k, crate[1], crate[2]] for k in (1, 2, 3)]
    check('each paste lands one grid step further along X', all(close(c, w) for c, w in zip(copies, want)),
          f'{copies} want {want}')
    places = [tuple(round(v, 4) for v in p) for p in copies + [crate]]
    check('all four are at different places', len(set(places)) == 4, places)

# A group: the copy moves with its children.
objs, out = run('Crates', ['ctrl+c', 'ctrl+v'])
check('a pasted group steps off the original', close(pos(objs, 'Crates (2)'), [group[0] + STEP, group[1], group[2]]),
      str(pos(objs, 'Crates (2)')) + ' | ' + out[-200:])
copy = find(objs, 'Crates (2)')
check('its children keep their places in it', copy and [c['transform']['position'] for c in copy['children']] ==
      [c['transform']['position'] for c in find(original, 'Crates')['children']], copy and copy['children'])

# Cut then paste moves it: nothing left to step off, the place is kept.
objs, out = run('Crates', ['ctrl+x', 'ctrl+v'])
check('cut then paste keeps the place', close(pos(objs, 'Crates'), group) and not find(objs, 'Crates (2)'),
      str(pos(objs, 'Crates')) + ' | ' + out[-200:])

# Undo takes the paste back in one step.
objs, out = run('Crate', ['ctrl+c', 'ctrl+v', 'ctrl+z'])
check('ctrl+z takes the paste back', not find(objs, 'Crate (4)') and close(pos(objs, 'Crate'), crate), out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
