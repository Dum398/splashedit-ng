# Palette groups (#215), driven through the GUI on a courtyard copy where only
# Floor (stone.png) and Back Wall (brick.png) keep a texture, both at 8 bpp.
# The inspector draws a Palette group field (obj.palgroup.<i>) under a 4 or 8
# bpp material's texture; typing "walls" into it on both objects saves
# "paletteGroup": "walls" on each, and each then prints "palgroup: 0 walls
# members 1 colours <n>" (it sees the other). splashpack-cli exports the saved
# scene with one CLUT for the two (two without the group), in the splashpack
# header and in the .vram. One Ctrl+Z takes the group off again.
# usage: python3 palgroupcheck.py <splashedit> [keep dir for screenshots]   (needs a display: xvfb-run -a)
# splashpack-cli is taken from beside the editor (build/cli/splashpack-cli).
import json, os, re, shutil, struct, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
keep = sys.argv[2] if len(sys.argv) > 2 else None
cli = os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
KEEP = ('Floor', 'Back Wall')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def walk(objs):
    for o in objs:
        yield o
        yield from walk(o.get('children', []))


def meshes(d):
    """Each object's mesh component, by name."""
    return {o['name']: c for o in walk(d['objects']) for c in o.get('components', []) if c['type'] == 'mesh'}


def two_textured(d):
    for name, mesh in meshes(d).items():
        mesh['bitDepth'] = 8
        for m in mesh['materials']:
            if name not in KEEP: m['texture'] = None
            m.pop('bitDepth', None)


rects = {}  # the last run's named controls


def run(args, shot=None):
    """Runs the editor on a fresh fixture with Floor selected, then `args`
    and Ctrl+S; returns the project, the saved scene and the output lines."""
    global rects
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    d = json.load(open(scene, encoding='utf-8'))
    two_textured(d)
    json.dump(d, open(scene, 'w', encoding='utf-8'), indent=2)
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Floor'] + W(5) + args + \
        ['--key', 'ctrl+s'] + W(10) + ['--move', '760,940'] + W(5) + ['--screenshot', shot or os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    rects = {}
    for l in lines:
        m = re.match(r'control (\S+(?: \S+)*) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m: rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return project, json.load(open(scene, encoding='utf-8')), [l for l in lines if not l.startswith('control ')]


def inside(name, outer='obj.panel'):
    a, b = rects.get(name), rects.get(outer)
    return bool(a and b and a[0] >= b[0] and a[1] >= b[1] and a[2] <= b[2] and a[3] <= b[3])


def cluts(project):
    """splashpack-cli export of the saved scene: (header clutCount, .vram CLUT count)."""
    out = os.path.join(project, 'out', 'scene.splashpack')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run([cli, 'export', os.path.join(project, 'courtyard.scene'), '-o', out], capture_output=True, text=True)
    if r.returncode != 0: return ('export failed', r.stdout + r.stderr)
    pack = open(out, 'rb').read()
    vram = open(out[:-len('.splashpack')] + '.vram', 'rb').read()
    return struct.unpack_from('<H', pack, 10)[0], struct.unpack_from('<H', vram, 4)[0]


def groups(d):
    return {n: [m.get('paletteGroup') for m in meshes(d)[n]['materials']] for n in KEEP}


GROUP = ['--click', '@obj.palgroup.0'] + W(3) + ['--text', 'walls'] + W(2) + ['--key', 'enter'] + W(5)
shot = os.path.join(keep, 'palgroup-inspector.png') if keep else None

# --- The group on both objects, then Floor shown again ---
project, d, lines = run(GROUP + ['--click', '@tree.Back Wall'] + W(5) + GROUP + ['--click', '@tree.Floor'] + W(8), shot)
check('a Palette group row under the texture, inside the inspector', inside('obj.palgroup.0') and inside('obj.texture') and
      rects['obj.palgroup.0'][1] >= rects['obj.texture'][3], (rects.get('obj.texture'), rects.get('obj.palgroup.0')))
check('its caption is drawn under it', inside('obj.palgroupinfo.0') and rects['obj.palgroupinfo.0'][1] >= rects['obj.palgroup.0'][3],
      rects.get('obj.palgroupinfo.0'))
check('"walls" is saved on both materials', groups(d) == {'Floor': ['walls'], 'Back Wall': ['walls']}, groups(d))
said = [l for l in lines if l.startswith('palgroup: ')]
first = re.compile(r'palgroup: 0 walls members 0 colours \d+$')
shared = re.compile(r'palgroup: 0 walls members 1 colours (\d+)$')
check('Floor alone in the group first reports members 0', said[:1] and first.match(said[0]), said)
check('each object then reports members 1 (it sees the other)', sum(bool(shared.match(l)) for l in said) == 2 and
      said[-1:] and shared.match(said[-1]), said)
colours = [int(shared.match(l).group(1)) for l in said if shared.match(l)]
check('the shared palette has colours, the same for both', colours and 0 < colours[0] <= 256 and len(set(colours)) == 1, said)
check('the export has one CLUT for the two', cluts(project) == (1, 1), cluts(project))

# --- Without the group: one CLUT each; one Ctrl+Z takes it off ---
project, d, lines = run([])
check('the control: without a group the export has two CLUTs', cluts(project) == (2, 2), cluts(project))
check('the control: no paletteGroup key is saved', groups(d) == {'Floor': [None], 'Back Wall': [None]}, groups(d))
project, d, lines = run(GROUP + ['--key', 'ctrl+z'] + W(5))
check('one Ctrl+Z takes the group off', groups(d) == {'Floor': [None], 'Back Wall': [None]}, groups(d))

print('palgroupcheck: %s' % ('all passed' if not fails else '%d failed' % fails))
sys.exit(1 if fails else 0)
