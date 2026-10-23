# Localisation (splashedit-ng#209) through the cli: tests/ui/ui.scene in a scratch project with a strings.json
# (en, fr) and its "sys" and "title" texts keyed. The pack must carry the default language's texts, the generated
# Strings library with only the keys the scene references (plus those under a scene prefix), and the export must
# warn per language, key and element; a scene that uses no strings gets no library.
# usage: python3 stringscheck.py <splashpack-cli>
import json, os, shutil, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


# The fixture's fonts and images are ../../assets and ../../examples from the project: keep that shape.
proj = os.path.join(tmp, 'p', 'ui')
shutil.copytree(os.path.join(here, 'ui'), proj)
shutil.copytree(os.path.join(here, '..', 'assets', 'fonts'), os.path.join(tmp, 'assets', 'fonts'))
shutil.copytree(os.path.join(here, '..', 'examples', 'courtyard', 'textures'), os.path.join(tmp, 'examples', 'courtyard', 'textures'))
base = json.load(open(os.path.join(proj, 'ui.scene')))


def scene(name, keys, script=None, prefixes=None):
    s = json.loads(json.dumps(base))
    for c in s['canvases']:
        for e in c['elements']:
            if e['name'] in keys: e['textKey'] = keys[e['name']]
    if script:
        os.makedirs(os.path.join(proj, 'scripts'), exist_ok=True)
        open(os.path.join(proj, 'scripts', name + '.lua'), 'w').write(script)
        s['settings']['script'] = 'scripts/%s.lua' % name
    if prefixes: s['settings']['stringPrefixes'] = prefixes
    p = os.path.join(proj, name + '.scene')
    json.dump(s, open(p, 'w'), indent=2)
    return p


def export(path, *extra):
    out = os.path.join(tmp, os.path.basename(path) + '.splashpack')
    r = subprocess.run([cli, 'export', path, '-o', out] + list(extra), capture_output=True, text=True)
    return r, (open(out, 'rb').read() if r.returncode == 0 else b'')


json.dump({'languages': [{'id': 'en', 'name': 'English'}, {'id': 'fr', 'name': 'Francais'}],
           'strings': {'hud.sys': {'en': 'SYSTEM EN', 'fr': 'SYSTEME FR, ' + 'X' * 40},
                       'hud.title': {'en': 'TITLE EN', 'fr': ''},
                       'menu.a': {'en': 'MENU A', 'fr': 'MENU A FR'},
                       'menu.b': {'en': 'MENU B', 'fr': 'MENU B FR'},
                       'other': {'en': 'OTHER EN', 'fr': 'AUTRE'}}},
          open(os.path.join(proj, 'strings.json'), 'w'), indent=2)

# Keyed texts: the default language in the pack, the library with the keys named.
r, d = export(scene('keyed', {'sys': 'hud.sys', 'title': 'hud.title'},
                    'function onSceneCreationStart() Debug.Log(Strings.Get("other")) end\n'))
check('keyed export', r.returncode == 0, r.stderr)
check('pack carries the default texts', b'SYSTEM EN\0' in d and b'TITLE EN\0' in d and b"system font\0" not in d)
check('library with the keys the scene names', b'_G.Strings = S' in d and b'SYSTEME FR' in d and b'AUTRE' in d)
check('keys the scene does not name stay out', b'MENU A' not in d)
w = r.stderr + r.stdout
check('warns: fr text wider than the element', "element 'sys', key 'hud.sys', language 'fr': text is" in w and 'px wide' in w, w)
check('warns: empty fr cell', "language 'fr': 'hud.title' has no text" in w, w)
check('no en warnings', "language 'en'" not in w, w)
# A scene prefix carries the keys under it and quiets the computed-key warning.
r, d = export(scene('prefix', {}, 'function f(n) return Strings.Get("menu." .. n) end\n'))
check('computed key warns without a prefix', 'computed key' in r.stderr + r.stdout, r.stderr)
r, d = export(scene('prefix2', {}, 'function f(n) return Strings.Get("menu." .. n) end\n', ['menu.']))
check('prefix carries menu.*', r.returncode == 0 and b'MENU A FR' in d and b'MENU B' in d and b'AUTRE' not in d)
check('prefix quiets the warning', 'computed key' not in r.stderr + r.stdout, r.stderr)
# A missing key is named; bytecode compiles the library.
r, d = export(scene('missing', {'sys': 'no.such'}, 'function f() return Strings.Get("gone") end\n'), '--lua-bytecode')
w = r.stderr + r.stdout
check('bytecode export', r.returncode == 0, r.stderr)
check('missing keys named', "text key 'no.such' is not in the string table" in w and 'Strings.Get("gone"): no such key' in w, w)
# No strings in the scene: no library.
r, d = export(scene('plain', {}))
check('plain scene: no library', r.returncode == 0 and b'_G.Strings' not in d)

shutil.rmtree(tmp, ignore_errors=True)
print('%d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
