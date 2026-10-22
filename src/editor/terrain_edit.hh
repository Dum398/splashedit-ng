// Terrain editing through the document's history: brush strokes in the
// Scene view (one undo step per stroke) and the inspector's grid changes.
// Headless, so editor_tests drives it the way the viewport does.
#pragma once

#include <optional>
#include <string>

#include "editor/document.hh"
#include "terrain.hh"

namespace editor {

// The merge key every brush application of one stroke shares.
inline constexpr const char* kTerrainStrokeKey = "terrain.stroke";

// Where a world-space ray (Unity space) first meets the terrain of the
// object at `path`, in world space; nullopt when it misses or the object has
// no loaded terrain.
std::optional<splash::Vec3> terrainHit(const Document& doc, const ObjectPath& path, splash::Vec3 origin, splash::Vec3 dir);

// A stroke: begin at the first hit (Flatten takes its target height there),
// step once per frame while the button is held, end on release.
struct TerrainStroke {
    bool active = false;
    ObjectPath path;
    splash::TerrainBrush brush;  // as the stroke started, target filled in
    int steps = 0;               // applications that changed the grid
};
void beginTerrainStroke(TerrainStroke& s, Document& doc, const ObjectPath& path, const splash::TerrainBrush& brush,
                        splash::Vec3 worldHit);
// One application at `worldHit` over `dt` seconds (clamped to 0..0.1).
// Merges under kTerrainStrokeKey. Returns whether the grid changed.
bool stepTerrainStroke(TerrainStroke& s, Document& doc, splash::Vec3 worldHit, float dt);
void endTerrainStroke(TerrainStroke& s, Document& doc);

// Inspector edits, each one undo step. Resolution resamples the grid.
bool setTerrainResolution(Document& doc, const ObjectPath& path, int resolution);
bool setTerrainSize(Document& doc, const ObjectPath& path, float sizeX, float sizeZ, const std::string& mergeKey = {});

// The inspector's info line: "N triangles, M stream regions when streamed"
// (M from the last export's regions this terrain falls in; -1 = unknown).
std::string terrainInfoLine(int triangles, int streamRegions);

}  // namespace editor
