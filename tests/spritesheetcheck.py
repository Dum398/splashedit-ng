# The scene inspector's Sprite sheets section, driven through the GUI on copies
# of the courtyard example with sprite/hero.png (64x32) in its textures: add a
# sheet from the Add sheet list, type a cell size, add an animation and type
# its frames, each landing in the saved scene's spriteSheets and in the
# section's scripted "scene: sprite" lines; each of those edits goes back in
# one Ctrl+Z; the preview steps through the animation's cells; a cell size
# that does not divide the texture is reported; a sheet's trash deletes it.
# Controls are clicked by name (--click @scene.spriteadd): the editor aims at
# where the frame before drew them, so no pixel positions live here.
# usage: python3 spritesheetcheck.py <splashedit> [screenshot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shotOut = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
H = 960
# Sprite sheets is the inspector's last section: once the background export has
# landed, the wheel over the inspector scrolls to the bottom, which brings it
# into view. The section grows as sheets and animations are added, so the
# script scrolls again after each of those.
SCROLL = ['--wheel', '1420,500,-200', '--wait', '3']
START = ['--wait', '20', '--wait-export'] + SCROLL
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
ADD = click('scene.spriteadd') + click('scene.spriteadd/textures/hero.png') + SCROLL


def run(args, onscreen=(), shot=None):
    """Runs `args` on a fresh courtyard copy after scrolling to the section,
    then Ctrl+S; returns the saved scene's spriteSheets (None when the key is
    absent) and the editor's output without its list of controls."""
    global mid
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shutil.copy(os.path.join(here, 'sprite', 'hero.png'), os.path.join(project, 'textures', 'hero.png'))
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
    # Drawn is not visible: a control scrolled out of the panel still has a rect.
    for name in onscreen:
        rect = None
        for l in lines:
            m = re.match(r'control ' + re.escape(name) + r' at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
            if m: rect = tuple(int(v) for v in m.groups())
        check(name + ' is on screen', rect is not None and rect[1] > 80 and rect[3] < H - 40, rect)
    mid = json.load(open(snap, encoding='utf-8')).get('spriteSheets') if os.path.exists(snap) else None
    out = '\n'.join(l for l in lines if not l.startswith('control '))
    return json.load(open(scene, encoding='utf-8')).get('spriteSheets'), out


def summary(sheets):
    if sheets is None: return None
    return [(s['name'], s['texture'], s['bitDepth'], s['cellWidth'], s['cellHeight'],
             [(a['name'], a['firstFrame'], a['frameCount'], a['frameDuration'], a['loop']) for a in s['anims']]) for s in sheets]


original = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
assert 'spriteSheets' not in original, 'the example already has sprite sheets'

s, out = run([], onscreen=('scene.spriteadd',))
check('save alone writes no spriteSheets', s is None, str(s) + ' | ' + out[-200:])
check('the section reports no sheets', 'scene: sprites 0 selected -1 anim 0' in out, out[-300:])

# Add sheet: named after the file, 4 bpp, 16x16 cells, one idle animation.
s, out = run(ADD, onscreen=('scene.spritename', 'scene.spritecellw', 'scene.spriteanimadd'))
check('Add sheet adds hero.png as sheet "hero"',
      summary(s) == [('hero', 'textures/hero.png', 4, 16, 16, [('idle', 0, 1, 6, True)])], f'{summary(s)} | ' + out[-200:])
check('the section reports the sheet and its grid',
      'scene: sprite 0 hero texture textures/hero.png bpp 4 cell 16x16 grid 4x2 anims idle:0+1@6L' in out, out[-400:])

# Cell size, then an animation whose frames are typed: all saved.
EDITS = ADD + typed('scene.spritecellw', '32') + click('scene.spriteanimadd') + SCROLL + typed('scene.animcount', '3') + \
    typed('scene.animdur', '2')
s, out = run(EDITS + ['--wait', '40'], onscreen=('scene.animcount', 'scene.spritepreview'), shot=shotOut)
want = [('hero', 'textures/hero.png', 4, 32, 16, [('idle', 0, 1, 6, True), ('anim', 1, 3, 2, True)])]
check('cell width, the new animation and its frames land in the saved scene', summary(s) == want, f'{summary(s)} | ' + out[-200:])
check('the section reports the 2x2 grid and the animation',
      'scene: sprite 0 hero texture textures/hero.png bpp 4 cell 32x16 grid 2x2 anims idle:0+1@6L anim:1+3@2L' in out, out[-400:])
# Only after the last edit (the frame time): every edit restarts the preview's
# clock as PlayAnim does, so a jump back to cell 1 before it is not a loop.
after = out[out.find('anim:1+3@2L'):] if 'anim:1+3@2L' in out else ''
cells = [int(m) for m in re.findall(r'^scene: sprite preview hero/anim cell (\d+)$', after, re.M)]
check('the preview plays cells 1, 2, 3 in order and loops', len(cells) >= 4 and
      all(b == (a % 3) + 1 for a, b in zip(cells, cells[1:])) and cells.count(1) >= 2, cells)
probs = re.findall(r'^scene: sprite problems (.*)$', out, re.M)
check('no problems with a whole grid', probs[-1:] == ['none'], probs)

# Undo arms: each edit goes back in one Ctrl+Z, leaving the ones before it.
s, out = run(EDITS + SAVE_MID + UNDO)
check('one Ctrl+Z takes the typed frame time back',
      summary(mid) == want and summary(s) == [('hero', 'textures/hero.png', 4, 32, 16, [('idle', 0, 1, 6, True), ('anim', 1, 3, 6, True)])],
      f'{summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(ADD + click('scene.spriteanimadd') + SAVE_MID + UNDO)
check('one Ctrl+Z removes the added animation',
      mid and len(mid[0]['anims']) == 2 and summary(s) == [('hero', 'textures/hero.png', 4, 16, 16, [('idle', 0, 1, 6, True)])],
      f'{summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(ADD + typed('scene.spritecellw', '32') + SAVE_MID + UNDO)
check('one Ctrl+Z puts the cell width back', mid and mid[0]['cellWidth'] == 32 and summary(s) and s[0]['cellWidth'] == 16,
      f'{summary(mid)} then {summary(s)} | ' + out[-200:])
s, out = run(ADD + SAVE_MID + UNDO)
check('one Ctrl+Z removes the added sheet', mid and len(mid) == 1 and s is None, f'{summary(mid)} then {summary(s)} | ' + out[-200:])
check('after the undo the section reports no sheets', out.rfind('scene: sprites 0') > out.rfind('scene: sprites 1'), out[-300:])

# A cell width the texture does not divide: reported, and export refuses.
s, out = run(ADD + typed('scene.spritecellw', '24'), onscreen=('scene.spritegrid',))
check('a 24 px cell on the 64 px texture is reported',
      "scene: sprite problems sprite sheet 'hero': the texture is 64x32, not a whole number of 24x16 cells" in out, out[-400:])
check('export fails and lists it in Problems', re.findall(r'^problems: (.*)$', out, re.M)[-1:] == ['1 error'] and
      re.findall(r'^statusbar: export (.*)$', out, re.M)[-1:] == ['failed, meters from last good'], out[-300:])

# The trash deletes the sheet.
s, out = run(ADD + click('scene.sprite.0.delete'))
check('the trash deletes the sheet', s is None, f'{summary(s)} | ' + out[-200:])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
