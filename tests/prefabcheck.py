# Prefabs, driven through the GUI on copies of the courtyard example: a
# right-click on the Crates row > Create Prefab writes prefabs/Crates.prefab
# and makes Crates its instance (one Ctrl+Z turns it back, the file stays);
# the .prefab dragged from the Project panel onto the viewport places a second
# instance, "Crates (2)"; the inspector's Unpack turns the first back into
# plain objects; its Open edits the prefab (prefab mode: a crate deleted,
# Ctrl+S); Back returns to the scene, where the remaining instance shows the
# edit and the unpacked copy keeps its three crates. The saved scene holds the
# instance as a link only. Controls are clicked by name (--click @obj.prefabopen),
# the drag and the search field by position (the Project panel at its default
# height, as assetbrowsercheck.py has them).
# usage: python3 prefabcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shotOut = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# At 1600x960 with the Project panel at its default height.
SEARCH = '880,731'      # the panel's search field
TILE = '530,812'        # the first file in the grid
VIEWPORT = '760,520'    # the courtyard floor in the Scene view
PREFAB = 'prefabs/Crates.prefab'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def tap(name, button=None):
    """Scrolls an inspector control into view first, then clicks it."""
    pre = ['--scroll-to', '@' + name] if name.startswith('obj.') else []
    target = '@' + name + (',0,0,%d' % button if button is not None else '')
    return pre + ['--click', target, '--wait', '6']


CREATE = tap('tree.Crates', 1) + tap('treemenu/Create Prefab') + ['--wait', '10']
PLACE = ['--click', SEARCH + ',0', '--text', 'Crates', '--wait', '30', '--drag', TILE + ',' + VIEWPORT + ',0', '--wait', '10']
UNPACK = tap('tree.Crates') + tap('obj.prefabunpack')
OPEN = tap('tree.Crates (2)') + tap('obj.prefabopen') + ['--wait', '6']
EDIT = tap('tree.Crate (3)') + ['--key', 'delete', '--wait', '4', '--key', 'ctrl+s', '--wait', '10']
BACK = tap('prefab.back') + ['--wait', '10']


def run(args, shot=None):
    """Runs `args` on a fresh courtyard copy, then Ctrl+S; returns the project
    and the editor's output without its list of controls."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '20'] + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    out = (r.stdout + r.stderr).strip()
    lines = out.splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return project, '\n'.join(l for l in lines if not l.startswith('control '))


def load(project, rel):
    p = os.path.join(project, rel)
    return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else None


def find(objects, name):
    for o in objects:
        if o['name'] == name:
            return o
    return None


def kids(o):
    return [c['name'] for c in (o or {}).get('children', [])]


# Create Prefab, then one Ctrl+Z: the object is plain again and the file stays.
project, out = run(CREATE + ['--key', 'ctrl+z', '--wait', '6'])
sc = load(project, 'courtyard.scene')
crates = find(sc['objects'], 'Crates')
check('Create Prefab reports the file', 'prefab: created ' + PREFAB in out, out[-400:])
check('the prefab file holds Crates and its three crates',
      kids((load(project, PREFAB) or {}).get('root')) == ['Crate', 'Crate (2)', 'Crate (3)'], load(project, PREFAB))
check('one Ctrl+Z turns the instance back into Crates with its children',
      crates and 'prefab' not in crates and kids(crates) == ['Crate', 'Crate (2)', 'Crate (3)'], crates)

# The whole round: create, place a second, unpack the first, edit the prefab, back.
project, out = run(CREATE + PLACE + UNPACK + OPEN + EDIT + BACK, shot=shotOut)
check('Create Prefab reports the file', 'prefab: created ' + PREFAB in out, out[-400:])
check('dragging the .prefab onto the viewport places "Crates (2)"', 'prefab: placed Crates (2) from ' + PREFAB in out, out[-400:])
check('Unpack reports Crates', 'prefab: unpacked Crates' in out, out[-400:])
check('Open enters prefab mode', 'prefab: opened ' + PREFAB in out and 'prefabs: editing ' + PREFAB in out, out[-400:])
check('Back returns to the scene', 'prefab: back to courtyard' in out, out[-400:])
states = re.findall(r'^prefabs: (.*)$', out, re.M)
before = [i for i, l in enumerate(states) if 'Crates (2)=' + PREFAB + ':3' in l]
after = [i for i, l in enumerate(states) if l == 'scene courtyard | Crates (2)=' + PREFAB + ':2']
check('the remaining instance showed three crates, then two after Back', before and after and before[0] < after[-1], states)
pf = load(project, PREFAB)
check('the saved prefab lost the deleted crate', kids((pf or {}).get('root')) == ['Crate', 'Crate (2)'], pf)
sc = load(project, 'courtyard.scene')
crates, second = find(sc['objects'], 'Crates'), find(sc['objects'], 'Crates (2)')
check('the unpacked Crates is plain and keeps its three crates',
      crates and 'prefab' not in crates and kids(crates) == ['Crate', 'Crate (2)', 'Crate (3)'], crates)
check('the scene saves the instance as a link only',
      second and second.get('prefab') == PREFAB and 'children' not in second and 'components' not in second, second)
check('no problems after Back', re.findall(r'^problems: (.*)$', out, re.M)[-1:] == ['No problems'],
      re.findall(r'^problems: (.*)$', out, re.M)[-3:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
