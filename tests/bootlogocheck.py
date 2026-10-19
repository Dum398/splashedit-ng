# Turning off psxsplash's boot logo (the intro and jingle before scene 0),
# driven through the GUI on copies of the courtyard:
#
#  - Build > Boot Logo in Test Runs (per user, on by default, saved with
#    Play's tools as boot_logo=): off, Play boots psxsplash-nologo.ps-exe,
#    the build beside psxsplash.ps-exe without the logo. The editor prints
#    "play: boots <file>" as it starts pcsx-redux (a stand-in here:
#    emulator_tests with SPLASHEDIT_FAKE_REDUX set, as restartcheck.sh uses).
#    With no -nologo build beside the chosen one, the logo build boots and
#    the build report carries one more warning saying so.
#  - The scene inspector's Disc section, Boot logo (per project, on by
#    default, saved in disc.json as "bootLogo": false only when off): off,
#    Export puts psxsplash-cdrom-nologo.ps-exe on the disc as PSX.EXE. The
#    disc's PSX.EXE is read back and compared byte for byte; with no
#    -nologo disc build the logo one goes on, again with one more warning.
#
# The engines are fakes in a temp folder (SPLASHEDIT_PSXSPLASH,
# SPLASHEDIT_DISC_ENGINE) with different bytes, so the file that was used
# is known. Menu items and controls are clicked by name.
# usage: python3 bootlogocheck.py <splashedit> <emulator_tests>   (needs a display: xvfb-run -a)
import json, os, re, shutil, struct, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
fake_redux = os.path.abspath(sys.argv[2])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
work = tempfile.mkdtemp()
# Folding the sections above it brings the Disc section into view at 1600x960 (as discsettingscheck.py).
FOLDS = ['--click', '@s_sequences', '--wait', '3', '--click', '@s_rendering', '--wait', '3', '--click', '@s_loading', '--wait', '3',
         '--click', '@s_vram', '--wait', '5']


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def cd_engine(tag):
    """A stand-in CD-ROM psxsplash (as disccheck.py makes it) whose code is `tag` repeated."""
    return b'PS-X EXE' + bytes(2040) + tag * 300 + b'SCENE%d/SCENE_%d.SPK;1\0' + bytes(100)


def engines(names):
    """A fresh folder holding the fake builds `names` ({file: bytes})."""
    d = tempfile.mkdtemp(dir=work)
    for n, data in names.items():
        with open(os.path.join(d, n), 'wb') as fh:
            fh.write(data)
    return d


def iso_files(path):
    """name -> bytes for every file on a raw Mode 2 Form 1 disc image (exportflipcheck.py's reader)."""
    raw = open(path, 'rb').read()
    sec = [raw[i * 2352 + 24:i * 2352 + 24 + 2048] for i in range(len(raw) // 2352)]

    def read(lba, size):
        return b''.join(sec[lba:lba + (size + 2047) // 2048])[:size]

    def walk(lba, size, prefix):
        data, i, out = read(lba, size), 0, {}
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            ext, sz = struct.unpack_from('<I', data, i + 2)[0], struct.unpack_from('<I', data, i + 10)[0]
            name = data[i + 33:i + 33 + data[i + 32]]
            if name not in (b'\0', b'\1'):
                full = prefix + name.decode()
                if data[i + 25] & 2:
                    out.update(walk(ext, sz, full + '/'))
                else:
                    out[full] = read(ext, sz)
            i += n
        return out

    root = sec[16][156:156 + 34]
    return walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')


def run(args, env, settings=None, files=None, folds=False, export_to=None):
    """Runs the editor on a fresh courtyard copy, with Play's tools in
    `settings` and `files` ({name: bytes}) written into the project first,
    then `args`. Returns all output lines and the project folder."""
    project = os.path.join(tempfile.mkdtemp(dir=work), 'courtyard')
    shutil.copytree(example, project)
    for name, data in (files or {}).items():
        with open(os.path.join(project, name), 'wb') as fh:
            fh.write(data)
    cmd = [editor, '--size', '1600x960', '--project', project, '--play-settings', settings or os.path.join(project, 'play.cfg')]
    if export_to:
        cmd += ['--export-to', export_to]
    cmd += ['--wait', '5'] + (FOLDS if folds else []) + args + ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return lines, project


def boots(lines):
    return [l[len('play: boots '):] for l in lines if l.startswith('play: boots ')]


def warnings(lines, what):
    """The warning counts of the build reports for `what` ("Play", "Export")."""
    out = []
    for l in lines:
        m = re.match(r'build report: %s scenes \d+ errors (\d+) warnings (\d+)$' % what, l)
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


# ---- Play: Build > Boot Logo in Test Runs ----
eng = engines({'psxsplash.ps-exe': b'PS-X EXE logo', 'psxsplash-nologo.ps-exe': b'PS-X EXE nologo'})
play_env = dict(os.environ, SPLASHEDIT_FAKE_REDUX='1', SPLASHEDIT_REDUX=fake_redux,
                SPLASHEDIT_PSXSPLASH=os.path.join(eng, 'psxsplash.ps-exe'), SPLASHEDIT_DISC_ENGINE='/nonexistent/cd.ps-exe')
settings = os.path.join(work, 'prefs', 'play.cfg')
PLAY = ['--key', 'f5', '--wait-play', '--wait', '30']
TOGGLE = ['--click', '@menu.build', '--wait', '3', '--click', '@menu.build/Boot Logo in Test Runs', '--wait', '3']

# Untouched: the logo build boots.
lines, _ = run(PLAY, play_env, settings)
check('by default Play boots psxsplash.ps-exe', boots(lines) == ['psxsplash.ps-exe'], boots(lines))
check('it started', 'play: started' in lines, [l for l in lines if l.startswith('play:')])
base_warn = warnings(lines, 'Play')
check('the default Play reports one build', len(base_warn) == 1 and base_warn[0][0] == 0, base_warn)

# Toggled off from the Build menu, then Play.
lines, _ = run(TOGGLE + PLAY, play_env, settings)
check('Boot Logo in Test Runs off: Play boots psxsplash-nologo.ps-exe', boots(lines) == ['psxsplash-nologo.ps-exe'], boots(lines))
check('the scripted engine line follows the toggle',
      'play: overlays none engine psxsplash.ps-exe' in lines and 'play: overlays none engine psxsplash-nologo.ps-exe' in lines,
      [l for l in lines if l.startswith('play: overlays')])
check('no extra warning when the -nologo build is there', warnings(lines, 'Play') == base_warn, warnings(lines, 'Play'))
check('no menu is left open', 'popups open: no' in lines, [l for l in lines if l.startswith('popups open')])
check('the toggle is no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])
saved = open(settings).read().splitlines() if os.path.exists(settings) else None
check('the settings file holds boot_logo=0', saved is not None and 'boot_logo=0' in saved, saved)

# A second launch on the same settings keeps it off.
lines, _ = run(PLAY, play_env, settings)
check('a second launch still boots psxsplash-nologo.ps-exe', boots(lines) == ['psxsplash-nologo.ps-exe'], boots(lines))

# No -nologo build beside the chosen one: the logo build boots, with a warning saying why.
os.remove(os.path.join(eng, 'psxsplash-nologo.ps-exe'))
lines, _ = run(PLAY, play_env, settings)
check('without psxsplash-nologo.ps-exe Play falls back to psxsplash.ps-exe', boots(lines) == ['psxsplash.ps-exe'], boots(lines))
w = warnings(lines, 'Play')
check('the fallback adds one build warning', len(w) == 1 and base_warn and w[0] == (0, base_warn[0][1] + 1), (w, base_warn))

# On again from the menu.
lines, _ = run(TOGGLE, play_env, settings)
saved = open(settings).read().splitlines() if os.path.exists(settings) else None
check('toggling again saves boot_logo=1', saved is not None and 'boot_logo=1' in saved, saved)

# ---- Export: the Disc section's Boot logo ----
LOGO, NOLOGO = cd_engine(b'LOGO'), cd_engine(b'NOLG')
cds = engines({'psxsplash-cdrom.ps-exe': LOGO, 'psxsplash-cdrom-nologo.ps-exe': NOLOGO})
disc_env = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe',
                SPLASHEDIT_DISC_ENGINE=os.path.join(cds, 'psxsplash-cdrom.ps-exe'))
EXPORT = ['--key', 'ctrl+b', '--wait-play', '--wait', '5']


def export(args, files=None, name='disc'):
    out = os.path.join(work, name + '.bin')
    lines, project = run(args + EXPORT, disc_env, files=files, folds=True, export_to=out)
    exe = iso_files(out).get('PSX.EXE;1') if os.path.exists(out) else None
    path = os.path.join(project, 'disc.json')
    data = json.load(open(path)) if os.path.exists(path) else None
    return lines, exe, data


# Untouched: the logo build goes on the disc; the section shows On.
lines, exe, data = export([], name='default')
check('by default the disc carries psxsplash-cdrom.ps-exe', exe == LOGO, None if exe is None else exe[2048:2060])
check('the Disc section reads On', 'disc logo: on' in lines, [l for l in lines if l.startswith('disc logo')])
check('nothing touched writes no disc.json', data is None, data)
base_disc = warnings(lines, 'Export')

# Switched off in the Disc section, then Export.
lines, exe, data = export(['--click', '@disc.bootlogo', '--wait', '5'], name='off')
check('Boot logo Off: the disc carries psxsplash-cdrom-nologo.ps-exe byte for byte', exe == NOLOGO,
      None if exe is None else exe[2048:2060])
check('the switch is printed', [l for l in lines if l.startswith('disc logo')] == ['disc logo: on', 'disc logo: off'],
      [l for l in lines if l.startswith('disc logo')])
check('disc.json holds bootLogo false', data == {'volume': '', 'licence': '', 'bootLogo': False}, data)
check('no extra warning when the -nologo disc build is there', warnings(lines, 'Export') == base_disc, warnings(lines, 'Export'))
check('the switch is no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])

# A project saved with it off reads Off and exports without the logo.
off = {'disc.json': json.dumps({'volume': '', 'licence': '', 'bootLogo': False}).encode()}
lines, exe, data = export([], files=off, name='saved')
check('a saved Off reads Off', [l for l in lines if l.startswith('disc logo')] == ['disc logo: off'],
      [l for l in lines if l.startswith('disc logo')])
check('a saved Off exports the -nologo build', exe == NOLOGO, None if exe is None else exe[2048:2060])

# No -nologo disc build: the logo build goes on, with a warning saying why.
os.remove(os.path.join(cds, 'psxsplash-cdrom-nologo.ps-exe'))
lines, exe, data = export([], files=off, name='fallback')
check('without psxsplash-cdrom-nologo the disc carries the logo build', exe == LOGO, None if exe is None else exe[2048:2060])
w = warnings(lines, 'Export')
check('the fallback adds one build warning', len(w) == 1 and base_disc and w[0] == (0, base_disc[0][1] + 1), (w, base_disc))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
