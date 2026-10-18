# The sample games pinned in templates/samples.lock, as fetched by
# tools/fetch-samples.sh: each one's template.json is one New Project lists
# (a non-empty string name, a string description, an integer order), every
# scene its scenes.list names is at its top, and each exports with the CLI
# without touching the template. A sample that is not fetched fails, unless
# --allow-missing is given.
# usage: python3 samplecheck.py <splashpack-cli> [<templates-dir>] [--allow-missing]
import json, os, subprocess, sys, tempfile

args = [a for a in sys.argv[1:] if a != '--allow-missing']
allow_missing = '--allow-missing' in sys.argv[1:]
if len(args) not in (1, 2):
    sys.exit('usage: samplecheck.py <splashpack-cli> [<templates-dir>] [--allow-missing]')
cli = args[0]
here = os.path.dirname(os.path.abspath(__file__))
root = args[1] if len(args) == 2 else os.path.join(here, '..', 'templates')
lock = os.path.join(here, '..', 'templates', 'samples.lock')
tmp = tempfile.mkdtemp()
fails = checks = 0

# What each pinned sample must be: the name New Project shows and its scenes.
expected = {'lamplighter': ('Lamplighter', ['title.scene', 'valley.scene', 'keep.scene', 'lighthouse.scene'])}


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def tree(folder):
    out = {}
    for d, _, files in os.walk(folder):
        for f in files:
            p = os.path.join(d, f)
            st = os.stat(p)
            out[os.path.relpath(p, folder)] = (st.st_size, st.st_mtime_ns)
    return out


samples = []
for line in open(lock, encoding='utf-8'):
    line = line.strip()
    if line and not line.startswith('#'):
        samples.append(line.split()[0])
check('samples.lock pins the expected samples', sorted(samples) == sorted(expected), samples)

for sid in samples:
    folder = os.path.join(root, sid)
    if not os.path.isdir(folder):
        if allow_missing:
            print('skip ' + sid + ': not fetched into ' + root + ' (--allow-missing)')
        else:
            check(sid + ': fetched into ' + root + ' (run tools/fetch-samples.sh, or pass --allow-missing)', False,
                  'no ' + folder)
        continue
    name, scenes = expected.get(sid, ('', []))
    try:
        meta = json.load(open(os.path.join(folder, 'template.json'), encoding='utf-8'))
    except (OSError, ValueError) as e:
        meta = {'error': str(e)}
    check(sid + ': template.json lists it as ' + name,
          meta.get('name') == name and isinstance(meta.get('description'), str) and
          isinstance(meta.get('order', 0), int) and not isinstance(meta.get('order', 0), bool), meta)
    top = sorted(f for f in os.listdir(folder) if f.endswith('.scene'))
    check(sid + ': its scenes are at the top', top == sorted(scenes), top)
    listed = []
    if os.path.isfile(os.path.join(folder, 'scenes.list')):
        listed = [l.strip() for l in open(os.path.join(folder, 'scenes.list'), encoding='utf-8') if l.strip()]
    check(sid + ': scenes.list starts on ' + scenes[0] + ' and names every scene',
          listed[:1] == scenes[:1] and sorted(listed) == sorted(scenes), listed)
    before = tree(folder)
    for sc in scenes:
        out = os.path.join(tmp, sid + '-' + sc.replace('.scene', '') + '.splashpack')
        r = subprocess.run([cli, 'export', os.path.join(folder, sc), '-o', out, '--project', folder],
                           capture_output=True, text=True)
        ok = r.returncode == 0 and os.path.isfile(out) and os.path.getsize(out) > 0
        check(sid + ': ' + sc + ' exports', ok, 'exit %d: %s' % (r.returncode, (r.stderr or r.stdout).strip()[-400:]))
        if ok:
            print('     ' + sc + ': %d bytes' % os.path.getsize(out))
    check(sid + ': exporting left the template as it was', tree(folder) == before)

print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
