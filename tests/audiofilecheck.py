# The object inspector's Source row for an Audio source whose clip is an Ogg
# Vorbis or MP3 file (tests/audio/formats: a 22050 Hz mono tone, 5512 frames;
# the MP3 decodes 6912 with its encoder delay and padding). Under scripted runs
# the editor prints "audio: source <rate> <seconds> <channels> adpcm <bytes>"
# when the row shows; a clip it cannot read shows no row and prints nothing.
# usage: python3 audiofilecheck.py <splashedit> [ogg.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
formats = os.path.join(here, 'audio', 'formats')
example = os.path.join(here, '..', 'examples', 'courtyard')
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def adpcm_bytes(samples, loop=False):
    b = 16 + (samples + 27) // 28 * 16 + (0 if loop else 16)
    return (b + 63) // 64 * 64


def obj(name, clip):
    return {'name': name, 'active': True, 'transform': {'position': [0, 1, 7], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
            'components': [{'type': 'audio', 'clip': clip, 'clipName': name.lower(), 'sampleRate': 22050, 'loop': False,
                            'defaultVolume': 100, 'trimLeadingSilence': False}], 'children': []}


def run(select, shot):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    os.makedirs(os.path.join(project, 'sounds'))
    for f in ('tone.ogg', 'tone.mp3'): shutil.copyfile(os.path.join(formats, f), os.path.join(project, 'sounds', f))
    open(os.path.join(project, 'sounds', 'broken.ogg'), 'wb').write(b'OggS but not really')
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'] += [obj('Music', 'sounds/tone.ogg'), obj('Jingle', 'sounds/tone.mp3')]
    if select == 'Broken': s['objects'].append(obj('Broken', 'sounds/broken.ogg'))  # its export error would show in every shot
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', select, '--wait', '8', '--move', '760,940', '--wait', '5',
           '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(select + ': the script found its object', not missing, missing)
    controls = {m.group(1) for m in (re.match(r'control (\S+) at ', l) for l in lines) if m}
    return [l for l in lines if l.startswith('audio: ')], controls


tmp = tempfile.mkdtemp()
lines, controls = run('Music', os.path.join(tmp, 'ogg.png'))
want = 'audio: source 22050 0.25 1 adpcm %d' % adpcm_bytes(5512)
check('an Ogg clip shows its Source row', lines == [want] and 'obj.ainfo' in controls, (lines, 'obj.ainfo' in controls))
if keep: shutil.copyfile(os.path.join(tmp, 'ogg.png'), keep)

lines, controls = run('Jingle', os.path.join(tmp, 'mp3.png'))
want = 'audio: source 22050 0.31 1 adpcm %d' % adpcm_bytes(6912)
check('an MP3 clip shows its Source row', lines == [want] and 'obj.ainfo' in controls, (lines, 'obj.ainfo' in controls))

# Positive control in the same view: the section and its Clip row are drawn.
lines, controls = run('Broken', os.path.join(tmp, 'broken.png'))
check('a broken Ogg clip shows the Audio section but no Source row and no line',
      not lines and 'obj.ainfo' not in controls and 's_audio' in controls and 'obj.aname' in controls,
      (lines, sorted(c for c in controls if c.startswith(('obj.a', 's_audio')))))

print('%d failures' % fails)
sys.exit(1 if fails else 0)
