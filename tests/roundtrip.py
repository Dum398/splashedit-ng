# Load/save checks for scene and mesh files through `splashpack-cli resave`.
# usage: python3 roundtrip.py <splashpack-cli>
import json, os, subprocess, sys, tempfile

cli = sys.argv[1]
tmp = tempfile.mkdtemp()
fails = 0


def resave(doc, ext):
    src, dst = os.path.join(tmp, 'in' + ext), os.path.join(tmp, 'out' + ext)
    json.dump(doc, open(src, 'w'))
    r = subprocess.run([cli, 'resave', src, dst], capture_output=True, text=True)
    return r.returncode, (json.load(open(dst)) if r.returncode == 0 else r.stderr.strip())


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + (': ' + str(detail) if detail and not ok else ''))
    fails += not ok


scene = {
    'format': 'splashedit-ng/scene', 'version': 1, 'futureRoot': [1, 2],
    'settings': {'gteScaling': 50, 'futureSetting': 'x', 'fog': {'enabled': True, 'futureFog': 3}},
    'objects': [{
        'name': 'A', 'futureObject': {'k': True}, 'startActive': False,
        'components': [
            {'type': 'mesh', 'mesh': 'a.mesh', 'futureMesh': 1,
             'materials': [{'texture': None, 'futureMaterial': 2}]},
            {'type': 'light', 'kind': 'spot', 'enabled': False, 'futureLight': 4},
            {'type': 'futureComponent', 'value': 7},
        ],
        'children': [{'name': 'B', 'components': [{'type': 'player', 'moveSpeed': 5, 'futurePlayer': 6}]}],
    }],
}
rc, out = resave(scene, '.scene')
check('scene loads', rc == 0, out)
if rc == 0:
    o = out['objects'][0]
    comps = {c['type']: c for c in o['components']}
    check('known values kept', out['settings']['gteScaling'] == 50 and comps['light']['enabled'] is False
          and o['children'][0]['components'][0]['moveSpeed'] == 5)
    check('unknown root key', out.get('futureRoot') == [1, 2])
    check('unknown settings key', out['settings'].get('futureSetting') == 'x')
    check('unknown fog key', out['settings']['fog'].get('futureFog') == 3)
    check('unknown object key', o.get('futureObject') == {'k': True})
    check('startActive false kept', o.get('startActive') is False)
    check('startActive true not written', 'startActive' not in o['children'][0])
    check('unknown mesh key', comps['mesh'].get('futureMesh') == 1)
    check('unknown material key', comps['mesh']['materials'][0].get('futureMaterial') == 2)
    check('unknown light key', comps['light'].get('futureLight') == 4)
    check('unknown component', comps.get('futureComponent') == {'type': 'futureComponent', 'value': 7})
    check('unknown key on a child', o['children'][0]['components'][0].get('futurePlayer') == 6)
    rc2, out2 = resave(out, '.scene')
    check('second save is identical', rc2 == 0 and out2 == out)

for v in (2, 0, None):
    doc = dict(scene, version=v) if v is not None else {k: x for k, x in scene.items() if k != 'version'}
    rc, err = resave(doc, '.scene')
    check(f'scene version {v} refused', rc != 0, err)

ui = {'format': 'splashedit-ng/scene', 'version': 1, 'objects': [],
      'fonts': [{'name': 'f', 'source': 'a.ttf', 'size': 12, 'kerning': True}],
      'canvases': [{'name': 'hud', 'layer': 3, 'elements': [
          {'type': 'text', 'name': 't', 'rect': [1, 2, 3, 4], 'text': 'hi', 'font': 'f', 'shadow': [1, 1]},
          {'type': 'line', 'name': 'l', 'from': [0, 300], 'to': [400, -5]},
          {'type': 'sprite', 'name': 'future'}]}]}
rc, out = resave(dict(ui, canvases=ui['canvases'][:1] and [dict(ui['canvases'][0], elements=ui['canvases'][0]['elements'][:2])]), '.scene')
els = out['canvases'][0]['elements'] if rc == 0 else []
check('ui values and unknown keys survive', rc == 0 and out['fonts'][0]['kerning'] is True
      and out['canvases'][0]['layer'] == 3 and els[0]['shadow'] == [1, 1] and els[0]['font'] == 'f'
      and els[1]['from'] == [0, 300] and els[1]['to'] == [400, -5], out)
rc, out = resave(ui, '.scene')
check('unknown ui element type kept', rc == 0 and out['canvases'][0]['elements'][2] == ui['canvases'][0]['elements'][2], out)
for name, edit in [('font with source and bitmap', lambda d: d['fonts'][0].update(bitmap='b.png')),
                   ('anchor above 1', lambda d: d['canvases'][0]['elements'][0].update(anchorMax=[1.5, 0]))]:
    d = json.loads(json.dumps(ui))
    d['canvases'][0]['elements'].pop()
    edit(d)
    rc, err = resave(d, '.scene')
    check(name + ' refused', rc != 0, err)

cs = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cutscene', 'cutscene.scene')))
cs['cutscenes'][0]['tracks'][0]['keyframes'][0]['tension'] = 0.5
cs['cutscenes'][0]['camera'] = 'main'
rc, out = resave(cs, '.scene')
check('cutscene values and unknown keys survive', rc == 0 and out['cutscenes'][0]['camera'] == 'main'
      and out['cutscenes'][0]['tracks'][0]['keyframes'][0] == dict(cs['cutscenes'][0]['tracks'][0]['keyframes'][0], interp='linear'), out)
rc, err = resave(dict(cs, cutscenes=[dict(cs['cutscenes'][0], durationFrames=0)]), '.scene')
check('cutscene duration 0 refused', rc != 0, err)

an = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'anim', 'anim.scene')))
an['animations'][0]['skinEvents'][0]['blend'] = 4
an['animations'][0]['speed'] = 2
rc, out = resave(an, '.scene')
check('animations, skin events and unknown keys survive', rc == 0 and out['animations'][0]['speed'] == 2
      and out['animations'][0]['skinEvents'] == [dict(an['animations'][0]['skinEvents'][0], loop=False)]
      and out['cutscenes'][0]['skinEvents'] == an['cutscenes'][0]['skinEvents'], out)
rc, out = resave(dict(an, animations=[dict(an['animations'][0], skinEvents=[])]), '.scene')
check('an empty skinEvents list is not written', rc == 0 and 'skinEvents' not in out['animations'][0], out)
rc, err = resave(dict(an, animations=[dict(an['animations'][0], skinEvents=[{'frame': -1}])]), '.scene')
check('skin event frame -1 refused', rc != 0, err)

# Sprite sheets: written as given, defaults filled in, unknown keys kept; no
# sheets writes no spriteSheets key.
base = {k: x for k, x in scene.items() if k not in ('objects',)}
rc, out = resave(base, '.scene')
check('no sprite sheets: no spriteSheets key', rc == 0 and 'spriteSheets' not in out, out)
sheets = [{'name': 'hero', 'texture': 'hero.png', 'bitDepth': 8, 'cellWidth': 24, 'cellHeight': 32, 'futureSheet': 1,
           'anims': [{'name': 'walk', 'firstFrame': 2, 'frameCount': 3, 'frameDuration': 5, 'loop': False, 'futureAnim': 2},
                     {}]},
          {'name': 'fx', 'texture': 'fx.png'}]
rc, out = resave(dict(base, spriteSheets=sheets), '.scene')
check('sprite sheets load', rc == 0, out)
if rc == 0:
    s = out.get('spriteSheets', [])
    check('sprite sheet values kept', len(s) == 2 and s[0]['name'] == 'hero' and s[0]['texture'] == 'hero.png' and
          s[0]['bitDepth'] == 8 and s[0]['cellWidth'] == 24 and s[0]['cellHeight'] == 32, s)
    check('sprite animation values kept', s and s[0]['anims'][0] == {'name': 'walk', 'firstFrame': 2, 'frameCount': 3,
                                                                      'frameDuration': 5, 'loop': False, 'futureAnim': 2}, s)
    check('sprite animation defaults', s and s[0]['anims'][1] == {'name': 'idle', 'firstFrame': 0, 'frameCount': 1,
                                                                  'frameDuration': 6, 'loop': True}, s)
    check('sprite sheet defaults', len(s) == 2 and s[1] == {'name': 'fx', 'texture': 'fx.png', 'bitDepth': 4, 'cellWidth': 16,
                                                            'cellHeight': 16, 'anims': []}, s)
    check('unknown sprite sheet key', s and s[0].get('futureSheet') == 1, s)
    rc2, out2 = resave(out, '.scene')
    check('sprite sheets: second save is identical', rc2 == 0 and out2 == out)
rc, err = resave(dict(base, spriteSheets=[{'name': 'x', 'bitDepth': 5}]), '.scene')
check('sprite sheet bitDepth 5 refused', rc != 0 and 'bitDepth' in str(err), err)

mesh = {'format': 'splashedit-ng/mesh', 'version': 1, 'positions': [0, 0, 0, 1, 0, 0, 0, 1, 0],
        'colors': [1, 1, 1, 1] * 3, 'submeshes': [[0, 1, 2]]}
rc, out = resave(mesh, '.mesh')
check('mesh loads', rc == 0 and out['colors'] == mesh['colors'], out)
for name, key, val in [('colour count', 'colors', [1, 1, 1, 1] * 2), ('colour stride', 'colors', [1] * 13),
                       ('position stride', 'positions', [0] * 10), ('uv stride', 'uv', [0] * 7)]:
    rc, err = resave(dict(mesh, **{key: val}), '.mesh')
    check(f'mesh {name} refused', rc != 0, err)
rc, err = resave(dict(mesh, version=2), '.mesh')
check('mesh version 2 refused', rc != 0, err)

print('FAILED' if fails else 'all passed')
sys.exit(1 if fails else 0)
