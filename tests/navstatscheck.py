# splashpack-cli export --stats prints the nav bake: "nav regions=N cell=C
# (asked A) extent=WxD portals=P". Checked against the splashpack header
# (navRegionCount at 44, navPortalCount at 46). Every shared edge is a portal
# from each side, so P is even. A scene without a Player or Navigation object
# bakes nothing and prints no nav line.
# usage: python3 navstatscheck.py <splashpack-cli>
import json, os, re, struct, subprocess, sys, tempfile

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
example = os.path.join(here, '..', 'examples', 'courtyard')
tmp = tempfile.mkdtemp()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))
    fails += not ok


def export(scene):
    path = os.path.join(tmp, 'n.scene')
    json.dump(scene, open(path, 'w', encoding='utf-8'))
    out = os.path.join(tmp, 'n.splashpack')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([cli, 'export', path, '-o', out, '--project', example, '--stats'], capture_output=True, text=True)
    data = open(out, 'rb').read() if r.returncode == 0 and os.path.exists(out) else b''
    m = re.search(r'^nav regions=(\d+) cell=\S+ \(asked \S+\) extent=\S+ portals=(\d+)$', r.stdout, re.M)
    return r, data, m and (int(m.group(1)), int(m.group(2)))


base = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
r, data, nav = export(base)
check('the courtyard exports', r.returncode == 0 and data, r.stderr[-300:])
check('--stats prints the nav line with portals at the end', nav is not None, r.stdout[-400:])
if nav and data:
    regions, portals = nav
    hr, hp = struct.unpack_from('<HH', data, 44)
    check('regions and portals match the header', (regions, portals) == (hr, hp), (nav, hr, hp))
    check('the courtyard has connected regions', regions > 1 and portals > 0, nav)
    check('portals come in pairs', portals % 2 == 0, nav)


def strip(objs):
    out = []
    for o in objs:
        o = dict(o)
        o['components'] = [c for c in o.get('components', []) if c.get('type') not in ('player', 'navigation')]
        o['children'] = strip(o.get('children', []))
        out.append(o)
    return out


bare = json.loads(json.dumps(base))
bare['objects'] = strip(bare['objects'])
r, data, nav = export(bare)
check('without a Player or Navigation object there is no nav line',
      r.returncode == 0 and nav is None and not re.search(r'^nav ', r.stdout, re.M), (nav, r.stdout[-300:], r.stderr[-300:]))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
