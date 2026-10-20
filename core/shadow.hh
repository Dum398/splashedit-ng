#pragma once

// Blob shadows: what the console draws under an object whose mesh has
// Shadow = Blob, in editor terms (world metres, Y up). The exporter writes the
// splashpack shadow table from these constants, the scene view draws blobFor()
// and the tests check it; psxsplash's src/shadowmath.hh and
// Renderer::renderShadows draw the same model on the console:
//
//   floor    = the nav region under the object (the one whose floor is
//              nearest the bottom of its bounds, no higher than their middle
//              and at most kFadeHeight below), else the bottom of its bounds.
//              On the console an object that has moved (Lua, an animation or
//              a cutscene) and has no nav floor under it, in a scene with
//              nav, gets no blob: it is in the air. The editor shows objects
//              where they start, so it always uses its bounds there.
//   h        = bounds bottom - floor
//   fade     = max(0, 1 - h / kFadeHeight)
//   radius   = size * fade
//   darkness = darkness * fade * edge
//
// edge fades a blob whose centre is within radius of where its floor ends (a
// nav region's walk-off edge: its other edges are a player radius in from a
// wall or a drop, so the floor goes on past them) linearly to 0 at that edge. The rim
// lies on the floor's plane, so the blob tilts with a ramp. The console also
// fades it into fog, which the scene view does not show.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "splashpack.hh"

namespace splash::shadow {

constexpr float kFadeHeight = 2.5f;  // metres above the floor at which a blob is gone
constexpr float kMinSize = 0.05f, kMaxSize = 10.f;  // metres
constexpr int kRimVerts = 8;
constexpr int kMaxSceneShadows = 32;  // psxsplash MAX_SCENE_SHADOWS; the rest are dropped
constexpr uint8_t kModeBlob = 1;     // the splashpack record's mode (2 is reserved for silhouettes)
constexpr size_t kRecordBytes = 8;
// Render buffer bytes per blob: two TPage fragments and eight Gouraud triangles.
constexpr uint32_t kBumpBytes = 2 * 8 + kRimVerts * 28;

// Darkness percent -> the 0..255 the console subtracts at the centre.
inline uint8_t darknessByte(float percent) {
    return uint8_t(std::lround(std::clamp(percent, 0.f, 100.f) * 2.55f));
}

struct Blob {
    bool visible = false;
    Vec3 centre;                     // on the floor
    std::array<Vec3, kRimVerts> rim;  // radius from the centre, on the floor plane
    float radius = 0;                // metres
    float darkness = 0;              // 0..1 of full black at the centre
    float height = 0;                // bounds bottom above the floor, metres
    int region = -1;                 // nav region under it, -1 = bounds bottom
};

namespace detail {
inline float planeY(const ExportStats::NavRegionInfo& r, float x, float z) { return r.planeA * x + r.planeB * z + r.planeD; }

inline bool inside(const std::vector<Vec3>& poly, float x, float z) {
    if (poly.size() < 3) return false;
    int sign = 0;
    for (size_t i = 0; i < poly.size(); i++) {
        const Vec3& a = poly[i];
        const Vec3& b = poly[(i + 1) % poly.size()];
        float c = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
        if (c == 0) continue;
        int s = c > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

inline float segmentDist(float px, float pz, float ax, float az, float bx, float bz) {
    float abx = bx - ax, abz = bz - az, len = abx * abx + abz * abz;
    float t = len > 0 ? std::clamp(((px - ax) * abx + (pz - az) * abz) / len, 0.f, 1.f) : 0.f;
    return std::hypot(px - (ax + t * abx), pz - (az + t * abz));
}
}  // namespace detail

// Distance from (x, z) to the nearest walk-off edge of the region (outline
// edge k to k + 1 with bit k of walkoffEdges). Infinity when it has none.
inline float walkoffEdgeDistance(const std::vector<Vec3>& outline, const ExportStats::NavRegionInfo& info, float x,
                                 float z) {
    float best = INFINITY;
    for (size_t k = 0; k < outline.size() && k < 8; k++) {
        if (!(info.walkoffEdges & (1u << k))) continue;
        const Vec3& a = outline[k];
        const Vec3& b = outline[(k + 1) % outline.size()];
        best = std::min(best, detail::segmentDist(x, z, a.x, a.z, b.x, b.z));
    }
    return best;
}

// The blob under an object at `position` whose world bounds span bottom..top
// in Y, over the exported nav (outlines and info as in ExportStats; empty =
// no nav).
inline Blob blobFor(Vec3 position, float bottom, float top, float size, float darknessPercent,
                    const std::vector<std::vector<Vec3>>& outlines,
                    const std::vector<ExportStats::NavRegionInfo>& info) {
    Blob b;
    const float middle = (bottom + top) / 2;
    float floorY = bottom;
    const size_t n = std::min(outlines.size(), info.size());
    float bestDist = INFINITY;
    for (size_t i = 0; i < n; i++) {
        if (!detail::inside(outlines[i], position.x, position.z)) continue;
        float fy = detail::planeY(info[i], position.x, position.z);
        if (fy > middle || fy < bottom - kFadeHeight) continue;
        if (std::fabs(fy - bottom) < bestDist) {
            bestDist = std::fabs(fy - bottom);
            b.region = int(i);
            floorY = fy;
        }
    }
    b.height = std::max(0.f, bottom - floorY);
    const float fade = std::max(0.f, 1 - b.height / kFadeHeight);
    b.radius = std::clamp(size, kMinSize, kMaxSize) * fade;
    float edge = 1;
    if (b.region >= 0 && b.radius > 0) {
        float d = walkoffEdgeDistance(outlines[size_t(b.region)], info[size_t(b.region)], position.x, position.z);
        if (d < b.radius) edge = std::max(0.f, d / b.radius);
    }
    b.darkness = darknessByte(darknessPercent) / 255.f * fade * edge;
    b.centre = {position.x, floorY, position.z};
    for (int i = 0; i < kRimVerts; i++) {
        float a = float(i) * 0.78539816f;  // 45 degrees
        float x = position.x + b.radius * std::cos(a), z = position.z + b.radius * std::sin(a);
        float y = b.region >= 0 ? detail::planeY(info[size_t(b.region)], x, z) : floorY;
        b.rim[size_t(i)] = {x, y, z};
    }
    b.visible = b.radius > 0 && b.darkness > 0;
    return b;
}

}  // namespace splash::shadow
