#include "rooms.hh"

#include <algorithm>
#include <cmath>
#include <set>

namespace splash {
namespace {

bool contains(const Bounds& b, Vec3 p) {
    const Vec3 mn = b.min(), mx = b.max();
    return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y && p.z >= mn.z && p.z <= mx.z;
}

struct TriWorld {
    Vec3 v[3];
};

TriWorld triWorld(const std::vector<RoomInputObject>& objects, TriangleRef r) {
    const RoomInputObject& o = objects[r.objectIndex];
    TriWorld t;
    for (size_t k = 0; k < 3; k++) t.v[k] = o.localToWorld.point((*o.triPositions)[size_t(r.triangleIndex) * 3 + k]);
    return t;
}

void sortByObject(std::vector<TriangleRef>& refs) {
    // List.Sort is unstable; a stable sort keeps the export deterministic.
    std::stable_sort(refs.begin(), refs.end(),
                     [](TriangleRef a, TriangleRef b) { return a.objectIndex < b.objectIndex; });
}

uint32_t key(TriangleRef r) { return (uint32_t(r.objectIndex) << 16) | r.triangleIndex; }

}  // namespace

int portalRoomIndex(const std::vector<const FlatObject*>& rooms, const std::string& name) {
    int found = -1;
    for (size_t i = 0; i < rooms.size(); i++) {
        if (rooms[i]->object->name != name) continue;
        if (found >= 0) return -1;
        found = int(i);
    }
    return found;
}

std::vector<std::string> portalProblems(const std::vector<const FlatObject*>& rooms, const FlatObject& portal) {
    std::vector<std::string> errors;
    if (!portal.object->portal) return errors;
    const PortalComponent& pc = *portal.object->portal;
    const std::string where = "portal " + portal.object->name;
    auto roomNamed = [&](const std::string& n, const std::string& side) -> int {
        if (n.empty()) {
            errors.push_back(side + ": not set, pick a Room object");
            return -1;
        }
        int count = 0;
        for (const FlatObject* r : rooms) count += r->object->name == n;
        if (count > 1) errors.push_back(side + ": two Room objects are named '" + n + "'");
        if (count == 0) errors.push_back(side + ": no exported Room object named '" + n + "'");
        return count == 1 ? portalRoomIndex(rooms, n) : -1;
    };
    const int a = roomNamed(pc.roomA, where + " room A");
    const int b = roomNamed(pc.roomB, where + " room B");
    if (a >= 0 && a == b) errors.push_back(where + ": room A and room B are the same Room '" + pc.roomA + "'");
    return errors;
}

RoomObjects collectRoomObjects(const std::vector<FlatObject>& flat, std::vector<std::string>& errors) {
    RoomObjects ro;
    for (const FlatObject& fo : flat) {
        if (!fo.activeInHierarchy || !fo.object->active) continue;
        if (fo.object->room) ro.rooms.push_back(&fo);
        if (fo.object->portal) ro.portals.push_back(&fo);
    }
    for (const FlatObject* p : ro.portals)
        for (std::string& e : portalProblems(ro.rooms, *p)) errors.push_back(std::move(e));
    return ro;
}

PortalFrame portalFrameAtoB(const FlatObject& portal, const FlatObject& roomA, const FlatObject& roomB) {
    PortalFrame f = portalWorldFrame(portal);
    Vec3 aMin, aMax, bMin, bMax;
    roomWorldBox(roomA, &aMin, &aMax);
    roomWorldBox(roomB, &bMin, &bMax);
    // Bounds::setMinMax's centre, so this matches the room boxes bit for bit.
    const Vec3 ca = aMin + (aMax - aMin) * 0.5f, cb = bMin + (bMax - bMin) * 0.5f;
    if (dot(f.normal, normalized(cb - ca)) < 0) {
        f.normal = f.normal * -1.f;
        f.right = f.right * -1.f;
    }
    return f;
}

std::vector<Bounds> roomBoxes(const RoomObjects& ro) {
    std::vector<Bounds> out;
    for (const FlatObject* fo : ro.rooms) {
        Vec3 mn, mx;
        roomWorldBox(*fo, &mn, &mx);
        Bounds b;
        b.setMinMax(mn, mx);
        out.push_back(b);
    }
    return out;
}

RoomSet buildRooms(const RoomObjects& ro, const std::vector<RoomInputObject>& objects,
                   std::vector<std::string>& errors) {
    RoomSet rs;
    const size_t n = ro.rooms.size();
    if (n == 0) return rs;
    if (n > kMaxRooms) {
        errors.push_back(std::to_string(n) + " Rooms, at most " + std::to_string(kMaxRooms) +
                         " (psxsplash only tracks visited rooms below index 32)");
        return rs;
    }

    const std::vector<Bounds> box = roomBoxes(ro);
    std::vector<Bounds> expanded = box;
    for (Bounds& b : expanded) b.extents = b.extents + Vec3{kRoomMargin, kRoomMargin, kRoomMargin};

    // 1. Each triangle to the room holding most of its corners (ties: the
    // nearest expanded-box centre), else the catch-all.
    std::vector<std::vector<TriangleRef>> lists(n + 1);  // [n] = catch-all
    for (size_t oi = 0; oi < objects.size(); oi++) {
        const RoomInputObject& o = objects[oi];
        if (!o.include) continue;
        const size_t tris = o.triPositions->size() / 3;
        for (size_t t = 0; t < tris; t++) {
            const TriangleRef ref{uint16_t(oi), uint16_t(t)};
            const TriWorld tw = triWorld(objects, ref);
            const Vec3 c = (tw.v[0] + tw.v[1] + tw.v[2]) / 3.f;
            int best = -1, bestHits = 0;
            float bestDist = 0;
            for (size_t r = 0; r < n; r++) {
                int hits = 0;
                for (const Vec3& v : tw.v) hits += contains(expanded[r], v) ? 1 : 0;
                if (hits == 0) continue;
                const float d = sqrMagnitude(expanded[r].center - c);
                if (hits > bestHits || (hits == bestHits && d < bestDist)) {
                    best = int(r);
                    bestHits = hits;
                    bestDist = d;
                }
            }
            lists[best >= 0 ? size_t(best) : n].push_back(ref);
        }
    }

    // 2. Portals, normal (and right) turned to point from A to B.
    for (const FlatObject* p : ro.portals) {
        const PortalComponent& pc = *p->object->portal;
        const int a = portalRoomIndex(ro.rooms, pc.roomA), b = portalRoomIndex(ro.rooms, pc.roomB);
        if (a < 0 || b < 0 || a == b) continue;  // reported by collectRoomObjects
        PortalOut po;
        po.name = p->object->name;
        po.roomA = uint16_t(a);
        po.roomB = uint16_t(b);
        po.frame = portalFrameAtoB(*p, *ro.rooms[size_t(a)], *ro.rooms[size_t(b)]);
        rs.portals.push_back(po);
    }

    // 3. Triangles near a portal go into both of its rooms.
    std::vector<std::set<uint32_t>> inRoom(n);
    for (size_t r = 0; r < n; r++)
        for (TriangleRef t : lists[r]) inRoom[r].insert(key(t));
    auto duplicate = [&](size_t src, size_t dst) {
        const std::vector<TriangleRef> srcList = lists[src];
        for (TriangleRef t : srcList) {
            if (inRoom[dst].count(key(t))) continue;
            const TriWorld tw = triWorld(objects, t);
            if (contains(expanded[dst], tw.v[0]) || contains(expanded[dst], tw.v[1]) ||
                contains(expanded[dst], tw.v[2])) {
                lists[dst].push_back(t);
                inRoom[dst].insert(key(t));
            }
        }
    };
    for (const PortalOut& p : rs.portals) {
        duplicate(p.roomA, p.roomB);
        duplicate(p.roomB, p.roomA);
    }
    for (auto& l : lists) sortByObject(l);

    // 4. Cells: a 2x2x2 grid over each room's box (the catch-all: its
    // triangles' bounds) by centroid; tight bounds; empty cells dropped and
    // the room's list rewritten cell by cell.
    rs.rooms.resize(n + 1);
    for (size_t ri = 0; ri <= n; ri++) {
        RoomOut& room = rs.rooms[ri];
        std::vector<TriangleRef>& l = lists[ri];
        if (ri < n) {
            room.name = ro.rooms[ri]->object->name;
            room.min = box[ri].min();
            room.max = box[ri].max();
        } else {
            room.name = "catch-all";
            room.min = {-1000, -1000, -1000};
            room.max = {1000, 1000, 1000};
        }
        room.firstTriRef = uint32_t(rs.refs.size());
        room.firstCell = uint32_t(rs.cells.size());
        if (!l.empty()) {
            Vec3 bmin, bsize;
            if (ri < n) {
                bmin = box[ri].min();
                bsize = box[ri].size();
            } else {
                Bounds b(triWorld(objects, l[0]).v[0], Vec3{});
                for (TriangleRef t : l) {
                    const TriWorld tw = triWorld(objects, t);
                    for (const Vec3& v : tw.v) b.encapsulate(v);
                }
                bmin = b.min();
                bsize = b.size();
            }
            bsize = vmax(bsize, Vec3{0.001f, 0.001f, 0.001f});
            constexpr int N = 2;
            std::vector<TriangleRef> cellRefs[N * N * N];
            Vec3 cmin[N * N * N], cmax[N * N * N];
            auto cellOf = [&](float c, float mn, float sz) {
                return std::clamp(int(std::floor((c - mn) / sz * float(N))), 0, N - 1);
            };
            for (TriangleRef t : l) {
                const TriWorld tw = triWorld(objects, t);
                const Vec3 c = (tw.v[0] + tw.v[1] + tw.v[2]) / 3.f;
                const int idx = cellOf(c.x, bmin.x, bsize.x) + cellOf(c.y, bmin.y, bsize.y) * N +
                                cellOf(c.z, bmin.z, bsize.z) * N * N;
                Vec3 tmn = vmin(vmin(tw.v[0], tw.v[1]), tw.v[2]);
                Vec3 tmx = vmax(vmax(tw.v[0], tw.v[1]), tw.v[2]);
                if (cellRefs[idx].empty()) {
                    cmin[idx] = tmn;
                    cmax[idx] = tmx;
                } else {
                    cmin[idx] = vmin(cmin[idx], tmn);
                    cmax[idx] = vmax(cmax[idx], tmx);
                }
                cellRefs[idx].push_back(t);
            }
            for (int c = 0; c < N * N * N; c++) {
                if (cellRefs[c].empty()) continue;
                sortByObject(cellRefs[c]);
                RoomCellOut cell;
                cell.min = cmin[c];
                cell.max = cmax[c];
                cell.firstTriRef = uint32_t(rs.refs.size());
                cell.triRefCount = uint32_t(cellRefs[c].size());
                rs.refs.insert(rs.refs.end(), cellRefs[c].begin(), cellRefs[c].end());
                rs.cells.push_back(cell);
            }
        }
        room.triRefCount = uint32_t(rs.refs.size()) - room.firstTriRef;
        room.cellCount = uint32_t(rs.cells.size()) - room.firstCell;
    }

    // 5. Portal refs per room, {portal, room on the other side}; none for the catch-all.
    std::vector<std::vector<RoomPortalRef>> perRoom(n + 1);
    for (size_t pi = 0; pi < rs.portals.size(); pi++) {
        const PortalOut& p = rs.portals[pi];
        perRoom[p.roomA].push_back({uint16_t(pi), p.roomB});
        perRoom[p.roomB].push_back({uint16_t(pi), p.roomA});
    }
    for (size_t ri = 0; ri <= n; ri++) {
        rs.rooms[ri].firstPortalRef = uint32_t(rs.portalRefs.size());
        rs.rooms[ri].portalRefCount = uint32_t(perRoom[ri].size());
        rs.portalRefs.insert(rs.portalRefs.end(), perRoom[ri].begin(), perRoom[ri].end());
        if (perRoom[ri].size() > 255)
            errors.push_back("Room " + rs.rooms[ri].name + ": " + std::to_string(perRoom[ri].size()) +
                             " portals, a room holds at most 255");
    }

    // The header carries every total as u16.
    auto limit = [&](size_t count, const char* what) {
        if (count > 65535)
            errors.push_back(std::to_string(count) + " " + what + ", the splashpack holds at most 65535");
    };
    limit(rs.rooms.size(), "rooms (with the catch-all)");
    limit(rs.portals.size(), "portals");
    limit(rs.refs.size(), "room triangle references");
    limit(rs.cells.size(), "room cells");
    limit(rs.portalRefs.size(), "room portal references");
    return rs;
}

void writeRooms(BinWriter& w, const RoomSet& rs, float gte) {
    auto box = [&](Vec3 mn, Vec3 mx) {
        w.i32(toWorldFixed12(mn.x / gte));
        w.i32(toWorldFixed12(-mx.y / gte));
        w.i32(toWorldFixed12(mn.z / gte));
        w.i32(toWorldFixed12(mx.x / gte));
        w.i32(toWorldFixed12(-mn.y / gte));
        w.i32(toWorldFixed12(mx.z / gte));
    };
    // RoomData, 36 bytes
    for (const RoomOut& r : rs.rooms) {
        box(r.min, r.max);
        w.u16(uint16_t(r.firstTriRef));
        w.u16(uint16_t(r.triRefCount));
        w.u16(uint16_t(r.firstCell));
        w.u8(uint8_t(r.cellCount));
        w.u8(uint8_t(r.portalRefCount));
        w.u16(uint16_t(r.firstPortalRef));
        w.u16(0);
    }
    // PortalData, 40 bytes
    auto unit = [&](Vec3 v) {
        w.i16(toFixed12(v.x));
        w.i16(toFixed12(-v.y));
        w.i16(toFixed12(v.z));
    };
    auto half = [&](float h) { w.i16(int16_t(clampv(roundToInt(h / gte * 4096.f), 1, 32767))); };
    for (const PortalOut& p : rs.portals) {
        w.u16(p.roomA);
        w.u16(p.roomB);
        w.i32(toWorldFixed12(p.frame.center.x / gte));
        w.i32(toWorldFixed12(-p.frame.center.y / gte));
        w.i32(toWorldFixed12(p.frame.center.z / gte));
        half(p.frame.halfW);
        half(p.frame.halfH);
        unit(p.frame.normal);
        w.i16(0);
        unit(p.frame.right);
        unit(p.frame.up);
    }
    // TriangleRef, 4 bytes
    for (const TriangleRef& r : rs.refs) {
        w.u16(r.objectIndex);
        w.u16(r.triangleIndex);
    }
    // RoomCell, 28 bytes
    for (const RoomCellOut& c : rs.cells) {
        box(c.min, c.max);
        w.u16(uint16_t(c.firstTriRef));
        w.u16(uint16_t(c.triRefCount));
    }
    // RoomPortalRef, 4 bytes
    for (const RoomPortalRef& r : rs.portalRefs) {
        w.u16(r.portalIndex);
        w.u16(r.otherRoom);
    }
}

}  // namespace splash
