# The UI tab at a small editor window (1024x768): the canvas header must lay
# its toolbar (fit/zoom in/out buttons, the zoom label, the snap and safe area switches) out
# without overlapping the resolution and hint text it shares the bar with, and
# the screen must fit at a better than 25% scale. The editor prints the header
# layout (and the canvas transform) with --ui; we read the rectangles back.
# usage: python3 uicanvassmallcheck.py <splashedit>
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project():
    d = os.path.join(tempfile.mkdtemp(), 'ui')
    os.makedirs(os.path.join(d, 'fonts'))
    os.makedirs(os.path.join(d, 'textures'))
    s = json.load(open(os.path.join(here, 'ui', 'ui.scene'), encoding='utf-8'))
    for f in s['fonts']:
        shutil.copy(os.path.join(here, 'ui', f['source']), os.path.join(d, 'fonts'))
        f['source'] = 'fonts/' + os.path.basename(f['source'])
    for e in s['canvases'][0]['elements']:
        if 'texture' in e:
            shutil.copy(os.path.join(here, 'ui', e['texture']), os.path.join(d, 'textures'))
            e['texture'] = 'textures/' + os.path.basename(e['texture'])
    json.dump(s, open(os.path.join(d, 'ui.scene'), 'w'), indent=1)
    return d


def run(size):
    d = project()
    cmd = [editor, '--size', size, '--project', d, '--ui', '--wait', '5',
           '--screenshot', os.path.join(d, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    return (r.stdout + r.stderr).strip()


def spans(out):
    """The last 'uicanvas: header ...' line as {item: (x0, x1) or None}."""
    m = None
    for line in out.splitlines():
        if line.startswith('uicanvas: header '):
            m = line[len('uicanvas: '):]
    if not m:
        return None
    toks = m.split()
    d = {}
    for i in range(0, len(toks) - 1, 2):
        v = toks[i + 1]
        d[toks[i]] = None if v == '-' else tuple(float(x) for x in v.split('-'))
    return d


def overlap(a, b):
    return a and b and a[0] < b[1] - 0.5 and b[0] < a[1] - 0.5


out = run('1024x768')
h = spans(out)
check('the UI header reports its layout at 1024x768', h is not None, out[-300:])
if not h:
    print(f'{fails} failures')
    sys.exit(1)

# The buttons and the zoom label are always drawn and must sit inside the bar.
buttons = ['zoomout', 'zoomin', 'zoomfit', 'snap', 'safe']
for b in buttons:
    check(f'the {b} button is present', h.get(b) is not None, h)
inside = all(h[b] and h['header'][0] <= h[b][0] and h[b][1] <= h['header'][1] for b in buttons)
check('every header button sits inside the bar', inside, h)

# Nothing in the header overlaps anything else: the buttons, the zoom box and
# its text, and whatever of the resolution and hint text survives.
items = {k: h[k] for k in ('res', 'hint', 'zoomout', 'pct', 'pcttext', 'zoomin', 'zoomfit', 'snap', 'safe') if h.get(k)}
bad = []
keys = list(items)
for i in range(len(keys)):
    for j in range(i + 1, len(keys)):
        if keys[i] == 'pct' and keys[j] == 'pcttext' or keys[i] == 'pcttext' and keys[j] == 'pct':
            continue  # the label lives inside its own box
        if overlap(items[keys[i]], items[keys[j]]):
            bad.append((keys[i], keys[j], items[keys[i]], items[keys[j]]))
check('no two header items overlap at 1024x768', not bad, bad)
if h.get('pct') and h.get('pcttext'):
    check('the zoom label stays inside its box', h['pct'][0] <= h['pcttext'][0] and h['pcttext'][1] <= h['pct'][1],
          (h['pct'], h['pcttext']))

# The fit at 1024x768 must do better than the 25% floor.
m = re.search(r'uicanvas: screen [\d.]+,[\d.]+ scale ([\d.]+)', out)
check('the canvas reports a fit scale', m is not None, out[-200:])
if m:
    scale = float(m.group(1))
    check('the screen fits at better than 25%', scale > 0.25, scale)

# Wide again (1600x960): the resolution and the hint both have room.
wide = spans(run('1600x960'))
check('the resolution shows when the bar is wide', wide and wide.get('res') is not None, wide)
check('the hint shows when the bar is wide', wide and wide.get('hint') is not None, wide)

print(f'{fails} failures')
sys.exit(1 if fails else 0)
