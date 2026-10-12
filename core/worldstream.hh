// World streaming planner: which static geometry streams from the .geo file,
// in which regions, and how much RAM the engine sets aside for them
// (psxsplash WorldStreamer / StreamPlanner). Pure: no scene types, no I/O.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splash::stream {

constexpr uint32_t kBytesPerTri = 52;  // one Tri record, as the pack stores it
constexpr uint32_t kSector = 2048;
constexpr int kMaxSlots = 32;  // StreamPlanner::kMaxResident
constexpr uint32_t kTargetRegionBytes = 64 * 1024;
constexpr uint32_t kLargeRegionBytes = 256 * 1024;
constexpr uint32_t kTableBytes = 20, kRegionBytes = 32, kRefBytes = 8;

// XZ in PS1 fixed point (fp12), as the engine compares against the camera.
struct Box {
    int32_t minX, minZ, maxX, maxZ;
};

// An object that may stream: its world-space XZ box and one box per triangle.
struct Candidate {
    int object;  // caller's index
    Box bounds;
    std::vector<Box> tris;
};

// Part of one candidate inside one region: indices into Candidate::tris.
// A candidate smaller than a region lands whole in the region holding its
// centre; a bigger one is cut by triangle centre, one piece per region.
struct Piece {
    int candidate;  // index into the planner input
    std::vector<int> tris;
};

struct Region {
    Box bounds;
    std::vector<Piece> pieces;
    uint32_t triBytes = 0;
    uint32_t byteSize = 0;  // triBytes rounded up to a sector
    uint32_t firstSector = 0;
};

struct Plan {
    std::vector<Region> regions;
    int64_t cell = 0;  // region size, fp12
    int32_t loadRadius = 0, unloadRadius = 0;
    uint32_t slotBytes = 0;
    int slotCount = 0;    // what the table carries, at most kMaxSlots
    int slotsWanted = 0;  // regions near the camera at once, plus the read in flight
    uint64_t streamedTriBytes = 0;
    uint64_t drawTris = 0;  // most streamed triangles within the draw distance of one camera position
    size_t pieceCount() const;
    uint64_t poolBytes() const { return uint64_t(slotCount) * slotBytes; }
    uint64_t tableBytes() const { return kTableBytes + kRegionBytes * regions.size() + kRefBytes * pieceCount(); }
};

// `drawDistance` (fp12) is how far the camera sees; a region loads within
// 1.25x of it and drops at 1.5x the load radius. `cell` 0 picks the region size with the
// least streaming RAM that keeps regions small and within the slot cap.
Plan plan(const std::vector<Candidate>& in, int64_t drawDistance, int64_t cell = 0);

// Squared XZ distance (fp12^2) from a point to a box, 0 inside (streamRegionDistSq).
int64_t distSq(const Box& b, int64_t x, int64_t z);

}  // namespace splash::stream
