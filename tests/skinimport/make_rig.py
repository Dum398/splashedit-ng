# Writes the skinned test rig as rig.gltf and rig.fbx, the same model twice.
# An "Armature" node (moved (1, 0, 0.5) m, turned 90 degrees about Y) holds
# joint "Upper" (y + 0.5) holding joint "Lower" (y + 1). Two boxes: one on
# Upper (0.5 m wide, 1 m tall), one on Lower (0.4 m wide); the four top
# corners of the lower box are weighted 3:1 to Upper and Lower, the rest to
# one joint. Animations: "Wave" turns Lower 90 degrees about Y at 0.5 s and
# back at 1 s (linear) and steps Upper up to y + 0.75 at 0.5 s; "Nod/Big"
# turns Upper 45 degrees about X over 1 s (cubic spline in the glTF).
# glTF: "Body", the skinned mesh node, is a child of Armature moved 5 m along
# X, which glTF ignores for a skinned mesh. FBX: Y up, centimetres, Body at
# the root moved 30 cm along X, which FBX does not ignore, so its control
# points sit 30 cm short of where they end up. Both give the same vertices.
# rig_host, rig_nod and rig_alien (.gltf and .fbx) are variations for adding
# clips from other files (see EXTRA and multiclipcheck.py); skin_saved.scene
# is tests/skin/skin.scene as the editor saved it before skins had clipSources.
# Regenerate with: python3 make_rig.py
import base64, json, math, os, struct

here = os.path.dirname(os.path.abspath(__file__))
S45 = math.sqrt(0.5)


def matmul(a, b):  # 4x4 row-major
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def trs(t, q=(0, 0, 0, 1)):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), t[0]],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), t[1]],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), t[2]],
            [0, 0, 0, 1]]


def inverse(m):  # rigid: rotation and translation only
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0, 0, 0, 1]]


def point(m, p):
    return [sum(m[i][k] * p[k] for k in range(3)) + m[i][3] for i in range(3)]


ARMATURE = trs((1, 0, 0.5), (0, S45, 0, S45))
UPPER = matmul(ARMATURE, trs((0, 0.5, 0)))
LOWER = matmul(UPPER, trs((0, 1, 0)))
JOINTS = [UPPER, LOWER]


def boxes():
    """Control points (root space, metres), their weights [(joint, w)], and quads."""
    pts, weights, quads = [], [], []
    for j, h in ((0, 0.25), (1, 0.2)):
        base = len(pts)
        for y in (0, 1):
            for x, z in ((-h, -h), (h, -h), (h, h), (-h, h)):
                pts.append(point(JOINTS[j], (x, y, z)))
                weights.append([(0, 3), (1, 1)] if j == 0 and y == 1 else [(j, 1)])
        b, t = [base + i for i in range(4)], [base + 4 + i for i in range(4)]
        # Counter-clockwise seen from outside (right-handed).
        quads += [[b[0], b[1], b[2], b[3]][::-1], [t[0], t[1], t[2], t[3]],
                  [b[0], t[0], t[1], b[1]][::-1], [b[1], t[1], t[2], b[2]][::-1],
                  [b[2], t[2], t[3], b[3]][::-1], [b[3], t[3], t[0], b[0]][::-1]]
    return pts, weights, quads


def face_normal(pts, q):
    a, b, c = pts[q[0]], pts[q[1]], pts[q[2]]
    u = [b[k] - a[k] for k in range(3)]
    v = [c[k] - a[k] for k in range(3)]
    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    l = math.sqrt(sum(x * x for x in n))
    return [x / l for x in n]


def axis_angle(ax, deg):
    s = math.sin(math.radians(deg) / 2)
    return [ax[0] * s, ax[1] * s, ax[2] * s, math.cos(math.radians(deg) / 2)]


def gltf(anims=('Wave', 'Nod/Big'), mesh=True, tail=False, names=('Upper', 'Lower')):
    """The rig as glTF. Other files for importing clips onto it (see
    multiclipcheck.py): `anims` picks the animations, without `mesh` it is the
    armature alone (no mesh, no skin, as Mixamo's "Without Skin"), `tail` adds
    joint "Tail" under Lower that Nod/Big turns 30 degrees about Z, and
    `names` renames Upper and Lower."""
    pts, weights, quads = boxes()
    pos, nrm, joints, wts, idx = [], [], [], [], []
    for q in quads:  # four vertices per face, so normals stay flat
        n = face_normal(pts, q)
        base = len(pos)
        for i in q:
            pos.append(pts[i])
            nrm.append(n)
            w = weights[i] + [(0, 0)] * (4 - len(weights[i]))
            total = sum(x[1] for x in w)
            joints.append([x[0] for x in w])
            wts.append([x[1] / total for x in w])
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    bin_, views, accs = bytearray(), [], []

    def acc(fmt, ctype, typ, values, extra=None):
        while len(bin_) % 4: bin_.append(0)
        flat = [x for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        data = struct.pack('<%d%s' % (len(flat), fmt), *flat)
        views.append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(data)})
        bin_.extend(data)
        a = {'bufferView': len(views) - 1, 'componentType': ctype, 'count': len(values), 'type': typ}
        a.update(extra or {})
        accs.append(a)
        return len(accs) - 1

    if mesh:
        P = acc('f', 5126, 'VEC3', pos, {'min': [min(p[k] for p in pos) for k in range(3)], 'max': [max(p[k] for p in pos) for k in range(3)]})
        N = acc('f', 5126, 'VEC3', nrm)
        J = acc('H', 5123, 'VEC4', joints)
        W = acc('f', 5126, 'VEC4', wts)
        I = acc('H', 5123, 'SCALAR', idx)
        ibm = [[inverse(m)[r][c] for c in range(4) for r in range(4)] for m in JOINTS]  # column-major
        IB = acc('f', 5126, 'MAT4', ibm)
    q90 = axis_angle((0, 1, 0), 90)
    q45 = axis_angle((1, 0, 0), 45)
    t3 = acc('f', 5126, 'SCALAR', [0, 0.5, 1], {'min': [0], 'max': [1]})
    t2 = acc('f', 5126, 'SCALAR', [0, 0.5], {'min': [0], 'max': [0.5]})
    t01 = acc('f', 5126, 'SCALAR', [0, 1], {'min': [0], 'max': [1]})
    wave_r = acc('f', 5126, 'VEC4', [[0, 0, 0, 1], q90, [0, 0, 0, 1]])
    wave_t = acc('f', 5126, 'VEC3', [[0, 0.5, 0], [0, 0.75, 0]])
    # Cubic spline: in-tangent, value, out-tangent per key.
    nod_r = acc('f', 5126, 'VEC4', [[0, 0, 0, 0], [0, 0, 0, 1], [0.1, 0, 0, 0], [0.1, 0, 0, 0], q45, [0, 0, 0, 0]])
    nodes = [
        {'name': 'Armature', 'translation': [1, 0, 0.5], 'rotation': [0, S45, 0, S45], 'children': [1, 3] if mesh else [1]},
        {'name': names[0], 'translation': [0, 0.5, 0], 'children': [2]},
        {'name': names[1], 'translation': [0, 1, 0]},
    ]
    if mesh: nodes.append({'name': 'Body', 'mesh': 0, 'skin': 0, 'translation': [5, 0, 0]})
    animations = [
        {'name': 'Wave', 'samplers': [{'input': t3, 'output': wave_r}, {'input': t2, 'output': wave_t, 'interpolation': 'STEP'}],
         'channels': [{'sampler': 0, 'target': {'node': 2, 'path': 'rotation'}},
                      {'sampler': 1, 'target': {'node': 1, 'path': 'translation'}}]},
        {'name': 'Nod/Big', 'samplers': [{'input': t01, 'output': nod_r, 'interpolation': 'CUBICSPLINE'}],
         'channels': [{'sampler': 0, 'target': {'node': 1, 'path': 'rotation'}}]},
    ]
    if tail:
        nodes[2]['children'] = [len(nodes)]
        nodes.append({'name': 'Tail', 'translation': [0, 0.5, 0]})
        tail_r = acc('f', 5126, 'VEC4', [[0, 0, 0, 1], axis_angle((0, 0, 1), 30)])
        animations[1]['samplers'].append({'input': t01, 'output': tail_r})
        animations[1]['channels'].append({'sampler': 1, 'target': {'node': len(nodes) - 1, 'path': 'rotation'}})
    j = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': nodes,
    }
    if mesh:
        j['meshes'] = [{'primitives': [{'attributes': {'POSITION': P, 'NORMAL': N, 'JOINTS_0': J, 'WEIGHTS_0': W}, 'indices': I}]}]
        j['skins'] = [{'joints': [1, 2], 'inverseBindMatrices': IB}]
    j.update({
        'animations': [a for a in animations if a['name'] in anims],
        'accessors': accs, 'bufferViews': views,
        'buffers': [{'byteLength': len(bin_), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(bin_)).decode()}],
    })
    return json.dumps(j, indent=1)


KTIME = 46186158000  # FBX time units per second


def fbx(anims=('Wave', 'Nod/Big'), mesh=True, tail=False, names=('Upper', 'Lower')):
    """The rig as FBX, with the options gltf() has (Tail turns about Z in Nod/Big)."""
    pts, weights, quads = boxes()
    body = 0.3  # metres along X
    cp = [((p[0] - body) * 100, p[1] * 100, p[2] * 100) for p in pts]
    num = lambda xs: ','.join('%.9g' % x for x in xs)
    pvi = [i if k < 3 else -i - 1 for q in quads for k, i in enumerate(q)]
    normals = [c for q in quads for _ in q for c in face_normal(pts, q)]
    # Column-major, translation in centimetres. Transform is the mesh's bind
    # transform in the bone's space, TransformLink the bone's world.
    cm = lambda m: [m[r][c] * (100 if r < 3 and c == 3 else 1) for c in range(4) for r in range(4)]
    clusters = ''
    for j, name in enumerate(names):
        ix = [i for i, w in enumerate(weights) if any(x[0] == j for x in w)]
        ws = [dict(weights[i])[j] for i in ix]  # 3 and 1, not normalised
        clusters += '''	Deformer: %d, "SubDeformer::%s", "Cluster" {
		Version: 100
		Indexes: *%d { a: %s }
		Weights: *%d { a: %s }
		Transform: *16 { a: %s }
		TransformLink: *16 { a: %s }
	}
''' % (510 + j, name, len(ix), num(ix), len(ws), num(ws), num(cm(matmul(inverse(JOINTS[j]), trs((body, 0, 0))))), num(cm(JOINTS[j])))

    def curve(cid, times, values, flag):
        return '''	AnimationCurve: %d, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4009
		KeyTime: *%d { a: %s }
		KeyValueFloat: *%d { a: %s }
		KeyAttrFlags: *1 { a: %d }
		KeyAttrDataFloat: *4 { a: 0,0,0,0 }
		KeyAttrRefCount: *1 { a: %d }
	}
''' % (cid, len(times), ','.join(str(int(t * KTIME)) for t in times), len(values), num(values), flag, len(times))

    linear, constant = 4, 2
    model = lambda mid, name, kind, props: '''	Model: %d, "Model::%s", "%s" {
		Version: 232
		Properties70:  {
%s		}
	}
''' % (mid, name, kind, ''.join('			P: %s\n' % p for p in props))
    attr = lambda aid, name: '''	NodeAttribute: %d, "NodeAttribute::%s", "LimbNode" {
		TypeFlags: "Skeleton"
	}
''' % (aid, name)
    rotNode = lambda cid: '''	AnimationCurveNode: %d, "AnimCurveNode::R", "" {
		Properties70:  {
			P: "d|X", "Number", "", "A",0
			P: "d|Y", "Number", "", "A",0
			P: "d|Z", "Number", "", "A",0
		}
	}
''' % cid
    geometry = '''	Geometry: 100, "Geometry::Body", "Mesh" {
		Vertices: *%d { a: %s }
		PolygonVertexIndex: *%d { a: %s }
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *%d { a: %s }
		}
		Layer: 0 {
			Version: 100
			LayerElement:  { Type: "LayerElementNormal" TypedIndex: 0 }
		}
	}
''' % (len(cp) * 3, num(c for p in cp for c in p), len(pvi), num(pvi), len(normals), num(normals))
    objects = geometry if mesh else ''
    objects += model(200, 'Armature', 'Null', ['"Lcl Translation", "Lcl Translation", "", "A",100,0,50',
                                               '"Lcl Rotation", "Lcl Rotation", "", "A",0,90,0'])
    objects += model(201, names[0], 'LimbNode', ['"Lcl Translation", "Lcl Translation", "", "A",0,50,0'])
    objects += model(202, names[1], 'LimbNode', ['"Lcl Translation", "Lcl Translation", "", "A",0,100,0'])
    if mesh: objects += model(203, 'Body', 'Mesh', ['"Lcl Translation", "Lcl Translation", "", "A",%g,0,0' % (body * 100)])
    if tail: objects += model(204, 'Tail', 'LimbNode', ['"Lcl Translation", "Lcl Translation", "", "A",0,50,0'])
    objects += attr(300, names[0]) + attr(301, names[1]) + (attr(302, 'Tail') if tail else '')
    if mesh:
        objects += '''	Deformer: 500, "Deformer::Skin", "Skin" {
		Version: 101
		Link_DeformAcuracy: 50
	}
''' + clusters
    stack = lambda sid, name: '''	AnimationStack: %d, "AnimStack::%s", "" {
		Properties70:  {
			P: "LocalStart", "KTime", "Time", "",0
			P: "LocalStop", "KTime", "Time", "",%d
		}
	}
	AnimationLayer: %d, "AnimLayer::BaseLayer", "" {
	}
''' % (sid, name, KTIME, sid + 1)
    links = ['"OO",200,0', '"OO",201,200', '"OO",202,201']
    if mesh: links.append('"OO",203,0')
    links += ['"OO",300,201', '"OO",301,202']
    if mesh: links += ['"OO",100,203', '"OO",500,100', '"OO",510,500', '"OO",511,500', '"OO",201,510', '"OO",202,511']
    if tail: links += ['"OO",204,202', '"OO",302,204']
    if 'Wave' in anims:
        objects += stack(600, 'Wave') + rotNode(602) + '''	AnimationCurveNode: 603, "AnimCurveNode::T", "" {
		Properties70:  {
			P: "d|X", "Number", "", "A",0
			P: "d|Y", "Number", "", "A",50
			P: "d|Z", "Number", "", "A",0
		}
	}
''' + curve(604, [0, 0.5, 1], [0, 90, 0], linear) + curve(605, [0, 0.5], [50, 75], constant)
        links += ['"OO",601,600', '"OO",602,601', '"OO",603,601', '"OP",602,202, "Lcl Rotation"', '"OP",603,201, "Lcl Translation"',
                  '"OP",604,602, "d|Y"', '"OP",605,603, "d|Y"']
    if 'Nod/Big' in anims:
        objects += stack(700, 'Nod/Big') + rotNode(702) + curve(704, [0, 1], [0, 45], linear)
        links += ['"OO",701,700', '"OO",702,701', '"OP",702,201, "Lcl Rotation"', '"OP",704,702, "d|X"']
        if tail:
            objects += rotNode(705) + curve(706, [0, 1], [0, 30], linear)
            links += ['"OO",705,701', '"OP",705,204, "Lcl Rotation"', '"OP",706,705, "d|Z"']
    return '''; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
%s}
Connections:  {
%s}
''' % (objects, ''.join('\tC: %s\n' % l for l in links))


# The files multiclipcheck.py imports clips from: the host with Wave alone,
# Nod/Big alone on the armature (no mesh) with an extra joint the host lacks,
# and an armature whose joints are named otherwise.
EXTRA = {
    'host': dict(anims=('Wave',)),
    'nod': dict(anims=('Nod/Big',), mesh=False, tail=True),
    'alien': dict(anims=('Nod/Big',), mesh=False, names=('BoneA', 'BoneB')),
}


if __name__ == '__main__':
    open(os.path.join(here, 'rig.gltf'), 'w').write(gltf() + '\n')
    open(os.path.join(here, 'rig.fbx'), 'w').write(fbx())
    for name, opts in EXTRA.items():
        open(os.path.join(here, 'rig_%s.gltf' % name), 'w').write(gltf(**opts) + '\n')
        open(os.path.join(here, 'rig_%s.fbx' % name), 'w').write(fbx(**opts))
