#!/usr/bin/env python3
"""Compare two splashpack exports (.splashpack + .vram) section by section.

Objects are matched by name, so two exports whose object order differs (Unity's
FindObjectsByType order is not stable) compare equal when their content is.
Every object-index field (BVH refs, collider gameObjectIndex) is translated
through the name mapping before comparing. Covers the sections the C++ writer
emits so far; anything else present in either file is reported, not compared.

usage: splashdiff.py A.splashpack B.splashpack [-v]
exit 0 = equal, 1 = differences, 2 = cannot compare
"""
import struct
import sys
from pathlib import Path

HEADER = [  # (name, fmt) in order, 156 bytes (v27)
    ("magic", "2s"), ("version", "H"), ("luaFileCount", "H"), ("gameObjectCount", "H"),
    ("textureAtlasCount", "H"), ("clutCount", "H"), ("colliderCount", "H"), ("interactableCount", "H"),
    ("playerStartPos", "3h"), ("playerStartRot", "3h"), ("playerHeight", "H"), ("sceneLuaFileIndex", "h"),
    ("bvhNodeCount", "H"), ("bvhTriangleRefCount", "H"), ("sceneType", "H"), ("triggerBoxCount", "H"),
    ("worldCollisionMeshCount", "H"), ("worldCollisionTriCount", "H"), ("navRegionCount", "H"),
    ("navPortalCount", "H"), ("moveSpeed", "H"), ("sprintSpeed", "H"), ("jumpVelocity", "H"),
    ("gravity", "H"), ("playerRadius", "H"), ("pad1", "H"), ("nameTableOffset", "I"),
    ("audioClipCount", "H"), ("pad2", "H"), ("audioTableOffset", "I"), ("fogEnabled", "B"),
    ("fogRGB", "3B"), ("fogDensity", "B"), ("pad3", "B"), ("roomCount", "H"), ("portalCount", "H"),
    ("roomTriRefCount", "H"), ("cutsceneCount", "H"), ("roomCellCount", "H"), ("cutsceneTableOffset", "I"),
    ("uiCanvasCount", "H"), ("uiFontCount", "B"), ("uiPad5", "B"), ("uiTableOffset", "I"),
    ("pixelDataOffset", "I"), ("animationCount", "H"), ("roomPortalRefCount", "H"),
    ("animationTableOffset", "I"), ("skinnedMeshCount", "H"), ("agentCount", "H"), ("skinTableOffset", "I"),
    ("memcardTableOffset", "I"), ("streamTableOffset", "I"), ("spriteTableOffset", "I"),
    ("spriteSheetCount", "H"), ("spriteAnimCount", "H"), ("sceneHash", "I"), ("tilemapTableOffset", "I"),
    ("lightTableOffset", "I"), ("orderingTableSize", "I"), ("bumpAllocatorSize", "I"),
]
OFFSET_FIELDS = {"nameTableOffset", "audioTableOffset", "cutsceneTableOffset", "uiTableOffset",
                 "animationTableOffset", "skinTableOffset", "memcardTableOffset", "streamTableOffset",
                 "spriteTableOffset", "tilemapTableOffset", "lightTableOffset"}
UNSUPPORTED = ["triggerBoxCount", "roomCount", "interactableCount", "audioClipCount",
               "cutsceneCount", "animationCount", "skinnedMeshCount", "agentCount", "uiCanvasCount",
               "uiFontCount", "spriteSheetCount", "memcardTableOffset", "streamTableOffset",
               "tilemapTableOffset", "lightTableOffset"]


class Pack:
    def __init__(self, path):
        self.path = Path(path)
        self.d = self.path.read_bytes()
        self.vram = self.path.with_suffix(".vram").read_bytes()
        p = 0
        self.h = {}
        for name, fmt in HEADER:
            v = struct.unpack_from("<" + fmt, self.d, p)
            p += struct.calcsize("<" + fmt)
            self.h[name] = v[0] if len(v) == 1 else v
        if self.h["magic"] != b"SP":
            raise ValueError(f"{path}: not a splashpack")
        if self.h["version"] != 27:
            raise ValueError(f"{path}: version {self.h['version']}, only v27 is decoded")
        assert p == 156
        h = self.h
        self.lua = []
        for _ in range(h["luaFileCount"]):
            off, ln = struct.unpack_from("<II", self.d, p)
            p += 8
            self.lua.append(self.d[off:off + ln])
        self.objects = []
        for _ in range(h["gameObjectCount"]):
            f = struct.unpack_from("<I3i9iHhIHHI6i", self.d, p)
            p += 92
            self.objects.append({"meshOffset": f[0], "pos": f[1:4], "rot": f[4:13], "polyCount": f[13],
                                 "lua": f[14], "flags": f[15], "interactable": f[16], "uvOffset": f[17],
                                 "eventMask": f[18], "aabb": f[19:25]})
        self.colliders = []
        for _ in range(h["colliderCount"]):
            f = struct.unpack_from("<6iBBHI", self.d, p)
            p += 32
            self.colliders.append({"aabb": f[0:6], "type": f[6], "layer": f[7], "go": f[8], "pad": f[9]})
        p += h["triggerBoxCount"] * 32
        p = (p + 3) & ~3
        self.nodes = []
        for _ in range(h["bvhNodeCount"]):
            f = struct.unpack_from("<6iHHHH", self.d, p)
            p += 32
            self.nodes.append({"aabb": f[0:6], "left": f[6], "right": f[7], "first": f[8], "count": f[9]})
        self.refs = []
        for _ in range(h["bvhTriangleRefCount"]):
            self.refs.append(struct.unpack_from("<HH", self.d, p))
            p += 4
        p = (p + 3) & ~3
        self.cursorAfterBvh = p
        p += h["interactableCount"] * 32
        waypoints = 0
        for i in range(h["agentCount"]):
            waypoints += self.d[p + i * 28 + 3]
        p += h["agentCount"] * 28 + waypoints * 12
        self.nav = None
        if h["navRegionCount"]:
            p = (p + 3) & ~3
            nr, np_, start, _pad = struct.unpack_from("<4H", self.d, p)
            p += 8
            regions, portals = [], []
            for _ in range(nr):
                f = struct.unpack_from("<8i8i3iHBBBBBB", self.d, p)
                p += 84
                n = min(f[21], 8)
                regions.append({"x": f[0:n], "z": f[8:8 + n], "unused": f[n:8] + f[8 + n:16], "plane": f[16:19],
                                "portalStart": f[19], "portalCount": f[20], "vertCount": f[21], "surface": f[22],
                                "room": f[23], "flags": f[24], "walkoff": f[25]})
            for _ in range(np_):
                f = struct.unpack_from("<4iHh", self.d, p)
                p += 20
                portals.append({"a": f[0:2], "b": f[2:4], "neighbor": f[4], "heightDelta": f[5]})
            self.nav = {"regionCount": nr, "portalCount": np_, "startRegion": start, "regions": regions,
                        "portals": portals}
        # name table
        p = h["nameTableOffset"]
        self.names = []
        for _ in range(h["gameObjectCount"]):
            n = self.d[p]
            self.names.append(self.d[p + 1:p + 1 + n].decode("utf-8", "replace"))
            p += n + 2
        for o in self.objects:
            o["tris"] = [self.d[o["meshOffset"] + i * 52:o["meshOffset"] + (i + 1) * 52]
                         for i in range(o["polyCount"])]
        self.decode_vram()

    def decode_vram(self):
        v = self.vram
        if v[:2] != b"VR":
            raise ValueError("bad vram magic")
        na, nc, nf = struct.unpack_from("<HHB", v, 2)
        p = 8
        self.atlases = []
        for _ in range(na):
            x, y, w, hh = struct.unpack_from("<4H", v, p)
            p += 8
            px = v[p:p + w * hh * 2]
            p += w * hh * 2
            p = (p + 3) & ~3
            self.atlases.append({"x": x, "y": y, "w": w, "h": hh, "pixels": px})
        self.cluts = []
        for _ in range(nc):
            cx, cy, ln, _pad = struct.unpack_from("<4H", v, p)
            p += 8
            self.cluts.append({"x": cx, "y": cy, "len": ln, "data": v[p:p + ln * 2]})
            p += ln * 2
            p = (p + 3) & ~3
        self.vramFonts = nf


def tri_fields(t):
    f = struct.unpack_from("<9h3h12B6BHHHHH", t)
    return {"pos": f[0:9], "normal": f[9:12], "colors": f[12:24], "uv": f[24:30], "pad": f[30],
            "tpage": f[31], "clutX": f[32], "clutY": f[33], "flags": f[34]}


def compare_nav(a, b, out):
    """Regions and portals in file order (Recast's order). Reports every differing
    field, then a summary with the largest vertex/portal coordinate delta."""
    if not a.nav or not b.nav:
        if a.nav or b.nav:
            out("nav: present in only one file")
        return
    if a.nav["startRegion"] != b.nav["startRegion"]:
        out(f"nav startRegion: {a.nav['startRegion']} != {b.nav['startRegion']}")
    maxd = 0
    ndiff = 0
    for i, (ra, rb) in enumerate(zip(a.nav["regions"], b.nav["regions"])):
        bad = [k for k in ra if ra[k] != rb[k]]
        if bad:
            ndiff += 1
            out(f"nav region {i}: " + ", ".join(f"{k} {ra[k]} != {rb[k]}" for k in bad))
        if ra["vertCount"] == rb["vertCount"]:
            for k in ("x", "z"):
                maxd = max([maxd] + [abs(u - v) for u, v in zip(ra[k], rb[k])])
    for i, (pa, pb) in enumerate(zip(a.nav["portals"], b.nav["portals"])):
        bad = [k for k in pa if pa[k] != pb[k]]
        if bad:
            ndiff += 1
            out(f"nav portal {i}: " + ", ".join(f"{k} {pa[k]} != {pb[k]}" for k in bad))
        for k in ("a", "b"):
            maxd = max([maxd] + [abs(u - v) for u, v in zip(pa[k], pb[k])])
    if ndiff:
        out(f"nav: {ndiff} differing regions/portals, max matched XZ delta {maxd} (20.12 units)")


def compare(a, b, verbose):
    diffs = []
    out = lambda s: diffs.append(s)

    for name, _ in HEADER:
        if name in OFFSET_FIELDS:
            continue
        if a.h[name] != b.h[name]:
            out(f"header.{name}: {a.h[name]} != {b.h[name]}")
    for name in UNSUPPORTED:
        if a.h[name] or b.h[name]:
            out(f"NOT COMPARED: {name} = {a.h[name]} / {b.h[name]}")

    if sorted(a.names) != sorted(b.names) or len(set(a.names)) != len(a.names):
        out(f"object names differ or are not unique: {a.names} vs {b.names}")
        return diffs
    # index in A -> index in B
    amap = {i: b.names.index(n) for i, n in enumerate(a.names)}

    for i, n in enumerate(a.names):
        oa, ob = a.objects[i], b.objects[amap[i]]
        for k in ("pos", "rot", "polyCount", "lua", "flags", "interactable", "uvOffset", "eventMask", "aabb"):
            if oa[k] != ob[k]:
                out(f"object '{n}'.{k}: {oa[k]} != {ob[k]}")
        for ti, (ta, tb) in enumerate(zip(oa["tris"], ob["tris"])):
            if ta != tb:
                fa, fb = tri_fields(ta), tri_fields(tb)
                bad = [k for k in fa if fa[k] != fb[k]]
                out(f"object '{n}' tri {ti}: " + ", ".join(f"{k} {fa[k]} != {fb[k]}" for k in bad))

    ca = sorted((amap[c["go"]], c["aabb"], c["type"], c["layer"]) for c in a.colliders)
    cb = sorted((c["go"], c["aabb"], c["type"], c["layer"]) for c in b.colliders)
    if ca != cb:
        out(f"colliders: {ca} != {cb}")

    # BVH: node structure must match exactly; refs compared with object
    # indices translated. Leaf ref order inside one object is compared too.
    if len(a.nodes) != len(b.nodes):
        out(f"bvh node count {len(a.nodes)} != {len(b.nodes)}")
    else:
        for i, (na, nb) in enumerate(zip(a.nodes, b.nodes)):
            if na != nb:
                out(f"bvh node {i}: {na} != {nb}")
            ra = [(amap[o], t) for o, t in a.refs[na["first"]:na["first"] + na["count"]]]
            rb = list(b.refs[nb["first"]:nb["first"] + nb["count"]])
            if na["left"] == 0xFFFF and na["right"] == 0xFFFF and ra != rb:
                if sorted(ra) == sorted(rb):
                    out(f"bvh leaf {i}: same refs, different order (object-order artefact if A and B differ in object order)")
                else:
                    out(f"bvh leaf {i}: refs differ")

    compare_nav(a, b, out)

    if [x["data"] for x in a.cluts] != [x["data"] for x in b.cluts] or \
            [(x["x"], x["y"], x["len"]) for x in a.cluts] != [(x["x"], x["y"], x["len"]) for x in b.cluts]:
        for i, (x, y) in enumerate(zip(a.cluts, b.cluts)):
            if x != y:
                out(f"clut {i}: at ({x['x']},{x['y']}) len {x['len']} vs ({y['x']},{y['y']}) len {y['len']}, "
                    f"data {'same' if x['data'] == y['data'] else 'differs'}")
        if len(a.cluts) != len(b.cluts):
            out(f"clut count {len(a.cluts)} != {len(b.cluts)}")
    for i, (x, y) in enumerate(zip(a.atlases, b.atlases)):
        if (x["x"], x["y"], x["w"]) != (y["x"], y["y"], y["w"]):
            out(f"atlas {i}: placed {(x['x'], x['y'], x['w'])} vs {(y['x'], y['y'], y['w'])}")
        elif x["pixels"] != y["pixels"]:
            nw = sum(1 for k in range(0, len(x["pixels"]), 2) if x["pixels"][k:k + 2] != y["pixels"][k:k + 2])
            out(f"atlas {i}: {nw} of {len(x['pixels']) // 2} words differ")
    if len(a.atlases) != len(b.atlases):
        out(f"atlas count {len(a.atlases)} != {len(b.atlases)}")
    if a.lua != b.lua:
        out("lua data differs")
    return diffs


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("-")]
    verbose = "-v" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    try:
        a, b = Pack(args[0]), Pack(args[1])
    except Exception as e:
        print(f"cannot compare: {e}")
        return 2
    diffs = compare(a, b, verbose)
    limit = None if verbose else 40
    for d in diffs[:limit]:
        print(d)
    if limit and len(diffs) > limit:
        print(f"... {len(diffs) - limit} more (of {len(diffs)}); -v for all")
    nav = f"{a.nav['regionCount']} nav regions, {a.nav['portalCount']} nav portals, " if a.nav else ""
    print(f"{'EQUAL' if not diffs else 'DIFFERENT'}: {len(a.names)} objects, {len(a.nodes)} bvh nodes, "
          f"{nav}{len(a.atlases)} atlases, {len(a.cluts)} cluts; {len(diffs)} differences")
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
