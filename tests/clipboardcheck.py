# Copy, Cut and Paste of objects: Ctrl+C / Ctrl+X put the selected object on
# the system clipboard, Ctrl+V pastes it right after the selection, children
# included, a taken name numbered the way Duplicate numbers it. Drives the
# editor headless on copies of the courtyard example and reads the saved scene.
# usage: python3 clipboardcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
GROUP = 'Crates'  # a top-level group with three children


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(keys):
    """Selects GROUP, presses `keys`, then Ctrl+S; returns the saved scene's
    top-level objects and the editor's output."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    args = ['--select', GROUP]
    for k in keys + ['ctrl+s']:
        args += ['--key', k, '--wait', '3']
    cmd = [editor, '--size', '1600x960', '--project', project] + args + ['--wait', '10', '--screenshot',
                                                                         os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    out = (r.stdout + r.stderr).strip()
    scene = json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))
    return scene['objects'], out


def names(objects):
    return [o['name'] for o in objects]


def children(objects, name):
    return [names(o['children']) for o in objects if o['name'] == name]


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))['objects']
assert GROUP in names(original) and f'{GROUP} (2)' not in names(original)
kids = children(original, GROUP)[0]

# Saving alone changes nothing, so a change below is the keys' doing.
objs, out = run([])
check('save alone keeps the top level as it was', names(objs) == names(original), str(names(objs)) + ' | ' + out[-200:])

# Copy then Paste: a numbered copy right after the original, children included.
objs, out = run(['ctrl+c', 'ctrl+v'])
n = names(objs)
check(f'ctrl+c, ctrl+v adds "{GROUP} (2)" once', n.count(f'{GROUP} (2)') == 1 and n.count(GROUP) == 1, str(n) + ' | ' + out[-200:])
check('the copy is pasted right after the original',
      GROUP in n and n.index(GROUP) + 1 < len(n) and n[n.index(GROUP) + 1] == f'{GROUP} (2)', str(n))
check('the copy carries the children', children(objs, f'{GROUP} (2)') == [kids], str(children(objs, f'{GROUP} (2)')))
check('the original keeps its children', children(objs, GROUP) == [kids], str(children(objs, GROUP)))

# Pasting twice numbers on.
objs, out = run(['ctrl+c', 'ctrl+v', 'ctrl+v'])
n = names(objs)
check('a second ctrl+v adds a third numbered copy', n.count(f'{GROUP} (2)') == 1 and n.count(f'{GROUP} (3)') == 1,
      str(n) + ' | ' + out[-200:])

# Cut alone removes the object; Cut then Paste brings exactly one back.
objs, out = run(['ctrl+x'])
n = names(objs)
check('ctrl+x removes the object', GROUP not in n and len(n) == len(original) - 1, str(n) + ' | ' + out[-200:])

objs, out = run(['ctrl+x', 'ctrl+v'])
n = names(objs)
check('ctrl+x, ctrl+v keeps exactly one of it', n.count(GROUP) == 1 and not any(x.startswith(GROUP + ' (') for x in n),
      str(n) + ' | ' + out[-200:])
check('the pasted object keeps its children', children(objs, GROUP) == [kids], str(children(objs, GROUP)))

# Undo takes a paste back in one step.
objs, out = run(['ctrl+c', 'ctrl+v', 'ctrl+z'])
check('ctrl+z takes the paste back', names(objs) == names(original), str(names(objs)) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
