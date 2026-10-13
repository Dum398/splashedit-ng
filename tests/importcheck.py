# Builds small glTF files here (a .glb with an embedded texture, a .gltf with
# a data-URI buffer, an external texture, no normals and a mirrored node) and
# an OBJ+MTL whose texture is found by bare name in a Textures/ folder, an ASCII
# FBX (Z-up, centimetres) whose texture is found in tex/,
# imports them with `splashpack-cli import` and checks the .mesh against the
# conversion worked out independently: X mirrored, winding reversed so every
# triangle's cross product points along its normal (as in the bundled
# courtyard meshes), V flipped, node transforms baked, colours read, textures
# written unflipped, large textures scaled to 256. Then the import settings:
# scale, an up axis set for glTF, FBX and OBJ, normals recomputed on a cube at
# two smoothing angles, and a sidecar beside the source. Then exports a scene
# that uses the import.
# usage: python3 importcheck.py <splashpack-cli>
import json, math, os, struct, subprocess, sys, tempfile, zlib, base64

cli = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
fails = checks = 0


def check(name, ok, detail=''):
    global fails, checks
    checks += 1
    fails += not ok
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + str(detail)))


def png(w, h, px):  # px: rows top-down of (r, g, b, a)
    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in px)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def readpng(path):  # 8-bit RGBA only, which is what the importer writes
    d = open(path, 'rb').read()
    p, idat, w = 8, b'', 0
    while p < len(d):
        n, = struct.unpack_from('>I', d, p)
        t = d[p + 4:p + 8]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', d, p + 8)
            assert depth == 8 and ctype == 6, (depth, ctype)
        if t == b'IDAT':
            idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * 4, [], bytearray(w * 4)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)])
        prev = line
    return w, h, rows


class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.j = dict(asset={'version': '2.0'}, buffers=[], bufferViews=[], accessors=[], meshes=[], nodes=[],
                      scenes=[{'nodes': []}], scene=0, materials=[], textures=[], images=[])

    def view(self, data):
        while len(self.bin) % 4: self.bin += b'\0'
        self.j['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)})
        self.bin += data
        return len(self.j['bufferViews']) - 1

    def acc(self, fmt, ctype, typ, values, normalized=False):
        flat = [x for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        a = {'bufferView': self.view(struct.pack('<%d%s' % (len(flat), fmt), *flat)), 'componentType': ctype,
             'count': len(values), 'type': typ}
        if normalized: a['normalized'] = True
        if typ == 'VEC3' and ctype == 5126:
            a['min'] = [min(v[k] for v in values) for k in range(3)]
            a['max'] = [max(v[k] for v in values) for k in range(3)]
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def mesh(self, prims):
        self.j['meshes'].append({'primitives': prims})
        return len(self.j['meshes']) - 1

    def node(self, **kw):
        self.j['nodes'].append(kw)
        self.j['scenes'][0]['nodes'].append(len(self.j['nodes']) - 1)

    def glb(self, path):
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        js = json.dumps(self.j).encode()
        js += b' ' * (-len(js) % 4)
        b = bytes(self.bin) + b'\0' * (-len(self.bin) % 4)
        out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(b))
        out += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(b), 0x004E4942) + b
        open(path, 'wb').write(out)

    def gltf(self, path):
        self.j['buffers'] = [{'byteLength': len(self.bin),
                              'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(self.bin)).decode()}]
        json.dump(self.j, open(path, 'w'))


def run(*args):
    return subprocess.run([cli, *args], capture_output=True, text=True)


def crossdot(m, tri):
    P = lambda i: m['positions'][3 * i:3 * i + 3]
    a, b, c = (P(i) for i in tri)
    u = [b[k] - a[k] for k in range(3)]
    v = [c[k] - a[k] for k in range(3)]
    cr = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    n = m['normals'][3 * tri[0]:3 * tri[0] + 3]
    return sum(cr[k] * n[k] for k in range(3))


# The convention every bundled mesh follows, measured rather than assumed.
crate = json.load(open(os.path.join(here, '..', 'examples', 'courtyard', 'meshes', 'crate.mesh')))
ctris = [crate['submeshes'][0][t:t + 3] for t in range(0, len(crate['submeshes'][0]), 3)]
check('courtyard crate: cross(b-a, c-a) points along the normal', all(crossdot(crate, t) > 0 for t in ctris))

# 1. .glb: a quad facing +Z (glTF front), node translated and scaled, normals,
# UVs, byte colours, a textured material with a colour factor.
tex = [[(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)],
       [(10, 20, 30, 255), (40, 50, 60, 255), (70, 80, 90, 128), (0, 0, 0, 0)]]
g = Gltf()
quad = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
cols = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 51)]
img = g.view(png(4, 2, tex))
g.j['images'].append({'bufferView': img, 'mimeType': 'image/png'})
g.j['textures'].append({'source': 0})
g.j['materials'].append({'pbrMetallicRoughness': {'baseColorFactor': [0.5, 0.25, 1, 1], 'baseColorTexture': {'index': 0}}})
g.mesh([{'attributes': {'POSITION': g.acc('f', 5126, 'VEC3', quad), 'NORMAL': g.acc('f', 5126, 'VEC3', [(0, 0, 1)] * 4),
                        'TEXCOORD_0': g.acc('f', 5126, 'VEC2', uvs), 'COLOR_0': g.acc('B', 5121, 'VEC4', cols, True)},
         'indices': g.acc('H', 5123, 'SCALAR', [0, 1, 2, 0, 2, 3]), 'material': 0}])
g.node(mesh=0, translation=[1, 2, 3], scale=[2, 2, 2])
proj = os.path.join(tmp, 'proj')
os.makedirs(proj)
g.glb(os.path.join(tmp, 'Sign Post.glb'))
r = run('import', os.path.join(tmp, 'Sign Post.glb'), '--project', proj)
check('glb import exits 0', r.returncode == 0, r.stderr)
check('mesh lands in models/ with a safe name', 'mesh models/Sign_Post.mesh (2 triangles)' in r.stdout, r.stdout)
m = json.load(open(os.path.join(proj, 'models', 'Sign_Post.mesh')))
want = [(-(2 * x + 1), 2 * y + 2, 2 * z + 3) for x, y, z in quad]
got = [tuple(m['positions'][i:i + 3]) for i in range(0, len(m['positions']), 3)]
check('positions: node transform baked, X mirrored', got == want, got)
check('normals: X mirrored, still +Z', all(m['normals'][i:i + 3] == [0, 0, 1] for i in range(0, 12, 3)), m['normals'])
check('UVs: V flipped to a bottom-left origin', [tuple(m['uv'][i:i + 2]) for i in range(0, 8, 2)] == [(u, 1 - v) for u, v in uvs], m['uv'])
gc = [tuple(round(c * 255) for c in m['colors'][i:i + 4]) for i in range(0, 16, 4)]
check('vertex colours read from normalized bytes', gc == cols, gc)
tris = [m['submeshes'][0][t:t + 3] for t in range(0, 6, 3)]
check('winding matches the courtyard meshes', all(crossdot(m, t) > 0 for t in tris), tris)
check('material colour and texture', 'material 0 texture models/Sign_Post.png colour 0.5 0.25 1 1' in r.stdout, r.stdout)
w, h, rows = readpng(os.path.join(proj, 'models', 'Sign_Post.png'))
check('texture written top row first, pixels kept', (w, h, rows) == (4, 2, tex), (w, h, rows))

# 2. .gltf: data-URI buffer, external PNG, no normals, two materials (one
# untextured, one with no material at all), a node mirrored by negative scale.
g = Gltf()
tri = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
open(os.path.join(tmp, 'wood grain.png'), 'wb').write(png(2, 2, [[(1, 2, 3, 255)] * 2] * 2))
g.j['images'].append({'uri': 'wood%20grain.png'})
g.j['textures'].append({'source': 0})
g.j['materials'] += [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}},
                     {'pbrMetallicRoughness': {'baseColorFactor': [0, 1, 0, 1]}}]
pos = g.acc('f', 5126, 'VEC3', tri)
g.mesh([{'attributes': {'POSITION': pos}, 'material': 0}, {'attributes': {'POSITION': pos}, 'material': 1},
        {'attributes': {'POSITION': pos}}])
g.node(mesh=0)
g.node(mesh=0, scale=[-1, 1, 1])
g.gltf(os.path.join(tmp, 'pieces.gltf'))
r = run('import', os.path.join(tmp, 'pieces.gltf'), '--project', proj)
check('gltf import exits 0', r.returncode == 0, r.stderr)
m = json.load(open(os.path.join(proj, 'models', 'pieces.mesh')))
check('one submesh per material, triangles grouped', [len(s) for s in m['submeshes']] == [6, 6, 6], m['submeshes'])
allt = [s[t:t + 3] for s in m['submeshes'] for t in range(0, len(s), 3)]
check('flat normals are unit and face outward, mirrored node included',
      all(crossdot(m, t) > 0 and abs(sum(x * x for x in m['normals'][3 * t[0]:3 * t[0] + 3]) - 1) < 1e-5 for t in allt),
      [crossdot(m, t) for t in allt])
# The triangle faces +Z (counter-clockwise seen from +Z); a mirror in X keeps
# that, so every flat normal must come out +Z, which ties winding to geometry.
check('flat normals face +Z, mirrored node included',
      all(m['normals'][i:i + 3] == [0, 0, 1] for i in range(0, len(m['normals']), 3)), m['normals'])
check('external texture (URI decoded) and colour factor',
      'material 0 texture models/pieces.png colour 1 1 1 1' in r.stdout and 'material 1 texture - colour 0 1 0 1' in r.stdout and
      'material 2 texture - colour 1 1 1 1' in r.stdout, r.stdout)
check('no UV or colour arrays when the file has none', 'uv' not in m and 'colors' not in m, list(m))

# 3. A texture too large for the PS1 is scaled to fit.
g = Gltf()
big = g.view(png(512, 300, [[(x % 256, y % 256, 0, 255) for x in range(512)] for y in range(300)]))
g.j['images'].append({'bufferView': big, 'mimeType': 'image/png'})
g.j['textures'].append({'source': 0})
g.j['materials'].append({'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}})
g.mesh([{'attributes': {'POSITION': g.acc('f', 5126, 'VEC3', tri)}, 'material': 0}])
g.node(mesh=0)
g.glb(os.path.join(tmp, 'big.glb'))
r = run('import', os.path.join(tmp, 'big.glb'), '--project', proj)
w, h, _ = readpng(os.path.join(proj, 'models', 'big.png'))
check('512x300 texture scaled to 256x150 with a warning', (w, h) == (256, 150) and 'scaled down to 256 x 150' in r.stdout, (w, h, r.stdout))

# 3b. .obj + .mtl through ufbx: a textured quad, the texture found by bare name in a
# Textures/ folder next to the model; V is already bottom-up in OBJ so it is kept.
objdir = os.path.join(tmp, 'objs')
os.makedirs(os.path.join(objdir, 'Textures'))
oquad = [(0, 0, 0), (2, 0, 0), (2, 1, 0), (0, 1, 0)]
ouv = [(0.1, 0.2), (0.9, 0.2), (0.9, 0.7), (0.1, 0.7)]
open(os.path.join(objdir, 'brick wall.obj'), 'w').write(
    'mtllib brick.mtl\n' + ''.join('v %g %g %g\n' % p for p in oquad) + ''.join('vt %g %g\n' % t for t in ouv) +
    'vn 0 0 1\nusemtl bricks\nf 1/1/1 2/2/1 3/3/1 4/4/1\n')
open(os.path.join(objdir, 'brick.mtl'), 'w').write('newmtl bricks\nKd 0.5 0.25 1\nmap_Kd bricks.png\n')
open(os.path.join(objdir, 'Textures', 'bricks.png'), 'wb').write(png(4, 2, tex))
r = run('import', os.path.join(objdir, 'brick wall.obj'), '--project', proj)
check('obj import exits 0', r.returncode == 0, r.stderr)
check('obj quad is two triangles in models/', 'mesh models/brick_wall.mesh (2 triangles)' in r.stdout, r.stdout)
m = json.load(open(os.path.join(proj, 'models', 'brick_wall.mesh')))
got = [tuple(m['positions'][i:i + 3]) for i in range(0, len(m['positions']), 3)]
check('obj positions: X mirrored', set(got) == {(-x, y, z) for x, y, z in oquad}, got)
check('obj normals: X mirrored, still +Z', all(m['normals'][i:i + 3] == [0, 0, 1] for i in range(0, len(m['normals']), 3)), m['normals'])
ugot = {p: tuple(round(c, 5) for c in m['uv'][2 * i:2 * i + 2]) for i, p in enumerate(got)}
check('obj UVs: V kept, OBJ already has a bottom-left origin',
      all(ugot[(-x, y, z)] == t for (x, y, z), t in zip(oquad, ouv)), ugot)
otris = [m['submeshes'][0][t:t + 3] for t in range(0, len(m['submeshes'][0]), 3)]
check('obj winding matches the courtyard meshes', len(otris) == 2 and all(crossdot(m, t) > 0 for t in otris), otris)
check('obj texture found by bare name in Textures/, colour from Kd',
      'material 0 texture models/brick_wall.png colour 0.5 0.25 1 1' in r.stdout and 'warning' not in r.stdout, r.stdout)
pngfile = os.path.join(proj, 'models', 'brick_wall.png')
check('obj texture pixels kept', os.path.exists(pngfile) and readpng(pngfile) == (4, 2, tex), pngfile)

# An OBJ with no normals (flat shading, faces +Z) whose texture does not exist is a
# warning that names the file, not a failure.
open(os.path.join(objdir, 'lost.obj'), 'w').write(
    'mtllib lost.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl gone\nf 1 2 3\n')
open(os.path.join(objdir, 'lost.mtl'), 'w').write('newmtl gone\nmap_Kd missing.png\n')
r = run('import', os.path.join(objdir, 'lost.obj'), '--project', proj)
check('obj with a missing texture still imports', r.returncode == 0 and 'mesh models/lost.mesh (1 triangles)' in r.stdout, r.stderr + r.stdout)
check('missing texture warns by name, material left untextured',
      'warning: texture missing.png was not found' in r.stdout and 'material 0 texture - ' in r.stdout, r.stdout)
m = json.load(open(os.path.join(proj, 'models', 'lost.mesh')))
check('obj flat normals face +Z with winding intact',
      all(m['normals'][i:i + 3] == [0, 0, 1] for i in range(0, len(m['normals']), 3)) and crossdot(m, m['submeshes'][0]) > 0, m)

# 3c. .fbx through ufbx: a hand-written ASCII FBX, Z-up and in centimetres, so the
# axis and unit conversion is exercised. Source (x, y, z) becomes glTF (x, z, -y) / 100,
# so this quad stands where the OBJ one does, facing +Z. The texture is named by a
# path that does not exist and is found by bare name in tex/.
fbxdir = os.path.join(tmp, 'fbxs')
os.makedirs(os.path.join(fbxdir, 'tex'))
open(os.path.join(fbxdir, 'tex', 'Bricks.png'), 'wb').write(png(4, 2, tex))
fquad = [(0, 0, 0), (100, 0, 0), (100, 0, 100), (0, 0, 100)]
fnum = lambda vs: ','.join('%g' % c for v in vs for c in v)
open(os.path.join(fbxdir, 'Wall.fbx'), 'w').write('''; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",2
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",1
		P: "FrontAxisSign", "int", "Integer", "",-1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 100, "Geometry::Wall", "Mesh" {
		Vertices: *12 { a: %s }
		PolygonVertexIndex: *4 { a: 0,1,2,-4 }
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *12 { a: 0,-1,0,0,-1,0,0,-1,0,0,-1,0 }
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "UVMap"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 { a: %s }
			UVIndex: *4 { a: 0,1,2,3 }
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 { a: 0 }
		}
		Layer: 0 {
			Version: 100
			LayerElement:  { Type: "LayerElementNormal" TypedIndex: 0 }
			LayerElement:  { Type: "LayerElementMaterial" TypedIndex: 0 }
			LayerElement:  { Type: "LayerElementUV" TypedIndex: 0 }
		}
	}
	Model: 200, "Model::Wall", "Mesh" {
		Version: 232
		Properties70:  {
		}
	}
	Material: 300, "Material::Brick", "" {
		Version: 102
		ShadingModel: "lambert"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.5,0.25,1
		}
	}
	Texture: 400, "Texture::Brick", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::Brick"
		FileName: "C:\\\\Nowhere\\\\Art\\\\bricks.png"
		RelativeFilename: "Art\\\\bricks.png"
	}
}
Connections:  {
	C: "OO",200,0
	C: "OO",100,200
	C: "OO",300,200
	C: "OP",400,300, "DiffuseColor"
}
''' % (fnum(fquad), fnum(ouv)))
r = run('import', os.path.join(fbxdir, 'Wall.fbx'), '--project', proj)
check('fbx import exits 0', r.returncode == 0, r.stderr)
check('fbx quad is two triangles', 'mesh models/Wall.mesh (2 triangles)' in r.stdout, r.stdout)
m = json.load(open(os.path.join(proj, 'models', 'Wall.mesh')))
got = {tuple(round(c, 4) + 0.0 for c in m['positions'][i:i + 3]) for i in range(0, len(m['positions']), 3)}
check('fbx: Z-up centimetres became Y-up metres, X mirrored', got == {(0, 0, 0), (-1, 0, 0), (-1, 1, 0), (0, 1, 0)}, got)
check('fbx normals face +Z like the OBJ', all(abs(m['normals'][i + 2] - 1) < 1e-5 for i in range(0, len(m['normals']), 3)), m['normals'])
ftris = [m['submeshes'][0][t:t + 3] for t in range(0, len(m['submeshes'][0]), 3)]
check('fbx winding matches the courtyard meshes', len(ftris) == 2 and all(crossdot(m, t) > 0 for t in ftris), ftris)
fuv = {tuple(round(c, 4) + 0.0 for c in m['positions'][3 * i:3 * i + 3]): tuple(round(c, 5) for c in m['uv'][2 * i:2 * i + 2])
       for i in range(len(m['positions']) // 3)}
check('fbx UVs: V kept', fuv[(0, 0, 0)] == ouv[0] and fuv[(-1, 1, 0)] == ouv[2], fuv)
check('fbx texture found by bare name in tex/, colour from DiffuseColor',
      'material 0 texture models/Wall.png colour 0.5 0.25 1 1' in r.stdout and 'warning' not in r.stdout, r.stdout)

# 4. The node tree. Each node's transform, composed down the tree the way a
# scene composes Unity transforms (full matrices), over its own .mesh must
# give back that node's part of the baked mesh: the baked mesh lists the nodes'
# vertices in tree order, so the concatenation is compared vertex by vertex.
def nodes_of(out):
    found = []
    for line in out.splitlines():
        if not line.startswith('node '): continue
        w = line.split(' ')
        found.append({'parent': int(w[3]), 'pos': [float(x) for x in w[5:8]], 'rot': [float(x) for x in w[9:13]],
                      'scale': [float(x) for x in w[14:17]], 'mesh': w[18], 'materials': int(w[20]),
                      'name': ' '.join(w[22:])})
    return found


def trs(t, q, s):  # Matrix4x4.TRS, rows of a 3x4
    x, y, z, w = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    return [[r[i][0] * s[0], r[i][1] * s[1], r[i][2] * s[2], t[i]] for i in range(3)]


def mul(a, b):  # 3x4 affine product a * b
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def composed(proj, nodes):
    world, pts = [], []
    for n in nodes:
        local = trs(n['pos'], n['rot'], n['scale'])
        world.append(mul(world[n['parent']], local) if n['parent'] >= 0 else local)
        if n['mesh'] == '-': continue
        nm = json.load(open(os.path.join(proj, n['mesh'])))
        M = world[-1]
        for i in range(0, len(nm['positions']), 3):
            p = nm['positions'][i:i + 3]
            pts.append(tuple(sum(M[r][k] * p[k] for k in range(3)) + M[r][3] for r in range(3)))
    return pts


def composes(proj, nodes, baked):
    want = [tuple(baked['positions'][i:i + 3]) for i in range(0, len(baked['positions']), 3)]
    got = composed(proj, nodes)
    err = max((abs(a - b) for p, q in zip(got, want) for a, b in zip(p, q)), default=1e9)
    return len(got) == len(want) and err < 1e-4, 'max error %g, %d vs %d vertices' % (err, len(got), len(want))


def axis_angle(ax, deg):
    l = math.sqrt(sum(a * a for a in ax))
    s = math.sin(math.radians(deg) / 2)
    return [ax[0] / l * s, ax[1] / l * s, ax[2] / l * s, math.cos(math.radians(deg) / 2)]


# A .gltf (base64 buffer) with a parent, two children under it named so their
# file names collide, and a grandchild given as a matrix. Rotations about
# skewed axes, so a wrong mirror of any quaternion component shows.
g = Gltf()
tri = g.acc('f', 5126, 'VEC3', [(0, 0, 0), (1, 0, 0.5), (0.25, 1, -0.5)])
nrm = g.acc('f', 5126, 'VEC3', [(0, 0, 1)] * 3)
g.mesh([{'attributes': {'POSITION': tri, 'NORMAL': nrm}}])
q_body, q_arm = axis_angle((1, 2, 3), 40), axis_angle((-2, 1, 0.5), 75)
qm = axis_angle((0.3, -1, 2), 120)
x, y, z, w = qm
rm = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
      [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
      [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
mat = [rm[r][c] * 1.5 for c in range(3) for r in range(3)]
mat = mat[0:3] + [0] + mat[3:6] + [0] + mat[6:9] + [0] + [0.25, -2, 0.75, 1]  # column-major, uniform scale 1.5
g.j['nodes'] += [{'name': 'Body', 'mesh': 0, 'translation': [1, 2, 3], 'rotation': q_body, 'scale': [2, 2, 2], 'children': [1, 3]},
                 {'name': 'Arm.L', 'mesh': 0, 'translation': [0.5, -1, 2], 'rotation': q_arm, 'scale': [1, 0.5, 3], 'children': [2]},
                 {'name': 'Tip', 'mesh': 0, 'matrix': mat},
                 {'name': 'Arm_L', 'mesh': 0, 'translation': [-3, 0, 0]}]
g.j['scenes'][0]['nodes'] = [0]
g.gltf(os.path.join(tmp, 'robot.gltf'))
r = run('import', os.path.join(tmp, 'robot.gltf'), '--project', proj)
check('gltf tree import exits 0', r.returncode == 0, r.stderr)
nodes = nodes_of(r.stdout)
check('gltf tree: four nodes, parents before children',
      [(n['name'], n['parent']) for n in nodes] == [('Body', -1), ('Arm.L', 0), ('Tip', 1), ('Arm_L', 0)], nodes)
check('gltf tree: one .mesh per node, names file-safe and unique',
      [n['mesh'] for n in nodes] == ['models/robot/Body.mesh', 'models/robot/Arm_L.mesh', 'models/robot/Tip.mesh',
                                     'models/robot/Arm_L_2.mesh'] and all(n['materials'] == 1 for n in nodes), nodes)
check('gltf tree: the baked mesh is still written', 'mesh models/robot.mesh (4 triangles)' in r.stdout, r.stdout)
b = nodes[0]
check('gltf tree: position X mirrored', all(abs(a - c) < 1e-6 for a, c in zip(b['pos'], [-1, 2, 3])), b['pos'])
check('gltf tree: rotation (x, y, z, w) -> (x, -y, -z, w)',
      all(abs(a - c) < 1e-6 for a, c in zip(b['rot'], [q_body[0], -q_body[1], -q_body[2], q_body[3]])), b['rot'])
check('gltf tree: scale kept', all(abs(a - c) < 1e-5 for a, c in zip(b['scale'] + nodes[1]['scale'], [2, 2, 2, 1, 0.5, 3])), (b['scale'], nodes[1]['scale']))
check('gltf tree: matrix node split into a uniform 1.5 scale', all(abs(s - 1.5) < 1e-5 for s in nodes[2]['scale']), nodes[2]['scale'])
ok, why = composes(proj, nodes, json.load(open(os.path.join(proj, 'models', 'robot.mesh'))))
check('gltf tree: node transforms over node meshes give the baked mesh (1e-4)', ok, why)
local = json.load(open(os.path.join(proj, 'models', 'robot', 'Tip.mesh')))
check('gltf tree: a node mesh is in its own space, X mirrored',
      [round(c, 6) + 0.0 for c in local['positions']] == [0, 0, 0, -1, 0, 0.5, -0.25, 1, -0.5], local['positions'])
ltris = [local['submeshes'][0][t:t + 3] for t in range(0, len(local['submeshes'][0]), 3)]
check('gltf tree: node mesh winding matches the courtyard meshes', all(crossdot(local, t) > 0 for t in ltris), ltris)

# Re-importing replaces the node meshes: a node that is gone leaves no file.
g.j['nodes'][0]['children'] = [1]
g.gltf(os.path.join(tmp, 'robot.gltf'))
r = run('import', os.path.join(tmp, 'robot.gltf'), '--project', proj)
check('gltf tree: re-import drops meshes of nodes that are gone',
      sorted(os.listdir(os.path.join(proj, 'models', 'robot'))) == ['Arm_L.mesh', 'Body.mesh', 'Tip.mesh'],
      os.listdir(os.path.join(proj, 'models', 'robot')))

# An OBJ with two objects (`o`) and one with two groups (`g`): ufbx makes each
# a top-level node with an identity transform, the vertices in model space.
for kw in ('o', 'g'):
    open(os.path.join(objdir, 'pair_%s.obj' % kw), 'w').write(
        '%s Left\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n'
        '%s Right side\nv 2 0 0\nv 3 0 1\nv 2 1 1\nvn 0 0 1\nf 4//2 5//2 6//2\n' % (kw, kw))
    r = run('import', os.path.join(objdir, 'pair_%s.obj' % kw), '--project', proj)
    nodes = nodes_of(r.stdout)
    check('obj `%s`: two top-level nodes named after them' % kw,
          [(n['name'], n['parent'], n['mesh']) for n in nodes] ==
          [('Left', -1, 'models/pair_%s/Left.mesh' % kw), ('Right side', -1, 'models/pair_%s/Right_side.mesh' % kw)], r.stdout)
    ok, why = composes(proj, nodes, json.load(open(os.path.join(proj, 'models', 'pair_%s.mesh' % kw))))
    check('obj `%s`: node meshes give the baked mesh' % kw, ok, why)

# The FBX above (Z-up centimetres): ufbx's axis and unit conversion lives on
# its root node and must reach the top-level node's transform.
r = run('import', os.path.join(fbxdir, 'Wall.fbx'), '--project', proj)
nodes = nodes_of(r.stdout)
ok, why = composes(proj, nodes, json.load(open(os.path.join(proj, 'models', 'Wall.mesh'))))
check('fbx: one node, its transform over its mesh gives the baked mesh', len(nodes) == 1 and ok, (nodes, why))

# A model with a single unnamed node gives one node.
r = run('import', os.path.join(objdir, 'lost.obj'), '--project', proj)
nodes = nodes_of(r.stdout)
check('single-node model: one node', len(nodes) == 1 and nodes[0]['mesh'] == 'models/lost/node.mesh', r.stdout)

# 5. Refusals carry a message.
open(os.path.join(tmp, 'x.dae'), 'w').write('<COLLADA/>\n')
r = run('import', os.path.join(tmp, 'x.dae'), '--project', proj)
check('.dae refused with a reason', r.returncode == 1 and 'only .glb, .gltf, .fbx and .obj' in r.stderr, r.stderr)
open(os.path.join(tmp, 'junk.glb'), 'wb').write(b'not a model at all')
r = run('import', os.path.join(tmp, 'junk.glb'), '--project', proj)
check('garbage refused with a reason', r.returncode == 1 and 'junk.glb:' in r.stderr, r.stderr)

# 6. The import exports.
obj = lambda name, mesh, mats: {'name': name, 'active': True, 'transform': {'position': [0, 0, 2], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                                'components': [{'type': 'mesh', 'mesh': mesh, 'materials': mats}], 'children': []}
scene = {'format': 'splashedit-ng/scene', 'version': 1, 'settings': {'gteScaling': 100.0},
         'objects': [obj('Sign', 'models/Sign_Post.mesh', [{'texture': 'models/Sign_Post.png', 'color': [0.5, 0.25, 1, 1]}]),
                     obj('Pieces', 'models/pieces.mesh', [{'texture': 'models/pieces.png'}, {'color': [0, 1, 0, 1]}, {}])]}
json.dump(scene, open(os.path.join(proj, 'i.scene'), 'w'))
r = run('export', os.path.join(proj, 'i.scene'), '-o', os.path.join(tmp, 'i.splashpack'))
check('a scene using the imports exports', r.returncode == 0, r.stderr)

# 7. Import settings, each against the conversion worked out here: the .glb of
# section 1 (glTF world w = 2 * quad + (1, 2, 3), facing +Z) scaled and turned,
# the FBX and OBJ given an up axis of their own, a cube's normals recomputed,
# and a sidecar beside the source picked up by the CLI.
sproj = os.path.join(tmp, 'settings')
os.makedirs(sproj)
glb = os.path.join(tmp, 'Sign Post.glb')
world = [(2 * x + 1, 2 * y + 2, 2 * z + 3) for x, y, z in quad]


def positions(m):
    return [tuple(m['positions'][i:i + 3]) for i in range(0, len(m['positions']), 3)]


def close(a, b, eps=1e-5):
    return len(a) == len(b) and all(abs(x - y) < eps for p, q in zip(a, b) for x, y in zip(p, q))


r = run('import', glb, '--project', sproj, '--scale', '2')
m = json.load(open(os.path.join(sproj, 'models', 'Sign_Post.mesh')))
check('scale 2: every position doubles', close(positions(m), [(-2 * x, 2 * y, 2 * z) for x, y, z in world]), positions(m))
nodes = nodes_of(r.stdout)
check('scale 2: node translation doubles, node scale kept',
      len(nodes) == 1 and close([nodes[0]['pos'], nodes[0]['scale']], [(-2, 4, 6), (2, 2, 2)]), nodes)
ok, why = composes(sproj, nodes, m)
check('scale 2: node transform over node mesh gives the baked mesh', ok, why)

# Z up: the file is turned -90 degrees about X before the conversion, glTF
# (x, y, z) -> (x, z, -y), then X mirrored. The quad faced +Z, so it now faces +Y.
r = run('import', glb, '--project', sproj, '--up', 'z')
m = json.load(open(os.path.join(sproj, 'models', 'Sign_Post.mesh')))
check('up z (glTF): positions turned (x, y, z) -> (-x, z, -y)', close(positions(m), [(-x, z, -y) for x, y, z in world]), positions(m))
check('up z (glTF): normals turned from +Z to +Y', close(positions({'positions': m['normals']}), [(0, 1, 0)] * 4), m['normals'])
tris = [m['submeshes'][0][t:t + 3] for t in range(0, 6, 3)]
check('up z (glTF): winding still along the normal', all(crossdot(m, t) > 0 for t in tris), tris)
nodes = nodes_of(r.stdout)
h = math.sqrt(0.5)
check('up z (glTF): the top node is turned, rotation (-sqrt(1/2), 0, 0, sqrt(1/2)), position (-1, 3, -2)',
      len(nodes) == 1 and close([nodes[0]['rot'], nodes[0]['pos']], [(-h, 0, 0, h), (-1, 3, -2)]), nodes)
ok, why = composes(sproj, nodes, m)
check('up z (glTF): node transform over node mesh gives the baked mesh', ok, why)
r = run('import', glb, '--project', sproj, '--up', 'y')
m = json.load(open(os.path.join(sproj, 'models', 'Sign_Post.mesh')))
check('up y (glTF): as the file says', close(positions(m), [(-x, y, z) for x, y, z in world]), positions(m))

# The FBX says Z up. Taken as Y up its quad lies flat (only centimetres
# converted); taken as Z up it stands as it does when the file is believed.
fbx = os.path.join(fbxdir, 'Wall.fbx')
r = run('import', fbx, '--project', sproj, '--up', 'y')
m = json.load(open(os.path.join(sproj, 'models', 'Wall.mesh')))
got = {tuple(round(c, 4) + 0.0 for c in p) for p in positions(m)}
check('up y (FBX declaring Z up): the file axes are ignored, the quad lies flat',
      got == {(0, 0, 0), (-1, 0, 0), (-1, 0, 1), (0, 0, 1)}, got)
check('up y (FBX): normals -Y as in the file', all(abs(m['normals'][i + 1] + 1) < 1e-5 for i in range(0, len(m['normals']), 3)), m['normals'])
ok, why = composes(sproj, nodes_of(r.stdout), m)
check('up y (FBX): node transform over node mesh gives the baked mesh', ok, why)
r = run('import', fbx, '--project', sproj, '--up', 'z')
m = json.load(open(os.path.join(sproj, 'models', 'Wall.mesh')))
got = {tuple(round(c, 4) + 0.0 for c in p) for p in positions(m)}
check('up z (FBX declaring Z up): the same as the file', got == {(0, 0, 0), (-1, 0, 0), (-1, 1, 0), (0, 1, 0)}, got)
ok, why = composes(sproj, nodes_of(r.stdout), m)
check('up z (FBX): node transform over node mesh gives the baked mesh', ok, why)
r = run('import', os.path.join(objdir, 'brick wall.obj'), '--project', sproj, '--up', 'z')
m = json.load(open(os.path.join(sproj, 'models', 'brick_wall.mesh')))
got = {tuple(round(c, 5) + 0.0 for c in p) for p in positions(m)}
check('up z (OBJ): positions turned (x, y, z) -> (-x, z, -y)', got == {(-x + 0.0, z + 0.0, -y + 0.0) for x, y, z in oquad}, got)
check('up z (OBJ): normals +Y', all(abs(m['normals'][i + 1] - 1) < 1e-5 for i in range(0, len(m['normals']), 3)), m['normals'])

# A cube of 8 shared corners, every normal in the file pointing up (wrong).
# Recompute at 60 degrees: the 90-degree edges stay hard, each corner of a
# side gets that side's normal (24 vertices). At 120 they are smoothed: each
# corner gets the average of its three sides, its position over sqrt(3) (8 vertices).
corners = [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
cidx = []
for axis in range(3):
    for sign in (-1, 1):
        f = [i for i, c in enumerate(corners) if c[axis] == sign]
        a, b, c, d = f[0], f[1], f[3], f[2]  # around the side
        u = [corners[b][k] - corners[a][k] for k in range(3)]
        v = [corners[c][k] - corners[a][k] for k in range(3)]
        cr = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        if cr[axis] * sign < 0: b, d = d, b  # counter-clockwise seen from outside
        cidx += [a, b, c, a, c, d]
g = Gltf()
g.mesh([{'attributes': {'POSITION': g.acc('f', 5126, 'VEC3', corners), 'NORMAL': g.acc('f', 5126, 'VEC3', [(0, 1, 0)] * 8)},
         'indices': g.acc('H', 5123, 'SCALAR', cidx)}])
g.node(mesh=0)
g.glb(os.path.join(tmp, 'cube.glb'))
r = run('import', os.path.join(tmp, 'cube.glb'), '--project', sproj)
m = json.load(open(os.path.join(sproj, 'models', 'cube.mesh')))
check('cube, normals imported: kept as the file has them', all(m['normals'][i:i + 3] == [0, 1, 0] for i in range(0, len(m['normals']), 3)), m['normals'])


def cube_normals(m):  # (position, normal) of every corner of every triangle
    out = []
    for s in m['submeshes']:
        for t in range(0, len(s), 3):
            tri = s[t:t + 3]
            P = [m['positions'][3 * i:3 * i + 3] for i in tri]
            u = [P[1][k] - P[0][k] for k in range(3)]
            v = [P[2][k] - P[0][k] for k in range(3)]
            cr = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            l = math.sqrt(sum(c * c for c in cr))
            for i in tri: out.append((m['positions'][3 * i:3 * i + 3], m['normals'][3 * i:3 * i + 3], [c / l for c in cr]))
    return out


r = run('import', os.path.join(tmp, 'cube.glb'), '--project', sproj, '--normals', 'recompute', '--smoothing', '60')
m = json.load(open(os.path.join(sproj, 'models', 'cube.mesh')))
cn = cube_normals(m)
check('cube, recompute at 60: every corner has its side\'s normal (hard edges)',
      len(cn) == 36 and all(close([n], [f]) for p, n, f in cn), cn[:6])
check('cube, recompute at 60: 24 vertices, one per corner of each side', len(m['positions']) == 24 * 3, len(m['positions']) // 3)
r = run('import', os.path.join(tmp, 'cube.glb'), '--project', sproj, '--normals', 'recompute', '--smoothing', '120')
m = json.load(open(os.path.join(sproj, 'models', 'cube.mesh')))
cn = cube_normals(m)
s3 = 1 / math.sqrt(3)
check('cube, recompute at 120: every corner has its position over sqrt(3) (smooth)',
      len(cn) == 36 and all(close([n], [[c * s3 for c in p]]) for p, n, f in cn), cn[:6])
check('cube, recompute at 120: 8 vertices', len(m['positions']) == 8 * 3, len(m['positions']) // 3)
# Without normals in the file, Recompute replaces the flat shading too.
r = run('import', os.path.join(objdir, 'pair_o.obj'), '--project', sproj, '--normals', 'recompute')
check('recompute on an OBJ exits 0', r.returncode == 0, r.stderr)

# A sidecar beside the source sets the defaults; the flags win over it.
side = glb + '.import.json'
json.dump({'scale': 3, 'up': 'file', 'normals': 'import', 'mergeMeshes': True, 'futureKey': 1}, open(side, 'w'))
r = run('import', glb, '--project', sproj)
m = json.load(open(os.path.join(sproj, 'models', 'Sign_Post.mesh')))
check('CLI reads the sidecar beside the source: scale 3', r.returncode == 0 and
      close(positions(m), [(-3 * x, 3 * y, 3 * z) for x, y, z in world]) and 'settings scale 3 up file normals import smoothing 60 merge yes' in r.stdout,
      r.stdout + r.stderr)
r = run('import', glb, '--project', sproj, '--scale', '0.5')
m = json.load(open(os.path.join(sproj, 'models', 'Sign_Post.mesh')))
check('a flag overrides the sidecar', close(positions(m), [(-0.5 * x, 0.5 * y, 0.5 * z) for x, y, z in world]), positions(m))
json.dump({'up': 'sideways'}, open(side, 'w'))
r = run('import', glb, '--project', sproj)
check('a sidecar with a bad value is refused with its name', r.returncode == 1 and 'Sign Post.glb.import.json' in r.stderr, r.stderr)
os.remove(side)
r = run('import', glb, '--project', sproj, '--scale', '0')
check('scale 0 is refused', r.returncode == 1 and 'scale' in r.stderr, r.stderr)

print('%d/%d checks passed' % (checks - fails, checks))
sys.exit(1 if fails else 0)
