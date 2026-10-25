// Vertex colour painting (#199) through the document's history: the Scene
// view's Paint tool brushes colour onto the selected meshes' painted layer
// (MeshComponent::paint, exported in Painted mode), one undo step per
// stroke. Headless, so editor_tests drives it the way the viewport does.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "editor/document.hh"
#include "scene.hh"

namespace editor {

using Rgb = std::array<uint8_t, 3>;

// The merge key every brush application of one stroke shares.
inline constexpr const char* kPaintStrokeKey = "paint.stroke";

struct PaintBrush {
    Rgb color{255, 255, 255};
    float radius = 1.0f;    // metres, world space
    float strength = 1.0f;  // 0..1: how far a stroke takes a vertex at the centre towards the colour
    float falloff = 0.5f;   // 0: hard edge; 1: fades smoothly from the centre to the rim
};

// Brush weight at `dist` from the centre, 0..strength (smoothstep over the outer `falloff` of the radius).
float paintWeight(const PaintBrush& b, float dist);

// Why the object at `path` cannot be painted ("" when it can): no mesh,
// a terrain (painted with its own tools), a skinned mesh, part of a prefab
// instance, or a model that does not load.
std::string paintRefusal(const Document& doc, const ObjectPath& path);
// The selected objects that can be painted, in Document::selectedPaths order.
std::vector<ObjectPath> paintTargets(const Document& doc);

// The mesh file a paintable object uses, loaded once per file version.
const splash::Mesh* paintMesh(const Document& doc, const std::string& meshPath);

// The colours the object exports now, one byte triple per mesh vertex: its
// painted layer in Painted mode when that fits, else what the exporter
// writes for its mode (splash::exportVertexColors, as bytes). Empty when it
// cannot be painted.
std::vector<Rgb> currentPaint(const Document& doc, const ObjectPath& path);

// Where a world-space ray (Unity space) first meets one of `paths`' meshes.
struct PaintHit {
    ObjectPath path;
    splash::Vec3 world;
    splash::Vec3 normal;  // of the triangle hit, unit length, world space (for the brush circle)
    Rgb color;            // the current colours interpolated across the triangle hit
};
std::optional<PaintHit> paintHit(const Document& doc, const std::vector<ObjectPath>& paths, splash::Vec3 origin, splash::Vec3 dir);

// A stroke over `paths` (paintTargets): begin on the button press, step
// once per frame at the brush's hit point, end on release. A vertex takes
// the colour by the largest weight the brush has reached it with during the
// stroke, so holding still or going over it again does not build up. The
// first step that reaches an object also sets it to Painted, its layer
// starting from what it exported (currentPaint); all of it is one undo step.
struct PaintStroke {
    bool active = false;
    PaintBrush brush;
    std::vector<ObjectPath> paths;
    std::vector<std::vector<Rgb>> start;      // per path, as the stroke began
    std::vector<std::vector<float>> weight;   // per path and vertex, the most it got
    std::vector<char> touched;                // per path: some vertex changed
    int steps = 0;                            // applications that changed something
};
void beginPaintStroke(PaintStroke& s, Document& doc, const std::vector<ObjectPath>& paths, const PaintBrush& brush);
// Merges under kPaintStrokeKey. Returns whether any colour changed.
bool stepPaintStroke(PaintStroke& s, Document& doc, splash::Vec3 worldHit);
void endPaintStroke(PaintStroke& s, Document& doc);

// Every vertex of `paths` set to `color` in Painted mode, one undo step. Returns how many objects changed.
int fillPaint(Document& doc, const std::vector<ObjectPath>& paths, Rgb color);
// Painted mode for the object, its layer started from what it exports now
// when it has none that fits (so nothing changes on screen). One undo step.
bool setPaintedMode(Document& doc, const ObjectPath& path);
// The same for each of `paths` with a mesh not yet Painted, as one undo step. Returns how many changed.
int setPaintedModes(Document& doc, const std::vector<ObjectPath>& paths);
// Drops the painted layer; a Painted mesh goes back to Baked. One undo step.
bool clearPaint(Document& doc, const ObjectPath& path);

}  // namespace editor
