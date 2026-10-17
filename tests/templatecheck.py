# The new-project templates (templates/<id>/) as a user gets them: every
# template folder's template.json names it, and its scene exports with the
# CLI with no warnings, carries a Player (so the camera follows it and the
# scene gets nav regions), and attaches at least one Lua and one blocks
# script that exist; each block script converts to Lua. The built-in
# "empty" template has no folder and is not here.
# usage: python3 templatecheck.py <splashpack-cli>
import glob, json, os, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..', 'templates')
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def components(objects):
    for o in objects:
        for c in o.get('components', []):
            yield o['name'], c
        yield from components(o.get('children', []))


ids = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))
check('the shipped templates are first-person and third-person', ids == ['first-person', 'third-person'], ids)
for tid in ids:
    folder = os.path.join(root, tid)
    meta = json.load(open(os.path.join(folder, 'template.json'), encoding='utf-8'))
    check(tid + ': template.json has a name, a description and an integer order',
          isinstance(meta.get('name'), str) and meta['name'] and isinstance(meta.get('description'), str) and
          isinstance(meta.get('order'), int), meta)
    scenes = glob.glob(os.path.join(folder, '*.scene'))
    check(tid + ': one scene at the top', len(scenes) == 1, scenes)
    if len(scenes) != 1:
        continue
    scene = json.load(open(scenes[0], encoding='utf-8'))
    comps = list(components(scene['objects']))
    players = [n for n, c in comps if c.get('type') == 'player']
    check(tid + ': has a Player', len(players) == 1, players)
    scripts = [c.get('lua', '') for _, c in comps if c.get('type') == 'script']
    lua = [s for s in scripts if s.endswith('.lua')]
    blocks = [s for s in scripts if s.endswith('.blocks')]
    check(tid + ': at least one Lua script attached', len(lua) >= 1, scripts)
    check(tid + ': at least one blocks script attached', len(blocks) >= 1, scripts)
    missing = [s for s in scripts if not os.path.isfile(os.path.join(folder, s))]
    check(tid + ': every attached script exists', not missing, missing)
    for b in blocks:
        r = subprocess.run([cli, 'blocks', os.path.join(folder, b)], capture_output=True, text=True)
        check(tid + ': ' + b + ' converts to Lua', r.returncode == 0 and 'function onInteract' in r.stdout,
              r.stderr or r.stdout)
    out = os.path.join(tmp, tid + '.splashpack')
    r = subprocess.run([cli, 'export', scenes[0], '-o', out, '--stats'], capture_output=True, text=True)
    check(tid + ': exports', r.returncode == 0 and os.path.exists(out) and os.path.getsize(out) > 0, r.stderr)
    problems = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith(('warning', 'error'))]
    check(tid + ': no export warnings or errors', not problems and not r.stderr.strip(), problems or r.stderr)
    nav = [l for l in r.stdout.splitlines() if l.startswith('nav regions=')]
    check(tid + ': the export has nav regions', nav and not nav[0].startswith('nav regions=0 '), nav)
    if tid == 'third-person':
        cam = [s for s in lua if 'camera' in s]
        text = open(os.path.join(folder, cam[0]), encoding='utf-8').read() if cam else ''
        check(tid + ': a Lua camera script takes the camera over',
              'Camera.FollowPsxPlayer(false)' in text and 'Camera.SetPosition' in text, cam)
        check(tid + ': the description says the camera is Lua only', 'Lua' in meta.get('description', ''), meta)

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
