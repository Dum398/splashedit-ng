# Clips from a second model file and clip renames on a skin, driven through
# the editor (#237). A project holds skinimport/rig_host.gltf (the rig with
# Wave) imported, and Hero drawing its mesh with a skin playing Wave. The
# Skinned animation section's "Add clips from model..." field
# (obj.skinaddclips) takes skinimport/rig_nod.gltf dropped on it: its Nod/Big
# joins the skin, imported by joint name into models/rig_host/clips/. Each
# clip row's name (obj.skinclip.<i>) renames on a double-click; the name goes
# into the .anim and, as the clip came from a model import, the host's sidecar.
# Under scripted runs the editor prints
#   skinclips: added <n> left <n> refused <n>   (and a line per refused file)
#   skinclip: renamed <old> -> <new> | skinclip: rename refused: <why>
#   skin: clip <name> frames <f> bytes <n> loops|once   (the live export)
# usage: python3 multiclipguicheck.py <splashedit> [shot.png]   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
rigdir = os.path.join(here, 'skinimport')
cli = os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
NOD = 'models/rig_host/clips/rig_nod_gltf/Nod_Big.anim'
WAVE = 'models/rig_host/Wave.anim'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    """rig_host.gltf imported (by splashpack-cli, beside this editor) with a sidecar, Hero drawing it."""
    root = tempfile.mkdtemp()
    project = os.path.join(root, 'game')
    os.makedirs(os.path.join(project, 'models'))
    model = os.path.join(project, 'models', 'rig_host.gltf')
    shutil.copyfile(os.path.join(rigdir, 'rig_host.gltf'), model)
    json.dump({}, open(model + '.import.json', 'w'))
    r = subprocess.run([cli, 'import', model, '--project', project], capture_output=True, text=True)
    if r.returncode != 0: sys.exit('splashpack-cli import failed: ' + r.stderr)
    scene = json.load(open(os.path.join(rigdir, 'skin_saved.scene'), encoding='utf-8'))
    scene['settings'].pop('script', None)
    hero = scene['objects'][0]
    hero['name'] = 'Hero'
    hero['transform']['position'] = [0.0, 0.0, 3.0]
    hero['components'][0]['mesh'] = 'models/rig_host.mesh'
    hero['components'][1]['clips'] = [WAVE]
    json.dump(scene, open(os.path.join(project, 'game.scene'), 'w'), indent=2)
    # The second file, outside the project, as a download would be.
    src = os.path.join(root, 'downloads')
    os.makedirs(src)
    for n in ('rig_nod.gltf', 'rig_alien.gltf'): shutil.copyfile(os.path.join(rigdir, n), os.path.join(src, n))
    return project, src


def run(name, args, shot=None, project=None):
    if project is None: project, src = project_copy()
    else: src = os.path.join(os.path.dirname(project), 'downloads')
    shot = shot or os.path.join(project, name + '.png')
    args = [a.replace('{src}', src) for a in args]
    cmd = [editor, '--size', '1600x960', '--project', project, '--scene', os.path.join(project, 'game.scene'),
           '--select', 'Hero'] + W(5) + ['--wait-export', '--scroll-to', '@obj.skinaddclips'] + W(4) + args + W(3) + \
          ['--wait-export'] + W(3) + ['--move', '650,20'] + W(5) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=240)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l or 'still behind' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    clips = []
    for l in lines:
        if re.match(r'skin: Hero bones ', l): clips.append([])
        m = re.match(r'skin: clip (\S+) frames (\d+) bytes \d+ (loops|once)$', l)
        if m and clips: clips[-1].append(m.group(1))
    said = [l for l in lines if l.startswith('skinclips: ') or l.startswith('skinclip: ')]
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return project, clips, said, rects, shot


def anim_name(project, rel):
    return json.load(open(os.path.join(project, rel), encoding='utf-8'))['name']


ADD = ['--drop-now', '{src}/rig_nod.gltf@@obj.skinaddclips'] + W(6)
RENAME = lambda i, text: ['--scroll-to', '@obj.skinclip.%d' % i] + W(4) + ['--dblclick', '@obj.skinclip.%d' % i] + W(3) + ['--key', 'ctrl+a', '--text', text, '--key', 'enter'] + W(4)

# 1. Dropping rig_nod.gltf on the field adds Nod_Big; renaming it "nod" and
# saving: the scene, the .anim, the sidecar and the export all say so.
project, clips, said, rects, shot = run('add', ADD + RENAME(1, 'nod') + ['--key', 'ctrl+s'] + W(5) + ['--wait-export', '--scroll-to', '@obj.skininfo'] + W(4))
check('the drop adds one clip, none refused', 'skinclips: added 1 left 0 refused 0' in said, said)
check('the rename is reported', 'skinclip: renamed Nod_Big -> nod' in said, said)
check('the export bakes Wave and nod', clips[-1:] == [['Wave', 'nod']], clips)
scene = json.load(open(os.path.join(project, 'game.scene'), encoding='utf-8'))
skin = scene['objects'][0]['components'][1]
check('the saved scene lists both clips and the clip source',
      skin.get('clips') == [WAVE, NOD] and skin.get('clipSources') == ['models/rig_nod.gltf'], skin)
check('the source was copied into models/ with a sidecar naming where it came from',
      json.load(open(os.path.join(project, 'models', 'rig_nod.gltf.import.json'))).get('source', '').endswith('rig_nod.gltf'))
check('the .anim carries the new name', anim_name(project, NOD) == 'nod')
side = json.load(open(os.path.join(project, 'models', 'rig_host.gltf.import.json')))
check("the host's sidecar keeps it for the next import", side.get('clipNames') == {'clips/rig_nod_gltf/Nod_Big': 'nod'}, side)
check('a row per clip, in the panel', all(k in rects for k in ('obj.skinclip.0', 'obj.skinclip.1', 'obj.skinloop.1')),
      sorted(k for k in rects if k.startswith('obj.skin')))
if keep: shutil.copyfile(shot, keep)

# 2. On that saved project: renaming Wave over 24 characters or to another
# clip's name is refused; a good name sticks; reimporting the host (Import
# settings changed on disk: the watcher) keeps both names.
_, clips, said, _, _ = run('refuse', RENAME(0, 'a_name_longer_than_24_chars') + RENAME(0, 'NOD') + RENAME(0, 'walk') +
                           ['--key', 'ctrl+s'] + W(5), project=project)
check('over 24 characters is refused', any(l.startswith('skinclip: rename refused:') and 'longer than 24' in l for l in said), said)
check('a name another clip has is refused', any(l.startswith("skinclip: rename refused: another clip of this skin is named 'nod'") for l in said), said)
check('walk is taken', 'skinclip: renamed Wave -> walk' in said and anim_name(project, WAVE) == 'walk', said)
check('the export bakes walk and nod', clips[-1:] == [['walk', 'nod']], clips)
# The host file touched: ModelWatcher imports it again.
model = os.path.join(project, 'models', 'rig_host.gltf')
written = os.stat(os.path.join(project, WAVE)).st_mtime_ns, os.stat(os.path.join(project, NOD)).st_mtime_ns
_, clips, said, _, _ = run('reimport', ['--copy-file', model + '@' + model + '.new'] + W(2) + ['--copy-file', model + '.new@' + model] + W(90) +
                           ['--wait-import'] + W(10), project=project)
check('the host and its added clips were written again',
      os.stat(os.path.join(project, WAVE)).st_mtime_ns != written[0] and os.stat(os.path.join(project, NOD)).st_mtime_ns != written[1])
check('after the host is imported again both names stay', (anim_name(project, WAVE), anim_name(project, NOD)) == ('walk', 'nod'),
      (anim_name(project, WAVE), anim_name(project, NOD)))
check('and the export still bakes walk and nod', clips[-1:] == [['walk', 'nod']], clips)

# The file the clips came from changed where it was downloaded: the watcher
# copies it in and its clips are imported again, the name kept.
down = os.path.join(os.path.dirname(project), 'downloads', 'rig_nod.gltf')
written = os.stat(os.path.join(project, NOD)).st_mtime_ns
_, clips, said, _, _ = run('source', ['--copy-file', down + '@' + down + '.new'] + W(2) + ['--copy-file', down + '.new@' + down] + W(90),
                           project=project)
check('a change to the source file imports its clips again', 'skinclips: refreshed rig_nod.gltf objects 0 problems 0' in said and
      os.stat(os.path.join(project, NOD)).st_mtime_ns != written, said)
check('keeping the name', anim_name(project, NOD) == 'nod' and clips[-1:] == [['walk', 'nod']], (anim_name(project, NOD), clips))

# 3. Undo puts the old name back in both files.
project, clips, said, _, _ = run('undo', ADD + RENAME(1, 'nod') + ['--key', 'ctrl+z'] + W(5))
check('Ctrl+Z after the rename gives Nod_Big back', anim_name(project, NOD) == 'Nod_Big', anim_name(project, NOD))
side = json.load(open(os.path.join(project, 'models', 'rig_host.gltf.import.json')))
check('and the sidecar as it was', 'clipNames' not in side, side)
check('the export bakes Wave and Nod_Big', clips[-1:] == [['Wave', 'Nod_Big']], clips)

# 4. A file with other joint names is refused, and says why.
project, clips, said, _, _ = run('alien', ['--drop-now', '{src}/rig_alien.gltf@@obj.skinaddclips'] + W(6))
check('rig_alien.gltf is refused', any(l.startswith('skinclips: error') or ('refused rig_alien.gltf shares no joint' in l) for l in said), said)
check('and not copied into the project', not os.path.exists(os.path.join(project, 'models', 'rig_alien.gltf')))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
