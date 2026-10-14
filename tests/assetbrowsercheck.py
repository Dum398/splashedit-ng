# The Project panel: search narrows the files shown; a model or mesh dragged
# from it onto the viewport or a scene tree row becomes an object (one undo
# step); a texture dragged onto the inspector's Texture field is assigned;
# Rename (F2) of a texture the scene uses carries its references along, in
# the open scene and in another scene on disk, and one Ctrl+Z puts it all
# back; a Lua script renamed is followed the same way; a file dragged onto a
# folder of the panel's tree moves there, references along; Delete refuses a
# used file and moves an unused one to the project's .trash folder after the
# prompt, and Ctrl+Z brings it back; opening the project empties deletes
# a month old from .trash. A rename leaves the scene unsaved; a file dropped
# on a folder tile moves into it; View > Project reopens the folded panel.
# The dock's tab strip is the Timeline's 46 px with either in front. Drives the editor headless on copies of
# the courtyard example and reads the saved scenes and the files on disk.
# usage: python3 assetbrowsercheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# At 1600x960 with the Project panel at its default height.
SEARCH = '880,731'      # the panel's search field
TILE = '530,812'        # the first file in the grid
VIEWPORT = '760,520'    # the courtyard floor in the Scene view
CRATES_ROW = '86,335'   # the "Crates" row in the scene tree
TEXTURE_FIELD = '1468,418'  # the inspector's Texture field with a crate selected
MESHES_ROW = '363,802'  # "meshes" in the panel's folder tree
# The bottom dock's tabs: with the Timeline in front (its 320 px panel) and
# with Project in front; the Timeline's ruler and second lane at frame f.
PROJECT_TAB = '334,631'
TIMELINE_TAB = '420,752'  # low in the strip: inside it only at its full 46 px
RULER_Y = 668
# The Project panel's collapse chevron; the View menu and its Panels > Project item for the courtyard.
COLLAPSE = '1222,733'
VIEW_MENU = '322,20'
VIEW_PROJECT = '350,391'
# The root's grid: meshes, scripts, textures, courtyard.scene, then a file.
TEXTURES_TILE = '726,812'
ROOT_FILE_TILE = '922,812'
def fx(frame):
    return round(532 + frame * (1224 - 532 - 28) / 150)
# A one-quad OBJ, never imported yet: the drag imports it.
QUAD_OBJ = 'v -0.5 0 -0.5\nv 0.5 0 -0.5\nv 0.5 0 0.5\nv -0.5 0 0.5\nf 1 2 3 4\n'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy(extra=None):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    for rel, data in (extra or {}).items():
        os.makedirs(os.path.dirname(os.path.join(project, rel)), exist_ok=True)
        open(os.path.join(project, rel), 'wb').write(data)
    return project


def run(project, args, select=None):
    """Runs the editor on `project` with `args`; returns its output and the
    names the panel showed at the end. Sets run.open and run.unsaved from
    the dump (None when it lacks them)."""
    run.open = run.unsaved = None
    cmd = [editor, '--size', '1600x960', '--project', project] + (['--select', select] if select else []) + \
        ['--wait', '5'] + args + ['--wait', '10', '--screenshot', os.path.join(os.path.dirname(project), 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    shown = None
    for line in out.splitlines():
        if line.startswith('project panel open:'):
            run.open = line.split(':', 1)[1].strip() == 'yes'
        if line.startswith('unsaved changes:'):
            run.unsaved = line.split(':', 1)[1].strip() == 'yes'
        if line.startswith('project panel:'):
            shown = [n for n in line[len('project panel:'):].strip().split('|') if n]
    return out, shown


def search(text):
    return ['--click', SEARCH + ',0', '--text', text, '--wait', '5']


def scene(project):
    return json.load(open(os.path.join(project, 'courtyard.scene'), encoding='utf-8'))


def names(objects):
    return [o['name'] for o in objects]


def find(objects, name):
    for o in objects:
        if o['name'] == name:
            return o
        hit = find(o.get('children', []), name)
        if hit:
            return hit
    return None


def comp(obj, kind):
    for c in (obj or {}).get('components', []):
        if c.get('type') == kind:
            return c
    return None


original = scene(example)
top = names(original['objects'])
CUTSCENE = {'name': 'intro', 'durationFrames': 150, 'audioEvents': [], 'tracks': [
    {'type': 'cameraPosition', 'target': '', 'keyframes': [{'frame': 0, 'value': [0, 3, -10]}, {'frame': 150, 'value': [6, 2, 0]}]},
    {'type': 'cameraRotation', 'target': '', 'keyframes': [{'frame': 0, 'value': [10, 0, 0]}, {'frame': 150, 'value': [10, -60, 0]}]}]}
# The scene as the editor saves it untouched: what "unchanged" compares with.
project = project_copy()
run(project, ['--key', 'ctrl+s', '--wait', '5'])
saved = scene(project)

# --- search ---
project = project_copy()
out, shown = run(project, [])
check('the panel opens on the project root', shown == ['meshes', 'scripts', 'textures', 'courtyard.scene'], str(shown) + ' | ' + out[-200:])
out, shown = run(project, search('crate'))
check('searching "crate" lists the crate mesh and texture from every folder', shown == ['crate.mesh', 'crate.png'], str(shown))
out, shown = run(project, search('bri'))
check('searching "bri" lists brick.png only', shown == ['brick.png'], str(shown))
out, shown = run(project, search('zzzz'))
check('a search that matches nothing lists nothing', shown == [], str(shown))

# --- drag a model onto the viewport: imported, placed, one undo ---
project = project_copy({'models/quad.obj': QUAD_OBJ.encode()})
out, shown = run(project, search('quad') + ['--drag', TILE + ',' + VIEWPORT + ',0', '--wait', '10', '--key', 'ctrl+s', '--wait', '5'])
sc = scene(project)
quad = find(sc['objects'], 'quad')
check('the panel lists the mesh the import wrote, within a second', shown == ['quad.mesh', 'quad.obj'], str(shown))
check('dragging quad.obj onto the viewport adds one top-level object "quad"',
      names(sc['objects']) == top + ['quad'], str(names(sc['objects'])) + ' | ' + out[-300:])
check('the new object draws the imported mesh', (comp(quad, 'mesh') or {}).get('mesh') == 'models/quad.mesh', str(quad)[:200])
check('the import wrote its mesh and sidecar', os.path.exists(os.path.join(project, 'models', 'quad.mesh')) and
      os.path.exists(os.path.join(project, 'models', 'quad.obj.import.json')))

project = project_copy({'models/quad.obj': QUAD_OBJ.encode()})
out, shown = run(project, search('quad') + ['--drag', TILE + ',' + VIEWPORT + ',0', '--wait', '10', '--key', 'ctrl+z',
                                            '--key', 'ctrl+s', '--wait', '5'])
check('one ctrl+z takes the dropped model back', names(scene(project)['objects']) == top, str(names(scene(project)['objects'])))

# A .mesh onto a scene tree row: a child drawing it, with the material of the crates already drawing it.
project = project_copy()
out, shown = run(project, search('crate') + ['--drag', TILE + ',' + CRATES_ROW + ',0', '--wait', '10', '--key', 'ctrl+s', '--wait', '5'])
sc = scene(project)
crates = find(sc['objects'], 'Crates')
kids = names(crates['children']) if crates else []
check('dragging crate.mesh onto the Crates row adds a child "crate"', kids[-1:] == ['crate'] and len(kids) == 4,
      str(kids) + ' | ' + out[-300:])
mesh = comp(find(crates['children'], 'crate') if crates else None, 'mesh') or {}
check('the child draws crate.mesh with the crate texture', mesh.get('mesh') == 'meshes/crate.mesh' and
      [m.get('texture') for m in mesh.get('materials', [])] == ['textures/crate.png'], str(mesh)[:200])
check('the top level is unchanged', names(sc['objects']) == top, str(names(sc['objects'])))

# --- drag a texture onto the inspector's Texture field ---
project = project_copy()
out, shown = run(project, search('brick') + ['--drag', TILE + ',' + TEXTURE_FIELD + ',0', '--wait', '10', '--key', 'ctrl+s', '--wait', '5'],
                 select='Crate')
mesh = comp(find(scene(project)['objects'], 'Crate'), 'mesh') or {}
check('dragging brick.png onto the Texture field sets it on the crate',
      [m.get('texture') for m in mesh.get('materials', [])] == ['textures/brick.png'], str(mesh)[:200] + ' | ' + out[-300:])
mesh2 = comp(find(scene(project)['objects'], 'Crate (2)'), 'mesh') or {}
check('the other crates keep theirs', [m.get('texture') for m in mesh2.get('materials', [])] == ['textures/crate.png'], str(mesh2)[:200])

# --- rename ---
def textures_of(sc, objs=('Back Wall', 'Side Wall')):
    return [((comp(find(sc['objects'], n), 'mesh') or {}).get('materials') or [{}])[0].get('texture') for n in objs]


def missing_textures(project, sc):
    return [m['texture'] for o in ('Floor', 'Back Wall', 'Side Wall', 'Pillar', 'Platform', 'Crate')
            for m in (comp(find(sc['objects'], o), 'mesh') or {}).get('materials', [])
            if not os.path.exists(os.path.join(project, m['texture']))]


def level2(project):
    return json.load(open(os.path.join(project, 'level2.scene'), encoding='utf-8'))


# brick.png is used by both walls here and in level2.scene on disk: F2 renames
# it and every reference follows.
example_scene = open(os.path.join(example, 'courtyard.scene'), 'rb').read()
project = project_copy({'level2.scene': example_scene})
out, shown = run(project, search('brick') + ['--click', TILE + ',0', '--key', 'f2', '--wait', '3', '--text', 'wall',
                                             '--key', 'enter', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
tex = os.path.join(project, 'textures')
check('renaming the used brick.png gives textures/wall.png',
      os.path.exists(os.path.join(tex, 'wall.png')) and not os.path.exists(os.path.join(tex, 'brick.png')), str(os.listdir(tex)) + ' | ' + out[-300:])
check('the walls of the open scene point at textures/wall.png', textures_of(scene(project)) == ['textures/wall.png'] * 2,
      str(textures_of(scene(project))))
check('the walls of level2.scene on disk too', textures_of(level2(project)) == ['textures/wall.png'] * 2, str(textures_of(level2(project))))
check('every texture the scene names is on disk', not missing_textures(project, scene(project)), str(missing_textures(project, scene(project))))
check('the status says how many references moved', 'references updated' in out, out[-300:])
check('no prompt is left open', 'popups open: no' in out, out[-300:])
# Unsaved, the open scene's references moved all the same: the document is
# dirty, so the rename is not lost on quit.
project = project_copy()
out, shown = run(project, search('brick') + ['--click', TILE + ',0', '--key', 'f2', '--wait', '3', '--text', 'wall',
                                             '--key', 'enter', '--wait', '5'])
check('the rename leaves the open scene with unsaved changes', getattr(run, 'unsaved', None) is True and
      os.path.exists(os.path.join(project, 'textures', 'wall.png')), out[-300:])

# One Ctrl+Z puts the file, the open scene and level2.scene back.
project = project_copy({'level2.scene': example_scene})
level2_before = open(os.path.join(project, 'level2.scene'), 'rb').read()
out, shown = run(project, search('brick') + ['--click', TILE + ',0', '--key', 'f2', '--wait', '3', '--text', 'wall',
                                             '--key', 'enter', '--wait', '5', '--click', VIEWPORT + ',0', '--key', 'ctrl+z', '--wait', '3',
                                             '--key', 'ctrl+s', '--wait', '5'])
tex = os.path.join(project, 'textures')
check('Ctrl+Z after the rename brings brick.png back',
      os.path.exists(os.path.join(tex, 'brick.png')) and not os.path.exists(os.path.join(tex, 'wall.png')), str(os.listdir(tex)) + ' | ' + out[-300:])
check('and the walls point at textures/brick.png again', textures_of(scene(project)) == ['textures/brick.png'] * 2, str(textures_of(scene(project))))
check('and level2.scene is byte for byte what it was', open(os.path.join(project, 'level2.scene'), 'rb').read() == level2_before)

# spare.png is used by nothing: F2, a new name and Enter rename it on disk.
spare = open(os.path.join(example, 'textures', 'crate.png'), 'rb').read()
project = project_copy({'textures/spare.png': spare})
out, shown = run(project, search('spare') + ['--click', TILE + ',0', '--key', 'f2', '--wait', '3', '--text', 'rubble',
                                             '--key', 'enter', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
tex = os.path.join(project, 'textures')
check('renaming the unused spare.png gives textures/rubble.png',
      os.path.exists(os.path.join(tex, 'rubble.png')) and not os.path.exists(os.path.join(tex, 'spare.png')), str(os.listdir(tex)) + ' | ' + out[-300:])
check('the scene is unchanged by it', scene(project)['objects'] == saved['objects'])

# A Lua script is a reference like any other: Game Logic's Script follows.
project = project_copy()
out, shown = run(project, search('logic') + ['--click', TILE + ',0', '--key', 'f2', '--wait', '3', '--text', 'brain',
                                             '--key', 'enter', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
lua = (comp(find(scene(project)['objects'], 'Game Logic'), 'script') or {}).get('lua')
check('renaming game_logic.lua gives scripts/brain.lua', os.path.exists(os.path.join(project, 'scripts', 'brain.lua')) and
      not os.path.exists(os.path.join(project, 'scripts', 'game_logic.lua')), out[-300:])
check("Game Logic's Script points at scripts/brain.lua", lua == 'scripts/brain.lua', str(lua))

# --- move: drag a file onto a folder of the tree ---
project = project_copy({'level2.scene': example_scene})
out, shown = run(project, search('brick') + ['--drag', TILE + ',' + MESHES_ROW + ',0', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
check('dragging brick.png onto "meshes" moves it to meshes/brick.png', os.path.exists(os.path.join(project, 'meshes', 'brick.png')) and
      not os.path.exists(os.path.join(project, 'textures', 'brick.png')), out[-300:])
check('the walls follow it, here and in level2.scene',
      textures_of(scene(project)) == ['meshes/brick.png'] * 2 and textures_of(level2(project)) == ['meshes/brick.png'] * 2,
      str(textures_of(scene(project))) + str(textures_of(level2(project))))
check('the scene tree is unchanged by the drop', names(scene(project)['objects']) == top, str(names(scene(project)['objects'])))
project = project_copy()
out, shown = run(project, search('brick') + ['--drag', TILE + ',' + MESHES_ROW + ',0', '--wait', '5', '--click', VIEWPORT + ',0',
                                             '--key', 'ctrl+z', '--wait', '3', '--key', 'ctrl+s', '--wait', '5'])
check('Ctrl+Z moves it back', os.path.exists(os.path.join(project, 'textures', 'brick.png')) and
      not os.path.exists(os.path.join(project, 'meshes', 'brick.png')) and textures_of(scene(project)) == ['textures/brick.png'] * 2, out[-300:])

# A file dropped onto a folder tile of the grid moves into that folder, a
# reference to it along.
with_root = json.loads(json.dumps(original))
for m in comp(find(with_root['objects'], 'Back Wall'), 'mesh')['materials']:
    m['texture'] = 'zwall.png'
project = project_copy({'zwall.png': open(os.path.join(example, 'textures', 'brick.png'), 'rb').read(),
                        'courtyard.scene': json.dumps(with_root, indent=2).encode()})
out, shown = run(project, ['--drag', ROOT_FILE_TILE + ',' + TEXTURES_TILE + ',0', '--wait', '5', '--key', 'ctrl+s', '--wait', '5'])
check('dragging zwall.png onto the "textures" tile moves it to textures/zwall.png',
      os.path.exists(os.path.join(project, 'textures', 'zwall.png')) and not os.path.exists(os.path.join(project, 'zwall.png')),
      str(shown) + ' | ' + out[-300:])
check('the Back Wall follows it', textures_of(scene(project), ('Back Wall',)) == ['textures/zwall.png'],
      str(textures_of(scene(project), ('Back Wall',))))
check('the root no longer lists it', shown == ['meshes', 'scripts', 'textures', 'courtyard.scene'], str(shown))

# --- delete ---
project = project_copy({'textures/spare.png': spare})
out, shown = run(project, search('brick') + ['--click', TILE + ',0', '--key', 'delete', '--wait', '3', '--key', 'enter', '--wait', '5'])
check('Delete on the used brick.png is refused', os.path.exists(os.path.join(project, 'textures', 'brick.png')), out[-300:])
out, shown = run(project, search('spare') + ['--click', TILE + ',0', '--key', 'delete', '--wait', '3', '--key', 'enter', '--wait', '5'])
check('Delete on the unused spare.png moves it to .trash after the prompt', not os.path.exists(os.path.join(project, 'textures', 'spare.png')) and
      os.path.exists(os.path.join(project, '.trash', '1', 'textures', 'spare.png')),
      str(os.listdir(os.path.join(project, 'textures'))) + ' | ' + out[-300:])
check('the panel no longer lists it', shown == [], str(shown))
project = project_copy({'textures/spare.png': spare})
out, shown = run(project, search('spare') + ['--click', TILE + ',0', '--key', 'delete', '--wait', '3', '--key', 'enter', '--wait', '5',
                                             '--click', VIEWPORT + ',0', '--key', 'ctrl+z', '--wait', '5'])
check('Ctrl+Z brings the deleted spare.png back', os.path.exists(os.path.join(project, 'textures', 'spare.png')) and
      not os.path.exists(os.path.join(project, '.trash', '1')), out[-300:])

# Opening the project empties deletes older than 30 days from .trash, and
# only those.
project = project_copy({'.trash/1/textures/old.png': spare, '.trash/2/textures/new.png': spare})
month = 31 * 24 * 3600
for p in (os.path.join(project, '.trash', '1'), os.path.join(project, '.trash', '1', 'textures')):
    os.utime(p, (os.path.getmtime(p) - month, os.path.getmtime(p) - month))
out, shown = run(project, [])
check('opening the project empties the month-old .trash/1 and keeps .trash/2',
      not os.path.exists(os.path.join(project, '.trash', '1')) and
      os.path.exists(os.path.join(project, '.trash', '2', 'textures', 'new.png')), out[-300:])
check('the status says so', 'Emptied 1 delete older than 30 days' in out, out[-300:])

# --- the bottom dock: Project and Timeline as tabs ---
with_cutscene = dict(original, cutscenes=[CUTSCENE])
project = project_copy({'courtyard.scene': json.dumps(with_cutscene, indent=2).encode()})
out, shown = run(project, ['--timeline', 'intro', '--wait', '3'])
check('opening the Timeline puts it in front of the Project panel', shown == [], str(shown) + ' | ' + out[-200:])
out, shown = run(project, ['--timeline', 'intro', '--wait', '3', '--click', PROJECT_TAB + ',0', '--wait', '5'])
check("the Project tab beside it shows the project's files", shown == ['meshes', 'scripts', 'textures', 'courtyard.scene'], str(shown))
out, shown = run(project, ['--timeline', 'intro', '--wait', '3', '--click', PROJECT_TAB + ',0', '--wait', '3', '--click', TIMELINE_TAB + ',0',
                           '--wait', '3', '--click', f'{fx(38)},{RULER_Y},0', '--wait', '3', '--dblclick', f'{fx(100)},740', '--wait', '3',
                           '--key', 'ctrl+s', '--wait', '5'])
frames = [k['frame'] for k in scene(project)['cutscenes'][0]['tracks'][1]['keyframes']]
check('the Timeline tab brings its lanes back: a key added there is saved', frames == [0, 38, 150], str(frames) + ' | ' + out[-200:])

# --- View > Project: the panel folded by its chevron comes back ---
project = project_copy()
out, shown = run(project, ['--click', COLLAPSE + ',0', '--wait', '3'])
check('the chevron folds the Project panel', getattr(run, 'open', None) is False, out[-200:])
out, shown = run(project, ['--click', COLLAPSE + ',0', '--wait', '3', '--click', VIEW_MENU + ',0', '--wait', '3',
                           '--click', VIEW_PROJECT + ',0', '--wait', '5'])
check('View > Project opens it again on the project root', getattr(run, 'open', None) is True and
      shown == ['meshes', 'scripts', 'textures', 'courtyard.scene'] and 'popups open: no' in out, str(shown) + ' | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
