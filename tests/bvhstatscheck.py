# splashpack-cli export --stats prints the BVH: "bvh nodes=N leaves=L
# depth=D refs=R". Checked against the splashpack header (bvhNodeCount at 32,
# bvhTriangleRefCount at 34) and the tree's shape: buildBvh gives every inner
# node two children, so leaves = (nodes + 1) / 2, and a tree with L leaves is
# at least log2(L) deep. A scene without meshes has no BVH.
# usage: python3 bvhstatscheck.py <splashpack-cli>
import json, math, os, re, struct, subprocess, sys, tempfile

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
    path = os.path.join(tmp, 'b.scene')
    json.dump(scene, open(path, 'w', encoding='utf-8'))
    out = os.path.join(tmp, 'b.splashpack')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([cli, 'export', path, '-o', out, '--project', example, '--stats'], capture_output=True, text=True)
    data = open(out, 'rb').read() if r.returncode == 0 and os.path.exists(out) else b''
    m = re.search(r'^bvh nodes=(\d+) leaves=(\d+) depth=(\d+) refs=(\d+)$', r.stdout, re.M)
    tris = re.search(r' triangles=(\d+)$', r.stdout, re.M)
    return r, data, m and tuple(int(v) for v in m.groups()), tris and int(tris.group(1))


base = json.load(open(os.path.join(example, 'courtyard.scene'), encoding='utf-8'))
r, data, bvh, tris = export(base)
check('the courtyard exports', r.returncode == 0 and data, r.stderr[-300:])
check('--stats prints the bvh line', bvh is not None, r.stdout[-400:])
if bvh and data:
    nodes, leaves, depth, refs = bvh
    hn, hr = struct.unpack_from('<HH', data, 32)
    check('nodes and refs match the header', (nodes, refs) == (hn, hr), (bvh, hn, hr))
    check('every triangle is referenced once', refs == tris, (refs, tris))
    check('two children per inner node', nodes > 1 and leaves == (nodes + 1) // 2, bvh)
    check('the depth fits the leaves', math.ceil(math.log2(leaves)) <= depth <= min(16, leaves - 1), bvh)

bare = json.loads(json.dumps(base))
bare['objects'] = []
r, data, bvh, tris = export(bare)
check('a scene without meshes has an empty BVH', r.returncode == 0 and bvh == (0, 0, 0, 0), (bvh, r.stdout[-300:], r.stderr[-300:]))

print('%d failure%s' % (fails, '' if fails == 1 else 's'))
sys.exit(1 if fails else 0)
