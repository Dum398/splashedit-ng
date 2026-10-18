# The Sprite sheets section's Add direction set, driven through the GUI on
# copies of the courtyard example with sprite/dirs.png (64x64, a 4 x 4 grid of
# 16x16 cells) in its textures: the form's Create adds base_n, base_ne, ... in
# increasing-yaw order on consecutive runs of cells, in the saved scene and in
# the section's scripted "scene: sprite" lines, as one Ctrl+Z; cells past the
# sheet and a name already used are refused with a message and add nothing;
# the selected direction's facing dial lights its sector, a click on another
# sector selects that direction, and Copy puts the SetFacingFromYaw line on
# the clipboard; deleting one direction raises the Problems warning.
# Controls are clicked by name (--click @scene.spritedirset).
# usage: python3 dirsetcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
#   DIRSET_SCENE=<file> keeps a copy of the scene the 8-direction arm saved.
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shotOut = sys.argv[2] if len(sys.argv) > 2 else None
# The form's screenshot (after a refused Create) goes beside it as <name>-form.png.
formShot = os.path.splitext(shotOut)[0] + '-form.png' if shotOut else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
H = 960
# Sprite sheets is the inspector's last section: the wheel over the inspector
# scrolls to the bottom once the background export has landed; again after
# each change that grows the section.
SCROLL = ['--wheel', '1420,500,-200', '--wait', '3']
START = ['--wait', '20', '--wait-export'] + SCROLL
SUF8 = ['n', 'ne', 'e', 'se', 's', 'sw', 'w', 'nw']
SUF4 = ['n', 'e', 's', 'w']
mid = None


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(name, text):
    """Double-click a number field, replace its text, Enter."""
    return ['--dblclick', '@' + name, '--wait', '3', '--key', 'ctrl+a', '--text', text, '--key', 'enter', '--wait', '3']


UNDO = ['--key', 'ctrl+z', '--wait', '3']
SAVE_MID = ['--key', 'ctrl+s', '--wait', '10', '--copy-file', '{scene}@{snap}', '--wait', '2']
ADD = click('scene.spriteadd') + click('scene.spriteadd/textures/dirs.png') + SCROLL
FORM = ADD + click('scene.spritedirset') + SCROLL
# 8 directions of 2 frames from cell 0: the 16 cells exactly.
CREATE8 = FORM + typed('scene.dirsetframes', '2') + click('scene.dirsetcreate') + SCROLL


def run(args, onscreen=(), shot=None, keep=None):
    """Runs `args` on a fresh courtyard copy after scrolling to the section,
    then Ctrl+S; returns the saved scene's spriteSheets (None when absent)
    and the editor's output without its list of controls."""
    global mid
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shutil.copy(os.path.join(here, 'sprite', 'dirs.png'), os.path.join(project, 'textures', 'dirs.png'))
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project] + START + args + [
        '--key', 'ctrl+s', '--wait', '10', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    out = (r.stdout + r.stderr).strip()
    lines = out.splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    for name in onscreen:
        rect = None
        for l in lines:
            m = re.match(r'control ' + re.escape(name) + r' at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
            if m: rect = tuple(int(v) for v in m.groups())
        check(name + ' is on screen', rect is not None and rect[1] > 80 and rect[3] < H - 40, rect)
    mid = json.load(open(snap, encoding='utf-8')).get('spriteSheets') if os.path.exists(snap) else None
    if keep: shutil.copy(scene, keep)
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')).get('spriteSheets'), out


def anims(sheets):
    if not sheets: return None
    return [(a['name'], a['firstFrame'], a['frameCount'], a['frameDuration'], a['loop']) for a in sheets[0]['anims']]


def last(out, prefix):
    found = re.findall(r'^' + re.escape(prefix) + r'(.*)$', out, re.M)
    return found[-1] if found else None


IDLE = ('idle', 0, 1, 6, True)

# The form opens with a row of cells per direction: 4 frames each (the sheet's
# columns), which for 8 directions is 32 cells. Create refuses and adds nothing.
s, out = run(FORM + click('scene.dirsetcreate'), onscreen=('scene.dirsetcreate', 'scene.dirsetmsg'), shot=formShot)
check('Create past the 16 cells adds nothing', anims(s) == [IDLE], f'{anims(s)} | ' + out[-300:])
check('and says why', last(out, 'scene: sprite dirset message ') ==
      "8 x 4 frames from cell 0 end on cell 31, past the sheet's 16 cells (0..15).", out[-400:])

# 8 directions of 2 frames: n ne e se s sw w nw on cells 0-1, 2-3, ... 14-15.
keep = os.environ.get('DIRSET_SCENE')
s, out = run(CREATE8 + ['--wait', '10'], onscreen=('scene.dirdial', 'scene.dirsetcopy'), keep=keep)
want8 = [IDLE] + [('walk_' + x, 2 * k, 2, 6, True) for k, x in enumerate(SUF8)]
check('Create adds the 8 directions in increasing-yaw order, 2 cells each', anims(s) == want8, f'{anims(s)} | ' + out[-300:])
check('the section reports them', last(out, 'scene: sprite 0 ') ==
      'dirs texture textures/dirs.png bpp 4 cell 16x16 grid 4x4 anims idle:0+1@6L ' +
      ' '.join(f'walk_{x}:{2 * k}+2@6L' for k, x in enumerate(SUF8)), out[-500:])
check('and recognise the set', last(out, 'scene: sprite dirsets') == ' walk:8@1', out[-300:])
check('the new set is selected and the dial shows walk_n', last(out, 'scene: sprite facing ') == 'walk_n 1/8', out[-300:])
check('no warnings', last(out, 'scene: sprite warnings ') == 'none', out[-300:])

# The dial: a click on the w sector selects walk_w; Copy puts the Lua line on the clipboard.
s, out = run(CREATE8 + click('scene.dirdial.w') + click('scene.dirsetcopy') + ['--wait', '5'],
             onscreen=('scene.dirdial.w', 'scene.dirsetlua'), shot=shotOut)
check('a click on the w sector selects walk_w', last(out, 'scene: sprite facing ') == 'walk_w 7/8', out[-300:])
check('Copy puts the SetFacingFromYaw line on the clipboard',
      last(out, 'scene: sprite copied ') == 'Sprite.SetFacingFromYaw(sprite, "walk_n", 8, yaw)', out[-300:])

# One Ctrl+Z takes the whole set back.
s, out = run(CREATE8 + SAVE_MID + UNDO)
check('one Ctrl+Z removes all 8', anims(mid) == want8 and anims(s) == [IDLE], f'{anims(mid)} then {anims(s)} | ' + out[-300:])

# A second set with the same base: refused, the first kept.
# No scroll after opening the form: it opens under the button, and the bottom of the
# grown section would leave its fields above the panel.
s, out = run(CREATE8 + click('scene.spritedirset') + typed('scene.dirsetframes', '2') + click('scene.dirsetcreate'),
             onscreen=('scene.dirsetframes', 'scene.dirsetcreate'))
check('a base name already used is refused', anims(s) == want8, f'{anims(s)} | ' + out[-300:])
check('and says which name clashes', (last(out, 'scene: sprite dirset message ') or '').startswith(
    "'walk_n' is already an animation in sheet 'dirs'"), out[-400:])

# 4 directions: n e s w, a row (4 cells) each.
s, out = run(FORM + click('scene.dirsetcount') + click('scene.dirsetcount/4 directions') + click('scene.dirsetcreate') + SCROLL)
want4 = [IDLE] + [('walk_' + x, 4 * k, 4, 6, True) for k, x in enumerate(SUF4)]
check('4 directions: n e s w, 4 cells each', anims(s) == want4, f'{anims(s)} | ' + out[-300:])
check('recognised as a set of 4', last(out, 'scene: sprite dirsets') == ' walk:4@1', out[-300:])
check('Problems is clear', last(out, 'problems: ') == 'No problems', out[-300:])

# Deleting walk_e leaves n s w: Problems warns that SetFacingFromYaw would play the wrong animation.
s, out = run(FORM + click('scene.dirsetcount') + click('scene.dirsetcount/4 directions') + click('scene.dirsetcreate') + SCROLL +
             click('scene.spriteanim.2.delete') + SCROLL)
check('walk_e deleted', anims(s) == [IDLE, want4[1], want4[3], want4[4]], f'{anims(s)} | ' + out[-300:])
check('the warning names the missing direction', last(out, 'scene: sprite warnings ') ==
      'sprite animations walk_*: no walk_e, so Sprite.SetFacingFromYaw(sprite, "walk_n", 4, yaw) would play the wrong '
      'animation for some directions: add the missing ones', out[-400:])
check('and the set is no longer recognised', last(out, 'scene: sprite dirsets') == ' none', out[-300:])
check('Problems counts the warning', last(out, 'problems: ') == '1 warning', out[-300:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
