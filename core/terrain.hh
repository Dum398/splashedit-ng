// Height-field terrain (TerrainComponent, core/scene.hh): its data file, the
// mesh built from it (one builder for the exporter and the scene view), and
// the sculpt / paint brushes the editor applies to it.
//
// Data file (.terrain), little endian:
//   char[4] "SPTR", uint32 version (1), uint32 resolution N (1..kTerrainMaxResolution),
//   float32 heights[(N+1)^2] (metres, row z then column x),
//   uint8 layers[N^2] (layer index per cell, row z then column x).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

constexpr uint32_t kTerrainFileVersion = 1;

// A flat grid of `resolution` cells per side (clamped to 1..kTerrainMaxResolution), all layer 0.
void initTerrain(TerrainComponent& t, int resolution);
// The grid is loaded and consistent (sizes match the resolution).
bool terrainLoaded(const TerrainComponent& t);

// Reads / writes the grid of `t` (resolution, heights, cellLayers). Throw std::runtime_error.
void loadTerrainData(TerrainComponent& t, const std::filesystem::path& file);
void saveTerrainData(const TerrainComponent& t, const std::filesystem::path& file);
// Fills every terrain in the scene whose grid is not loaded from its data
// file under `projectRoot`. Returns one message per file it could not read.
std::vector<std::string> loadTerrains(Scene& scene, const std::filesystem::path& projectRoot);
// Writes every loaded terrain's grid to its data file (folders created).
// Returns one message per file it could not write.
std::vector<std::string> saveTerrains(const Scene& scene, const std::filesystem::path& projectRoot);

// `want` (a project-relative .terrain path), or it with _2, _3 ... before the
// extension when another terrain of the scene uses it or the file exists
// under `projectRoot` (skipped when it is empty).
std::string uniqueTerrainData(const Scene& scene, const std::filesystem::path& projectRoot, const std::string& want);

// The mesh: (N+1)^2 shared vertices centred on the object, normals from the
// height field, UVs repeating every tileCells cells, and `layers` submeshes
// (submesh i = cells painted layer i, empty when none are; cells painted a
// layer at or past `layers` go to the last). Each cell splits along its
// shorter diagonal, alternating on a tie.
Mesh buildTerrainMesh(const TerrainComponent& t, int layers);
inline int terrainTriangles(const TerrainComponent& t) { return 2 * t.resolution * t.resolution; }

// Height at local (x, z), bilinear over the cell; clamped to the edges.
float terrainHeightAt(const TerrainComponent& t, float x, float z);
// Changes the resolution, resampling the heights (bilinear) and the cell
// layers (nearest cell centre).
void resampleTerrain(TerrainComponent& t, int resolution);

// The first point where a local-space ray meets the height field, or nullopt.
std::optional<Vec3> terrainRayHit(const TerrainComponent& t, Vec3 origin, Vec3 dir);
// World point -> the object's local space (inverse of an affine Mat34).
Vec3 inverseTransformPoint(const Mat34& m, Vec3 p);
Vec3 inverseTransformDirection(const Mat34& m, Vec3 d);

enum class TerrainTool : uint8_t { Raise, Lower, Smooth, Flatten, Paint };
struct TerrainBrush {
    TerrainTool tool = TerrainTool::Raise;
    float radius = 3;      // metres
    float strength = 2;    // Raise/Lower: metres per second at the centre; Smooth/Flatten: rate per second
    float falloff = 0.5f;  // 0 = hard edge, 1 = fades from the centre
    float target = 0;      // Flatten: the height taken at the start of the stroke
    int layer = 0;         // Paint
};
// Brush weight at distance d from the centre: 1 inside radius*(1-falloff),
// smoothly down to 0 at the radius.
float terrainBrushWeight(const TerrainBrush& b, float d);
// One application at local (x, z) over `dt` seconds. Returns whether anything changed.
bool applyTerrainBrush(TerrainComponent& t, float x, float z, const TerrainBrush& b, float dt);

}  // namespace splash
