# File > New Project through the GUI: Ctrl+N opens the modal with a card per
# template (Empty, First person, Third person), a Project name and a Location
# field, Choose folder..., Create and Cancel. For each template: pick its
# card, type a name and a location (a temp folder), Create; the editor prints
# "newproject: created <template id> <scene>", the scene is on disk and open,
# and the first- and third-person projects carry both a .lua and a .blocks
# script. Also: Create is off until there is a name (clicking it does nothing),
# Cancel closes the modal, a folder that already holds a scene prints
# "newproject: failed ..." and keeps the modal open, and with unsaved changes
# the Save changes prompt comes first. Controls are clicked by name.
# The Lamplighter sample (templates/lamplighter, fetched by
# tools/fetch-samples.sh) has a card too; a project made from it prints
# "newproject: created lamplighter title.scene", carries its four scenes,
# and each exports with the splashpack-cli beside the editor (cli/). Without
# the sample fetched that fails, unless --allow-missing is given.
# usage: python3 newprojectcheck.py <splashedit> [screenshot.png] [--allow-missing] [--sample-shots <dir>]
#   the optional screenshot is the modal with Third person picked, before Create;
#   --sample-shots writes newproject-lamplighter.png (the modal, Lamplighter
#   picked) and lamplighter-created.png (the project it made, open) into <dir>
import os, shutil, subprocess, sys, tempfile

argv = sys.argv[1:]
allow_missing = '--allow-missing' in argv
argv = [a for a in argv if a != '--allow-missing']
sample_shots = None
if '--sample-shots' in argv:
    i = argv.index('--sample-shots')
    sample_shots = os.path.abspath(argv[i + 1])
    del argv[i:i + 2]
editor = os.path.abspath(argv[0])
shot = os.path.abspath(argv[1]) if len(argv) > 1 else None
here = os.path.dirname(os.path.abspath(__file__))
sample = os.path.join(here, '..', 'templates', 'lamplighter')
cli = os.path.join(os.path.dirname(editor), 'cli', 'splashpack-cli')
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args, final=None):
    """Runs the editor on a fresh courtyard copy, then `args`; returns the output lines."""
    work = tempfile.mkdtemp()
    project = os.path.join(work, 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Crate', '--wait', '5'] + args + [
        '--wait', '5', '--screenshot', final or os.path.join(work, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return lines


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def typed(field, text):
    return click(field) + ['--text', text, '--wait', '3']


def controls(lines):
    return {l.split(' ')[1] for l in lines if l.startswith('control ')}


def rects(lines):
    """control name -> (x0, y0, x1, y1), from 'control <name> at x0,y0 x1,y1'."""
    out = {}
    for l in lines:
        p = l.split(' ')
        if l.startswith('control ') and len(p) == 5 and p[2] == 'at':
            out[p[1]] = tuple(int(v) for v in p[3].split(',') + p[4].split(','))
    return out


def created(lines):
    return [l for l in lines if l.startswith('newproject:')]


OPEN = ['--key', 'ctrl+n', '--wait', '5']
CARDS = {'newproj.tmpl.empty', 'newproj.tmpl.first-person', 'newproj.tmpl.third-person'}
FIELDS = {'newproj.name', 'newproj.location', 'newproj.browse', 'newproj.create', 'newproj.cancel'}

# The modal with nothing typed: every card and control, Create does nothing.
lines = run(OPEN + click('newproj.tmpl.first-person') + click('newproj.create'))
have = controls(lines)
check('the modal shows a card per template', CARDS <= have, sorted(c for c in have if c.startswith('newproj')))
check('the modal shows name, location, browse, create, cancel', FIELDS <= have,
      sorted(c for c in have if c.startswith('newproj')))
if os.path.isdir(sample):
    check('the modal shows the Lamplighter card', 'newproj.tmpl.lamplighter' in have,
          sorted(c for c in have if c.startswith('newproj.tmpl')))
# Each card's wrapped description (newproj.tmpl.<id>.desc) fits inside it, so no text is cut.
box = rects(lines)
cards = sorted(n for n in box if n.startswith('newproj.tmpl.') and not n.endswith('.desc'))
for n in cards:
    c, d = box[n], box.get(n + '.desc')
    check(n + ': the description fits the card',
          d is not None and c[0] <= d[0] and c[1] <= d[1] and d[2] <= c[2] and d[3] <= c[3], ('card', c, 'desc', d))
check('the cards in a row are the same height',
      all(box[a][3] - box[a][1] == box[b][3] - box[b][1] for a in cards for b in cards if box[a][1] == box[b][1]),
      {n: box[n] for n in cards})
check('Create without a name creates nothing', created(lines) == [], created(lines))
check('and the modal stays open', 'popups open: yes' in lines, [l for l in lines if l.startswith('popups')])

lines = run(OPEN + click('newproj.cancel'))
check('Cancel closes the modal', 'popups open: no' in lines and not (FIELDS & controls(lines)),
      [l for l in lines if l.startswith('popups')])

# Each template, into its own temp location.
for tid, name in [('empty', 'Plain Game'), ('first-person', 'fp_game'), ('third-person', 'tp_game')]:
    where = tempfile.mkdtemp()
    args = OPEN + click('newproj.tmpl.' + tid) + typed('newproj.name', name) + typed('newproj.location', where)
    if shot and tid == 'third-person':
        args += ['--shot', shot, '--wait', '2']
    lines = run(args + click('newproj.create'))
    proj = os.path.join(where, name)
    scene = name + '.scene'
    check(tid + ': prints the created line', created(lines) == ['newproject: created %s %s' % (tid, scene)], created(lines))
    check(tid + ': the scene is on disk', os.path.isfile(os.path.join(proj, scene)), sorted(os.listdir(where)))
    check(tid + ': the modal closed and the project opened',
          'popups open: no' in lines and any(l.startswith('status: Created project ' + name) for l in lines),
          [l for l in lines if l.startswith(('popups', 'status'))])
    scripts = sorted(os.listdir(os.path.join(proj, 'scripts'))) if os.path.isdir(os.path.join(proj, 'scripts')) else None
    if tid == 'empty':
        check(tid + ': an empty scripts folder', scripts == [], scripts)
    else:
        check(tid + ': a .lua script was copied', any(s.endswith('.lua') for s in scripts or []), scripts)
        check(tid + ': a .blocks script was copied', any(s.endswith('.blocks') for s in scripts or []), scripts)
        check(tid + ': template.json stays behind', not os.path.exists(os.path.join(proj, 'template.json')))
    if shot and tid == 'third-person':
        check('the modal screenshot was written', os.path.isfile(shot) and os.path.getsize(shot) > 0, shot)

# The Lamplighter sample: its card, and a project made from it opens on its
# title scene (the first in its scenes.list), not the alphabetically first.
SAMPLE_SCENES = ['title.scene', 'valley.scene', 'keep.scene', 'lighthouse.scene']
if not os.path.isdir(sample):
    if allow_missing:
        print('skip lamplighter: templates/lamplighter is not fetched (--allow-missing)')
    else:
        check('templates/lamplighter is fetched (run tools/fetch-samples.sh templates, or pass --allow-missing)', False,
              'no ' + sample)
else:
    where = tempfile.mkdtemp()
    args = OPEN + click('newproj.tmpl.lamplighter') + typed('newproj.name', 'my_lamp') + typed('newproj.location', where)
    final = None
    if sample_shots:
        os.makedirs(sample_shots, exist_ok=True)
        modal = os.path.join(sample_shots, 'newproject-lamplighter.png')
        final = os.path.join(sample_shots, 'lamplighter-created.png')
        for f in (modal, final):
            if os.path.exists(f):
                os.remove(f)
        args += ['--shot', modal, '--wait', '2']
    # The final shot waits for the live export of the new scene, so the
    # status bar's meters are its, not the courtyard's.
    lines = run(args + click('newproj.create') + ['--wait', '3', '--wait-export', '--wait', '5'], final)
    check('lamplighter: prints the created line on its title scene',
          created(lines) == ['newproject: created lamplighter title.scene'], created(lines))
    proj = os.path.join(where, 'my_lamp')
    on_disk = sorted(f for f in os.listdir(proj) if f.endswith('.scene')) if os.path.isdir(proj) else None
    check('lamplighter: its four scenes are on disk', on_disk == sorted(SAMPLE_SCENES), on_disk)
    check('lamplighter: scenes.list and disc.json came along, template.json stayed behind',
          os.path.isfile(os.path.join(proj, 'scenes.list')) and os.path.isfile(os.path.join(proj, 'disc.json')) and
          not os.path.exists(os.path.join(proj, 'template.json')))
    check('lamplighter: the modal closed and the project opened',
          'popups open: no' in lines and any(l.startswith('status: Created project my_lamp from the Lamplighter') for l in lines),
          [l for l in lines if l.startswith(('popups', 'status'))])
    if sample_shots:
        check('lamplighter: both screenshots were written',
              all(os.path.isfile(f) and os.path.getsize(f) > 0 for f in (modal, final)), (modal, final))
    check('lamplighter: splashpack-cli beside the editor', os.path.isfile(cli), cli)
    if os.path.isfile(cli) and on_disk:
        for sc in SAMPLE_SCENES:
            out = os.path.join(where, sc.replace('.scene', '.splashpack'))
            r = subprocess.run([cli, 'export', os.path.join(proj, sc), '-o', out, '--stats'], capture_output=True, text=True)
            if sc == 'title.scene':
                cli_tris = [w.split('=')[1] for l in r.stdout.splitlines() for w in l.split() if w.startswith('triangles=')]
                bar = [l.split()[2].split('/')[0] for l in lines if l.startswith('statusbar: tris ')]
                check('lamplighter: the status bar counts the title scene\'s triangles after its export',
                      len(cli_tris) == 1 and len(bar) >= 2 and bar[-1] == cli_tris[0] and bar[0] != bar[-1],
                      ('status bar', bar, 'cli', cli_tris))
            check('lamplighter: the created ' + sc + ' exports',
                  r.returncode == 0 and os.path.isfile(out) and os.path.getsize(out) > 0,
                  'exit %d: %s' % (r.returncode, (r.stderr or r.stdout).strip()[-400:]))

# A folder that already holds a scene: refused, the modal stays open with the reason.
where = tempfile.mkdtemp()
os.makedirs(os.path.join(where, 'taken'))
open(os.path.join(where, 'taken', 'old.scene'), 'w').write('{}')
lines = run(OPEN + click('newproj.tmpl.first-person') + typed('newproj.name', 'taken') + typed('newproj.location', where) +
            click('newproj.create'))
out = created(lines)
check('a folder holding a scene is refused', len(out) == 1 and out[0].startswith('newproject: failed ') and 'old.scene' in out[0],
      out)
check('the refused modal stays open', 'popups open: yes' in lines, [l for l in lines if l.startswith('popups')])
check('nothing was copied over it', sorted(os.listdir(os.path.join(where, 'taken'))) == ['old.scene'],
      os.listdir(os.path.join(where, 'taken')))

# Unsaved changes: the Save changes prompt comes first; Don't save opens the modal.
lines = run(['--key', 'delete', '--wait', '3'] + OPEN + click('discard'))
check('with changes, Ctrl+N asks first and then opens the modal', FIELDS <= controls(lines),
      sorted(c for c in controls(lines) if c.startswith('newproj')))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
