# Imports the small Unity project in fixtures/unity-mini with
# `splashpack-cli import-unity` and checks the new project: the scene order,
# the object tree (children in Unity's m_Children order, the prefab instance
# unpacked with its overrides), the copied Lua and texture, the report (the
# camera left out) and its summary line, the UI canvas (each element's rect
# and anchors as PSXUILayout.Bake makes them, worked out by hand below), the
# font's advances, the sprite sheet, the 3x2 tilemap's cells, the audio clip,
# the loading screen's canvas, a CD track in disc.json, and that the imported
# scene exports (with its .loading). Then the refusals: a folder that is not
# empty, a scene name that is not there, a folder inside the Unity project, a
# CD track with a gap before it.
# When /home/pixel/sources/psxsus (or $PSXSUS) is there, imports its five
# game scenes in the game's order, checks their canvases, tilemap, audio and
# loading screens came over, and exports each of them; otherwise prints SKIP
# for that part.
# usage: python3 unityimportcheck.py <splashpack-cli>
import json, os, re, subprocess, sys, tempfile

cli = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
mini = os.path.join(here, 'fixtures', 'unity-mini')
psxsus = os.environ.get('PSXSUS', '/home/pixel/sources/psxsus')
tmp = tempfile.mkdtemp()
fails = checks = 0
SUMMARY = re.compile(r'^imported scenes=(\d+) objects=(\d+) meshes=(\d+) scripts=(\d+) textures=(\d+) skipped=(\d+)$')


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def run(*args):
    r = subprocess.run([cli] + list(args), capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout, r.stderr


def tree(objs):
    return [(o['name'], tree(o.get('children', []))) for o in objs]


def comp(o, kind):
    return next((c for c in o.get('components', []) if c['type'] == kind), None)


# ---- the fixture ----
dest = os.path.join(tmp, 'mini')
wav = os.path.join(mini, 'Assets', 'Audio', 'beep.wav')
rc, out, err = run('import-unity', mini, dest, '--cd-track', '2=' + wav)
check('import of the fixture succeeds', rc == 0, err)
lines = out.splitlines()
summary = [l for l in lines if SUMMARY.match(l)]
check('the summary line is printed last', lines and SUMMARY.match(lines[-1]) is not None, out[-300:])
check('the summary counts', summary == ['imported scenes=1 objects=14 meshes=4 scripts=2 textures=1 skipped=2'], summary)
check('the canvas is converted, not reported', not any(' / HUD:' in l for l in lines), out)
check("the canvas's 3D object is reported where it went",
      '[changed] Mini / HUD/Marker: a 3D object under the canvas HUD: placed where the canvas was, without the UI objects\' transforms' in lines, out)
check('the camera is reported as left out', '[left out] Mini / Main Camera: Camera, AudioListener (no PS1 equivalent)' in lines, out)
check('the approximated sphere is reported', any(l.startswith('[changed] Mini / RenamedLamp/Bulb:') for l in lines), out)
report = open(os.path.join(dest, 'import-report.txt')).read() if os.path.exists(os.path.join(dest, 'import-report.txt')) else ''
check('import-report.txt holds the report and the summary', 'Mini / Main Camera' in report and summary and summary[0] in report, report[:400])
check('scenes.list names the scene', open(os.path.join(dest, 'scenes.list')).read().split() == ['Mini.scene'] if os.path.exists(os.path.join(dest, 'scenes.list')) else False)
for f in ['scripts/cube.lua', 'scripts/scene.lua', 'textures/checker.png', 'meshes/unity_cube.mesh', 'meshes/unity_quad.mesh',
          'meshes/unity_sphere.mesh', 'models/tri.obj', 'vram.json', 'fonts/minifont.png', 'audio/beep.wav']:
    check('the project has ' + f.replace('/', ' '), os.path.isfile(os.path.join(dest, f)), f)
scene = json.load(open(os.path.join(dest, 'Mini.scene'))) if os.path.exists(os.path.join(dest, 'Mini.scene')) else {'objects': [], 'settings': {}}
want = [('Parent', [('ChildB', []), ('ChildA', [])]), ('Cube', []), ('Quad', []),
        ('RenamedLamp', [('Bulb', []), ('Extra', [])]), ('Marker', []), ('Main Camera', []), ('Tri', []),
        ('Tiles', []), ('Beep', []), ('Sprites', [])]
check('the object tree in Unity order', tree(scene['objects']) == want, tree(scene['objects']))
by = {o['name']: o for o in scene['objects']}
lamp = by.get('RenamedLamp', {})
check('the prefab root keeps its position override', lamp.get('transform', {}).get('position') == [3, 1.5, 0], lamp.get('transform'))
bulb = (lamp.get('children') or [{}])[0]
check('the prefab child keeps its bit depth override', (comp(bulb, 'mesh') or {}).get('bitDepth') == 16, comp(bulb, 'mesh'))
cube = by.get('Cube', {})
check('the cube carries its Lua file', (comp(cube, 'script') or {}).get('lua') == 'scripts/cube.lua', comp(cube, 'script'))
check('the cube starts hidden', cube.get('startActive') is False, cube.get('startActive'))
quad = by.get('Quad', {})
check('the quad carries its material texture', ((comp(quad, 'mesh') or {}).get('materials') or [{}])[0].get('texture') == 'textures/checker.png',
      comp(quad, 'mesh'))
st = scene['settings']
check('the scene settings', st.get('gteScaling') == 150 and st.get('sceneType') == 'interior' and st.get('networkId') == 'mini/main' and
      st.get('script') == 'scripts/scene.lua', st)

# The UI. HUD's RectTransform is 640x480, so the scale is 320/640 = 240/480 = 0.5.
# Bake, one anchor point (x, y from the anchor; size; at least 1):
#   x = pos.x*s - pivot.x*(size.x*s),  y = -pos.y*s - (1 - pivot.y)*(size.y*s)
# Bake, stretched: offsetMin = pos - size*pivot, offsetMax = pos + size*(1 - pivot);
#   x = offsetMin.x*s, y = -offsetMax.y*s, w = (offsetMax.x - offsetMin.x)*s, h = (offsetMax.y - offsetMin.y)*s
# Anchors top-down: anchorMin = (min.x, 1 - max.y), anchorMax = (max.x, 1 - min.y).
# Label: anchor (0,1), pivot (0,1), pos (10,-20), size (100,30):
#   x = 5 - 0*50 = 5, y = 10 - 0*15 = 10, w = 50, h = 15; anchors (0, 1-1) = (0, 0) both.
# Bar: anchors (0,0)-(1,0), pivot (0.5,0), pos (0,20), size (-40,24):
#   offsetMin = (0+20, 20-0) = (20,20), offsetMax = (0-20, 20+24) = (-20,44)
#   x = 10, y = -22, w = -10-10 = -20, h = -10-(-22) = 12; anchorMin (0, 1-0) = (0,1), anchorMax (1, 1).
# Icon (an inactive object, still exported): anchor (0.5,0.5), pivot (0.5,0.5), pos (8,-6), size (32,32):
#   x = 4 - 0.5*16 = -4, y = 3 - 0.5*16 = -5, w = h = 16. Cell 9 of a 16x16 texture in 8x8 cells: clamped to 3.
# Loading prefab's Bar (canvas 320x240, s = 1): anchor (0,0), pivot (0,0), pos (20,30), size (200,8):
#   x = 20, y = -30 - 1*8 = -38, w = 200, h = 8; anchors (0, 1) both.
canvases = {c['name']: c for c in scene.get('canvases', [])}
hud = canvases.get('hud', {})
check('the canvas keeps its settings', hud.get('visible') is False and hud.get('sortOrder') == 3 and hud.get('defaultFont') == 'MiniFont',
      {k: hud.get(k) for k in ('visible', 'sortOrder', 'defaultFont')})
els = {e['name']: e for e in hud.get('elements', [])}
check('the elements in hierarchy order', [e['name'] for e in hud.get('elements', [])] == ['label', 'bar', 'icon'], list(els))
lab, bar, icon = els.get('label', {}), els.get('bar', {}), els.get('icon', {})
check('the fixed text: rect and anchors', lab.get('rect') == [5, 10, 50, 15] and lab.get('anchorMin') == [0, 0] and lab.get('anchorMax') == [0, 0],
      lab)
check('the text keeps its colour and text', lab.get('color') == [1, 0.5, 0.25] and lab.get('text') == 'Hello, world', lab)
check('the stretched box: rect and anchors', bar.get('rect') == [10, -22, -20, 12] and bar.get('anchorMin') == [0, 1] and
      bar.get('anchorMax') == [1, 1], bar)
check('the UI sprite: rect, sheet, clamped cell, neutral tint',
      icon.get('type') == 'image' and icon.get('rect') == [-4, -5, 16, 16] and icon.get('sheet') == 'icons' and icon.get('cell') == 3 and
      icon.get('color') == [0.5, 0.5, 0.5], icon)
st = scene['settings']
load = canvases.get('hud_loading', {})
check('the loading screen canvas, renamed off the scene canvas hud', st.get('loadingCanvas') == 'hud_loading' and
      [(e['type'], e['name'], e['rect'], e['value']) for e in load.get('elements', [])] == [('progress', 'loading', [20, -38, 200, 8], 40)], load)
fonts = scene.get('fonts', [])
check('the font: bitmap, cells, advances', len(fonts) == 1 and fonts[0].get('bitmap') == 'fonts/minifont.png' and
      fonts[0].get('glyphWidth') == 8 and fonts[0].get('advances') == [i % 5 + 3 for i in range(96)], fonts)
sheets = scene.get('spriteSheets', [])
check('one sprite sheet for the sprite, the UI sprite and the tileset', len(sheets) == 1 and sheets[0].get('name') == 'icons' and
      sheets[0].get('texture') == 'textures/checker.png' and sheets[0].get('bitDepth') == 8 and sheets[0].get('cellWidth') == 8 and
      [(a['name'], a['firstFrame'], a['frameCount'], a['frameDuration'], a['loop']) for a in sheets[0].get('anims', [])] == [('blink', 1, 2, 10, False)],
      sheets)
tm = scene.get('tilemap') or {}
cells = [int(v) for row in tm.get('cells', []) for v in row.split()]
# cells: 00000000 01000000 ffffffff / 01000000 01000000 00000000, little-endian int32s.
check('the tilemap: size, tileset, cells', tm.get('width') == 3 and tm.get('height') == 2 and tm.get('tileset') == 'icons' and
      cells == [0, 1, -1, 1, 1, 0], (tm.get('width'), tm.get('height'), tm.get('tileset'), cells))
check('the tilemap brushes and kind names', [(b['name'], b['cell'], b['walkable'], b['kind'], b['id']) for b in tm.get('brushes', [])] ==
      [('floor', 0, True, 0, -1), ('door', 1, False, 1, 7)] and tm.get('kindNames') == ['(none)', 'door'], tm.get('brushes'))
au = comp(by.get('Beep', {}), 'audio') or {}
check('the audio clip', au.get('clip') == 'audio/beep.wav' and au.get('clipName') == 'beep' and au.get('loop') is True and
      au.get('sampleRate') == 11025 and au.get('defaultVolume') == 90, au)
disc = os.path.join(dest, 'disc.json')
check('the CD track is in disc.json', os.path.exists(disc) and json.load(open(disc)).get('tracks') == ['audio/beep.wav'],
      open(disc).read() if os.path.exists(disc) else 'no disc.json')
rc, out, err = run('export', os.path.join(dest, 'Mini.scene'), '-o', os.path.join(tmp, 'mini.splashpack'))
check('the imported fixture scene exports', rc == 0 and os.path.getsize(os.path.join(tmp, 'mini.splashpack')) > 0, err)
check('with its loading screen', os.path.exists(os.path.join(tmp, 'mini.loading')), os.listdir(tmp))

# ---- refusals ----
rc, out, err = run('import-unity', mini, dest)
check('a folder that is not empty is refused', rc == 1 and 'not empty' in err, (rc, err))
rc, out, err = run('import-unity', mini, os.path.join(tmp, 'none'), '--scenes', 'Nope')
check('an unknown scene name is refused and writes nothing', rc == 1 and 'Nope' in err and not os.path.exists(os.path.join(tmp, 'none')),
      (rc, err))
inside = os.path.join(mini, 'Assets', 'imported')
rc, out, err = run('import-unity', mini, inside)
check('a folder inside the Unity project is refused', rc == 1 and not os.path.exists(inside), (rc, err))
rc, out, err = run('import-unity', mini, os.path.join(tmp, 'both'), '--scenes', 'Empty,Mini')
check('scenes come in the order asked', rc == 0 and open(os.path.join(tmp, 'both', 'scenes.list')).read().split() == ['Empty.scene', 'Mini.scene'],
      (rc, err))
rc, out, err = run('import-unity', mini, os.path.join(tmp, 'gap'), '--cd-track', '3=' + wav)
check('a CD track after a gap is refused and writes nothing', rc == 1 and 'not track 2' in err and not os.path.exists(os.path.join(tmp, 'gap')),
      (rc, err))

# ---- psxsus ----
if not os.path.isdir(os.path.join(psxsus, 'Assets')):
    print('SKIP psxsus: %s is not there' % psxsus)
else:
    order = ['SCN_Menu', 'SCN_Lobby', 'SCN_Game_Hulk', 'SCN_Game_Rim', 'SCN_Game_Core']
    pdest = os.path.join(tmp, 'psxsus')
    rc, out, err = run('import-unity', psxsus, pdest, '--scenes', ','.join(order))
    check('psxsus imports', rc == 0, err)
    lines = out.splitlines()
    print('psxsus: ' + (lines[-1] if lines else '(no output)'))
    m = SUMMARY.match(lines[-1]) if lines else None
    check('psxsus: a summary line with five scenes', m is not None and m.group(1) == '5', lines[-1:] if lines else out)
    check('psxsus: scenes.list in the game order',
          os.path.exists(os.path.join(pdest, 'scenes.list')) and open(os.path.join(pdest, 'scenes.list')).read().split() == [s + '.scene' for s in order])
    for s in order:
        sc = os.path.join(pdest, s + '.scene')
        d = json.load(open(sc)) if os.path.exists(sc) else {}
        names = [c['name'] for c in d.get('canvases', [])]
        loading = d.get('settings', {}).get('loadingCanvas')
        check('psxsus: %s has its canvases and its loading screen' % s, len(names) >= 2 and loading in names and
              all(c.get('elements') for c in d.get('canvases', [])), (names, loading))
        if s.startswith('SCN_Game'):
            game = d.get('objects', [])
            audio = [o for o in game if o['name'] == 'GameAudio']
            clips = [c for o in (audio[0].get('children', []) if audio else []) for c in o.get('components', []) if c['type'] == 'audio']
            check('psxsus: %s has its tilemap and the audio prefab\'s 21 clips' % s, d.get('tilemap', {}).get('tileset') == 'tiles' and
                  len(clips) == 21 and all(c.get('clip', '').startswith('audio/') for c in clips), (d.get('tilemap', {}).get('tileset'), len(clips)))
        rc, out, err = run('export', sc, '-o', os.path.join(tmp, s + '.splashpack'))
        check('psxsus: %s exports, with its loading screen' % s, rc == 0 and os.path.exists(os.path.join(tmp, s + '.loading')), err[-400:])

print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
