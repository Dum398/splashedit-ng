#include "editor/terrain_edit.hh"

#include <algorithm>

namespace editor {

namespace {

const splash::FlatObject* flatOf(const std::vector<splash::FlatObject>& flats, const splash::Object* o) {
    for (const splash::FlatObject& fo : flats)
        if (fo.object == o) return &fo;
    return nullptr;
}

// A terrain the history may change: not prefab content, not an instance's own (its grid is the prefab's).
bool editable(const Document& doc, const ObjectPath& path) {
    const splash::Object* o = doc.object(path);
    return o && o->terrain && o->prefab.empty() && !doc.inInstance(path);
}

}  // namespace

std::optional<splash::Vec3> terrainHit(const Document& doc, const ObjectPath& path, splash::Vec3 origin, splash::Vec3 dir) {
    const splash::Object* o = doc.object(path);
    if (!o || !o->terrain || !splash::terrainLoaded(*o->terrain)) return std::nullopt;
    const std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    const splash::FlatObject* fo = flatOf(flats, o);
    if (!fo) return std::nullopt;
    const splash::Vec3 lo = splash::inverseTransformPoint(fo->localToWorld, origin);
    const splash::Vec3 ld = splash::inverseTransformDirection(fo->localToWorld, dir);
    std::optional<splash::Vec3> hit = splash::terrainRayHit(*o->terrain, lo, ld);
    if (!hit) return std::nullopt;
    return fo->localToWorld.point(*hit);
}

static bool localPoint(const Document& doc, const ObjectPath& path, splash::Vec3 world, splash::Vec3* local) {
    const splash::Object* o = doc.object(path);
    if (!o || !o->terrain) return false;
    const std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    const splash::FlatObject* fo = flatOf(flats, o);
    if (!fo) return false;
    *local = splash::inverseTransformPoint(fo->localToWorld, world);
    return true;
}

void beginTerrainStroke(TerrainStroke& s, Document& doc, const ObjectPath& path, const splash::TerrainBrush& brush,
                        splash::Vec3 worldHit) {
    doc.endMerge();
    s = {};
    s.active = true;
    s.path = path;
    s.brush = brush;
    splash::Vec3 lp;
    const splash::Object* o = doc.object(path);
    if (o && o->terrain && localPoint(doc, path, worldHit, &lp)) s.brush.target = splash::terrainHeightAt(*o->terrain, lp.x, lp.z);
}

bool stepTerrainStroke(TerrainStroke& s, Document& doc, splash::Vec3 worldHit, float dt) {
    if (!s.active || !editable(doc, s.path)) return false;
    splash::Vec3 lp;
    if (!localPoint(doc, s.path, worldHit, &lp)) return false;
    dt = std::clamp(dt, 0.f, 0.1f);
    // Try on a copy first, so a step that changes nothing makes no history.
    const splash::Object* o = doc.object(s.path);
    splash::TerrainComponent probe = *o->terrain;
    if (!splash::applyTerrainBrush(probe, lp.x, lp.z, s.brush, dt)) return false;
    doc.edit(s.path, [&](splash::Object& ob) {
        if (ob.terrain) ob.terrain->heights = probe.heights, ob.terrain->cellLayers = probe.cellLayers;
    }, kTerrainStrokeKey);
    s.steps++;
    return true;
}

void endTerrainStroke(TerrainStroke& s, Document& doc) {
    if (s.active) doc.endMerge();
    s.active = false;
}

bool setTerrainResolution(Document& doc, const ObjectPath& path, int resolution) {
    if (!editable(doc, path)) return false;
    const splash::Object* o = doc.object(path);
    resolution = std::clamp(resolution, 1, splash::kTerrainMaxResolution);
    if (resolution == o->terrain->resolution) return false;
    return doc.edit(path, [resolution](splash::Object& ob) {
        if (ob.terrain) splash::resampleTerrain(*ob.terrain, resolution);
    });
}

bool setTerrainSize(Document& doc, const ObjectPath& path, float sizeX, float sizeZ, const std::string& mergeKey) {
    if (!editable(doc, path)) return false;
    const splash::Object* o = doc.object(path);
    sizeX = std::clamp(sizeX, 0.1f, 4000.f);
    sizeZ = std::clamp(sizeZ, 0.1f, 4000.f);
    if (sizeX == o->terrain->sizeX && sizeZ == o->terrain->sizeZ) return false;
    return doc.edit(path, [sizeX, sizeZ](splash::Object& ob) {
        if (ob.terrain) ob.terrain->sizeX = sizeX, ob.terrain->sizeZ = sizeZ;
    }, mergeKey);
}

std::string terrainInfoLine(int triangles, int streamRegions) {
    std::string s = std::to_string(triangles) + " triangles";
    if (streamRegions >= 0) s += ", " + std::to_string(streamRegions) + (streamRegions == 1 ? " region" : " regions") + " when streamed";
    return s;
}

}  // namespace editor
