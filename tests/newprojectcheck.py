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
# usage: python3 newprojectcheck.py <splashedit> [screenshot.png]
#   the optional screenshot is the modal with Third person picked, before Create
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
shot = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args):
    """Runs the editor on a fresh courtyard copy, then `args`; returns the output lines."""
    work = tempfile.mkdtemp()
    project = os.path.join(work, 'courtyard')
    shutil.copytree(example, project)
    cmd = [editor, '--size', '1600x960', '--project', project, '--select', 'Crate', '--wait', '5'] + args + [
        '--wait', '5', '--screenshot', os.path.join(work, 'shot.png')]
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
