# The mesh inspector's Flat colour row (#223): fixtures/unity-models'
# materials.unity is imported with `splashpack-cli import-unity`, and its
# plane_2x2 (8) (a Unity PSXObjectExporter on Flat Color, grey 100) is
# selected. The row shows under Lighting with the imported grey, a pick in its
# colour picker changes the colour and Ctrl+Z puts it back, and switching
# Lighting to Baked vertex hides the row (Ctrl+Z shows it again). Controls are
# clicked by name.
# usage: python3 flatcolourcheck.py <splashedit> <splashpack-cli> [screenshot]
import os, re, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
cli = os.path.abspath(sys.argv[2])
keep = sys.argv[3] if len(sys.argv) > 3 else None
here = os.path.dirname(os.path.abspath(__file__))
unity = os.path.join(here, 'fixtures', 'unity-models')
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
W = lambda n: ['--wait', str(n)]
OBJ = 'plane_2x2 (8)'


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project():
    dest = os.path.join(tempfile.mkdtemp(), 'imported')
    r = subprocess.run([cli, 'import-unity', unity, dest, '--scenes', 'materials'], capture_output=True, text=True, timeout=120)
    check('import-unity materials: rc 0', r.returncode == 0, r.stdout + r.stderr)
    return dest


def run(name, args):
    """Runs the editor on a fresh import with plane_2x2 (8) selected, then
    `args`; returns the flatcolor lines and the controls drawn at the end."""
    dest = project()
    shot = keep if keep and name == 'shown' else os.path.join(dest, name + '.png')
    cmd = [editor, '--size', '1600x960', '--project', dest, '--scene', os.path.join(dest, 'materials.scene'),
           '--select', OBJ] + W(5) + args + W(3) + ['--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    lines = (r.stdout + r.stderr).strip().splitlines()
    missing = [l for l in lines if 'no such control drawn' in l or 'no object named' in l]
    check(name + ': every control the script names was drawn', not missing, missing)
    flat = [l for l in lines if l.startswith('flatcolor: ')]
    ctl = {m.group(1) for l in lines for m in [re.match(r'control (\S+) at ', l)] if m}
    return flat, ctl


SHOW = ['--scroll-to', '@obj.flatcolor'] + W(3)

# 1. The imported grey, under Lighting = Flat.
flat, ctl = run('shown', SHOW)
check('the row shows the imported grey', flat[:1] == ['flatcolor: 100,100,100'], flat)
check('the row and its Lighting dropdown are drawn', 'obj.flatcolor' in ctl and 'obj.lighting' in ctl,
      sorted(c for c in ctl if c.startswith('obj.')))

# 2. A pick in the picker changes it; Ctrl+Z gives the grey back.
# Left of the picker's middle is its saturation/value square: the middle
# itself is the hue bar, and a new hue leaves a grey the same grey.
# The picker opens below its row: the inspector is scrolled to its end first
# so the row is up from the window's bottom edge and the picker fits.
PICK = SHOW + ['--scroll-to', '@obj.addcomp'] + W(3) + ['--click', '@obj.flatcolor'] + W(3) + ['--click', '@obj.flatcolorpick,-40,0'] + W(3) + ['--key', 'escape'] + W(3)
flat, _ = run('pick', PICK + ['--key', 'ctrl+z'] + W(3))
check('a pick prints a new colour', len(flat) >= 2 and flat[1] != 'flatcolor: 100,100,100', flat)
check('Ctrl+Z puts 100,100,100 back after a changed colour',
      len(flat) >= 3 and any(l != 'flatcolor: 100,100,100' for l in flat[1:-1]) and flat[-1] == 'flatcolor: 100,100,100', flat)

# 3. Lighting = Baked vertex hides the row; Ctrl+Z shows it again.
BAKED = SHOW + ['--click', '@obj.lighting'] + W(3) + ['--click', '@lighting/Baked vertex'] + W(3)
flat, ctl = run('baked', BAKED)
check('Baked vertex hides the row', 'obj.flatcolor' not in ctl and 'obj.lighting' in ctl, sorted(c for c in ctl if c.startswith('obj.')))
flat, ctl = run('undo', BAKED + ['--key', 'ctrl+z'] + W(3))
check('Ctrl+Z brings it back', 'obj.flatcolor' in ctl and flat[-1:] == ['flatcolor: 100,100,100'], (flat, 'obj.flatcolor' in ctl))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
