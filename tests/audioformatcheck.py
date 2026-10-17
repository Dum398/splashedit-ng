# AIFF, MP3, Ogg Vorbis and FLAC clips through splashpack-cli. Each file in
# audio/formats was encoded from tone.wav (a 440 Hz sine, 22050 Hz mono,
# 5512 frames); `splashpack-cli audio` must decode it to the same tone (the
# --pcm output, matched at the best lag up to 2400 samples for the MP3 encoder
# delay), and an export with the Ogg clip on an Audio source must write the
# same SPU data size as the WAV clip.
# usage: python3 audioformatcheck.py <splashpack-cli>
import json, math, os, re, shutil, subprocess, sys, tempfile, wave

cli = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
formats = os.path.join(here, 'audio', 'formats')
tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def samples(path):
    with wave.open(path, 'rb') as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        raw = w.readframes(w.getnframes())
        return w.getframerate(), [int.from_bytes(raw[i:i + 2], 'little', signed=True) for i in range(0, len(raw), 2)]


def match(ref, got):
    best = 0.0
    for lag in range(0, min(2400, len(got)) + 1, 1):
        n = min(len(ref), len(got) - lag)
        if n <= len(ref) // 2: break
        xy = sum(ref[i] * got[i + lag] for i in range(n))
        xx = sum(ref[i] * ref[i] for i in range(n))
        yy = sum(got[i + lag] * got[i + lag] for i in range(n))
        if xx and yy: best = max(best, xy / math.sqrt(xx * yy))
        if best > 0.9999: break
    return best


def encode(src):
    pcm = os.path.join(tmp, os.path.basename(src) + '.pcm.wav')
    r = subprocess.run([cli, 'audio', src, '-o', os.path.join(tmp, 'out.adpcm'), '--rate', '22050', '--pcm', pcm],
                       capture_output=True, text=True, timeout=120)
    return r, pcm


rate, ref = samples(os.path.join(formats, 'tone.wav'))
assert rate == 22050 and len(ref) == 5512
for name, least, exact in [('tone.aiff', 0.9999, True), ('tone.flac', 0.9999, True), ('tone.ogg', 0.99, True), ('tone.mp3', 0.98, False),
                           ('tone.OGG', 0.99, True)]:
    src = os.path.join(formats, name)
    if name == 'tone.OGG':  # an upper-case extension reads the same
        src = os.path.join(tmp, name)
        shutil.copyfile(os.path.join(formats, 'tone.ogg'), src)
    r, pcm = encode(src)
    ok = r.returncode == 0 and re.search(r'rate=22050->22050 ', r.stdout) and os.path.exists(pcm)
    check(name + ': splashpack-cli audio reads it', ok, (r.returncode, r.stdout.strip(), r.stderr.strip()))
    if not ok: continue
    got_rate, got = samples(pcm)
    m = match(ref, got)
    length_ok = len(got) == len(ref) if exact else len(ref) <= len(got) <= len(ref) + 2400
    check('%s: decodes to the tone (match %.5f, %d samples)' % (name, m, len(got)), got_rate == 22050 and length_ok and m >= least)

r, _ = encode(os.path.join(here, 'README.md'))
check('a file that is not audio is refused by extension', r.returncode != 0 and 'not an audio file' in r.stderr, r.stderr.strip())
bad = os.path.join(tmp, 'bad.ogg')
open(bad, 'wb').write(b'OggS but not really')
r, _ = encode(bad)
check('a broken .ogg names its format', r.returncode != 0 and 'not a readable Ogg Vorbis file' in r.stderr, r.stderr.strip())


def export(clip):
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(os.path.join(here, '..', 'examples', 'courtyard'), project)
    os.makedirs(os.path.join(project, 'sounds'))
    shutil.copyfile(os.path.join(formats, clip), os.path.join(project, 'sounds', clip))
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    s['objects'].append({'name': 'Music', 'active': True,
                         'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                         'components': [{'type': 'audio', 'clip': 'sounds/' + clip, 'clipName': 'music', 'sampleRate': 22050,
                                         'loop': False, 'defaultVolume': 100, 'trimLeadingSilence': False}], 'children': []})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    r = subprocess.run([cli, 'export', scene, '-o', os.path.join(project, 'out.splashpack'), '--stats'], capture_output=True, text=True,
                       timeout=300)
    m = re.search(r'spu_file=(\d+)', r.stdout)
    return r, int(m.group(1)) if m else None


rw, spu_wav = export('tone.wav')
ro, spu_ogg = export('tone.ogg')
check('the export with the WAV clip succeeds', rw.returncode == 0 and spu_wav, (rw.returncode, rw.stderr.strip()[-400:]))
check('the export with the Ogg clip succeeds', ro.returncode == 0 and spu_ogg, (ro.returncode, ro.stderr.strip()[-400:]))
check('and writes the same SPU data size (%s vs %s)' % (spu_ogg, spu_wav), spu_wav and spu_ogg == spu_wav and spu_wav > 0)

print('%d failures' % fails)
sys.exit(1 if fails else 0)
