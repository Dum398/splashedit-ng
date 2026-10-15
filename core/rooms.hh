// Rooms and portals: SplashEdit 2.4's PSXRoomBuilder. Each static triangle
// goes to the Room whose box holds most of its corners (a catch-all room
// takes the rest), triangles near a portal are copied into both rooms, and
// each room is split into up to 2x2x2 cells for frustum culling. When the
// splashpack carries rooms, psxsplash renders static geometry through them
// instead of the BVH.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "binwriter.hh"
#include "bvh.hh"
#include "scene.hh"

namespace splash {

// psxsplash marks rooms visited in a 32-bit mask that only covers rooms
// below 32 (renderer.cpp); 31 real rooms keep every one of them in it.
constexpr size_t kMaxRooms = 31;
constexpr float kRoomMargin = 0.5f;  // per side, world units (PSXRoomBuilder ROOM_MARGIN)

// One exported object, in pack order: its exported Tris' mesh-space corners
// (3 per Tri, Tri order), the same input the BVH gets.
struct RoomInputObject {
    bool include;
    Mat34 localToWorld;
    const std::vector<Vec3>* triPositions;
};

struct RoomCellOut {
    Vec3 min, max;  // world, tight over the cell's triangles
    uint32_t firstTriRef = 0, triRefCount = 0;  // global index into refs
};

struct RoomOut {
    std::string name;  // the Room object's name; "catch-all" for the last
    Vec3 min, max;     // world box (unexpanded); the catch-all is +-1000
    uint32_t firstTriRef = 0, triRefCount = 0;
    uint32_t firstCell = 0, cellCount = 0;
    uint32_t firstPortalRef = 0, portalRefCount = 0;
};

struct PortalOut {
    std::string name;
    uint16_t roomA = 0, roomB = 0;
    PortalFrame frame;  // normal and right already flipped to point A to B
};

struct RoomPortalRef {
    uint16_t portalIndex, otherRoom;
};

struct RoomSet {
    std::vector<RoomOut> rooms;  // real rooms in flatten order, then the catch-all
    std::vector<PortalOut> portals;
    std::vector<TriangleRef> refs;
    std::vector<RoomCellOut> cells;
    std::vector<RoomPortalRef> portalRefs;
    bool empty() const { return rooms.empty(); }
    size_t realRooms() const { return rooms.empty() ? 0 : rooms.size() - 1; }
};

// The Room and Portal objects of the scene (active in hierarchy and
// exported), in flatten order. Appends to errors for portals that do not
// name two different Room objects. Rooms build iff one Room exists.
struct RoomObjects {
    std::vector<const FlatObject*> rooms, portals;
};
RoomObjects collectRoomObjects(const std::vector<FlatObject>& flat, std::vector<std::string>& errors);

// World boxes of the rooms, unexpanded, in room order.
std::vector<Bounds> roomBoxes(const RoomObjects& ro);

// Empty result when there are no rooms. Appends hard errors for anything the
// splashpack cannot carry (u16 totals, u8 per-room portal refs, more than
// kMaxRooms rooms).
RoomSet buildRooms(const RoomObjects& ro, const std::vector<RoomInputObject>& objects,
                   std::vector<std::string>& errors);

// The room block (rooms, portals, tri refs, cells, portal refs), no alignment.
void writeRooms(BinWriter& w, const RoomSet& rs, float gte);

}  // namespace splash
