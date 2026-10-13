#include "viewport/ps1view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <set>

#include <stb_image.h>

#include "editor/document.hh"
#include "editor/pick.hh"
#include "gl.h"
#include "scene.hh"
#include "splashpack.hh"
#include "unitymath.hh"

namespace viewport {

using namespace gl;

static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 normalize(Vec3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}

Mat4 Mat4::identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}

Mat4 Mat4::perspective(float fovy, float aspect, float zn, float zf) {
    Mat4 r;
    float f = 1.0f / std::tan(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1;
    r.m[14] = 2 * zf * zn / (zn - zf);
    return r;
}

Mat4 Mat4::ortho(float hw, float hh, float zn, float zf) {
    Mat4 r = identity();
    r.m[0] = 1 / hw;
    r.m[5] = 1 / hh;
    r.m[10] = -2 / (zf - zn);
    r.m[14] = -(zf + zn) / (zf - zn);
    return r;
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = identity();
    r.m[0] = s.x, r.m[4] = s.y, r.m[8] = s.z;
    r.m[1] = u.x, r.m[5] = u.y, r.m[9] = u.z;
    r.m[2] = -f.x, r.m[6] = -f.y, r.m[10] = -f.z;
    r.m[12] = -dot(s, eye), r.m[13] = -dot(u, eye), r.m[14] = dot(f, eye);
    return r;
}

// Column-major, matching GL.
Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += m[k * 4 + row] * o.m[c * 4 + k];
            r.m[c * 4 + row] = v;
        }
    return r;
}

static const char* kVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec3 aCol;
uniform mat4 uViewProj;
uniform vec2 uRes;
uniform int uClean;
noperspective out vec2 vUvAffine;
out vec2 vUvPersp;
out vec3 vCol;
out float vDepth;
void main() {
    vec4 clip = uViewProj * vec4(aPos, 1.0);
    if (uClean == 0) {
        // Snap to the integer pixel grid like the GTE's output does.
        vec2 ndc = clip.xy / clip.w;
        ndc = floor(ndc * uRes * 0.5 + 0.5) / (uRes * 0.5);
        clip.xy = ndc * clip.w;
    }
    gl_Position = clip;
    vUvAffine = aUv;
    vUvPersp = aUv;
    vCol = aCol;
    vDepth = clip.w;
}
)";

static const char* kFrag = R"(#version 330 core
noperspective in vec2 vUvAffine;
in vec2 vUvPersp;
in vec3 vCol;
in float vDepth;
uniform sampler2D uTex;
uniform int uClean;
uniform vec3 uFog;
uniform float uFogScale;  // grows with the orbit distance, so a big world is not all fog
out vec4 oColor;
const float kDither[16] = float[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);
void main() {
    vec2 uv = uClean == 0 ? vUvAffine : vUvPersp;
    vec3 c = texture(uTex, fract(uv)).rgb * vCol * 2.0;
    float fog = clamp((vDepth - 10.0 * uFogScale) / (22.0 * uFogScale), 0.0, 1.0);
    c = mix(c, uFog, fog * 0.85);
    if (uClean == 0) {
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        vec3 v = floor(clamp(c * 255.0 + kDither[p.y * 4 + p.x], 0.0, 255.0));
        c = floor(v / 8.0) * 8.0 / 255.0;
    }
    oColor = vec4(c, 1.0);
}
)";

// Screen-space sky: a fullscreen triangle whose colour is a vertical gradient
// driven by the camera's view ray, so it fills the whole background and tilts
// with the camera (looking up shows the top colour, down the horizon colour).
static const char* kSkyVert = R"(#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)";

static const char* kSkyFrag = R"(#version 330 core
in vec2 vNdc;
uniform vec3 uRight, uUp, uForward;  // camera basis (GL space)
uniform float uTanHalf, uAspect;     // tan(fovY/2) and width/height
uniform vec3 uSkyLo, uSkyMid, uSkyHi;  // horizon-down, horizon, top
uniform int uClean;
out vec4 oColor;
const float kDither[16] = float[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);
void main() {
    vec3 dir = normalize(uForward + uRight * (vNdc.x * uTanHalf * uAspect) + uUp * (vNdc.y * uTanHalf));
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);  // 0 = straight down, 0.5 = horizon, 1 = straight up
    vec3 c = t < 0.5 ? mix(uSkyLo, uSkyMid, t * 2.0) : mix(uSkyMid, uSkyHi, (t - 0.5) * 2.0);
    if (uClean == 0) {
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        vec3 v = floor(clamp(c * 255.0 + kDither[p.y * 4 + p.x], 0.0, 255.0));
        c = floor(v / 8.0) * 8.0 / 255.0;
    }
    oColor = vec4(c, 1.0);
}
)";

// Sky gradient colours (constants, as the world-space backdrop used before):
// bottom/horizon-down, horizon band, and top of the sky.
static const float kSkyLo[3] = {0.13f, 0.12f, 0.20f};
static const float kSkyMid[3] = {0.20f, 0.15f, 0.24f};
static const float kSkyHi[3] = {0.05f, 0.06f, 0.10f};

static unsigned compile(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

// Small deterministic hash for the fallback texture.
static float hash(int x, int y, int seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

// A 64x64 flagstone pattern, the fallback for a texture that fails to load.
static void makeFallback(std::vector<uint8_t>& px) {
    const int T = 64;
    px.assign((size_t)T * T * 4, 255);
    for (int y = 0; y < T; ++y)
        for (int x = 0; x < T; ++x) {
            bool grout = (x % 32) < 2 || (y % 32) < 2;
            float n = hash(x / 2, y / 2, 1) * 0.12f + hash(x / 32, y / 32, 7) * 0.1f;
            float base = grout ? 0.2f : 0.36f + n;
            size_t i = (size_t)(y * T + x) * 4;
            px[i] = (uint8_t)std::clamp(base * 255.0f, 0.0f, 255.0f);
            px[i + 1] = (uint8_t)std::clamp(base * 0.95f * 255.0f, 0.0f, 255.0f);
            px[i + 2] = (uint8_t)std::clamp(base * 0.9f * 255.0f, 0.0f, 255.0f);
        }
}

unsigned Ps1View::textureFor(const std::string& projectPath) {
    if (projectPath.empty()) return m_white;
    auto it = m_texCache.find(projectPath);
    if (it != m_texCache.end()) return it->second;

    unsigned id = m_fallback;
    std::string file = m_doc->resolve(projectPath).string();
    int w = 0, h = 0, n = 0;
    // Row 0 at the bottom, so UV v=0 is the bottom edge. Only for this load and
    // this thread: the global flag would flip every image export decodes later.
    stbi_set_flip_vertically_on_load_thread(1);
    unsigned char* data = stbi_load(file.c_str(), &w, &h, &n, 4);
    stbi_set_flip_vertically_on_load_thread(0);
    if (data) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        m_texSize[id] = {w, h};
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        stbi_image_free(data);
    } else {
        std::fprintf(stderr, "viewport: cannot load texture %s\n", file.c_str());
    }
    m_texCache[projectPath] = id;
    return id;
}

const splash::Scene* Ps1View::drawnScene() const {
    if (m_posed) return &*m_posed;
    return m_doc ? &m_doc->scene() : nullptr;
}

void Ps1View::rebuild() {
    m_verts.clear();
    m_batches.clear();
    m_pick.clear();
    m_posed.reset();
    if (!m_doc) {
        // No scene: the screen-space sky fills the background on its own.
        m_built = true;
        return;
    }

    // A preview poses a copy; picking indexes it in the same flatten order.
    if (!m_preview.empty()) {
        m_posed = m_doc->scene();
        editor::applyPreview(*m_posed, m_preview);
    }
    const splash::Scene& scene = m_posed ? *m_posed : m_doc->scene();
    std::vector<splash::FlatObject> flats = splash::flatten(scene);

    // Collect geometry into per-texture buckets.
    std::map<unsigned, std::vector<Vertex>> buckets;
    std::map<std::string, splash::Mesh>& meshCache = m_meshCache;
    if (m_doc->loadId() != m_meshCacheLoad || m_doc->assetsId() != m_meshCacheAssets) {
        meshCache.clear();
        // An import may have rewritten a texture under the same name.
        if (m_doc->assetsId() != m_meshCacheAssets)
            for (auto& [path, id] : m_texCache)
                if (id != m_white && id != m_fallback) glDeleteTextures(1, &id);
        if (m_doc->assetsId() != m_meshCacheAssets) m_texCache.clear();
        m_meshCacheLoad = m_doc->loadId();
        m_meshCacheAssets = m_doc->assetsId();
    }
    auto meshCacheKey = [](const std::string& p) { return p; };
    m_pick.clear();
    std::set<std::string> uvPosed;
    for (size_t fi = 0; fi < flats.size(); ++fi) {
        const splash::FlatObject& fo = flats[fi];
        // Inactive objects ship switched off and the runtime skips them until a
        // script turns them on; their lights still bake, so only meshes drop out.
        if (!fo.activeInHierarchy || !fo.object->active || !fo.object->mesh) continue;
        const splash::MeshComponent& mc = *fo.object->mesh;
        if (mc.mesh.empty() || mc.materials.empty()) continue;
        const splash::Mesh* mesh = nullptr;
        auto mit = meshCache.find(meshCacheKey(mc.mesh));
        if (mit != meshCache.end()) {
            mesh = &mit->second;
        } else {
            try {
                splash::Mesh m = splash::loadMesh(m_doc->resolve(mc.mesh));
                mesh = &(meshCache[meshCacheKey(mc.mesh)] = std::move(m));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "viewport: cannot load mesh %s: %s\n", mc.mesh.c_str(), e.what());
                continue;
            }
        }
        if (mesh->positions.empty() || mesh->normals.size() != mesh->positions.size()) continue;

        // World positions (Unity space); render space negates Z.
        std::vector<splash::Vec3> wp(mesh->positions.size());
        for (size_t i = 0; i < mesh->positions.size(); ++i) wp[i] = fo.localToWorld.point(mesh->positions[i]);
        const bool haveUv = mesh->uv.size() == mesh->positions.size();
        // A UV offset track: texels added on the mesh's uvOffsetMaterial, as
        // the runtime adds them to that material's triangles.
        std::optional<std::array<float, 2>> uvOff;
        if (auto pit = m_preview.objects.find(fo.object->name); pit != m_preview.objects.end() && pit->second.uvOffset &&
                                                              uvPosed.insert(fo.object->name).second)
            uvOff = pit->second.uvOffset;
        const std::vector<splash::Vec3> colors = splash::exportVertexColors(flats, fo, *mesh);

        for (size_t sub = 0; sub < mesh->submeshes.size(); ++sub) {
            const splash::Material& mat = mc.materials[std::min(sub, mc.materials.size() - 1)];
            unsigned tex = textureFor(mat.texture);
            bool textured = !mat.texture.empty();
            splash::Vec2 duv{0, 0};
            if (uvOff && textured && int(std::min(sub, mc.materials.size() - 1)) == mc.uvOffsetMaterial) {
                // PSX u = uv.x * (w - 1), v = (1 - uv.y) * (h - 1).
                auto ts = m_texSize.find(tex);
                if (ts != m_texSize.end() && ts->second.first > 1 && ts->second.second > 1)
                    duv = {(*uvOff)[0] / float(ts->second.first - 1), -(*uvOff)[1] / float(ts->second.second - 1)};
            }
            std::vector<Vertex>& bucket = buckets[tex];
            const std::vector<int>& tri = mesh->submeshes[sub];
            // The same triangles in Unity world space for picking, tagged with the object.
            const size_t firstTri = m_pick.positions.size() / 3;
            for (size_t k = 0; k + 2 < tri.size(); k += 3)
                for (size_t j = 0; j < 3; ++j) m_pick.positions.push_back(wp[(size_t)tri[k + j]]);
            m_pick.ranges.push_back({(int)fi, firstTri, m_pick.positions.size() / 3 - firstTri});
            for (int idx : tri) {
                size_t i = (size_t)idx;
                // The exporter's colour, so this view lights the scene the way the game does.
                Vec3 col = i < colors.size() ? Vec3{colors[i].x, colors[i].y, colors[i].z} : Vec3{0.5f, 0.5f, 0.5f};
                if (!textured) col = {col.x * mat.color[0], col.y * mat.color[1], col.z * mat.color[2]};
                splash::Vec2 uv = haveUv ? mesh->uv[i] : splash::Vec2{};
                uv = {uv.x + duv.x, uv.y + duv.y};
                bucket.push_back({wp[i].x, wp[i].y, -wp[i].z, uv.x, uv.y, col.x, col.y, col.z});
            }
        }
    }

    // One batch per texture; the sky is drawn separately in screen space.
    for (auto& [tex, verts] : buckets) {
        if (verts.empty()) continue;
        int start = (int)m_verts.size();
        m_verts.insert(m_verts.end(), verts.begin(), verts.end());
        m_batches.push_back({tex, start, (int)verts.size()});
    }

    // Frame the scene bounds from the current eye direction so any scene fits.
    splash::Bounds bounds;
    bool any = false;
    for (const Batch& b : m_batches) {
        for (int i = b.start; i < b.start + b.count; ++i) {
            Vec3 gp{m_verts[(size_t)i].px, m_verts[(size_t)i].py, m_verts[(size_t)i].pz};
            if (!any) {
                bounds = splash::Bounds({gp.x, gp.y, gp.z}, {0, 0, 0});
                any = true;
            } else {
                bounds.encapsulate({gp.x, gp.y, gp.z});
            }
        }
    }
    m_haveSceneBounds = any;
    if (any) m_sceneBounds = bounds;
    // Frame once per loaded scene; edits must not move the camera.
    if (any && m_framedLoad != m_doc->loadId()) {
        m_framedLoad = m_doc->loadId();
        Vec3 center{bounds.center.x, bounds.center.y, bounds.center.z};
        float radius = std::sqrt(splash::sqrMagnitude(bounds.extents));
        radius = std::max(radius, 1.0f);
        // Keep the old courtyard's eye direction. The distance is a fixed
        // multiple of the bounding radius: it crops a large flat floor the way
        // the original tuned camera did, while scaling to frame other scenes.
        Vec3 dir = normalize(Vec3{7.6f, 5.2f, 9.2f} - Vec3{-0.9f, 0.9f, -2.0f});
        float dist = radius * 1.15f;
        m_target = center;
        m_eye = center + dir * dist;
    }

    m_built = true;
}

bool Ps1View::init() {
    unsigned vs = compile(GL_VERTEX_SHADER, kVert), fs = compile(GL_FRAGMENT_SHADER, kFrag);
    m_prog = glCreateProgram();
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glLinkProgram(m_prog);
    int ok = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok) return false;

    unsigned svs = compile(GL_VERTEX_SHADER, kSkyVert), sfs = compile(GL_FRAGMENT_SHADER, kSkyFrag);
    m_skyProg = glCreateProgram();
    glAttachShader(m_skyProg, svs);
    glAttachShader(m_skyProg, sfs);
    glLinkProgram(m_skyProg);
    glGetProgramiv(m_skyProg, GL_LINK_STATUS, &ok);
    if (!ok) return false;

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, px));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, r));
    for (int i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    glBindVertexArray(0);

    const uint8_t whitePx[4] = {255, 255, 255, 255};
    glGenTextures(1, &m_white);
    glBindTexture(GL_TEXTURE_2D, m_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePx);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    std::vector<uint8_t> fb;
    makeFallback(fb);
    glGenTextures(1, &m_fallback);
    glBindTexture(GL_TEXTURE_2D, m_fallback);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, fb.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_color);
    glGenRenderbuffers(1, &m_depth);
    return true;
}

unsigned Ps1View::render(int panelW, int panelH, int lines) {
    if (!m_built || m_previewDirty || (m_doc && m_doc->revision() != m_builtRevision)) {
        if (m_doc) m_builtRevision = m_doc->revision();
        m_previewDirty = false;
        rebuild();
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(Vertex), m_verts.data(), GL_STATIC_DRAW);
        m_vboCap = m_verts.size();
    }

    int h = clean ? panelH : lines;
    int w = clean ? panelW : std::max(1, (int)std::round((float)lines * panelW / panelH));
    if (w != m_fboW || h != m_fboH) {
        m_fboW = w, m_fboH = h;
        glBindTexture(GL_TEXTURE_2D, m_color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
    }

    Vec3 bRight, bUp, bFwd;
    basis(&bRight, &bUp, &bFwd);
    const float aspect = (float)panelW / panelH;
    if (m_ortho) {
        float hh = distance() * std::tan(kFovY * 0.5f);
        m_viewProj = Mat4::ortho(hh * aspect, hh, 0.1f, 2 * kOrthoBack) * Mat4::lookAt(eye(), m_target, bUp);
    } else {
        m_viewProj = Mat4::perspective(kFovY, aspect, 0.1f * viewScale(), 200.0f * viewScale()) * Mat4::lookAt(m_eye, m_target, bUp);
    }

    const float fog[3] = {0.10f, 0.11f, 0.17f};
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, w, h);
    glClearColor(fog[0], fog[1], fog[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    // Screen-space sky first, filling the whole background. Depth test off so it
    // covers every pixel and writes no depth, letting the scene draw over it.
    Vec3 sRight, sUp, sForward;
    basis(&sRight, &sUp, &sForward);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_skyProg);
    glUniform3f(glGetUniformLocation(m_skyProg, "uRight"), sRight.x, sRight.y, sRight.z);
    glUniform3f(glGetUniformLocation(m_skyProg, "uUp"), sUp.x, sUp.y, sUp.z);
    glUniform3f(glGetUniformLocation(m_skyProg, "uForward"), sForward.x, sForward.y, sForward.z);
    glUniform1f(glGetUniformLocation(m_skyProg, "uTanHalf"), std::tan(kFovY * 0.5f));
    glUniform1f(glGetUniformLocation(m_skyProg, "uAspect"), (float)panelW / panelH);
    glUniform3f(glGetUniformLocation(m_skyProg, "uSkyLo"), kSkyLo[0], kSkyLo[1], kSkyLo[2]);
    glUniform3f(glGetUniformLocation(m_skyProg, "uSkyMid"), kSkyMid[0], kSkyMid[1], kSkyMid[2]);
    glUniform3f(glGetUniformLocation(m_skyProg, "uSkyHi"), kSkyHi[0], kSkyHi[1], kSkyHi[2]);
    glUniform1i(glGetUniformLocation(m_skyProg, "uClean"), clean ? 1 : 0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_DEPTH_TEST);
    glUseProgram(m_prog);
    glUniformMatrix4fv(glGetUniformLocation(m_prog, "uViewProj"), 1, GL_FALSE, m_viewProj.m);
    glUniform2f(glGetUniformLocation(m_prog, "uRes"), (float)w, (float)h);
    glUniform1i(glGetUniformLocation(m_prog, "uClean"), clean ? 1 : 0);
    glUniform3f(glGetUniformLocation(m_prog, "uFog"), fog[0], fog[1], fog[2]);
    glUniform1f(glGetUniformLocation(m_prog, "uFogScale"), viewScale());
    glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);
    glActiveTexture_(GL_TEXTURE0);
    glBindVertexArray(m_vao);
    for (const Batch& b : m_batches) {
        glBindTexture(GL_TEXTURE_2D, b.tex);
        glDrawArrays(GL_TRIANGLES, b.start, b.count);
    }
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return m_color;
}

void Ps1View::basis(Vec3* right, Vec3* up, Vec3* forward) const {
    Vec3 f = normalize(m_target - m_eye);
    // Straight down (top view) or up has no yaw; screen-up is then Unity +Z
    // (GL -Z) looking down, like Unity's Top view.
    Vec3 worldUp = std::fabs(f.y) > 0.9999f ? Vec3{0, 0, f.y < 0 ? -1.0f : 1.0f} : Vec3{0, 1, 0};
    Vec3 s = normalize(cross(f, worldUp));
    if (right) *right = s;
    if (up) *up = cross(s, f);
    if (forward) *forward = f;
}

Vec3 Ps1View::eye() const {
    if (!m_ortho) return m_eye;
    return m_target - normalize(m_target - m_eye) * kOrthoBack;
}

float Ps1View::worldPerPixel(float depth, float panelH) const {
    return 2 * (m_ortho ? distance() : depth) * std::tan(kFovY * 0.5f) / std::max(panelH, 1.0f);
}

float Ps1View::distance() const {
    Vec3 d = m_eye - m_target;
    return std::sqrt(dot(d, d));
}

void Ps1View::ray(ImVec2 screen, ImVec2 mn, ImVec2 sz, Vec3* origin, Vec3* dir) const {
    Vec3 s, u, f;
    basis(&s, &u, &f);
    float nx = (screen.x - mn.x) / sz.x * 2 - 1;
    float ny = 1 - (screen.y - mn.y) / sz.y * 2;
    float th = std::tan(kFovY * 0.5f);
    if (m_ortho) {
        float hh = distance() * th;
        *origin = eye() + s * (nx * hh * sz.x / sz.y) + u * (ny * hh);
        *dir = f;
        return;
    }
    *origin = m_eye;
    *dir = normalize(f + s * (nx * th * sz.x / sz.y) + u * (ny * th));
}

void Ps1View::orbit(float dx, float dy) {
    Vec3 off = m_eye - m_target;
    float r = std::sqrt(dot(off, off));
    if (r <= 0) return;
    float yaw = std::atan2(off.x, off.z);
    float pitch = std::asin(std::clamp(off.y / r, -1.0f, 1.0f));
    const float k = 0.008f;  // radians per pixel
    yaw -= dx * k;
    pitch = std::clamp(pitch + dy * k, -1.55f, 1.55f);
    m_eye = m_target + Vec3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * r;
}

void Ps1View::pan(float dx, float dy, float panelH) {
    Vec3 s, u;
    basis(&s, &u, nullptr);
    float perPx = worldPerPixel(distance(), panelH);
    Vec3 move = s * (-dx * perPx) + u * (dy * perPx);
    m_eye = m_eye + move;
    m_target = m_target + move;
}

void Ps1View::dolly(float steps) {
    Vec3 off = m_eye - m_target;
    float r = std::sqrt(dot(off, off));
    if (r <= 0) return;
    float nr = std::clamp(r * std::pow(0.85f, steps), 0.25f, 400.0f);
    m_eye = m_target + off * (nr / r);
}

void Ps1View::frame(Vec3 center, float radius) {
    Vec3 dir = normalize(m_eye - m_target);
    if (dot(dir, dir) == 0) dir = {0, 0, 1};
    float r = std::max(radius, 0.1f) / std::sin(kFovY * 0.5f) * 1.3f;
    m_target = center;
    m_eye = center + dir * std::max(r, 0.5f);
}

void Ps1View::lookAround(float dx, float dy) {
    Vec3 f = normalize(m_target - m_eye);
    float r = distance();
    if (r <= 0) return;
    float yaw = std::atan2(f.x, -f.z);
    float pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
    const float k = 0.004f;  // radians per pixel
    yaw += dx * k;
    pitch = std::clamp(pitch - dy * k, -1.55f, 1.55f);
    Vec3 nf{std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
    m_target = m_eye + nf * r;
}

void Ps1View::fly(Vec3 move) {
    Vec3 s, u, f;
    basis(&s, &u, &f);
    translate(s * move.x + u * move.y + f * move.z);
}

void Ps1View::translate(Vec3 d) {
    m_eye = m_eye + d;
    m_target = m_target + d;
}

void Ps1View::lookAlong(Vec3 forward) {
    float r = distance();
    m_eye = m_target - normalize(forward) * (r > 0 ? r : 1.0f);
}

std::optional<int> Ps1View::pick(ImVec2 screen, ImVec2 mn, ImVec2 sz) const {
    Vec3 o, d;
    ray(screen, mn, sz, &o, &d);
    // GL to Unity: negate Z.
    std::optional<editor::PickHit> hit = editor::pickNearest({{o.x, o.y, -o.z}, {d.x, d.y, -d.z}}, m_pick);
    if (!hit) return std::nullopt;
    return hit->object;
}

std::optional<Vec3> Ps1View::pickPoint(ImVec2 screen, ImVec2 mn, ImVec2 sz) const {
    Vec3 o, d;
    ray(screen, mn, sz, &o, &d);
    std::optional<editor::PickHit> hit = editor::pickNearest({{o.x, o.y, -o.z}, {d.x, d.y, -d.z}}, m_pick);
    if (!hit) return std::nullopt;
    return Vec3{o.x + d.x * hit->t, o.y + d.y * hit->t, o.z + d.z * hit->t};
}

bool Ps1View::sceneBounds(Vec3* lo, Vec3* hi) const {
    if (!m_haveSceneBounds) return false;
    splash::Vec3 a = m_sceneBounds.min(), b = m_sceneBounds.max();
    *lo = {a.x, a.y, a.z};
    *hi = {b.x, b.y, b.z};
    return true;
}

bool Ps1View::project(Vec3 p, ImVec2 mn, ImVec2 sz, ImVec2* out) const {
    const float* m = m_viewProj.m;
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (w <= 0.01f) return false;
    out->x = mn.x + (x / w * 0.5f + 0.5f) * sz.x;
    out->y = mn.y + (1.0f - (y / w * 0.5f + 0.5f)) * sz.y;
    return true;
}

}  // namespace viewport
