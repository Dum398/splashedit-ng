# Localisation in the editor (splashedit-ng#209), driven through the GUI on a
# copy of tests/ui/ui.scene with a strings.json (en, fr):
#  - View > Strings: Add key, rename it in the Key column, type its texts,
#    delete another key and bring it back with Ctrl+Z (one undo step per edit,
#    each saved to strings.json at once); Add language and give it an id;
#    the search hides the rows that do not match; Import CSV opens its dialog
#    in the project folder.
#  - Renaming a key re-keys the scene's UI texts that use it.
#  - The UI element inspector's Text key picker sets the element's key.
#  - The canvas header's preview language: the canvas draws the keyed text
#    in that language (the screenshots of the canvas differ between en and fr).
#  - The open scene's String prefixes (a scene setting) in the window.
# The editor prints "strings: table ...", "strings: key <key> [<text>]...",
# "strings: preview <id>" and "strings: scene prefixes ...".
# usage: python3 stringseditorcheck.py <splashedit>   (needs a display: xvfb-run -a)
import json, os, re, shutil, subprocess, sys, tempfile

editor = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
fails = 0
ENV = dict(os.environ, SPLASHEDIT_REDUX='/nonexistent/pcsx-redux', SPLASHEDIT_PSXSPLASH='/nonexistent/psxsplash.ps-exe')
STRINGS = {'languages': [{'id': 'en', 'name': 'English'}, {'id': 'fr', 'name': 'Francais'}],
           'strings': {'hud.sys': {'en': 'SYSTEM', 'fr': 'SYSTEME'},
                       'hud.title': {'en': 'TITLE', 'fr': 'TITRE'},
                       'hud.big': {'en': 'HELLO', 'fr': 'BONJOUR LE MONDE'}}}


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def project(keyed=None):
    """tests/ui/ui.scene as a project of its own (fonts and texture copied in)
    with STRINGS as its strings.json; `keyed` maps element names to keys."""
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
        if keyed and e['name'] in keyed: e['textKey'] = keyed[e['name']]
    json.dump(s, open(os.path.join(d, 'ui.scene'), 'w'), indent=1)
    json.dump(STRINGS, open(os.path.join(d, 'strings.json'), 'w'), indent=1)
    return d


def run(args, d=None, ui=False, env=None, save=False):
    """The editor on project `d` (fresh when None), then `args`; returns the
    output, the project and the end screenshot's path."""
    d = d or project()
    shot = os.path.join(d, 'shot.png')
    cmd = [editor, '--size', '1600x960', '--project', d] + (['--ui'] if ui else []) + ['--wait', '5'] + args + \
        (['--key', 'ctrl+s', '--wait', '3'] if save else []) + ['--wait', '5', '--screenshot', shot]
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(ENV, **(env or {})), timeout=180)
    out = (r.stdout + r.stderr).strip()
    missing = [l for l in out.splitlines() if 'no such control drawn' in l]
    check('every control the script names was drawn', not missing, missing)
    return out, d, shot


def table(d):
    return json.load(open(os.path.join(d, 'strings.json'), encoding='utf-8'))


def elements(d):
    s = json.load(open(os.path.join(d, 'ui.scene'), encoding='utf-8'))
    return {e['name']: e for e in s['canvases'][0]['elements']}


W = ['--wait', '3']
OPEN = ['--click', '@menu.view', '--wait', '3', '--click', '@menu.view/Strings', '--wait', '5']


def typed(handle, text):
    return ['--click', handle] + W + ['--text', text, '--key', 'enter'] + W


# ---- The window: a key added, renamed, its texts typed; a key deleted and undone.
out, d, shot = run(OPEN + ['--click', '@strings.addkey'] + W + typed('@strings.key/new.key', 'menu.quit') +
                   typed('@strings.cell/menu.quit/en', 'QUIT') + typed('@strings.cell/menu.quit/fr', 'QUITTER') +
                   ['--click', '@strings.del/hud.title'] + W + ['--key', 'ctrl+z'] + W +
                   typed('@strings.cell/hud.big/fr', 'L' * 70))
t = table(d)
check('the window opens and lists the table', 'strings: table languages en=English,fr=Francais keys 3' in out, out[-400:])
check('the new key is renamed and both its texts are saved',
      t['strings'].get('menu.quit') == {'en': 'QUIT', 'fr': 'QUITTER'} and 'new.key' not in t['strings'], t['strings'])
check('a text past 63 bytes is kept whole and counted against 63 in the warning colour',
      t['strings']['hud.big']['fr'] == 'L' * 70 and 'strings: cell hud.big/fr 70/63 warn' in out, t['strings']['hud.big'])
check('Ctrl+Z in the window brings the deleted key back', t['strings'].get('hud.title') == {'en': 'TITLE', 'fr': 'TITRE'}, t['strings'])
tables = [l for l in out.splitlines() if l.startswith('strings: table ')]
# open, add, rename, en, fr, delete, undo: one line (one revision) per edit.
check('one undo step per edit (8 table states, the last a new edit after the undo)', len(tables) == 8 and
      tables[-1].endswith('undo 1 redo 0') and 'keys 3' in tables[-3] and 'keys 4' in tables[-2], tables)

# ---- A language added and named; the search; Import CSV's dialog.
out, d, shot = run(OPEN + ['--click', '@strings.addlang'] + W + typed('@strings.langid/lang', 'de') +
                   typed('@strings.langname/de', 'Deutsch') + ['--click', '@strings.search'] + W + ['--text', 'bonjour'] + W +
                   ['--click', '@strings.import'] + W, env={'SPLASHEDIT_FILE_DIALOG': 'hold'})
t = table(d)
check('Add language adds a column, renamed to de / Deutsch',
      [l['id'] for l in t['languages']] == ['en', 'fr', 'de'] and t['languages'][2]['name'] == 'Deutsch' and
      all(v.get('de') == '' for v in t['strings'].values()), t)
controls = [l.split()[1] for l in out.splitlines() if l.startswith('control strings.key/')]
check('the search keeps the rows whose texts match', controls == ['strings.key/hud.big'], controls)
check('Import CSV opens its dialog in the project folder', re.search(r'file dialog: strings-import in .*ui', out) is not None, out[-300:])

# ---- Renaming a key re-keys the scene's elements that use it.
out, d, shot = run(OPEN + typed('@strings.key/hud.sys', 'hud.system') + ['--click', '@strings.close'] + W,
                   d=project({'sys': 'hud.sys'}), save=True)
check('a renamed key follows into the scene', elements(d)['sys'].get('textKey') == 'hud.system' and
      'hud.system' in table(d)['strings'], elements(d)['sys'])

# ---- The inspector's Text key picker.
cv0, d0, _ = run([], ui=True)
m = re.search(r'uicanvas: screen [\d.]+,[\d.]+ scale [\d.]+ list ([\d.]+),([\d.]+) row ([\d.]+)', cv0)
check('--ui reports the element list', m is not None, cv0[-300:])
LX, LY, LH = map(float, m.groups()) if m else (0, 0, 0)
ORDER = list(elements(d0))


def row(name):
    return ['--click', '%d,%d' % (round(LX + 40), round(LY + (ORDER.index(name) + 0.5) * LH)), '--wait', '3']


out, d, shot = run(row('sys') + ['--click', '@ui.textkey'] + W + ['--click', '@##textkeys/hud.big'] + W, ui=True, save=True)
check('the Text key picker keys the element', elements(d)['sys'].get('textKey') == 'hud.big', elements(d)['sys'])

# ---- The preview language: the canvas draws the keyed text in it.
d = project({'sys': 'hud.big'})
out_en, _, shot_en = run([], d=d, ui=True)
shutil.copy(shot_en, os.path.join(d, 'shot-en.png'))
out_fr, _, shot_fr = run(['--click', '@uipreviewlang'] + W + ['--click', '@uipreviewlang/fr'] + W, d=d, ui=True)
check('the preview language picker switches to fr', 'strings: preview fr' in out_fr and 'strings: preview en' in out_en,
      [l for l in out_fr.splitlines() if 'preview' in l])
try:
    from PIL import Image, ImageChops
    m = re.search(r'uicanvas: screen ([\d.]+),([\d.]+) scale ([\d.]+)', out_fr)
    x0, y0, sc = map(float, m.groups())
    box = (int(x0), int(y0), int(x0 + 320 * sc), int(y0 + 240 * sc))
    a = Image.open(os.path.join(d, 'shot-en.png')).convert('RGB').crop(box)
    b = Image.open(shot_fr).convert('RGB').crop(box)
    diff = ImageChops.difference(a, b).getbbox()
    check('the canvas draws the fr text (en and fr canvases differ)', diff is not None, box)
    print('preview shots:', os.path.join(d, 'shot-en.png'), shot_fr)
except ImportError:
    print('skip: no PIL for the canvas comparison')

# ---- The open scene's String prefixes, in the window.
out, d, shot = run(OPEN + typed('@strings.prefixes', 'menu., hud.') + ['--click', '@strings.close'] + W, save=True)
s = json.load(open(os.path.join(d, 'ui.scene'), encoding='utf-8'))
check('String prefixes are saved with the scene', s['settings'].get('stringPrefixes') == ['menu.', 'hud.'], s['settings'])

print(f'{fails} failures')
sys.exit(1 if fails else 0)
