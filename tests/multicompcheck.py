# Multi-object editing of the Mesh, Script and Audio components, driven
# through the GUI. A copy of the courtyard gets three props, PropA/PropB/PropC,
# last in the Scene panel, each with a mesh (flat lit), a script and an audio
# clip: PropB differs in bit depth, shadow, script, clip, sample rate, loop
# and flat colour; PropC has no clip Name. With the three selected, the
# multi inspector shows Mesh, Script and Audio sections instead of naming them
# in its "select one object" card; fields that differ read "mixed", and one
# edit of each kind (number, switch, dropdown, colour, file dropped from the
# Project panel) sets every prop, and one Ctrl+Z puts every one back. A clip
# dropped on the field leaves each prop's clip Name as it was.
# usage: python3 multicompcheck.py <splashedit> [keep-dir]   (needs a display: xvfb-run -a)
# keep-dir, when given, receives inspector.png (the multi inspector, Mesh to Audio).
import json, os, re, shutil, struct, subprocess, sys, tempfile, wave

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
# At 1600x960 with the Project panel at its default height (assetbrowsercheck's).
SEARCH = '880,731'  # the panel's search field
TILE = '530,812'    # the first file in the grid
PROPS = {
    'PropA': {'bpp': 8, 'shadow': None, 'flat': [200, 40, 40], 'lua': 'scripts/alpha.lua', 'clip': 'audio/boom.wav', 'name': 'boom',
              'rate': 22050, 'loop': True},
    'PropB': {'bpp': 16, 'shadow': 'blob', 'flat': [40, 200, 40], 'lua': 'scripts/beta.lua', 'clip': 'audio/hiss.wav', 'name': 'hiss',
              'rate': 16000, 'loop': False},
    'PropC': {'bpp': 8, 'shadow': None, 'flat': [200, 40, 40], 'lua': 'scripts/alpha.lua', 'clip': 'audio/boom.wav', 'name': '',
              'rate': 22050, 'loop': True},
}


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def write_wav(path, frames):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes(b''.join(struct.pack('<h', (i * 397) % 8000 - 4000) for i in range(frames)))


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    os.makedirs(os.path.join(project, 'audio'), exist_ok=True)
    for n, frames in (('boom', 2205), ('hiss', 4410), ('tick', 1100)):
        write_wav(os.path.join(project, 'audio', n + '.wav'), frames)
    for n in ('alpha', 'beta', 'zap'):
        open(os.path.join(project, 'scripts', n + '.lua'), 'w').write('function onCreate(self)\nend\n')
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    for i, (name, p) in enumerate(PROPS.items()):
        mesh = {'type': 'mesh', 'mesh': 'meshes/crate.mesh', 'materials': [{'texture': 'textures/crate.png', 'color': [1, 1, 1, 1]}],
                'bitDepth': p['bpp'], 'vertexColors': 'flat', 'flatColor': p['flat'], 'smoothNormals': True, 'uvOffsetMaterial': 0,
                'dynamicLighting': 'auto'}
        if p['shadow']: mesh.update({'shadow': p['shadow'], 'shadowSize': 0.8})
        s['objects'].append({'name': name, 'active': True,
                             'transform': {'position': [i * 2.0 - 2, 0.5, 4.0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                             'components': [mesh, {'type': 'script', 'lua': p['lua']},
                                            {'type': 'audio', 'clip': p['clip'], 'clipName': p['name'], 'sampleRate': p['rate'],
                                             'loop': p['loop'], 'defaultVolume': 100, 'trimLeadingSilence': False}],
                             'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


W = lambda n: ['--wait', str(n)]
mid = None  # with SAVE_MID in the script: the props as saved at that point


def props(s):
    """name -> {field: value} of each prop's mesh, script and audio in a saved scene."""
    out = {}
    for o in s['objects']:
        if o['name'] not in PROPS: continue
        c = {k['type']: k for k in o.get('components', [])}
        m, a = c.get('mesh', {}), c.get('audio', {})
        out[o['name']] = {'bpp': m.get('bitDepth'), 'lighting': m.get('vertexColors'), 'flat': m.get('flatColor'),
                          'smooth': m.get('smoothNormals'), 'dyn': m.get('dynamicLighting'), 'shadow': m.get('shadow', 'off'),
                          'shsize': round(float(m.get('shadowSize', 0.5)), 4), 'shdark': round(float(m.get('shadowDarkness', 60)), 4),
                          'mesh': m.get('mesh'), 'lua': c.get('script', {}).get('lua'), 'clip': a.get('clip'),
                          'name': a.get('clipName'), 'rate': a.get('sampleRate'), 'loop': a.get('loop'), 'vol': a.get('defaultVolume'),
                          'trim': a.get('trimLeadingSilence')}
    return out


def run(name, args, save=True):
    """A fresh copy with PropA selected, then `args` (and Ctrl+S); returns the
    saved props, the output lines, the named controls' rects and the screenshot."""
    global mid
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    snap = os.path.join(os.path.dirname(project), 'mid.scene')
    args = [a.replace('{scene}', scene).replace('{snap}', snap) for a in args]
    shot = os.path.join(project, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'PropA'] + W(20) + args + \
        (['--key', 'ctrl+s'] + W(10) if save else []) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    mid = props(json.load(open(snap, encoding='utf-8'))) if os.path.exists(snap) else None
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return props(json.load(open(scene, encoding='utf-8'))), lines, rects, shot


def tree(name):
    return ['--click', '@tree.' + name] + W(3)


def typed(name, text):
    return ['--dblclick', '@' + name] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(3)


def click(name, dx=0, dy=0):
    return ['--click', '@%s,%d,%d' % (name, dx, dy)] + W(4)


def shown(lines, field):
    """What the multi inspector last reported for `field`."""
    v = None
    for l in lines:
        if l.startswith('inspector: ' + field + ' '): v = l[len('inspector: ' + field + ' '):]
    return v


def centre(rects, n):
    r = rects.get(n)
    return r and '%d,%d' % ((r[0] + r[2]) // 2, (r[1] + r[3]) // 2)


ALL3 = ['--keydown', 'shift'] + W(2) + tree('PropC') + ['--keyup', 'shift'] + W(5)  # PropA to PropC
UNDO = ['--key', 'ctrl+z'] + W(5)
SAVE_MID = ['--key', 'ctrl+s'] + W(10) + ['--copy-file', '{scene}@{snap}'] + W(2)
ORIG, _, _, _ = run('orig', [], save=True)
print('original:', ORIG)
check('the three props load', sorted(ORIG) == sorted(PROPS), ORIG)


def same(field, value):
    return mid is not None and all(mid[n][field] == value for n in PROPS)


def kept_but(s, *fields):
    """Every field of every prop in `s` as in ORIG, but `fields`."""
    return s is not None and all({k: v for k, v in s[n].items() if k not in fields} ==
                                 {k: v for k, v in ORIG[n].items() if k not in fields} for n in PROPS)


# Mixed values, and the sections drawn (the screenshot).
_, lines, rects, shot = run('mixed', ALL3, save=False)
mixed_rects = rects
check('the multi inspector is drawn', 'obj.multi' in rects, sorted(k for k in rects if k.startswith('obj.'))[:10])
for sec, field in (('Mesh', 'bpp'), ('Script', 'lua'), ('Audio', 'clip')):
    check('a %s section is drawn for the three' % sec, shown(lines, field) is not None, field)
check('the single-only card no longer names Mesh, Script or Audio',
      shown(lines, 'singleonly') is not None and not any(c in shown(lines, 'singleonly') for c in ('Mesh', 'Script', 'Audio')),
      shown(lines, 'singleonly'))
check('differing bit depths show "mixed"', shown(lines, 'bpp') == 'mixed', shown(lines, 'bpp'))
check('differing shadows show "mixed"', shown(lines, 'shadow') == 'mixed', shown(lines, 'shadow'))
check('a shared lighting shows itself (Flat)', shown(lines, 'lighting') == 'Flat', shown(lines, 'lighting'))
check('a shared runtime lighting shows itself (Auto)', shown(lines, 'dynlight') == 'Auto', shown(lines, 'dynlight'))
check('differing flat colours show "mixed"', shown(lines, 'flatcolor') == 'mixed', shown(lines, 'flatcolor'))
check('a shared switch shows itself (smooth normals on)', shown(lines, 'smooth') == 'on', shown(lines, 'smooth'))
check('differing scripts show "mixed"', shown(lines, 'lua') == 'mixed', shown(lines, 'lua'))
check('differing clips show "mixed"', shown(lines, 'clip') == 'mixed', shown(lines, 'clip'))
check('differing sample rates show "mixed"', shown(lines, 'arate') == 'mixed', shown(lines, 'arate'))
check('differing loops show "mixed"', shown(lines, 'aloop') == 'mixed', shown(lines, 'aloop'))
check('with a shadow Off among them, no shadow Size or Darkness', 'obj.shadowsize' not in rects and 'obj.shadowdark' not in rects,
      [k for k in rects if k.startswith('obj.shadow')])
check('no clip Name field with several selected', 'obj.aname' not in rects, [k for k in rects if k.startswith('obj.a')])
check('no Volume field (export never reads it)', 'obj.avol' not in rects, [k for k in rects if k.startswith('obj.a')])
for n in ('obj.bpp', 'obj.lighting', 'obj.flatcolor', 'obj.smooth', 'obj.dynlight', 'obj.shadow', 'obj.lua', 'obj.clip', 'obj.arate',
          'obj.aloop', 'obj.atrim'):
    check(n + ' is drawn', n in rects, sorted(k for k in rects if k.startswith('obj.')))
if keep:
    shutil.copy(shot, os.path.join(keep, 'inspector.png'))

# Number: a typed sample rate sets all three; one Ctrl+Z puts every one back.
s, _, _, _ = run('rate', ALL3 + typed('obj.arate', '11025') + SAVE_MID + UNDO)
print('rate typed:', mid)
check('typed sample rate 11025 sets all three', same('rate', 11025), mid)
check('the rate edit leaves everything else', kept_but(mid, 'rate'), mid)
check('one Ctrl+Z restores every sample rate', s == ORIG, s)

# Switch: Loop is mixed, a click turns it on for all.
s, _, _, _ = run('loop', ALL3 + click('obj.aloop') + SAVE_MID + UNDO)
print('loop clicked:', mid)
check('a click on the mixed Loop turns it on for all three', same('loop', True), mid)
check('one Ctrl+Z restores every loop', s == ORIG, s)
s, _, _, _ = run('smooth', ALL3 + click('obj.smooth') + SAVE_MID + UNDO)
check('a click on Smooth normals turns it off for all three', same('smooth', False), mid)
check('one Ctrl+Z restores every smooth normals', s == ORIG, s)

# Dropdowns: bit depth (mixed) to 4 bpp; shadow (mixed) to Blob, then Size and Darkness show.
s, _, _, _ = run('bpp', ALL3 + click('obj.bpp') + click('bpp/4 bpp') + SAVE_MID + UNDO)
print('bpp picked:', mid)
check('4 bpp picked sets all three', same('bpp', 4), mid)
check('the bit depth edit leaves everything else', kept_but(mid, 'bpp'), mid)
check('one Ctrl+Z restores every bit depth', s == ORIG, s)
s, lines, rects, _ = run('shadow', ALL3 + click('obj.shadow') + click('shadow/Blob') + SAVE_MID, save=False)
print('shadow picked:', mid)
check('Blob picked sets all three', same('shadow', 'blob'), mid)
check('PropB keeps its own blob size', mid is not None and mid['PropB']['shsize'] == 0.8, mid)
check('with every shadow Blob, Size and Darkness show', 'obj.shadowsize' in rects and 'obj.shadowdark' in rects,
      [k for k in rects if k.startswith('obj.shadow')])
s, _, _, _ = run('shadowundo', ALL3 + click('obj.shadow') + click('shadow/Blob') + UNDO)
check('one Ctrl+Z restores every shadow', s == ORIG, s)
s, _, _, _ = run('runtime', ALL3 + click('obj.dynlight') + click('dynlight/Off') + SAVE_MID + UNDO)
check('runtime lighting Off picked sets all three', same('dyn', 'off'), mid)
check('one Ctrl+Z restores every runtime lighting', s == ORIG, s)

# Colour: a click in the flat colour picker sets one colour on all three.
s, _, _, _ = run('colour', ALL3 + click('obj.flatcolor') + click('obj.flatcolorpick', -40, -60) + ['--key', 'escape'] + W(3) +
                 SAVE_MID + UNDO)
print('colour picked:', mid)
check('the picked flat colour is the same on all three', mid is not None and all(mid[n]['flat'] == mid['PropA']['flat'] for n in PROPS),
      mid)
check('the picked flat colour is a new one', mid is not None and mid['PropA']['flat'] not in ([200, 40, 40], [40, 200, 40]), mid)
check('one Ctrl+Z restores every flat colour', s == ORIG, s)

# Files dropped from the Project panel: a script and a clip on all three.
lua_at, clip_at = centre(mixed_rects, 'obj.lua'), centre(mixed_rects, 'obj.clip')
if lua_at and clip_at:
    s, _, _, _ = run('script', ALL3 + ['--click', SEARCH + ',0', '--text', 'zap'] + W(5) + ['--drag', TILE + ',' + lua_at + ',0'] + W(10) +
                     SAVE_MID + UNDO)
    print('script dropped:', mid)
    check('zap.lua dropped on the File field sets it on all three', same('lua', 'scripts/zap.lua'), mid)
    check('the script drop leaves everything else', kept_but(mid, 'lua'), mid)
    check('one Ctrl+Z restores every script', s == ORIG, s)
    s, _, _, _ = run('clip', ALL3 + ['--click', SEARCH + ',0', '--text', 'tick'] + W(5) + ['--drag', TILE + ',' + clip_at + ',0'] + W(10) +
                     SAVE_MID + UNDO)
    print('clip dropped:', mid)
    check('tick.wav dropped on the Clip field sets it on all three', same('clip', 'audio/tick.wav'), mid)
    check('the clip drop leaves every clip Name as it was (PropC\'s empty)',
          mid is not None and all(mid[n]['name'] == PROPS[n]['name'] for n in PROPS), mid)
    check('the clip drop leaves everything else', kept_but(mid, 'clip'), mid)
    check('one Ctrl+Z restores every clip', s == ORIG, s)
else:
    check('the File and Clip fields found for the drops', False, sorted(k for k in rects if k.startswith('obj.')))

print('FAILS=%d' % fails)
sys.exit(1 if fails else 0)
