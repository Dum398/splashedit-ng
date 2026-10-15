# What the object inspector says about a mesh's colours and an audio clip,
# driven through the GUI. A mesh without vertex colours set to Mesh colours
# shows the "No mesh colours" card (obj.fixmeshcol) under Lighting, whose fix
# (obj.fixmeshcol.fix) switches it to Baked vertex; an Audio source with a
# readable clip shows a Source row (obj.ainfo) with the WAV's format and the
# sound RAM the exported clip takes. Under scripted runs the editor prints
# "mesh: colours missing <mesh>" while the card shows and
# "audio: source <rate> <seconds> <channels> adpcm <bytes>" as either changes.
# usage: python3 inspectorinfocheck.py <splashedit> [mesh.png] [audio.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep_mesh = sys.argv[2] if len(sys.argv) > 2 else None
keep_audio = sys.argv[3] if len(sys.argv) > 3 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')

# The clip: 16-bit stereo at 44100 Hz, 103635 frames (2.35 s).
RATE, CHANNELS, FRAMES = 44100, 2, 103635


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def write_wav(path, rate, channels, frames):
    data = bytearray()
    for i in range(frames * channels):
        data += struct.pack('<h', (i * 131) % 8000 - 4000)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) +
                b'data' + struct.pack('<I', len(data)) + bytes(data))


def resampled(n, src, dst):
    """splash::resampledCount: llround(n * dst / src), n when the rates match."""
    if src == dst or n == 0: return n
    v = n * (dst / src)
    return int(v + 0.5)


def adpcm_bytes(samples, loop):
    """splash::spuAdpcmBytes: a silent lead block, one 16-byte block per 28
    samples, a trap block unless looping, padded to 64 bytes."""
    b = 16 + (samples + 27) // 28 * 16 + (0 if loop else 16)
    return (b + 63) // 64 * 64


def obj(name, pos, components):
    return {'name': name, 'active': True, 'transform': {'position': list(pos), 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
            'components': components, 'children': []}


def project_copy():
    """The courtyard plus Grey (the crate mesh, which has no vertex colours,
    set to Mesh colours) and Bell (an Audio source playing sounds/bell.wav)."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    os.makedirs(os.path.join(project, 'sounds'))
    write_wav(os.path.join(project, 'sounds', 'bell.wav'), RATE, CHANNELS, FRAMES)
    crate = json.load(open(os.path.join(project, 'meshes', 'crate.mesh'), encoding='utf-8'))
    assert 'colors' not in crate, 'the fixture needs a mesh without vertex colours'
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append(obj('Grey', (0, 0.5, 6), [{'type': 'mesh', 'mesh': 'meshes/crate.mesh', 'materials': [{'texture': '', 'color': [1, 1, 1, 1]}],
                                                    'bitDepth': 8, 'vertexColors': 'mesh', 'flatColor': [128, 128, 128],
                                                    'smoothNormals': True, 'uvOffsetMaterial': 0}]))
    s['objects'].append(obj('Bell', (0, 1, 7), [{'type': 'audio', 'clip': 'sounds/bell.wav', 'clipName': 'bell',
                                                 'sampleRate': 22050, 'loop': False, 'defaultVolume': 100,
                                                 'trimLeadingSilence': False}]))
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(select, args, shot=None):
    """Runs the editor on a fresh copy with `select` selected, then `args` and
    Ctrl+S; returns the saved scene, the printed mesh:/audio: lines and the
    named controls' rects at the end."""
    project = project_copy()
    scene = os.path.join(project, 'courtyard.scene')
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select, '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '5', '--move', '760,940', '--wait', '5', '--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(select + ': every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return json.load(open(scene, encoding='utf-8')), [l for l in lines if l.startswith(('mesh: ', 'audio: '))], rects


def comp(scene, name, kind):
    for o in scene['objects']:
        if o['name'] == name:
            return next(c for c in o['components'] if c['type'] == kind)


def inside(rects, name):
    r, p = rects.get(name), rects.get('obj.panel')
    return r is not None and p is not None and p[0] <= r[0] and r[2] <= p[2] and p[1] <= r[1] and r[3] <= p[3]


click = lambda name: ['--click', '@' + name, '--wait', '3']
UNDO = ['--key', 'ctrl+z', '--wait', '3']
MISSING = 'mesh: colours missing meshes/crate.mesh'

# Mesh colours on a mesh without them: the card, in the panel, and the line.
tmp = tempfile.mkdtemp()
_, lines, rects = run('Grey', [], shot=os.path.join(tmp, 'mesh.png'))
check('Grey prints that its mesh colours are missing', lines == [MISSING], lines)
check('the No mesh colours card lies in the inspector panel', inside(rects, 'obj.fixmeshcol') and inside(rects, 'obj.fixmeshcol.fix'),
      (rects.get('obj.fixmeshcol'), rects.get('obj.fixmeshcol.fix'), rects.get('obj.panel')))
if keep_mesh: shutil.copyfile(os.path.join(tmp, 'mesh.png'), keep_mesh)

# The fix bakes the lighting, and the card goes.
s, lines, rects = run('Grey', click('obj.fixmeshcol.fix'))
check('Use baked lighting sets Grey to Baked vertex', comp(s, 'Grey', 'mesh')['vertexColors'] == 'baked', comp(s, 'Grey', 'mesh'))
check('and the card goes', 'obj.fixmeshcol' not in rects and 's_mesh' in rects, sorted(k for k in rects if k.startswith(('obj.fix', 's_'))))
check('the line is printed once, before the fix', lines == [MISSING], lines)
# Ctrl+Z puts Mesh colours back: the card and the line come back with it.
s, lines, rects = run('Grey', click('obj.fixmeshcol.fix') + UNDO)
check('Ctrl+Z brings the card back', comp(s, 'Grey', 'mesh')['vertexColors'] == 'mesh' and inside(rects, 'obj.fixmeshcol'),
      (comp(s, 'Grey', 'mesh')['vertexColors'], rects.get('obj.fixmeshcol')))
check('and prints the line again', lines == [MISSING, MISSING], lines)

# Objects that are fine say nothing about mesh colours.
_, lines, rects = run('Crate', [])
check('the courtyard Crate (Baked vertex) shows no card and no line', 'obj.fixmeshcol' not in rects and not lines, lines)


def source(rate, loop, trim=False):
    return 'audio: source %d %.2f %d adpcm %d' % (RATE, FRAMES / RATE, CHANNELS, adpcm_bytes(resampled(FRAMES, RATE, rate), loop))


# The Source row: the WAV's header and the clip's size at 22050 Hz, no loop.
_, lines, rects = run('Bell', [], shot=os.path.join(tmp, 'audio.png'))
check('Bell prints its source', lines == [source(22050, False)], (lines, source(22050, False)))
check('the Source row lies in the inspector panel, under the Clip row and above Name',
      inside(rects, 'obj.ainfo') and rects['obj.ainfo'][1] >= rects.get('s_audio', (0, 0, 0, 1 << 30))[3] + 32 and
      rects['obj.ainfo'][3] <= rects.get('obj.aname', (0, 0, 0, 0))[1],
      (rects.get('s_audio'), rects.get('obj.ainfo'), rects.get('obj.aname')))
if keep_audio: shutil.copyfile(os.path.join(tmp, 'audio.png'), keep_audio)

# Loop drops the trap block; a lower rate makes fewer samples.
s, lines, _ = run('Bell', click('obj.aloop'))
check('Loop changes the size to the looping one', comp(s, 'Bell', 'audio')['loop'] is True and
      lines == [source(22050, False), source(22050, True)] and source(22050, False) != source(22050, True),
      (lines, source(22050, True)))
s, lines, _ = run('Bell', ['--dblclick', '@obj.arate', '--wait', '3', '--key', 'ctrl+a', '--text', '11025', '--key', 'enter', '--wait', '3'])
check('11025 Hz changes the size to what it resamples to', comp(s, 'Bell', 'audio')['sampleRate'] == 11025 and
      lines == [source(22050, False), source(11025, False)], (lines, source(11025, False)))

# A missing clip: the Clip card warns and there is no Source row.
project = project_copy()
os.remove(os.path.join(project, 'sounds', 'bell.wav'))
r = subprocess.run([editor, '--size', '1600x960', '--project', project, '--select', 'Bell', '--wait', '5', '--screenshot',
                    os.path.join(project, 'shot.png')], capture_output=True, text=True, env=ENV, timeout=180)
out = (r.stdout + r.stderr).splitlines()
check('a missing clip shows no Source row and no line', not any(l.startswith(('control obj.ainfo ', 'audio: ')) for l in out) and
      any(l.startswith('control obj.fixclip ') for l in out), [l for l in out if 'ainfo' in l or 'fixclip' in l or 'audio:' in l])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
