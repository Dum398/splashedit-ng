# A canvas's default font (Unity's PSXCanvas.defaultFont): the canvas
# inspector's Default font row picks one of the scene's fonts; it is saved
# with the canvas; a text element without a font of its own then shows
# "Canvas default (<name>)" in its Font row, and its Font menu offers that
# instead of the system font. Opens a copy of tests/ui/ui.scene with --ui and
# reads the editor's scripted lines ("uicanvas: default font ...",
# "uicanvas: element font ...") and the saved scene. Needs a display (xvfb-run -a).
# usage: python3 cvfontcheck.py <splashedit>
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
    """tests/ui/ui.scene as a project of its own, its fonts and texture copied in."""
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


def run(args):
    """Runs the editor on a fresh project with the UI tab open (the canvas
    inspector, as nothing is selected), then Ctrl+S. Returns the saved scene,
    the scene file's text, the editor's output and the named controls (those
    drawn in the last frame: a menu left open lists its items)."""
    d = project()
    cmd = [editor, '--size', '1600x960', '--project', d, '--ui', '--wait', '5'] + args + [
        '--key', 'ctrl+s', '--wait', '5', '--screenshot', os.path.join(d, 'shot.png')]
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=180)
    out = (r.stdout + r.stderr).strip()
    text = open(os.path.join(d, 'ui.scene'), encoding='utf-8').read()
    controls = {m.group(1): tuple(map(int, m.groups()[1:])) for m in re.finditer(r'^control (.+?) at (-?\d+),(-?\d+) (-?\d+),(-?\d+)$', out, re.M)}
    return json.loads(text), text, out, controls


def said(out, what):
    return re.findall(r'^uicanvas: ' + what + r' (.*)$', out, re.M)


def element_fonts(out):
    """The 'uicanvas: element font <shown> width <px>' lines: (shown, px)."""
    return [(m.group(1), int(m.group(2))) for m in re.finditer(r'^uicanvas: element font (.*) width (\d+)$', out, re.M)]


SYS_WIDTH = 8 * len('system font')  # the text of 'sys' in the system font, 8 px a character


def font_of(scene, element):
    for e in scene['canvases'][0]['elements']:
        if e['name'] == element:
            return e.get('font') or ''  # the system font is saved as null
    return None


# At first: no default font, the row and its control drawn.
scene, text, out, ctl = run([])
m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out)
check('--ui opens the UI tab and reports its layout', m is not None, out[-300:])
if not m:
    print(f'{fails} failures')
    sys.exit(1)
X, Y, S = map(float, m.groups())
SYS = '%d,%d' % (round(X + 100.5 * S), round(Y + 60.5 * S))  # inside the text element 'sys' (8..308 x 52..68)
check('the canvas reports no default font', said(out, 'default font') == ['none'], said(out, 'default font'))
check('the Default font row is drawn', 'cv.font' in ctl, sorted(ctl)[:30])
check('no default font is saved as null', '"defaultFont": null' in text and scene['canvases'][0].get('defaultFont') is None,
      scene['canvases'][0])
check('the project has fonts to pick from', [f['name'] for f in scene['fonts']] == ['title', 'body'], scene['fonts'])

# The control for the element: 'sys' (no font of its own) without a canvas
# default shows the system font. Its Font menu is left open, to list its items.
scene, text, out, ctl = run(['--click', SYS, '--wait', '5', '--click', '@ui.font', '--wait', '5'])
check('the Font menu is open at the end', 'popups open: yes' in out, out[-300:])
check('clicking sys selects it', 'uicanvas: inspector element sys' in out, out[-400:])
check('without a canvas default, a fontless text element shows the system font and its width',
      element_fonts(out) == [('System (8 x 16)', SYS_WIDTH)], element_fonts(out))
check('without a canvas default, its Font menu offers the system font',
      'ui.font/System (8 x 16)' in ctl and not any(k.startswith('ui.font/Canvas default') for k in ctl), sorted(k for k in ctl if k.startswith('ui.font')))

# The Default font menu, left open: the system font and each scene font.
scene, text, out, ctl = run(['--click', '@cv.font', '--wait', '5'])
check('the Default font menu lists the system font and each scene font', 'popups open: yes' in out and
      all(k in ctl for k in ('cv.font/System (8 x 16)', 'cv.font/title', 'cv.font/body')), sorted(k for k in ctl if k.startswith('cv.font')))

# Pick 'title' as the canvas's default font: reported and saved; then 'sys' shows it.
scene, text, out, ctl = run(['--click', '@cv.font', '--wait', '5', '--click', '@cv.font/title', '--wait', '5',
                             '--click', SYS, '--wait', '5'])
check('picking title reports it', said(out, 'default font') == ['none', 'title'], said(out, 'default font'))
check('the default font is saved with the canvas', scene['canvases'][0].get('defaultFont') == 'title' and '"defaultFont": "title"' in text,
      scene['canvases'][0].get('defaultFont'))
check('the text element keeps no font of its own', font_of(scene, 'sys') == '', font_of(scene, 'sys'))
check('the other text elements keep theirs', font_of(scene, 'title') == 'title' and font_of(scene, 'body') == 'body',
      (font_of(scene, 'title'), font_of(scene, 'body')))
ef = element_fonts(out)
check('the fontless text element shows the canvas default', 'uicanvas: inspector element sys' in out and
      [s for s, _ in ef] == ['Canvas default (title)'], ef)
check('and is measured in the canvas default, not the system font', len(ef) == 1 and ef[0][1] > 0 and ef[0][1] != SYS_WIDTH, ef)
scene, text, out, ctl = run(['--click', '@cv.font', '--wait', '5', '--click', '@cv.font/title', '--wait', '5',
                             '--click', SYS, '--wait', '5', '--click', '@ui.font', '--wait', '5'])
check('its Font menu offers the canvas default instead of the system font',
      'ui.font/Canvas default (title)' in ctl and 'ui.font/System (8 x 16)' not in ctl, sorted(k for k in ctl if k.startswith('ui.font')))

# Back to the system font, then Ctrl+Z restores the pick.
scene, text, out, ctl = run(['--click', '@cv.font', '--wait', '5', '--click', '@cv.font/body', '--wait', '5',
                             '--click', '@cv.font', '--wait', '5', '--click', '@cv.font/System (8 x 16)', '--wait', '5'])
check('picking body then System reports each', said(out, 'default font') == ['none', 'body', 'none'], said(out, 'default font'))
check('System saves null', scene['canvases'][0].get('defaultFont') is None, scene['canvases'][0])
scene, text, out, ctl = run(['--click', '@cv.font', '--wait', '5', '--click', '@cv.font/body', '--wait', '5',
                             '--key', 'ctrl+z', '--wait', '5'])
check('Ctrl+Z takes the pick back', said(out, 'default font') == ['none', 'body', 'none'] and
      scene['canvases'][0].get('defaultFont') is None, (said(out, 'default font'), scene['canvases'][0].get('defaultFont')))

print(f'{fails} failures')
sys.exit(1 if fails else 0)
