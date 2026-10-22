// Nav regions: SplashEdit 2.4's PSXNavRegionBuilder. Recast voxelises the
// static geometry, each polygon of the resulting poly mesh becomes a convex
// region with a least-squares floor plane, and shared polygon edges become
// portals.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "binwriter.hh"
#include "scene.hh"

namespace splash {

constexpr int kNavMaxVertsPerRegion = 8;

// PSXNavRegionBuilder's fields, with its defaults.
struct NavBuildParams {
    float agentHeight = 1.8f;
    float agentRadius = 0.3f;
    float maxStepHeight = 0.35f;
    float walkableSlopeAngle = 46.0f;
    float cellSize = 0.05f;
    float cellHeight = 0.025f;
    int minRegionArea = 8;
    int mergeRegionArea = 20;
    float maxSimplifyError = 1.3f;
    float maxEdgeLength = 12.0f;
    NavPartition partition = NavPartition::Watershed;
    float detailSampleDist = 6.0f;
    float detailMaxError = 0.025f;
    float maxPlaneError = 0.15f;
    // Cut regions on a grid so each one's plane fits the ground within
    // max(maxPlaneError, maxStepHeight) (terrain: a height field rolls inside
    // one Recast region). One extra bake pass at most.
    bool splitByPlaneError = false;
};

// One exported object, in exporter order.
struct NavInputObject {
    bool walkable;  // collider kind Static (2.4.0 skips None and Dynamic)
    bool platform;
    Mat34 localToWorld;
    const std::vector<Vec3>* positions;
    std::vector<int> triangles;  // all submeshes concatenated (Mesh.triangles)
    Bounds localBounds;
};

struct NavRegion {
    std::vector<Vec2> verts;  // XZ, counter-clockwise
    float planeA = 0, planeB = 0, planeD = 0;  // Y = A*X + B*Z + D
    int portalStart = 0, portalCount = 0;
    uint8_t surfaceType = 0;  // 0 flat, 1 ramp, 2 stairs
    uint8_t roomIndex = 0;
    uint8_t flags = 0;  // bit 0 = platform
    uint8_t walkoffEdgeMask = 0;
    uint8_t boundaryEdgeMask = 0;  // not written
    float maxPlaneDeviation = 0;
};

struct NavPortal {
    Vec2 a, b;
    int neighborRegion = 0;
    float heightDelta = 0;
};

struct NavMesh {
    std::vector<NavRegion> regions;
    std::vector<NavPortal> portals;
    int startRegion = 0;
    // The voxel size the bake used. Larger than NavBuildParams::cellSize when
    // that one would have needed more spans than Recast can index.
    float cellSize = 0;
    // Width and depth of the walkable geometry, in world units.
    float extentX = 0, extentZ = 0;
    // Per walk-off box (in the order passed to buildNavRegions): how many
    // boundary edges it opened. An edge inside two boxes counts for the first.
    std::vector<int> walkoffEdgesPerZone;
};

// Recast's compact heightfield indexes spans with 24 bits (rcCompactCell::index),
// so a bake over more spans than this links the wrong spans together and
// produces no regions without reporting an error.
constexpr int kNavMaxSpans = (1 << 24) - 1;
// The bake's cost grows with its span count, about 4 s per million on a
// 241 m terrain, and the editor re-exports on every edit. Past this many the
// cell size grows; the region count barely moves (80 regions at 0.3 m, 85 at
// 0.06 m on that terrain).
constexpr int kNavSpanBudget = 1000000;
// splitByPlaneError never cuts finer than this many metres.
constexpr float kNavMinTile = 2.0f;

// roomBoxes: the Room objects' world boxes (unexpanded) in room order. With
// any, each region's roomIndex is the smallest room whose box (0.5 larger
// per side) holds its centroid, else 0xFF; with none, connectivity islands.
// walkoffBoxes: the walk-off zones' world boxes; a boundary edge whose
// midpoint (on the region's floor plane) lies inside one becomes walk-off.
NavMesh buildNavRegions(const std::vector<NavInputObject>& objects, const NavBuildParams& params, Vec3 spawn,
                        std::vector<std::string>& errors, const std::vector<Bounds>& roomBoxes = {},
                        const std::vector<Bounds>& walkoffBoxes = {});

// PSXNavRegionBuilder.ApplyWalkoffZones: sets walkoffEdgeMask bits for the
// boundary edges whose midpoint lies inside a box (inclusive) and fills
// nav.walkoffEdgesPerZone.
void applyWalkoffZones(NavMesh& nav, const std::vector<Bounds>& boxes);

// PSXNavRegionBuilder.WriteToBinary
void writeNavRegions(BinWriter& w, const NavMesh& nav, float gte);

}  // namespace splash
