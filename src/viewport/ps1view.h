#pragma once

#include <imgui.h>

#include "editor/pick.hh"
#include "scene.hh"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Edit-view renderer that approximates what the PS1 draws: low internal
// resolution, 15-bit colour with the GPU's 4x4 dither, affine texture
// mapping, vertex snapping and depth fog. It renders the loaded scene.
namespace editor {
class Document;
}

namespace viewport {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Mat4 {
    float m[16] = {};
    static Mat4 identity();
    static Mat4 perspective(float fovyRad, float aspect, float zn, float zf);
    static Mat4 ortho(float halfW, float halfH, float zn, float zf);
    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
    Mat4 operator*(const Mat4& o) const;
};

class Ps1View {
  public:
    bool init();
    // The scene to draw. The buffer is rebuilt when the document's revision changes.
    void setDocument(const editor::Document& doc) { m_doc = &doc; }
    // Renders at an internal height of `lines` and returns the colour texture.
    unsigned render(int panelW, int panelH, int lines);
    // Projects a world point into a screen rect matching the panel. Returns false behind the camera.
    bool project(Vec3 p, ImVec2 panelMin, ImVec2 panelSize, ImVec2* out) const;
    // The ray from the eye through a screen point of the panel (GL space, unit direction).
    void ray(ImVec2 screen, ImVec2 panelMin, ImVec2 panelSize, Vec3* origin, Vec3* dir) const;

    // Camera, in GL space. The pivot is the target the eye looks at.
    // Orbits around the pivot by a mouse delta in pixels (the scene follows the mouse).
    void orbit(float dxPx, float dyPx);
    // Slides eye and pivot so the scene follows the mouse. `panelH` sets the pixel scale.
    void pan(float dxPx, float dyPx, float panelH);
    // Moves the eye towards (positive steps) or away from the pivot.
    void dolly(float steps);
    // Puts the pivot at `center` and backs off until a sphere of `radius` fits.
    void frame(Vec3 center, float radius);
    // Turns the camera about the eye by a mouse delta in pixels; the pivot
    // stays in front of it at the same distance.
    void lookAround(float dxPx, float dyPx);
    // Moves eye and pivot by `move` in camera space (right, up, forward).
    void fly(Vec3 move);
    // Moves eye and pivot by a world offset.
    void translate(Vec3 d);
    // Looks along `forward` (GL space, unit) at the same pivot and distance.
    void lookAlong(Vec3 forward);
    void setOrtho(bool on) { m_ortho = on; }
    bool ortho() const { return m_ortho; }
    // World units covered by one panel pixel at `depth` in front of the eye.
    float worldPerPixel(float depth, float panelH) const;
    // Index (in splash::flatten order) of the object whose drawn triangles the
    // ray through a screen point hits first, or nullopt.
    std::optional<int> pick(ImVec2 screen, ImVec2 panelMin, ImVec2 panelSize) const;
    // Bounds of the scene's geometry, false when it has none.
    bool sceneBounds(Vec3* lo, Vec3* hi) const;
    // Unit right, up and forward vectors of the camera.
    void basis(Vec3* right, Vec3* up, Vec3* forward) const;
    float distance() const;
    // 1 up to a 50-unit orbit, then the orbit distance over 50: scales the far
    // plane and the preview fog so a large world (a streamed one) stays visible.
    float viewScale() const { return std::max(1.0f, distance() / 50.0f); }
    // Where the view is rendered from: the eye, or in orthographic a point
    // backed away along the view so nothing near the pivot is clipped.
    Vec3 eye() const;
    Vec3 pivot() const { return m_target; }
    static constexpr float kFovY = 0.95f;
    static constexpr float kOrthoBack = 150.0f;  // orthographic: render eye's distance behind the pivot

    bool clean = false;  // false = PS1 look, true = clean view

  private:
    struct Vertex {
        float px, py, pz;
        float u, v;
        float r, g, b;
    };
    // A run of vertices drawn with one GL texture bound.
    struct Batch {
        unsigned tex;
        int start;
        int count;
    };

    void rebuild();
    unsigned textureFor(const std::string& projectPath);  // cached; white/fallback on empty/error

    const editor::Document* m_doc = nullptr;
    unsigned m_builtRevision = 0;
    bool m_built = false;

    std::vector<Vertex> m_verts;
    std::vector<Batch> m_batches;
    std::map<std::string, unsigned> m_texCache;
    editor::PickMesh m_pick;  // drawn triangles in Unity world space, by object
    std::map<std::string, splash::Mesh> m_meshCache;  // by project path, for the current load
    unsigned m_meshCacheLoad = ~0u;
    unsigned m_meshCacheAssets = 0;
    unsigned m_framedLoad = ~0u;
    bool m_haveSceneBounds = false;
    splash::Bounds m_sceneBounds;  // GL space, geometry only

    unsigned m_prog = 0, m_skyProg = 0, m_vao = 0, m_vbo = 0, m_fbo = 0, m_color = 0, m_depth = 0;
    unsigned m_white = 0, m_fallback = 0;
    size_t m_vboCap = 0;
    int m_fboW = 0, m_fboH = 0;
    Vec3 m_eye{7.6f, 5.2f, 9.2f}, m_target{-0.9f, 0.9f, -2.0f};
    bool m_ortho = false;
    Mat4 m_viewProj;
};

}  // namespace viewport
