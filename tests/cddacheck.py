# Build a disc of examples/courtyard with two CD-DA music tracks (--track),
# then read the .cue and .bin back the way a PS1 does and check the audio
# tracks: TRACK 02/03 AUDIO with INDEX 00 (pregap) and INDEX 01 (PCM) at the
# right LBAs, the 150-sector pregap all zero, the ISO volume space size (PVD
# LBA 16, bytes 80-83 LE) equal to the DATA track's length, and each track's
# PCM correlating with an independent decode of the source tone (python wave
# on tone.wav; the FLAC track is the same tone, compared against the wav too).
# usage: python3 cddacheck.py <splashpack-cli>
import os, struct, subprocess, sys, tempfile, wave

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
scene = os.path.join(here, '..', 'examples', 'courtyard', 'courtyard.scene')
fmt = os.path.join(here, 'audio', 'formats')
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def engine(path, marker):
    open(path, 'wb').write(b'PS-X EXE' + bytes(2040) + b'code' * 300 + marker + b'\0' + bytes(100))


engine(os.path.join(tmp, 'cd.ps-exe'), b'SCENE%d/SCENE_%d.SPK;1')

wav = os.path.join(fmt, 'tone.wav')
flac = os.path.join(fmt, 'tone.flac')
binpath = os.path.join(tmp, 'music.bin')
r = subprocess.run([cli, 'disc', scene, '--engine', os.path.join(tmp, 'cd.ps-exe'), '-o', binpath,
                    '--track', wav, '--track', flac], capture_output=True, text=True)
check('disc with --track exits 0', r.returncode == 0, r.stderr)
check('summary names both tracks', 'track 2 ' in r.stdout and 'track 3 ' in r.stdout, r.stdout)
if r.returncode:
    print('%d checks, %d failed' % (checks, fails))
    sys.exit(1)

raw = open(binpath, 'rb').read()
check('whole raw sectors', len(raw) % 2352 == 0, len(raw))
n = len(raw) // 2352


# An independent cue parser: TRACK nn, INDEX nn mm:ss:ff (file-relative LBA).
def parse_cue(text):
    tracks = []
    cur = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('TRACK'):
            parts = line.split()
            cur = {'num': int(parts[1]), 'type': parts[2], 'index': {}}
            tracks.append(cur)
        elif line.startswith('INDEX') and cur is not None:
            parts = line.split()
            mm, ss, ff = (int(x) for x in parts[2].split(':'))
            cur['index'][int(parts[1])] = (mm * 60 + ss) * 75 + ff
    return tracks


cue_bytes = open(os.path.join(tmp, 'music.cue'), 'rb').read()
check('cue CRLF line endings', b'\r\n' in cue_bytes, cue_bytes[:40])
ct = parse_cue(cue_bytes.decode())
check('cue lists 3 tracks', len(ct) == 3, [t['num'] for t in ct])
check('track 1 is data MODE2/2352', ct[0]['num'] == 1 and ct[0]['type'] == 'MODE2/2352', ct[0])
check('tracks 2,3 are AUDIO', len(ct) >= 3 and ct[1]['type'] == 'AUDIO' and ct[2]['type'] == 'AUDIO', [t['type'] for t in ct])

# The PVD volume space size is the DATA track's length: track 2's INDEX 00
# (its pregap starts right after the data track).
user = b''.join(raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(n))
pvd = user[16 * 2048:17 * 2048]
vol = struct.unpack_from('<I', pvd, 80)[0]
data_len = ct[1]['index'][0]  # pregap of track 2 starts at the end of the data track
check('PVD volume space size = data track length', vol == data_len, (vol, data_len))

# INDEX 01 = INDEX 00 + 150 (the pregap); track 3 follows track 2's PCM.
check('track 2 INDEX 01 = INDEX 00 + 150', ct[1]['index'][1] == ct[1]['index'][0] + 150, ct[1]['index'])
check('track 3 INDEX 01 = INDEX 00 + 150', ct[2]['index'][1] == ct[2]['index'][0] + 150, ct[2]['index'])
t2_pcm = ct[1]['index'][1]
t2_len = ct[2]['index'][0] - t2_pcm  # track 2's PCM sectors, up to track 3's pregap
t3_pcm = ct[2]['index'][1]
check('track 2 PCM then track 3 pregap', ct[2]['index'][0] == t2_pcm + t2_len and t2_len > 0, (t2_pcm, t2_len))

# The 150-sector pregap before each audio track is all zero.
for t in (ct[1], ct[2]):
    start = t['index'][0]
    zero = all(raw[(start + k) * 2352:(start + k + 1) * 2352] == bytes(2352) for k in range(150))
    check('track %d pregap is 150 zero sectors' % t['num'], zero, start)


# Independent decode of the source: mono tone at 22050, upsampled to 44100 by
# linear interpolation (a pure tone reconstructs the same whichever resampler).
def ref_44100():
    w = wave.open(wav)
    assert w.getsampwidth() == 2 and w.getnchannels() == 1
    n0 = w.getnframes()
    data = w.readframes(n0)
    src = struct.unpack('<%dh' % n0, data)
    ratio = 44100 / w.getframerate()
    out = []
    i = 0.0
    while True:
        k = int(i)
        if k + 1 >= n0:
            break
        frac = i - k
        out.append(src[k] * (1 - frac) + src[k + 1] * frac)
        i += 1.0 / ratio
    return out


def left_channel(start, sectors):
    b = raw[start * 2352:(start + sectors) * 2352]
    inter = struct.unpack_from('<%dh' % (len(b) // 2), b, 0)
    return list(inter[0::2])  # left of the interleaved stereo


def correlate(ref, got):
    # Best normalised correlation over small lags (resampler phase), over the
    # overlap only so the zero padding at the track's tail does not count.
    best = 0.0
    nref = len(ref)
    for lag in range(0, 200):
        xy = xx = yy = 0.0
        m = min(nref, len(got) - lag)
        if m < nref // 2:
            break
        for i in range(m):
            x = ref[i]
            y = got[i + lag]
            xy += x * y
            xx += x * x
            yy += y * y
        if xx > 0 and yy > 0:
            c = xy / (xx * yy) ** 0.5
            if c > best:
                best = c
    return best


ref = ref_44100()
for t, length in ((ct[1], t2_len), (ct[2], n - t3_pcm)):
    got = left_channel(t['index'][1], length)
    c = correlate(ref, got)
    check('track %d PCM correlates with the source tone' % t['num'], c > 0.99, c)

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
