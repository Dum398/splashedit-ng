#!/usr/bin/env python3
"""Check a world-streaming export against the same scene exported without it.

    streamcheck.py OFF.splashpack ON.splashpack

ON.geo must sit next to ON.splashpack. Checks the stream table against
psxsplash's streamplanner.hh layout, that every streamed triangle is in the
.geo file exactly once and matches the unstreamed export (per object name, as
a multiset, since cut objects come back in region order), that every region's
bounds hold its triangles, and that everything else the engine reads is
unchanged. exit 0 = pass, 1 = fail.
"""
import struct
import sys
from collections import Counter
from pathlib import Path

HEADER_BYTES = 156
SECTOR = 2048
TRI = 52


def load(path):
    d = Path(path).read_bytes()
    assert d[:2] == b"SP", f"{path}: not a splashpack"
    lua, objs = struct.unpack_from("<HH", d, 4)
    name_off = struct.unpack_from("<I", d, 60)[0]
    stream_off = struct.unpack_from("<I", d, 124)[0]
    objects = []
    p = HEADER_BYTES + 8 * lua
    for _ in range(objs):
        f = struct.unpack_from("<I3i9iHhIHHI6i", d, p)
        objects.append({"mesh": f[0], "pos": f[1:4], "rot": f[4:13], "polys": f[13], "rest": f[14:19], "aabb": f[19:25]})
        p += 92
    q = name_off
    for o in objects:
        n = d[q]
        o["name"] = d[q + 1:q + 1 + n].decode()
        q += n + 2
        o["tris"] = [d[o["mesh"] + i * TRI:o["mesh"] + (i + 1) * TRI] for i in range(o["polys"])]
    return d, objects, stream_off


def world_xz(o, tri):
    """XZ of a Tri's corners in fp12 world space (object position + rotation)."""
    out = []
    r = o["rot"]
    for k in range(3):
        vx, vy, vz = struct.unpack_from("<3h", tri, k * 6)
        x = o["pos"][0] + (r[0] * vx + r[1] * vy + r[2] * vz) // 4096
        z = o["pos"][2] + (r[6] * vx + r[7] * vy + r[8] * vz) // 4096
        out.append((x, z))
    return out


def main():
    off_path, on_path = sys.argv[1], sys.argv[2]
    _, off, off_stream = load(off_path)
    on_d, on, table = load(on_path)
    geo = Path(on_path).with_suffix(".geo").read_bytes()
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    check(off_stream == 0, "unstreamed export has a stream table")
    check(table != 0 and table % 4 == 0, f"stream table offset {table}")
    if fails:
        print("FAIL", *fails, sep="\n  ")
        return 1
    nreg, nref, slots, pad, slot_bytes, load_r, unload_r = struct.unpack_from("<4HIii", on_d, table)
    check(pad == 0 and 1 <= slots <= 32, f"slots {slots}")
    check(slot_bytes % SECTOR == 0 and slot_bytes > 0, f"slotBytes {slot_bytes}")
    check(0 < load_r < unload_r, f"radii {load_r} {unload_r}")
    regions = [struct.unpack_from("<4iIIHHI", on_d, table + 20 + 32 * i) for i in range(nreg)]
    refs_at = table + 20 + 32 * nreg
    refs = [struct.unpack_from("<HHI", on_d, refs_at + 8 * i) for i in range(nref)]

    sector, first, seen = 0, 0, set()
    streamed = Counter()  # name -> tri multiset
    for ri, (x0, z0, x1, z1, fs, size, fr, nr, rpad) in enumerate(regions):
        check(fs == sector, f"region {ri} firstSector {fs}, expected {sector}")
        check(size % SECTOR == 0 and size <= slot_bytes, f"region {ri} byteSize {size}")
        check(fr == first and rpad == 0, f"region {ri} refs start {fr}")
        blob = geo[fs * SECTOR:fs * SECTOR + size]
        offset = 0
        for oi, polys, roff in refs[fr:fr + nr]:
            check(oi not in seen, f"object {oi} streamed twice")
            seen.add(oi)
            check(roff == offset, f"region {ri} ref offset {roff}, expected {offset}")
            o = on[oi]
            check(o["polys"] == 0, f"streamed object {oi} '{o['name']}' keeps polyCount {o['polys']}")
            for t in range(polys):
                tri = blob[roff + t * TRI:roff + (t + 1) * TRI]
                streamed[(o["name"], tri)] += 1
                for x, z in world_xz(o, tri):
                    # Region bounds come from float positions before rounding.
                    check(x0 - 8 <= x <= x1 + 8 and z0 - 8 <= z <= z1 + 8,
                          f"region {ri}: tri of '{o['name']}' at ({x},{z}) outside {x0},{z0}..{x1},{z1}")
            offset += polys * TRI
        check(offset <= size and size - offset < SECTOR, f"region {ri} padding {size - offset}")
        sector += size // SECTOR
        first += nr
    check(len(geo) == sector * SECTOR, f".geo is {len(geo)} bytes, table covers {sector * SECTOR}")
    check(first == nref, "object refs not all used")

    # Content: per object name, streamed triangles plus resident ones equal the
    # unstreamed export's.
    names_off = [o["name"] for o in off]
    check(on[:len(off)] and [o["name"] for o in on[:len(off)]] == names_off, "original objects moved")
    for o in on[len(off):]:
        check(o["name"] in names_off and on.index(o) in seen, f"extra object '{o['name']}' is not a streamed part")
    want, got = Counter(), Counter(streamed)
    for o in off:
        for t in o["tris"]:
            want[(o["name"], t)] += 1
    for o in on:
        for t in o["tris"]:
            got[(o["name"], t)] += 1
    check(want == got, f"triangles differ: {sum((want - got).values())} missing, {sum((got - want).values())} extra")
    # BVH triangle refs and nav region/portal counts (header +34, +44). The node
    # count may change: cut objects split differently.
    off_d = Path(off_path).read_bytes()
    check(off_d[34:36] == on_d[34:36], "BVH triangle ref count changed")
    check(off_d[44:48] == on_d[44:48], "nav counts changed")
    for a, b in zip(off, on):
        check(a["pos"] == b["pos"] and a["rot"] == b["rot"] and a["rest"] == b["rest"],
              f"object '{a['name']}' fields changed")

    if fails:
        print(f"FAIL ({len(fails)})", *fails[:30], sep="\n  ")
        return 1
    print(f"PASS regions={nreg} refs={nref} slots={slots}x{slot_bytes // 1024}KB streamed_objects={len(seen)} "
          f"extra_parts={len(on) - len(off)} tris_streamed={sum(streamed.values())} geo={len(geo)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
