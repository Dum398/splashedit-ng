#include "navregion.hh"

#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>

namespace splash {

namespace {

// Recast reports failures through the context log.
class Context : public rcContext {
  public:
    explicit Context(std::vector<std::string>& errors) : rcContext(true), errors_(errors) { enableTimer(false); }

  protected:
    void doLog(const rcLogCategory category, const char* msg, const int len) override {
        if (category == RC_LOG_ERROR) errors_.push_back("nav: " + std::string(msg, size_t(len)));
    }

  private:
    std::vector<std::string>& errors_;
};

template <typename T, void (*F)(T*)>
struct RcFree {
    void operator()(T* p) const { F(p); }
};
using HeightfieldPtr = std::unique_ptr<rcHeightfield, RcFree<rcHeightfield, rcFreeHeightField>>;
using CompactPtr = std::unique_ptr<rcCompactHeightfield, RcFree<rcCompactHeightfield, rcFreeCompactHeightfield>>;
using ContourSetPtr = std::unique_ptr<rcContourSet, RcFree<rcContourSet, rcFreeContourSet>>;
using PolyMeshPtr = std::unique_ptr<rcPolyMesh, RcFree<rcPolyMesh, rcFreePolyMesh>>;
using DetailPtr = std::unique_ptr<rcPolyMeshDetail, RcFree<rcPolyMeshDetail, rcFreePolyMeshDetail>>;

// Unity's Mono evaluates float expressions with double intermediates and
// rounds when the result is stored (see bvh.cpp). Multi-operation
// expressions below are written in double for that reason; it decides e.g.
// floor(0.35f / 0.025f), which is 14 in float and 13 in double.

// PSXNavRegionBuilder.CollectGeometry
void collectGeometry(const std::vector<NavInputObject>& objects, std::vector<float>& outVerts,
                     std::vector<int>& outTris) {
    for (const NavInputObject& o : objects) {
        if (!o.walkable) continue;
        int baseVert = int(outVerts.size() / 3);
        std::vector<Vec3> world(o.positions->size());
        for (size_t i = 0; i < world.size(); i++) {
            world[i] = o.localToWorld.point((*o.positions)[i]);
            outVerts.push_back(world[i].x);
            outVerts.push_back(world[i].y);
            outVerts.push_back(world[i].z);
        }
        // Downward-facing triangles (ceilings, undersides) are dropped.
        for (size_t i = 0; i + 2 < o.triangles.size(); i += 3) {
            Vec3 v0 = world[size_t(o.triangles[i])];
            Vec3 v1 = world[size_t(o.triangles[i + 1])];
            Vec3 v2 = world[size_t(o.triangles[i + 2])];
            Vec3 a = v1 - v0, b = v2 - v0;
            double ny = double(a.z) * b.x - double(a.x) * b.z;
            if (float(ny) < 0.f) continue;
            outTris.push_back(o.triangles[i] + baseVert);
            outTris.push_back(o.triangles[i + 1] + baseVert);
            outTris.push_back(o.triangles[i + 2] + baseVert);
        }
    }
}

float evalY(const NavRegion& r, Vec2 xz) {
    return float(double(r.planeA) * xz.x + double(r.planeB) * xz.y + r.planeD);
}

Vec3 centroid(const NavRegion& r) {
    float cx = 0, cz = 0;
    for (Vec2 v : r.verts) {
        cx += v.x;
        cz += v.y;
    }
    cx /= float(r.verts.size());
    cz /= float(r.verts.size());
    return {cx, evalY(r, {cx, cz}), cz};
}

// PSXNavRegionBuilder.FitPlane
void fitPlane(NavRegion& r, const std::vector<Vec3>& pts) {
    size_t n = pts.size();
    if (n < 3) {
        r.planeA = r.planeB = 0;
        r.planeD = n > 0 ? pts[0].y : 0;
        r.surfaceType = 0;
        r.maxPlaneDeviation = 0;
        return;
    }
    if (n == 3) {
        double x0 = pts[0].x, z0 = pts[0].z, y0 = pts[0].y;
        double x1 = pts[1].x, z1 = pts[1].z, y1 = pts[1].y;
        double x2 = pts[2].x, z2 = pts[2].z, y2 = pts[2].y;
        double det = (x0 - x2) * (z1 - z2) - (x1 - x2) * (z0 - z2);
        if (std::fabs(det) < 1e-12) {
            r.planeA = r.planeB = 0;
            r.planeD = float((y0 + y1 + y2) / 3);
        } else {
            double inv = 1.0 / det;
            r.planeA = float(((y0 - y2) * (z1 - z2) - (y1 - y2) * (z0 - z2)) * inv);
            r.planeB = float(((x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2)) * inv);
            r.planeD = float(y0 - r.planeA * x0 - r.planeB * z0);
        }
    } else {
        double sX = 0, sZ = 0, sY = 0, sXX = 0, sXZ = 0, sZZ = 0, sXY = 0, sZY = 0;
        for (Vec3 p : pts) {
            double x = p.x, y = p.y, z = p.z;
            sX += x;
            sZ += z;
            sY += y;
            sXX += x * x;
            sXZ += x * z;
            sZZ += z * z;
            sXY += x * y;
            sZY += z * y;
        }
        double dn = double(n);
        double det = sXX * (sZZ * dn - sZ * sZ) - sXZ * (sXZ * dn - sZ * sX) + sX * (sXZ * sZ - sZZ * sX);
        if (std::fabs(det) < 1e-12) {
            r.planeA = r.planeB = 0;
            r.planeD = float(sY / dn);
        } else {
            double inv = 1.0 / det;
            r.planeA = float(
                (sXY * (sZZ * dn - sZ * sZ) - sXZ * (sZY * dn - sZ * sY) + sX * (sZY * sZ - sZZ * sY)) * inv);
            r.planeB = float(
                (sXX * (sZY * dn - sZ * sY) - sXY * (sXZ * dn - sZ * sX) + sX * (sXZ * sY - sZY * sX)) * inv);
            r.planeD = float(
                (sXX * (sZZ * sY - sZ * sZY) - sXZ * (sXZ * sY - sZY * sX) + sXY * (sXZ * sZ - sZZ * sX)) * inv);
        }
    }
    float maxDev = 0;
    for (Vec3 p : pts) {
        float predicted = float(double(r.planeA) * p.x + double(r.planeB) * p.z + r.planeD);
        float dev = std::fabs(p.y - predicted);
        if (dev > maxDev) maxDev = dev;
    }
    r.maxPlaneDeviation = maxDev;
    constexpr float rad2deg = 57.29578f;  // Mathf.Rad2Deg
    // Mathf.Atan(Mathf.Sqrt(a * a + b * b)) * Mathf.Rad2Deg
    float grade = float(std::sqrt(double(r.planeA) * r.planeA + double(r.planeB) * r.planeB));
    float slope = float(std::atan(double(grade))) * rad2deg;
    r.surfaceType = slope < 3.f ? 0 : slope < 25.f ? 1 : 2;
}

// PSXNavRegionBuilder.AssignRoomsFromPSXRooms: the smallest room whose box,
// 0.5 larger per side, holds the region's centroid; 0xFF outside them all.
void assignRoomsFromBoxes(NavMesh& nav, const std::vector<Bounds>& rooms) {
    for (NavRegion& reg : nav.regions) {
        const Vec3 c = centroid(reg);
        uint8_t best = 0xFF;
        float bestVolume = 0;
        for (size_t r = 0; r < rooms.size() && r < 0xFF; r++) {
            const Vec3 mn = rooms[r].min() - Vec3{0.5f, 0.5f, 0.5f};
            const Vec3 mx = rooms[r].max() + Vec3{0.5f, 0.5f, 0.5f};
            if (c.x < mn.x || c.x > mx.x || c.y < mn.y || c.y > mx.y || c.z < mn.z || c.z > mx.z) continue;
            const Vec3 s = mx - mn;
            const float vol = s.x * s.y * s.z;
            if (best == 0xFF || vol < bestVolume) {
                best = uint8_t(r);
                bestVolume = vol;
            }
        }
        reg.roomIndex = best;
    }
}

// PSXNavRegionBuilder.AssignRoomsByBFS (no PSXRoom volumes)
void assignRoomsByBfs(NavMesh& nav) {
    uint8_t room = 0;
    std::vector<bool> vis(nav.regions.size());
    for (size_t i = 0; i < nav.regions.size(); i++) {
        if (vis[i]) continue;
        uint8_t rm = room++;
        std::deque<size_t> q{i};
        vis[i] = true;
        while (!q.empty()) {
            size_t ri = q.front();
            q.pop_front();
            NavRegion& r = nav.regions[ri];
            r.roomIndex = rm;
            for (int p = r.portalStart; p < r.portalStart + r.portalCount; p++) {
                int nb = nav.portals[size_t(p)].neighborRegion;
                if (nb >= 0 && size_t(nb) < nav.regions.size() && !vis[size_t(nb)]) {
                    vis[size_t(nb)] = true;
                    q.push_back(size_t(nb));
                }
            }
        }
    }
}

// PSXNavRegionBuilder.ApplyPlatformFlags
void applyPlatformFlags(NavMesh& nav, const std::vector<NavInputObject>& objects) {
    std::vector<Bounds> platformBounds;
    for (const NavInputObject& o : objects) {
        if (!o.platform) continue;
        Vec3 ext = o.localBounds.extents, center = o.localBounds.center;
        Vec3 mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
        Vec3 mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()};
        for (int i = 0; i < 8; i++) {
            Vec3 corner =
                center + Vec3{(i & 1) ? ext.x : -ext.x, (i & 2) ? ext.y : -ext.y, (i & 4) ? ext.z : -ext.z};
            Vec3 world = o.localToWorld.point(corner);
            mn = vmin(mn, world);
            mx = vmax(mx, world);
        }
        Bounds b;
        b.setMinMax(mn, mx);
        b.extents = b.extents + Vec3{0.05f, 0.05f, 0.05f};  // Bounds.Expand(0.1f)
        platformBounds.push_back(b);
    }
    if (platformBounds.empty()) return;
    for (NavRegion& r : nav.regions) {
        Vec3 c = centroid(r);
        c.y = c.y - 0.01f;
        for (const Bounds& b : platformBounds) {
            Vec3 mn = b.min(), mx = b.max();
            if (c.x >= mn.x && c.x <= mx.x && c.y >= mn.y && c.y <= mx.y && c.z >= mn.z && c.z <= mx.z) {
                r.flags |= 0x01;
                r.walkoffEdgeMask |= r.boundaryEdgeMask;
                break;
            }
        }
    }
}

// PSXNavRegionBuilder.FindClosestRegion
int findClosestRegion(const NavMesh& nav, Vec3 spawn) {
    int best = 0;
    float bestDist = std::numeric_limits<float>::max();
    for (size_t i = 0; i < nav.regions.size(); i++) {
        Vec3 c = centroid(nav.regions[i]);
        float dx = spawn.x - c.x, dy = spawn.y - c.y, dz = spawn.z - c.z;
        float dist = float(double(dx) * dx + double(dy) * dy + double(dz) * dz);
        if (dist < bestDist) {
            bestDist = dist;
            best = int(i);
        }
    }
    return best;
}

}  // namespace

NavMesh buildNavRegions(const std::vector<NavInputObject>& objects, const NavBuildParams& p, Vec3 spawn,
                        std::vector<std::string>& errors, const std::vector<Bounds>& roomBoxes,
                        const std::vector<Bounds>& walkoffBoxes) {
    NavMesh nav;

    // 1. World-space geometry
    std::vector<float> verts;
    std::vector<int> tris;
    collectGeometry(objects, verts, tris);
    if (verts.size() < 9 || tris.size() < 3) return nav;
    int nverts = int(verts.size() / 3);
    int ntris = int(tris.size() / 3);

    // 2. Bounds of the walkable geometry
    float gmin[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
    float gmax[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                     std::numeric_limits<float>::lowest()};
    for (size_t i = 0; i < verts.size(); i += 3)
        for (int k = 0; k < 3; k++) {
            gmin[k] = std::min(gmin[k], verts[i + size_t(k)]);
            gmax[k] = std::max(gmax[k], verts[i + size_t(k)]);
        }
    nav.extentX = gmax[0] - gmin[0];
    nav.extentZ = gmax[2] - gmin[2];

    // 3. Voxelise. Recast parameters are in voxel units, so they follow the
    // cell size, which grows when the spans would not fit Recast's index
    // (kNavMaxSpans). Spans scale with the inverse square of the cell size.
    Context ctx(errors);
    auto failed = [&](const char* step) {
        errors.push_back(std::string("nav: ") + step + " failed");
        return NavMesh{};
    };
    float cs = p.cellSize, ch = p.cellHeight;
    {
        // One span per cell over the padded bounds is the floor; layered
        // geometry adds more, which the loop below catches.
        const double pad = 2.0 * p.agentRadius + 2.0 * cs;
        const double cells = (double(nav.extentX) + pad) * (double(nav.extentZ) + pad) / (double(cs) * cs);
        if (cells > kNavSpanBudget) cs = float(double(cs) * std::sqrt(cells / kNavSpanBudget));
    }
    int walkableHeight = int(std::ceil(double(p.agentHeight) / ch));
    int walkableClimb = int(std::floor(double(p.maxStepHeight) / ch));
    int walkableRadius = 0, maxEdgeLen = 0;
    float detailSampleDist = 0;
    float bmin[3], bmax[3];
    std::vector<unsigned char> areas(size_t(ntris), 0);
    rcMarkWalkableTriangles(&ctx, p.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
    HeightfieldPtr solid;
    for (int attempt = 0;; attempt++) {
        walkableRadius = int(std::ceil(double(p.agentRadius) / cs));
        maxEdgeLen = int(double(p.maxEdgeLength) / cs);
        detailSampleDist = cs * p.detailSampleDist;
        // Padded by the erosion radius in XZ
        float borderPad = float(walkableRadius) * cs;
        for (int k = 0; k < 3; k++) bmin[k] = gmin[k], bmax[k] = gmax[k];
        bmin[0] -= borderPad;
        bmin[2] -= borderPad;
        bmax[0] += borderPad;
        bmax[2] += borderPad;
        int gw = int((double(bmax[0]) - bmin[0]) / cs + 0.5);
        int gh = int((double(bmax[2]) - bmin[2]) / cs + 0.5);

        solid.reset(rcAllocHeightfield());
        if (!solid || !rcCreateHeightfield(&ctx, *solid, gw, gh, bmin, bmax, cs, ch)) return failed("rcCreateHeightfield");
        if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *solid, walkableClimb))
            return failed("rcRasterizeTriangles");
        rcFilterLowHangingWalkableObstacles(&ctx, walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, walkableHeight, walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, walkableHeight, *solid);

        const int spans = rcGetHeightFieldSpanCount(&ctx, *solid);
        if (spans <= kNavSpanBudget) break;
        if (attempt == 4) {
            if (spans <= kNavMaxSpans) break;
            return failed("too many spans for Recast");
        }
        cs = float(double(cs) * std::sqrt(double(spans) / (0.95 * kNavSpanBudget)));
    }
    nav.cellSize = cs;

    CompactPtr chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, walkableHeight, walkableClimb, *solid, *chf))
        return failed("rcBuildCompactHeightfield");
    if (!rcErodeWalkableArea(&ctx, walkableRadius, *chf)) return failed("rcErodeWalkableArea");
    // DotRecast takes the border size from the heightfield, which is 0 here.
    switch (p.partition) {
        case NavPartition::Monotone:
            if (!rcBuildRegionsMonotone(&ctx, *chf, 0, p.minRegionArea, p.mergeRegionArea))
                return failed("rcBuildRegionsMonotone");
            break;
        case NavPartition::Layer:
            if (!rcBuildLayerRegions(&ctx, *chf, 0, p.minRegionArea)) return failed("rcBuildLayerRegions");
            break;
        default:
            if (!rcBuildDistanceField(&ctx, *chf)) return failed("rcBuildDistanceField");
            if (!rcBuildRegions(&ctx, *chf, 0, p.minRegionArea, p.mergeRegionArea)) return failed("rcBuildRegions");
            break;
    }
    ContourSetPtr cset(rcAllocContourSet());
    if (!cset || !rcBuildContours(&ctx, *chf, p.maxSimplifyError, maxEdgeLen, *cset, RC_CONTOUR_TESS_WALL_EDGES))
        return failed("rcBuildContours");
    PolyMeshPtr pmesh(rcAllocPolyMesh());
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, kNavMaxVertsPerRegion, *pmesh)) return failed("rcBuildPolyMesh");
    DetailPtr dmesh(rcAllocPolyMeshDetail());
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, detailSampleDist, p.detailMaxError, *dmesh))
        return failed("rcBuildPolyMeshDetail");

    // 5. One region per polygon with at least 3 vertices
    const int nvp = pmesh->nvp;
    auto poly = [&](int i) { return pmesh->polys + size_t(i) * 2 * size_t(nvp); };
    auto vertCount = [&](int i) {
        int nv = 0;
        while (nv < nvp && poly(i)[nv] != RC_MESH_NULL_IDX) nv++;
        return nv;
    };
    auto worldX = [&](unsigned short vi) {
        return float(double(pmesh->bmin[0]) + double(pmesh->verts[vi * 3 + 0]) * pmesh->cs);
    };
    auto worldZ = [&](unsigned short vi) {
        return float(double(pmesh->bmin[2]) + double(pmesh->verts[vi * 3 + 2]) * pmesh->cs);
    };
    std::vector<int> polyToRegion(size_t(pmesh->npolys), -1);
    for (int i = 0; i < pmesh->npolys; i++) {
        int nv = vertCount(i);
        if (nv < 3) continue;
        NavRegion region;
        std::vector<Vec3> pts;

        std::vector<bool> isBoundary(static_cast<size_t>(nv));
        for (int j = 0; j < nv; j++) {
            unsigned short nb = poly(i)[nvp + j];
            isBoundary[size_t(j)] = nb == RC_MESH_NULL_IDX || (nb & 0x8000) != 0;
        }
        for (int j = 0; j < nv; j++) {
            unsigned short vi = poly(i)[j];
            float wx = worldX(vi), wz = worldZ(vi);
            float wy;
            if (i < dmesh->nmeshes) {
                unsigned int vbase = dmesh->meshes[size_t(i) * 4 + 0];
                wy = dmesh->verts[(vbase + unsigned(j)) * 3 + 1];
            } else {
                wy = float(double(pmesh->bmin[1]) + double(pmesh->verts[vi * 3 + 1]) * pmesh->ch);
            }
            region.verts.push_back({wx, wz});
            pts.push_back({wx, wy, wz});
        }

        // Counter-clockwise winding
        float signedArea = 0;
        for (size_t j = 0; j < region.verts.size(); j++) {
            Vec2 a = region.verts[j], b = region.verts[(j + 1) % region.verts.size()];
            signedArea = float(signedArea + (double(a.x) * b.y - double(b.x) * a.y));
        }
        if (signedArea < 0) {
            std::reverse(region.verts.begin(), region.verts.end());
            std::reverse(pts.begin(), pts.end());
            std::vector<bool> reversed(static_cast<size_t>(nv));
            for (int j = 0; j < nv; j++) reversed[size_t((nv - 2 - j + nv) % nv)] = isBoundary[size_t(j)];
            isBoundary = reversed;
        }
        for (int j = 0; j < nv; j++)
            if (isBoundary[size_t(j)]) region.boundaryEdgeMask |= uint8_t(1 << j);

        // Interior detail samples join the plane fit.
        if (i < dmesh->nmeshes) {
            unsigned int vbase = dmesh->meshes[size_t(i) * 4 + 0];
            unsigned int vcount = dmesh->meshes[size_t(i) * 4 + 1];
            for (unsigned int dv = unsigned(nv); dv < vcount; dv++) {
                const float* v = &dmesh->verts[(vbase + dv) * 3];
                pts.push_back({v[0], v[1], v[2]});
            }
        }
        fitPlane(region, pts);
        polyToRegion[size_t(i)] = int(nav.regions.size());
        nav.regions.push_back(std::move(region));
    }

    // 6. Portals from the poly mesh neighbour links, grouped by region
    std::vector<std::vector<NavPortal>> perRegion(nav.regions.size());
    for (int i = 0; i < pmesh->npolys; i++) {
        int src = polyToRegion[size_t(i)];
        if (src < 0) continue;
        int nv = vertCount(i);
        for (int j = 0; j < nv; j++) {
            unsigned short nb = poly(i)[nvp + j];
            if (nb == RC_MESH_NULL_IDX || (nb & 0x8000) != 0) continue;
            int dst = size_t(nb) < polyToRegion.size() ? polyToRegion[nb] : -1;
            if (dst < 0) continue;
            // Edge from the poly mesh, not the (possibly reversed) region.
            unsigned short vi0 = poly(i)[j], vi1 = poly(i)[(j + 1) % nv];
            NavPortal portal;
            portal.a = {worldX(vi0), worldZ(vi0)};
            portal.b = {worldX(vi1), worldZ(vi1)};
            Vec2 mid{(portal.a.x + portal.b.x) / 2, (portal.a.y + portal.b.y) / 2};
            portal.neighborRegion = dst;
            portal.heightDelta = evalY(nav.regions[size_t(dst)], mid) - evalY(nav.regions[size_t(src)], mid);
            perRegion[size_t(src)].push_back(portal);
        }
    }
    for (size_t r = 0; r < perRegion.size(); r++) {
        nav.regions[r].portalStart = int(nav.portals.size());
        nav.regions[r].portalCount = int(perRegion[r].size());
        nav.portals.insert(nav.portals.end(), perRegion[r].begin(), perRegion[r].end());
    }

    // 7-9. Rooms (from the Room volumes, else the connectivity fallback),
    // platform flags, start region.
    if (roomBoxes.empty())
        assignRoomsByBfs(nav);
    else
        assignRoomsFromBoxes(nav, roomBoxes);
    applyPlatformFlags(nav, objects);
    applyWalkoffZones(nav, walkoffBoxes);
    nav.startRegion = findClosestRegion(nav, spawn);
    return nav;
}

void applyWalkoffZones(NavMesh& nav, const std::vector<Bounds>& boxes) {
    nav.walkoffEdgesPerZone.assign(boxes.size(), 0);
    if (boxes.empty()) return;
    for (NavRegion& r : nav.regions) {
        const size_t n = r.verts.size();
        for (size_t j = 0; j < n && j < 8; j++) {
            if (!(r.boundaryEdgeMask & (1u << j))) continue;
            const Vec2 a = r.verts[j], b = r.verts[(j + 1) % n];
            const Vec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            const Vec3 p{mid.x, evalY(r, mid), mid.y};
            for (size_t z = 0; z < boxes.size(); z++) {
                const Vec3 mn = boxes[z].min(), mx = boxes[z].max();
                if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y && p.z >= mn.z && p.z <= mx.z) {
                    r.walkoffEdgeMask |= uint8_t(1u << j);
                    nav.walkoffEdgesPerZone[z]++;
                    break;
                }
            }
        }
    }
}

void writeNavRegions(BinWriter& w, const NavMesh& nav, float gte) {
    w.u16(uint16_t(nav.regions.size()));
    w.u16(uint16_t(nav.portals.size()));
    w.u16(uint16_t(nav.startRegion));
    w.u16(0);
    for (const NavRegion& r : nav.regions) {
        for (size_t v = 0; v < kNavMaxVertsPerRegion; v++)
            w.i32(v < r.verts.size() ? toWorldFixed12(r.verts[v].x / gte) : 0);
        for (size_t v = 0; v < kNavMaxVertsPerRegion; v++)
            w.i32(v < r.verts.size() ? toWorldFixed12(r.verts[v].y / gte) : 0);
        w.i32(toWorldFixed12(-r.planeA));
        w.i32(toWorldFixed12(-r.planeB));
        w.i32(toWorldFixed12(-r.planeD / gte));
        w.u16(uint16_t(r.portalStart));
        w.u8(uint8_t(r.portalCount));
        w.u8(uint8_t(r.verts.size()));
        w.u8(r.surfaceType);
        w.u8(r.roomIndex);
        w.u8(r.flags);
        w.u8(r.walkoffEdgeMask);
    }
    for (const NavPortal& p : nav.portals) {
        w.i32(toWorldFixed12(p.a.x / gte));
        w.i32(toWorldFixed12(p.a.y / gte));
        w.i32(toWorldFixed12(p.b.x / gte));
        w.i32(toWorldFixed12(p.b.y / gte));
        w.u16(uint16_t(p.neighborRegion));
        w.i16(toFixed12(p.heightDelta / gte));
    }
}

}  // namespace splash
