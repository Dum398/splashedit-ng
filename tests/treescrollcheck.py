# The Scene panel's rows scroll when the scene has more objects than the
# panel fits (#227). A copy of the courtyard gets 80 more objects, Filler 1
# to Filler 80. The last row starts below the panel; the wheel over the rows
# brings it into view and a click on it selects it, as does dragging the
# thumb to the bottom; a row scrolled up under
# the filter field does not take a click meant for the field; and --select of
# the last object scrolls its row into view at startup, and an object picked
# in the viewport under a closed group opens it and is scrolled to; a row dragged to the
# list's bottom edge scrolls it. Rects come from the editor's list of named
# controls, as the last frame drew them; the list's visible part is
# treelist.panel.
# usage: python3 treescrollcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
H = 960
N = 80
LAST = 'Filler %d' % N


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project_copy():
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    scene = os.path.join(project, 'courtyard.scene')
    s = json.load(open(scene, encoding='utf-8'))
    for k in range(1, N + 1):
        s['objects'].append({'name': 'Filler %d' % k, 'active': True, 'transform': {'position': [k * 0.5, 0, 0], 'rotation': [0, 0, 0, 1],
                             'scale': [1, 1, 1]}, 'components': [], 'children': []})
    # Last of all, a group holding one object, clear of the fillers in the view.
    child = {'name': 'Deep Child', 'active': True, 'transform': {'position': [-3, 2, 3], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
             'components': [], 'children': []}
    s['objects'].append({'name': 'Deep Group', 'active': True, 'transform': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1],
                         'scale': [1, 1, 1]}, 'components': [], 'children': [child]})
    json.dump(s, open(scene, 'w', encoding='utf-8'), indent=2)
    return project


def run(args, select=None):
    """Runs the editor on a fresh copy with `args`; returns the output lines,
    every named control's rect and the selected object's name."""
    project = project_copy()
    cmd = [editor, '--size', '1600x%d' % H, '--project', project, '--scene', os.path.join(project, 'courtyard.scene')]
    if select:
        cmd += ['--select', select]
    cmd += ['--wait', '5'] + args + ['--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check('every control the script names was drawn', not missing, missing)
    rects, selected = {}, None
    for l in lines:
        m = re.match(r'control (.+) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', l)
        if m:
            rects[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        if l.startswith('selected: '):
            selected = l[len('selected: '):]
    return lines, rects, selected


def inside(row, panel):
    return row is not None and panel is not None and row[1] >= panel[1] and row[3] <= panel[3]


lines, rects, selected = run([])
panel = rects.get('treelist.panel')
check('the rows list is drawn', panel is not None, sorted(rects)[:20])
last = rects.get('tree.' + LAST)
check('the last row starts below the list (the scene overflows it)', last is not None and panel is not None and last[1] > panel[3],
      (last, panel))

# The wheel over the rows, far enough to reach the end, then a click on the last row.
mx = (panel[0] + panel[2]) // 2 if panel else 150
my = (panel[1] + panel[3]) // 2 if panel else 400
WHEEL = ['--wheel', '%d,%d,-60' % (mx, my), '--wait', '3']
lines, rects, selected = run(WHEEL + ['--click', '@tree.' + LAST, '--wait', '3'])
panel = rects.get('treelist.panel')
last = rects.get('tree.' + LAST)
check('the wheel brings the last row into the list', inside(last, panel), (last, panel))
check('a click on it selects it', selected == LAST, selected)
first = rects.get('tree.Environment')
check('the first rows are scrolled up out of the list', first is not None and panel is not None and first[3] <= panel[1], (first, panel))

# The thumb dragged to the bottom shows the last row too.
lines, rects, selected = run(['--drag', '@treethumb,0,0,0,%d' % (H - 200), '--wait', '3'])
panel = rects.get('treelist.panel')
last = rects.get('tree.' + LAST)
check('dragging the thumb to the bottom brings the last row into the list', inside(last, panel), (last, panel))

# Scrolled a little, some row lies under the filter field above the list:
# a click there is the field's, not the row's.
lines, rects, selected = run(['--wheel', '%d,%d,-3' % (mx, my), '--wait', '3'])
panel = rects.get('treelist.panel')
search = rects.get('treesearch')
hidden = None
for name, rr in sorted(rects.items()):
    if name.startswith('tree.') and panel and rr[3] <= panel[1] and search and rr[1] <= search[3] - 4 and rr[3] >= search[1] + 4:
        hidden = (name, rr)
check('a row is scrolled up under the filter field', hidden is not None, (search, panel))
if hidden:
    cx = (hidden[1][0] + hidden[1][2]) // 2
    cy = (max(hidden[1][1], search[1]) + min(hidden[1][3], search[3])) // 2
    lines, rects, selected = run(['--wheel', '%d,%d,-3' % (mx, my), '--wait', '3', '--click', '%d,%d' % (cx, cy), '--wait', '3'],
                                 select='Sun')
    check('a click over it leaves the selection alone', selected == 'Sun', (selected, hidden[0], cx, cy))
# Under the Project footer, below the list, too.
lines, rects, selected = run([], select='Sun')
panel = rects.get('treelist.panel')
below = None
for name, rr in sorted(rects.items()):
    if name.startswith('tree.') and panel and rr[1] >= panel[3] + 4 and rr[3] <= panel[3] + 36:
        below = (name, rr)
        break
check('a row lies under the footer', below is not None, panel)
if below:
    lines, rects, selected = run(['--click', '%d,%d' % ((below[1][0] + below[1][2]) // 2, (below[1][1] + below[1][3]) // 2), '--wait', '3'],
                                 select='Sun')
    check('a click on the footer does not select the row under it', selected == 'Sun', (selected, below[0]))

# A row dragged to the list's bottom edge, and held there, scrolls the list.
lines, rects, selected = run(['--drag-hold', '%d,%d,%d,%d' % (mx, panel[1] + 90, mx, panel[3] - 4), '--wait', '40'], select='Sun')
panel = rects.get('treelist.panel')
first = rects.get('tree.Environment')
check('dragging a row to the bottom edge scrolls the list', first is not None and panel is not None and first[3] <= panel[1],
      (first, panel))

# Selected at startup (as a viewport click or Ctrl F would), the last row is scrolled to.
lines, rects, selected = run([], select=LAST)
panel = rects.get('treelist.panel')
last = rects.get('tree.' + LAST)
check('--select of the last object scrolls its row into view', inside(last, panel), (last, panel))
check('and it is the selection', selected == LAST, selected)

# Deep Group closed from its chevron, the list wheeled back to the top, then
# Deep Child picked from its marker in the viewport: the group opens and the
# row is scrolled to. Closed again by hand, it stays closed.
COLLAPSE = ['--scroll-to', '@tree.Deep Group', '--click', '@tree.Deep Group,-105,0', '--wait', '3',
            '--wheel', '%d,%d,60' % (mx, my), '--wait', '3']
lines, rects, selected = run(COLLAPSE)
check('closing Deep Group hides its child row', 'tree.Deep Child' not in rects, rects.get('tree.Deep Child'))
lines, rects, selected = run(COLLAPSE + ['--click', '@vp.marker.Deep Child', '--wait', '5'])
panel = rects.get('treelist.panel')
deep = rects.get('tree.Deep Child')
check('picked in the viewport, Deep Child is the selection', selected == 'Deep Child', selected)
check('its closed group opens and its row is scrolled into view', inside(deep, panel), (deep, panel))
lines, rects, selected = run(COLLAPSE + ['--click', '@vp.marker.Deep Child', '--wait', '5', '--click', '@tree.Deep Group,-105,0', '--wait', '5'])
check('closed again by hand with the selection unchanged, the group stays closed',
      'tree.Deep Child' not in rects and selected == 'Deep Child', (rects.get('tree.Deep Child'), selected))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
