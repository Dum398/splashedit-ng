# The scene inspector's Build scenes section, driven through the GUI on a copy
# of the courtyard with a second scene, level2.scene, beside it: add the open
# scene and level2 from the Add dropdown, move level2 up, remove the other;
# and, on a fresh copy, add level2 to the empty list, which puts the open
# scene first. Every change is saved to scenes.list at once and is not a scene
# edit (Ctrl+Z leaves it, nothing is unsaved). The editor prints the list
# ("build: 0 a.scene | 1 b.scene") whenever it changes. Controls are clicked by
# name (--click @build.up.1), so no pixel positions live here.
# usage: python3 buildscenescheck.py <splashedit>   (needs a display: xvfb-run -a)
import os, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def run(args):
    """Runs the editor on a fresh courtyard copy holding level2.scene too
    (courtyard.scene opens, nothing selected, so the inspector shows the
    scene), then `args`. Returns the printed build: lines, the other output
    lines, and scenes.list's text (None when there is none)."""
    project = os.path.join(tempfile.mkdtemp(), 'courtyard')
    shutil.copytree(example, project)
    shutil.copyfile(os.path.join(project, 'courtyard.scene'), os.path.join(project, 'level2.scene'))
    cmd = [editor, '--size', '1600x960', '--project', project, '--wait', '5'] + args + [
        '--wait', '5', '--screenshot', os.path.join(project, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=120)
    lines = (r.stdout + r.stderr).strip().splitlines()
    # A control the script named and the editor never drew is a broken check, not a pass.
    missing = [l for l in lines if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    listed = os.path.join(project, 'scenes.list')
    text = open(listed, 'rb').read().decode('utf-8') if os.path.exists(listed) else None
    return [l for l in lines if l.startswith('build:')], lines, text


def click(name):
    return ['--click', '@' + name, '--wait', '3']


def add(item):
    return click('build.add') + click('build.add/' + item)


builds, lines, text = run([])
check('the list starts empty', builds == ['build: (empty)'] and text is None, (builds, text))

builds, lines, text = run(add('courtyard.scene') + add('level2.scene') + click('build.up.1') + click('build.remove.1') +
                          click('build.add'))
want = ['build: (empty)', 'build: 0 courtyard.scene', 'build: 0 courtyard.scene | 1 level2.scene',
        'build: 0 level2.scene | 1 courtyard.scene', 'build: 0 level2.scene']
check('add, add, up, remove: one line per change', builds == want, builds)
check('scenes.list holds what is left', text == 'level2.scene\n', repr(text))
# The Add dropdown, open at the end, offers only the scene not in the list.
offered = [l.split(' at ')[0] for l in lines if l.startswith('control build.add/')]
check('Add offers what is not listed', offered == ['control build.add/courtyard.scene'], offered)

builds, lines, text = run(add('level2.scene') + ['--key', 'ctrl+z', '--wait', '3'])
check('the first scene added to an empty list goes after the open one',
      builds == ['build: (empty)', 'build: 0 courtyard.scene | 1 level2.scene'], builds)
check('scenes.list is written at once, Ctrl+Z leaves it', text == 'courtyard.scene\nlevel2.scene\n', repr(text))
check('the build list is no scene edit', 'unsaved changes: no' in lines, [l for l in lines if l.startswith('unsaved')])

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
