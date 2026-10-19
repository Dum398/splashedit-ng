#include "splashpack.hh"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

#include "audio.hh"
#include "binwriter.hh"
#include "blocks.hh"
#include "budget.hh"
#include "bvh.hh"
#include "rooms.hh"
#include "font.hh"
#include "luacompile.hh"
#include "navregion.hh"
#include "skin.hh"
#include "texture.hh"
#include "vrampacker.hh"
#include "worldstream.hh"

namespace splash {

namespace fs = std::filesystem;

AgentRecord agentRecord(const AgentComponent& a, uint16_t gameObjectIndex, int skinClips, float gte) {
    AgentRecord r;
    uint8_t* b = r.bytes.data();
    auto u16 = [&](size_t at, int v) {
        b[at] = uint8_t(v & 0xff);
        b[at + 1] = uint8_t((v >> 8) & 0xff);
    };
    auto fpU16 = [&](float units) { return clampv(roundToInt(units / gte * 4096.f), 0, 65535); };
    const size_t n = std::min<size_t>(a.waypoints.size(), size_t(kAgentMaxWaypoints));
    u16(0, gameObjectIndex);
    b[2] = uint8_t((a.startEnabled ? 0x01 : 0) | (a.vision && a.visionRange > 0 ? 0x02 : 0) | (a.hearingRange > 0 ? 0x04 : 0) |
                   (n > 0 ? 0x08 : 0));
    b[3] = uint8_t(n);
    u16(4, clampv(roundToInt(a.moveSpeed / 30.f / gte * 4096.f), 0, 65535));
    u16(6, fpU16(a.stopDistance));
    u16(8, fpU16(a.visionRange));
    // Half the cone, as a cosine: the engine compares cos(angle to target) with it.
    const float half = clampv(a.fov, 0.f, 360.f) * 0.5f * 3.14159265358979f / 180.f;
    u16(10, clampv(roundToInt(std::cos(half) * 4096.f), -4096, 4096));
    u16(12, fpU16(a.hearingRange));
    u16(14, clampv(a.alertTimeout, 0, 65535));
    for (int s = 0; s < kAgentStates; s++) {
        const int c = a.stateClips[size_t(s)];
        b[16 + s] = c >= 0 && c < skinClips ? uint8_t(c) : 0xFF;
    }
    b[24] = uint8_t(clampv(a.visionRegionDepth, 0, 8));
    for (size_t i = 0; i < n; i++) {
        const Vec3& p = a.waypoints[i];
        r.waypoints.push_back({toWorldFixed12(p.x / gte), toWorldFixed12(-p.y / gte), toWorldFixed12(p.z / gte)});
    }
    return r;
}

std::vector<std::string> agentWarnings(const Object& o, const std::vector<AgentClipLoop>& loaded) {
    std::vector<std::string> out;
    if (!o.agent) return out;
    const AgentComponent& a = *o.agent;
    const std::string where = o.name + ": agent ";
    if (a.waypoints.size() > size_t(kAgentMaxWaypoints))
        out.push_back(where + "has " + std::to_string(a.waypoints.size()) + " waypoints and the PS1 keeps 8; the rest are left out");
    if (!(a.fov > 0 && a.fov <= 360))
        out.push_back(where + "field of view must be more than 0 and at most 360 degrees; it is written as " +
                      std::to_string(int(std::lround(clampv(a.fov, 0.f, 360.f)))));
    const int clips = o.skin ? int(o.skin->clips.size()) : 0;
    for (int s = 0; s < kAgentStates; s++) {
        const int c = a.stateClips[size_t(s)];
        if (c < 0) continue;
        if (!o.skin)
            out.push_back(where + "plays a clip in state " + kAgentStateNames[s] + " but the object has no skin; it plays nothing");
        else if (c >= clips)
            out.push_back(where + "state " + kAgentStateNames[s] + " plays clip " + std::to_string(c + 1) + " of a skin with " +
                          std::to_string(clips) + "; it plays nothing");
        else if (agentStateHolds(s) && c < int(loaded.size()) && !loaded[size_t(c)].loop)
            out.push_back(where + "state " + kAgentStateNames[s] + " plays clip " + loaded[size_t(c)].name +
                          ", which plays once; it stops on its last frame");
    }
    return out;
}

uint32_t hashSceneId(const std::string& id) {
    if (id.empty()) return 0;
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t unit) {
        h ^= unit & 0xffu;
        h *= 16777619u;
    };
    // FNV-1a over the low byte of each UTF-16 unit, as Unity hashes its C#
    // string: decode the UTF-8, split code points above U+FFFF into a
    // surrogate pair. A malformed byte counts as one unit of its own value.
    for (size_t i = 0; i < id.size();) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len && i + len <= id.size();
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1fu : len == 3 ? c & 0x0fu : c & 0x07u;
        for (size_t k = 1; ok && k < len; k++) {
            const unsigned char cc = static_cast<unsigned char>(id[i + k]);
            if ((cc >> 6) != 2) ok = false;
            cp = (cp << 6) | (cc & 0x3fu);
        }
        if (!ok) {
            mix(c);
            i++;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            mix(0xd800u + (cp >> 10));
            mix(0xdc00u + (cp & 0x3ffu));
        } else {
            mix(cp);
        }
        i += len;
    }
    // 0 means "not authored" to the engine, which then derives its own id.
    return h == 0 ? 1 : h;
}

namespace {

struct PsxVert {
    int16_t vx, vy, vz, nx, ny, nz;
    uint8_t u, v, r, g, b;
};

struct PsxTri {
    PsxVert v[3];
    int textureIndex;  // -1 = untextured
};

struct Color {
    float r = 0, g = 0, b = 0;
};

struct SceneLight {
    LightKind kind;
    Vec3 position, forward;
    float r, g, b, intensity, spotAngle, innerSpotAngle;
};

struct ExpObject {
    const FlatObject* flat;
    const Object* obj;
    const MeshComponent* mc;
    const Mesh* mesh;
    std::vector<PsxTexture*> textures;  // per-material list as built, before packing
    std::vector<PsxTri> tris;
    std::vector<PsxTexture*> finalTextures;  // deduplicated, after packing
    std::vector<int> vertexBone;             // skinned only: bone per mesh vertex
    std::vector<uint8_t> triBones;           // skinned only: bone per Tri vertex, Tri order
    std::vector<Vec3> triPositions;          // mesh-space corners of each Tri, Tri order (BVH input)
    bool dynamicLit = false;                 // runtime point lights are applied on the console
    bool dynamicLitSmooth = false;           // per vertex instead of per triangle
    std::optional<Bounds> pieceBounds{};     // streaming cut this object: mesh-space bounds of its part
};

// A runtime point light (light table, v24). Scene order is table order, which
// decides which four light a crowded mesh (MAX_LIGHTS_PER_MESH).
struct RuntimeLight {
    const FlatObject* flat;
    const LightComponent* light;
};
constexpr size_t kMaxSceneLights = 16;   // MAX_SCENE_LIGHTS in psxsplash lightmath.hh
constexpr size_t kMaxLightsPerMesh = 4;  // MAX_LIGHTS_PER_MESH
constexpr size_t kSpuStart = 0x1010;      // SPU_RAM_START in psxsplash audiomanager.hh

// String.Substring(0, 24) counts UTF-16 code units; names are stored as UTF-8.
std::string truncateUtf16(const std::string& s, size_t units) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        unsigned char c = uint8_t(s[i]);
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        size_t u = len == 4 ? 2 : 1;
        if (n + u > units) break;
        n += u;
        i += len;
    }
    return s.substr(0, i);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// PSXLightingBaker.ComputeLighting
Color bake(Vec3 vertex, Vec3 normal, const std::vector<SceneLight>& lights) {
    Color fin;
    for (const SceneLight& l : lights) {
        Color c;
        auto lit = [&](float k) {
            // light.color * light.intensity * ... evaluated left to right
            c = {l.r * l.intensity, l.g * l.intensity, l.b * l.intensity};
            return k;
        };
        if (l.kind == LightKind::Directional) {
            Vec3 dir = l.forward * -1.f;
            float ndl = mfMax(0.f, dot(normal, dir));
            lit(0);
            c = {c.r * ndl, c.g * ndl, c.b * ndl};
        } else if (l.kind == LightKind::Point) {
            Vec3 dir = l.position - vertex;
            float dist = magnitude(dir);
            dir = normalized(dir);
            float ndl = mfMax(0.f, dot(normal, dir));
            float att = 1.0f / mfMax(dist * dist, 0.0001f);
            lit(0);
            c = {c.r * ndl, c.g * ndl, c.b * ndl};
            c = {c.r * att, c.g * att, c.b * att};
        } else {
            Vec3 L = l.position - vertex;
            float dist = magnitude(L);
            L = L / dist;
            float ndl = mfMax(0.f, dot(normal, L));
            float att = 1.0f / mfMax(dist * dist, 0.0001f);
            constexpr float deg2rad = 0.0174532924f;  // Mathf.Deg2Rad
            float outer = (l.spotAngle * 0.5f) * deg2rad;
            float inner = outer * 0.8f;
            if (l.innerSpotAngle > 0) inner = (l.innerSpotAngle * 0.5f) * deg2rad;
            float cosOuter = float(std::cos(double(outer)));
            float cosInner = float(std::cos(double(inner)));
            float cosAngle = dot(L, l.forward * -1.f);
            if (cosAngle >= cosOuter) {
                float sf = clampv((cosAngle - cosOuter) / (cosInner - cosOuter), 0.f, 1.f);
                sf = float(std::pow(double(sf), 4.0));
                lit(0);
                c = {c.r * ndl, c.g * ndl, c.b * ndl};
                c = {c.r * att, c.g * att, c.b * att};
                c = {c.r * sf, c.g * sf, c.b * sf};
            }
        }
        fin.r += c.r;
        fin.g += c.g;
        fin.b += c.b;
    }
    fin.r = clampv(fin.r, 0.f, 0.8f);
    fin.g = clampv(fin.g, 0.f, 0.8f);
    fin.b = clampv(fin.b, 0.f, 0.8f);
    return fin;
}

// PSXMesh.RecalculateSmoothNormals: average over vertices with identical
// positions (exact float equality, as Dictionary<Vector3> keys).
std::vector<Vec3> smoothNormals(const Mesh& m) {
    struct K {
        float x, y, z;
        bool operator<(const K& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::map<K, std::vector<size_t>> groups;
    for (size_t i = 0; i < m.positions.size(); i++)
        groups[{m.positions[i].x, m.positions[i].y, m.positions[i].z}].push_back(i);
    std::vector<Vec3> out(m.positions.size());
    for (auto& [k, idx] : groups) {
        Vec3 s{};
        for (size_t i : idx) s = s + m.normals[i];
        s = normalized(s);
        for (size_t i : idx) out[i] = s;
    }
    return out;
}

SceneLight sceneLight(const FlatObject& fo) {
    const LightComponent& l = *fo.object->light;
    return {l.kind,       fo.worldPosition, rotate(fo.worldRotation, {0, 0, 1}), l.color[0], l.color[1], l.color[2],
            l.intensity, l.spotAngle,      l.innerSpotAngle};
}

// World-space positions and lighting normals, then one colour per vertex as
// the mesh component's colour mode picks it.
std::vector<Color> meshColors(const FlatObject& fo, const MeshComponent& mc, const Mesh& m,
                              const std::vector<SceneLight>& lights) {
    std::vector<Color> out(m.positions.size());
    bool hasColors = mc.vertexColors == VertexColorMode::Mesh && m.colors.size() == m.positions.size();
    std::vector<Vec3> lightNormals;
    if (mc.vertexColors != VertexColorMode::Flat && mc.vertexColors != VertexColorMode::Mesh)
        lightNormals = mc.smoothNormals ? smoothNormals(m) : m.normals;
    for (size_t i = 0; i < m.positions.size(); i++) {
        switch (mc.vertexColors) {
            case VertexColorMode::Flat:
                out[i] = {mc.flatColor[0] / 255.f, mc.flatColor[1] / 255.f, mc.flatColor[2] / 255.f};
                break;
            case VertexColorMode::Mesh:
                out[i] = hasColors ? Color{m.colors[i][0], m.colors[i][1], m.colors[i][2]} : Color{0.5f, 0.5f, 0.5f};
                break;
            default:
                out[i] = bake(fo.localToWorld.point(m.positions[i]),
                              normalized(rotate(fo.worldRotation, lightNormals[i])), lights);
                break;
        }
    }
    return out;
}

PsxVert toPsxVertex(Vec3 vertex, float gte, Vec3 normal, Vec2 uv, int width, int height, Color c) {
    PsxVert p;
    p.vx = toPsxCoord(vertex.x, gte);
    p.vy = toPsxCoord(-vertex.y, gte);
    p.vz = toPsxCoord(vertex.z, gte);
    p.nx = toPsxCoord(normal.x);
    p.ny = toPsxCoord(-normal.y);
    p.nz = toPsxCoord(normal.z);
    p.u = uint8_t(clampv(uv.x * float(width - 1), 0.f, 255.f));
    p.v = uint8_t(clampv((1.0f - uv.y) * float(height - 1), 0.f, 255.f));
    p.r = colorToPsx(c.r);
    p.g = colorToPsx(c.g);
    p.b = colorToPsx(c.b);
    return p;
}

// A triangle corner with everything the exporter interpolates when it cuts a
// face: mesh-space position, normal, baked colour, UV, and barycentric weights
// against the authored triangle (the skinned bone goes with the heaviest one).
struct Corner {
    Vec3 pos, normal;
    Color color;
    Vec2 uv;
    float w[3];
};

Corner lerpCorner(const Corner& a, const Corner& b, float t) {
    Corner r;
    r.pos = a.pos + (b.pos - a.pos) * t;
    r.normal = normalized(a.normal + (b.normal - a.normal) * t);
    r.color = {a.color.r + (b.color.r - a.color.r) * t, a.color.g + (b.color.g - a.color.g) * t,
               a.color.b + (b.color.b - a.color.b) * t};
    r.uv = {a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t};
    for (int j = 0; j < 3; j++) r.w[j] = a.w[j] + (b.w[j] - a.w[j]) * t;
    return r;
}

// Keep the part of a convex polygon where uv[axis] lies in [lo, hi].
std::vector<Corner> clipToBand(const std::vector<Corner>& in, int axis, float lo, float hi) {
    auto coord = [axis](const Corner& c) { return axis == 0 ? c.uv.x : c.uv.y; };
    std::vector<Corner> poly = in;
    for (int side = 0; side < 2 && poly.size() >= 3; side++) {
        auto inside = [&](const Corner& c) { return side == 0 ? coord(c) >= lo : coord(c) <= hi; };
        float edge = side == 0 ? lo : hi;
        std::vector<Corner> out;
        for (size_t i = 0; i < poly.size(); i++) {
            const Corner& p = poly[i];
            const Corner& q = poly[(i + 1) % poly.size()];
            bool pin = inside(p), qin = inside(q);
            if (pin) out.push_back(p);
            if (pin != qin) out.push_back(lerpCorner(p, q, (edge - coord(p)) / (coord(q) - coord(p))));
        }
        poly = std::move(out);
    }
    return poly;
}

// The PS1 cannot repeat a texture across a polygon, so a face whose UVs span
// more than one copy of the texture is cut along the integer U and V lines.
// Each piece then lies inside one copy and is shifted back into [0,1]. A face
// that already fits one copy is only shifted (not at all when it is the [0,1]
// copy) and false is returned: emit t itself instead of pieces.
bool tileTriangle(std::array<Corner, 3>& t, std::vector<std::array<Corner, 3>>& pieces) {
    constexpr float eps = 1e-4f;
    float lo[2], hi[2];
    for (int ax = 0; ax < 2; ax++) {
        auto c = [ax](const Corner& k) { return ax == 0 ? k.uv.x : k.uv.y; };
        lo[ax] = std::floor(std::min({c(t[0]), c(t[1]), c(t[2])}) + eps);
        hi[ax] = std::ceil(std::max({c(t[0]), c(t[1]), c(t[2])}) - eps);
        if (hi[ax] <= lo[ax]) hi[ax] = lo[ax] + 1;
    }
    if (hi[0] - lo[0] == 1 && hi[1] - lo[1] == 1) {
        if (lo[0] == 0 && lo[1] == 0) return false;
        for (Corner& k : t) k.uv = {k.uv.x - lo[0], k.uv.y - lo[1]};
        return false;
    }
    std::vector<Corner> whole(t.begin(), t.end());
    for (float u = lo[0]; u < hi[0]; u++) {
        std::vector<Corner> col = clipToBand(whole, 0, u, u + 1);
        if (col.size() < 3) continue;
        for (float v = lo[1]; v < hi[1]; v++) {
            std::vector<Corner> cell = clipToBand(col, 1, v, v + 1);
            if (cell.size() < 3) continue;
            for (Corner& k : cell) k.uv = {k.uv.x - u, k.uv.y - v};
            for (size_t j = 1; j + 1 < cell.size(); j++) {
                Vec3 n = cross(cell[j].pos - cell[0].pos, cell[j + 1].pos - cell[0].pos);
                if (sqrMagnitude(n) <= 0) continue;  // sliver along a cut line
                pieces.push_back({cell[0], cell[j], cell[j + 1]});
            }
        }
    }
    return true;
}

// PSXMesh.BuildFromMesh
void buildTris(ExpObject& e, float gte, const std::vector<SceneLight>& lights, ExportResult& res) {
    const Mesh& m = *e.mesh;
    const MeshComponent& mc = *e.mc;
    const FlatObject& fo = *e.flat;
    if (m.positions.empty()) return;
    if (m.normals.empty()) {
        res.errors.push_back(e.obj->name + ": mesh " + mc.mesh + " has no normals");
        return;
    }
    if (mc.materials.empty()) {
        res.errors.push_back(e.obj->name + ": mesh component has no materials");
        return;
    }
    const std::vector<Color> colors = meshColors(fo, mc, m, lights);
    std::vector<Vec2> uvs = m.uv;
    if (uvs.empty()) uvs.assign(m.positions.size(), {});

    for (size_t sub = 0; sub < m.submeshes.size(); sub++) {
        size_t mi = std::min(sub, mc.materials.size() - 1);
        const Material& mat = mc.materials[mi];
        int texIndex = -1;
        if (!mat.texture.empty())
            for (size_t i = 0; i < e.textures.size(); i++)
                if (e.textures[i]->source == mat.texture) {
                    texIndex = int(i);
                    break;
                }
        auto convert = [&](const Corner& k) {
            Vec3 v = scale(k.pos, fo.lossyScale);
            Color c = k.color;
            if (texIndex == -1) {
                c = {c.r * mat.color[0], c.g * mat.color[1], c.b * mat.color[2]};
                return toPsxVertex(v, gte, k.normal, {}, 0, 0, c);
            }
            const PsxTexture* t = e.textures[size_t(texIndex)];
            return toPsxVertex(v, gte, k.normal, k.uv, t->width, t->height, c);
        };
        auto corner = [&](int idx, int slot) {
            size_t i = size_t(idx);
            Corner k{m.positions[i], m.normals[i], colors[i], uvs[i], {}};
            k.w[slot] = 1;
            return k;
        };
        const auto& tri = m.submeshes[sub];
        for (size_t i = 0; i + 2 < tri.size(); i += 3) {
            int a = tri[i], b = tri[i + 1], c = tri[i + 2];
            Vec3 p0 = m.positions[size_t(a)];
            Vec3 fn = normalized(cross(m.positions[size_t(b)] - p0, m.positions[size_t(c)] - p0));
            if (dot(fn, m.normals[size_t(a)]) < 0) std::swap(b, c);
            int ids[3] = {a, b, c};
            std::array<Corner, 3> k{corner(a, 0), corner(b, 1), corner(c, 2)};
            auto bonesFor = [&](const Corner& q) {
                int best = 0;
                for (int j = 1; j < 3; j++)
                    if (q.w[j] > q.w[best]) best = j;
                return uint8_t(e.vertexBone[size_t(ids[best])]);
            };
            std::vector<std::array<Corner, 3>> pieces;
            if (texIndex == -1 || !tileTriangle(k, pieces)) {
                // One texture copy covers it: emit it as authored, corners in mesh order for the BVH.
                e.tris.push_back({{convert(k[0]), convert(k[1]), convert(k[2])}, texIndex});
                for (int v : {tri[i], tri[i + 1], tri[i + 2]}) e.triPositions.push_back(m.positions[size_t(v)]);
                if (!e.vertexBone.empty())
                    for (const Corner& q : k) e.triBones.push_back(bonesFor(q));
                continue;
            }
            for (const auto& pc : pieces) {
                e.tris.push_back({{convert(pc[0]), convert(pc[1]), convert(pc[2])}, texIndex});
                for (const Corner& q : pc) e.triPositions.push_back(q.pos);
                if (!e.vertexBone.empty())
                    for (const Corner& q : pc) e.triBones.push_back(bonesFor(q));
            }
        }
    }
    if (e.tris.size() > 65535)
        res.errors.push_back(e.obj->name + ": " + std::to_string(e.tris.size()) +
                             " triangles after splitting repeating textures, more than 65535");
}

// Same test the engine runs: does the light's range reach the mesh's world AABB?
bool lightReaches(const RuntimeLight& rl, const Mat34& m, const Bounds& local) {
    float range = rl.light->range;
    if (range <= 0) return false;
    Vec3 ext = local.extents, center = local.center;
    Vec3 mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
    for (int i = 0; i < 8; i++) {
        Vec3 world = m.point(center + Vec3{(i & 1) ? ext.x : -ext.x, (i & 2) ? ext.y : -ext.y, (i & 4) ? ext.z : -ext.z});
        mn = vmin(mn, world);
        mx = vmax(mx, world);
    }
    Vec3 p = rl.flat->worldPosition;
    Vec3 c = vmax(mn, vmin(mx, p));
    Vec3 d{c.x - p.x, c.y - p.y, c.z - p.z};
    return d.x * d.x + d.y * d.y + d.z * d.z < range * range;
}

void writeWorldAabb(BinWriter& w, const Mat34& m, const Bounds& local, float gte) {
    Vec3 ext = local.extents, center = local.center;
    Vec3 mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
    for (int i = 0; i < 8; i++) {
        Vec3 corner = center + Vec3{(i & 1) ? ext.x : -ext.x, (i & 2) ? ext.y : -ext.y, (i & 4) ? ext.z : -ext.z};
        Vec3 world = m.point(corner);
        mn = vmin(mn, world);
        mx = vmax(mx, world);
    }
    w.i32(toWorldFixed12(mn.x / gte));
    w.i32(toWorldFixed12(-mx.y / gte));
    w.i32(toWorldFixed12(mn.z / gte));
    w.i32(toWorldFixed12(mx.x / gte));
    w.i32(toWorldFixed12(-mn.y / gte));
    w.i32(toWorldFixed12(mx.z / gte));
}

// PSXTrig.ConvertRotationToPSXMatrix
void writeRotation(BinWriter& w, Quat q) {
    float x = q.x, y = q.y, z = q.z, qw = q.w;
    float m00 = 1.f - 2.f * (y * y + z * z);
    float m01 = 2.f * (x * y - z * qw);
    float m02 = 2.f * (x * z + y * qw);
    float m10 = 2.f * (x * y + z * qw);
    float m11 = 1.f - 2.f * (x * x + z * z);
    float m12 = 2.f * (y * z - x * qw);
    float m20 = 2.f * (x * z - y * qw);
    float m21 = 2.f * (y * z + x * qw);
    float m22 = 1.f - 2.f * (x * x + y * y);
    float r[3][3] = {{m00, -m01, m02}, {-m10, m11, -m12}, {m20, -m21, m22}};
    for (auto& row : r)
        for (float v : row) w.i32(toFixed12(v));
}

// PSXPlayer.FindNavmesh: Physics.Raycast(transform.position, Vector3.down,
// 100) and the hit point raised by the player height. The scene format has no
// physics colliders, so the ray is cast against the front faces of every
// active mesh (what Unity's primitive and mesh colliders amount to).
Vec3 findCamPoint(Vec3 origin, float height, const std::vector<ExpObject>& exporters) {
    double bestT = 100.0;
    bool hit = false;
    for (const ExpObject& e : exporters) {
        if (!e.obj->active || !e.flat->activeInHierarchy) continue;
        const Mat34& m = e.flat->localToWorld;
        for (const auto& sm : e.mesh->submeshes)
            for (size_t i = 0; i + 2 < sm.size(); i += 3) {
                Vec3 a = m.point(e.mesh->positions[size_t(sm[i])]);
                Vec3 b = m.point(e.mesh->positions[size_t(sm[i + 1])]);
                Vec3 c = m.point(e.mesh->positions[size_t(sm[i + 2])]);
                // Edge functions in XZ; a front face seen from above has normal.y > 0.
                double ax = a.x - double(origin.x), az = a.z - double(origin.z);
                double bx = b.x - double(origin.x), bz = b.z - double(origin.z);
                double cx = c.x - double(origin.x), cz = c.z - double(origin.z);
                double w0 = bz * cx - bx * cz, w1 = cz * ax - cx * az, w2 = az * bx - ax * bz;
                double area = w0 + w1 + w2;
                if (area <= 0 || w0 < 0 || w1 < 0 || w2 < 0) continue;
                double y = (w0 * a.y + w1 * b.y + w2 * c.y) / area;
                double t = double(origin.y) - y;
                if (t < 0 || t > bestT) continue;
                bestT = t;
                hit = true;
            }
    }
    if (!hit) return origin + Vec3{0, height, 0};
    Vec3 point{origin.x, float(double(origin.y) - bestT), origin.z};
    return point + Vec3{0, height, 0};
}

// One region of the stream table: (object index, triangle count) per part, in
// .geo order.
struct StreamRegionOut {
    stream::Box bounds;
    uint32_t byteSize, firstSector;
    std::vector<std::pair<uint16_t, uint16_t>> objects;
};

// Picks the static scenery, plans the regions, and cuts objects that span
// more than one region into one exporter per region. The first part keeps the
// object's index; the others go at the end, so no other object's index moves.
void planStreaming(const Scene& scene, std::vector<ExpObject>& exporters, std::vector<bool>& streamed,
                   std::vector<StreamRegionOut>& regions, const std::vector<std::string>& luaSources,
                   const std::vector<const FlatObject*>& interactables, float gte, ExportResult& res) {
    ExportStats& st = res.stats;
    const std::string pre = "World streaming: ";

    // Anything a script, track or interaction can reach keeps its triangles in RAM.
    std::set<std::string> trackTargets;
    auto addTracks = [&](const std::vector<CutsceneTrack>& tracks) {
        for (const CutsceneTrack& t : tracks)
            if (t.type == TrackType::ObjectPosition || t.type == TrackType::ObjectRotation ||
                t.type == TrackType::ObjectActive || t.type == TrackType::ObjectUVOffset)
                trackTargets.insert(t.target);
    };
    for (const Cutscene& c : scene.cutscenes) addTracks(c.tracks);
    for (const Animation& a : scene.animations) addTracks(a.tracks);
    auto namedInLua = [&](const std::string& name) {
        for (const std::string& src : luaSources)
            if (src.find('"' + name + '"') != std::string::npos || src.find('\'' + name + '\'') != std::string::npos)
                return true;
        return false;
    };

    std::vector<stream::Candidate> cands;
    std::vector<std::vector<float>> triLowY;  // per candidate, per tri: lowest world Y (scene view overlay)
    for (size_t i = 0; i < exporters.size(); i++) {
        const ExpObject& e = exporters[i];
        if (e.tris.empty() || e.obj->script || e.obj->skin || e.obj->agent) continue;
        if (e.obj->collider && e.obj->collider->kind == ColliderKind::Dynamic) continue;
        if (std::any_of(interactables.begin(), interactables.end(), [&](const FlatObject* fo) { return fo->object == e.obj; }))
            continue;
        if (trackTargets.count(e.obj->name) || namedInLua(e.obj->name)) continue;
        stream::Candidate c{int(i), {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN}, {}};
        std::vector<float> lowY;
        for (size_t t = 0; t < e.tris.size(); t++) {
            stream::Box b{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
            float low = FLT_MAX;
            for (int k = 0; k < 3; k++) {
                Vec3 p = e.flat->localToWorld.point(e.triPositions[t * 3 + size_t(k)]);
                low = std::min(low, p.y);
                int32_t x = toWorldFixed12(p.x / gte), z = toWorldFixed12(p.z / gte);
                b = {std::min(b.minX, x), std::min(b.minZ, z), std::max(b.maxX, x), std::max(b.maxZ, z)};
            }
            c.bounds = {std::min(c.bounds.minX, b.minX), std::min(c.bounds.minZ, b.minZ), std::max(c.bounds.maxX, b.maxX),
                        std::max(c.bounds.maxZ, b.maxZ)};
            c.tris.push_back(b);
            lowY.push_back(low);
        }
        cands.push_back(std::move(c));
        triLowY.push_back(std::move(lowY));
    }
    auto warn = [&](StreamProblem kind, const std::string& title, const std::string& body) {
        st.streamWarnings.push_back({kind, title, body});
        res.warnings.push_back(pre + title + ". " + body);
    };
    if (cands.empty()) {
        warn(StreamProblem::NothingStreams, "Nothing can stream",
             "Objects with a script, a skin, an interaction, an agent, a dynamic collider or an animation track stay loaded, and so "
             "do objects a script names. Take the script off scenery that does not need one, or turn streaming off.");
        return;
    }

    // Nothing past the fog wall or the ordering table is drawn, so a region
    // must be resident by then.
    int64_t draw = st.orderingTableSize;
    if (scene.settings.fog.enabled && scene.settings.fog.density > 0)
        draw = std::min<int64_t>(draw, 20000 / std::clamp(scene.settings.fog.density, 1, 10));
    stream::Plan plan = stream::plan(cands, draw);

    size_t extra = 0;
    std::map<int, int> firstPart;
    for (const stream::Region& r : plan.regions)
        for (const stream::Piece& p : r.pieces)
            if (!firstPart.emplace(p.candidate, 0).second) extra++;
    if (plan.regions.size() > 65535 || plan.pieceCount() > 65535 || exporters.size() + extra > 65535) {
        warn(StreamProblem::TooManyObjects, "Too many pieces to stream",
             "The world splits into " + std::to_string(plan.regions.size()) + " regions and " +
                 std::to_string(exporters.size() + extra) +
                 " objects, more than a scene holds (65535), so it exports without streaming. Merge small props into "
                 "bigger meshes.");
        return;
    }

    // Cut. Parts come out in region order, which is also .geo order.
    firstPart.clear();
    std::vector<std::pair<size_t, const stream::Piece*>> parts;  // exporter index, piece
    const size_t original = exporters.size();
    for (const stream::Region& r : plan.regions)
        for (const stream::Piece& p : r.pieces) {
            const size_t src = size_t(cands[size_t(p.candidate)].object);
            size_t dst = src;
            if (!firstPart.emplace(p.candidate, 0).second) {
                dst = exporters.size();
                exporters.push_back(exporters[src]);  // whole copy; trimmed below
            }
            parts.push_back({dst, &p});
        }
    streamed.assign(exporters.size(), false);
    std::vector<std::vector<PsxTri>> srcTris(original);
    std::vector<std::vector<Vec3>> srcPos(original);
    for (const stream::Candidate& c : cands) {
        srcTris[size_t(c.object)] = exporters[size_t(c.object)].tris;
        srcPos[size_t(c.object)] = exporters[size_t(c.object)].triPositions;
    }
    for (auto& [dst, piece] : parts) {
        const size_t src = size_t(cands[size_t(piece->candidate)].object);
        ExpObject& e = exporters[dst];
        const bool cut = piece->tris.size() != srcTris[src].size();
        e.tris.clear();
        e.triPositions.clear();
        for (int t : piece->tris) {
            e.tris.push_back(srcTris[src][size_t(t)]);
            for (int k = 0; k < 3; k++) e.triPositions.push_back(srcPos[src][size_t(t) * 3 + size_t(k)]);
        }
        if (cut) {
            Bounds b(e.triPositions[0], Vec3{0, 0, 0});
            for (const Vec3& v : e.triPositions) b.encapsulate(v);
            e.pieceBounds = b;
        }
        streamed[dst] = true;
    }

    size_t pi = 0;
    for (const stream::Region& r : plan.regions) {
        StreamRegionOut out{r.bounds, r.byteSize, r.firstSector, {}};
        for (size_t k = 0; k < r.pieces.size(); k++, pi++)
            out.objects.push_back({uint16_t(parts[pi].first), uint16_t(r.pieces[k].tris.size())});
        regions.push_back(std::move(out));
        st.streamRegionBounds.push_back({r.bounds.minX, r.bounds.minZ, r.bounds.maxX, r.bounds.maxZ});
        float low = FLT_MAX;
        for (const stream::Piece& pc : r.pieces)
            for (int t : pc.tris) low = std::min(low, triLowY[size_t(pc.candidate)][size_t(t)]);
        st.streamRegionFloor.push_back(low);
    }

    st.streamRegions = int(plan.regions.size());
    st.streamedObjects = int(cands.size());
    st.streamSplitObjects = int(extra);
    st.streamSlots = plan.slotCount;
    st.streamSlotsWanted = plan.slotsWanted;
    st.streamSlotBytes = plan.slotBytes;
    st.streamLoadRadius = plan.loadRadius;
    st.streamRegionSize = plan.cell;
    st.streamedTriBytes = plan.streamedTriBytes;
    st.streamDrawTris = plan.drawTris;
    st.streamTableBytes = plan.tableBytes();

    // Problems, each with what to do about it.
    std::vector<uint32_t> sizes;
    for (const stream::Region& r : plan.regions) sizes.push_back(r.byteSize);
    std::sort(sizes.begin(), sizes.end());
    const uint32_t median = sizes[sizes.size() / 2];
    // Shortening the draw distance is the fix the inspector offers for these.
    if (sizes.back() > stream::kLargeRegionBytes)
        warn(StreamProblem::LargeRegion, "A region is slow to load",
             "The biggest region holds " + std::to_string(sizes.back() / 1024) +
                 " KB of triangles, which takes long to read from the disc while the player moves. A shorter draw "
                 "distance lets regions be smaller.");
    else if (plan.regions.size() > 1 && sizes.back() > 2 * median)
        warn(StreamProblem::UnevenRegions, "One area is much denser",
             "The biggest region is " + std::to_string(sizes.back() / 1024) + " KB while most are " +
                 std::to_string(median / 1024) +
                 " KB, and every streaming slot is as big as the biggest region. Spread that area's detail out, or "
                 "simplify its meshes.");
    if (plan.slotsWanted > stream::kMaxSlots)
        warn(StreamProblem::TooManyNear, "Far scenery may appear late",
             "Up to " + std::to_string(plan.slotsWanted - 1) + " regions are near the camera at once, but psxsplash keeps " +
                 std::to_string(stream::kMaxSlots) + ". A shorter draw distance needs fewer.");
    if (st.streamRamSaved() <= 0)
        warn(StreamProblem::NoSaving, "Streaming does not save RAM here",
             "The scenery is " + std::to_string(st.streamedTriBytes / 1024) + " KB and streaming sets aside " +
                 std::to_string(st.streamPoolBytes() / 1024) +
                 " KB for the regions near the camera. Streaming pays off when the world is much bigger than the draw "
                 "distance: shorten the draw distance, or turn streaming off for this scene.");
    for (const std::string& src : luaSources)
        if (src.find("PlayCDDA") != std::string::npos) {
            warn(StreamProblem::CdMusic, "CD music will not play",
                 "A script plays CD music (Audio.PlayCDDA), but a streamed scene keeps the disc drive busy with the "
                 "world. Use SPU audio for this scene's music, or turn streaming off.");
            break;
        }
}

}  // namespace

namespace {

// ---- UI (uisystem.cpp)

constexpr int kUiMaxCanvases = 24, kUiMaxElements = 256, kUiMaxFonts = 3, kUiTextMax = 63;

// Cut a UTF-8 string to at most `bytes` bytes without splitting a character.
std::string truncateBytes(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t cut = bytes;
    while (cut > 0 && (uint8_t(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

// Elements of a type this build does not know are left out.
void stripUnknownElements(std::vector<UICanvas>& canvases, ExportResult& res) {
    for (UICanvas& cv : canvases)
        for (auto it = cv.elements.begin(); it != cv.elements.end();)
            if (it->unknown.empty()) {
                ++it;
            } else {
                res.warnings.push_back("canvas '" + cv.name + "': element '" + it->name +
                                       "' has a type this build does not export");
                it = cv.elements.erase(it);
            }
}

template <typename ImageFor>
void prepareUi(const Scene& scene, const std::vector<UICanvas>& canvases, const fs::path& root, const VramSettings& vs, ImageFor& imageFor,
               std::deque<PsxTexture>& store, std::vector<std::vector<PsxTexture*>>& uiTextures,
               std::vector<std::vector<int>>& uiSheets, bool shareSheets,
               std::vector<FontSheet>& sheets, std::vector<std::array<int, 2>>& fontVram,
               std::map<std::string, int>& fontIndex, ExportResult& res) {
    if (canvases.size() > size_t(kUiMaxCanvases))
        res.errors.push_back(std::to_string(canvases.size()) + " UI canvases, psxsplash loads at most " +
                             std::to_string(kUiMaxCanvases));
    if (scene.fonts.size() > size_t(kUiMaxFonts))
        res.errors.push_back(std::to_string(scene.fonts.size()) + " UI fonts, psxsplash loads at most " +
                             std::to_string(kUiMaxFonts));
    // Fonts go in the column at x = 960 that the VRAM packer keeps free, above
    // the system font (960, 464, 48 rows). A sheet must not cross a texture
    // page, so the column is two bins: rows 0-255 and 256-463.
    struct Bin {
        int y, end;
    } bins[2] = {{0, 256}, {256, 464}};
    auto clash = [&](int x, int y, int w, int h) {
        auto hit = [&](int ax, int ay, int aw, int ah) { return ax < x + w && x < ax + aw && ay < y + h && y < ay + ah; };
        for (const auto& a : vs.prohibited)
            if (hit(a.x, a.y, a.w, a.h)) return true;
        if (hit(0, 0, vs.resolutionX, vs.resolutionY)) return true;
        return vs.verticalBuffering ? hit(0, 256, vs.resolutionX, vs.resolutionY)
                                    : hit(vs.resolutionX, 0, vs.resolutionX, vs.resolutionY);
    };
    for (size_t i = 0; i < scene.fonts.size() && i < size_t(kUiMaxFonts); i++) {
        const UIFont& f = scene.fonts[i];
        if (fontIndex.count(f.name)) {
            res.errors.push_back("two UI fonts are named '" + f.name + "'");
            continue;
        }
        try {
            sheets.push_back(buildFont(f, root));
        } catch (const std::exception& ex) {
            res.errors.push_back(ex.what());
            continue;
        }
        FontSheet& sh = sheets.back();
        res.warnings.insert(res.warnings.end(), sh.warnings.begin(), sh.warnings.end());
        bool placed = false;
        for (Bin& b : bins)
            if (b.y + sh.height <= b.end && !clash(960, b.y, 64, sh.height)) {
                fontVram.push_back({960, b.y});
                b.y += sh.height;
                placed = true;
                break;
            }
        if (!placed) {
            res.errors.push_back("font '" + f.name + "' (" + std::to_string(sh.height) +
                                 " rows) does not fit in the font column of VRAM");
            sheets.pop_back();
            continue;
        }
        fontIndex[f.name] = int(sheets.size());
    }

    int total = 0;
    for (size_t ci = 0; ci < canvases.size(); ci++) {
        const UICanvas& cv = canvases[ci];
        if (cv.elements.size() > 255)
            res.errors.push_back("canvas '" + cv.name + "' has " + std::to_string(cv.elements.size()) +
                                 " elements, the format stores at most 255");
        total += int(cv.elements.size());
        uiTextures[ci].assign(cv.elements.size(), nullptr);
        uiSheets[ci].assign(cv.elements.size(), -1);
        if (!cv.defaultFont.empty() && !fontIndex.count(cv.defaultFont))
            res.errors.push_back("canvas '" + cv.name + "': no UI font named '" + cv.defaultFont + "' (its default font)");
        for (size_t ei = 0; ei < cv.elements.size(); ei++) {
            const UIElement& e = cv.elements[ei];
            std::string where = "canvas '" + cv.name + "', element '" + e.name + "'";
            if (e.type == UIElementType::Text) {
                if (!e.font.empty() && !fontIndex.count(e.font))
                    res.errors.push_back(where + ": no UI font named '" + e.font + "'");
                if (e.text.size() > size_t(kUiTextMax))
                    res.warnings.push_back(where + ": text cut to " + std::to_string(kUiTextMax) + " bytes");
                for (unsigned char c : e.text)
                    if (c < 0x20 || c > 0x7E) {
                        res.warnings.push_back(where + ": psxsplash draws only ASCII 0x20-0x7E, other bytes as '?'");
                        break;
                    }
            }
            if (e.type == UIElementType::Image && !e.sheet.empty()) {
                // Sheet-backed: one cell of a scene sprite sheet. In a
                // splashpack it shares the sheet's texture (packed once, for the
                // sprite table); a loader pack has no sprite table, so there it
                // gets a copy of the sheet's texture of its own.
                const auto& ss = scene.spriteSheets;
                const auto it = std::find_if(ss.begin(), ss.end(), [&](const SpriteSheet& s) { return s.name == e.sheet; });
                if (it == ss.end()) {
                    res.errors.push_back(where + ": no sprite sheet named '" + e.sheet + "'");
                    continue;
                }
                const SpriteSheet& sh = *it;
                const Image* img = nullptr;
                try {
                    img = &imageFor(sh.texture);
                } catch (const std::exception& ex) {
                    // The sprite sheet check reports a splashpack's sheet textures.
                    if (!shareSheets) res.errors.push_back(where + ": sprite sheet '" + sh.name + "': " + ex.what());
                    continue;
                }
                if (sh.cellW > 0 && sh.cellH > 0) {
                    const int cols = img->width / sh.cellW, rows = img->height / sh.cellH;
                    if (e.cell < 0 || e.cell >= cols * rows) {
                        res.errors.push_back(where + ": cell " + std::to_string(e.cell) + " is outside sprite sheet '" + sh.name +
                                             "', which has " + std::to_string(cols * rows) + " cells (0.." +
                                             std::to_string(cols * rows - 1) + ")");
                        continue;
                    }
                }
                uiSheets[ci][ei] = int(it - ss.begin());
                if (!shareSheets) {
                    try {
                        store.push_back(convertTexture(*img, sh.bitDepth, true));
                    } catch (const std::exception& ex) {
                        res.errors.push_back(where + ": sprite sheet '" + sh.name + "': " + ex.what());
                        continue;
                    }
                    store.back().source = sh.texture;
                    uiTextures[ci][ei] = &store.back();
                }
                continue;
            }
            if (e.type == UIElementType::Image) {
                if (e.texture.empty()) {
                    res.warnings.push_back(where + ": image has no texture");
                    continue;
                }
                try {
                    store.push_back(convertTexture(imageFor(e.texture), e.bitDepth, e.cutout));
                } catch (const std::exception& ex) {
                    res.errors.push_back(ex.what());
                    continue;
                }
                store.back().source = e.texture;
                uiTextures[ci][ei] = &store.back();
            }
            if (e.type == UIElementType::Line)
                for (int v : {e.from[0], e.from[1], e.to[0], e.to[1]})
                    if (v < -32768 || v > 32767) res.errors.push_back(where + ": line endpoint out of range");
        }
    }
    if (total > kUiMaxElements)
        res.errors.push_back(std::to_string(total) + " UI elements, psxsplash loads at most " +
                             std::to_string(kUiMaxElements));
}

uint8_t rgbByte(float c) { return uint8_t(clampv(roundToInt(c * 255.f), 0, 255)); }

// Anchors are stored as bytes, a = byte / 256 of the screen, so 1.0 can only
// be 255 and lands short of the edge. The rounding error of each anchor at
// the project resolution goes into the stored offsets, so the element lands
// on the pixel the scene file asks for.
struct BakedAxis {
    uint8_t amin, amax;
    int16_t pos, size;
};
BakedAxis bakeAxis(float amin, float amax, int pos, int size, int res) {
    BakedAxis b;
    b.amin = uint8_t(clampv(roundToInt(amin * 255.f), 0, 255));
    b.amax = uint8_t(clampv(roundToInt(amax * 255.f), 0, 255));
    if (amin == amax) b.amax = b.amin;
    int errMin = roundToInt(amin * float(res)) - ((b.amin * res) >> 8);
    int errMax = roundToInt(amax * float(res)) - ((b.amax * res) >> 8);
    b.pos = int16_t(clampv(pos + errMin, -32768, 32767));
    b.size = int16_t(clampv(b.amin == b.amax ? size : size + errMax - errMin, -32768, 32767));
    return b;
}

void writeUi(BinWriter& w, size_t tableOffsetPos, const std::vector<UICanvas>& canvases,
             const std::vector<std::vector<PsxTexture*>>& uiTextures, const std::vector<FontSheet>& sheets,
             const std::vector<std::array<int, 2>>& fontVram, const std::map<std::string, int>& fontIndex,
             const VramSettings& vs, const std::vector<std::vector<int>>& uiSheets, const std::vector<SpriteSheet>& spriteSheets,
             const std::vector<PsxTexture*>& spriteTextures, const std::vector<uint32_t>* fontDataOffsets = nullptr) {
    w.align4();
    w.patchU32(tableOffsetPos, uint32_t(w.pos()));
    // Font descriptors (112 bytes). In a splashpack the pixels are in the
    // .vram file (offset 0); a loader pack carries them at fontDataOffsets.
    for (size_t i = 0; i < sheets.size(); i++) {
        w.u8(uint8_t(sheets[i].glyphWidth));
        w.u8(uint8_t(sheets[i].glyphHeight));
        w.u16(uint16_t(fontVram[i][0]));
        w.u16(uint16_t(fontVram[i][1]));
        w.u16(uint16_t(sheets[i].height));
        w.u32(fontDataOffsets ? fontDataOffsets->at(i) : 0);
        w.u32(uint32_t(sheets[i].texels.size() / 2));
        w.bytes(sheets[i].advances.data(), 96);
    }
    // Canvas descriptors (12 bytes).
    std::vector<size_t> dataPos, namePos;
    std::vector<std::string> names;
    for (const UICanvas& cv : canvases) {
        names.push_back(truncateUtf16(cv.name, 24));
        dataPos.push_back(w.pos());
        w.u32(0);
        w.u8(uint8_t(names.back().size()));
        w.u8(uint8_t(cv.sortOrder));
        w.u8(uint8_t(cv.elements.size()));
        w.u8(cv.visible ? 1 : 0);
        namePos.push_back(w.pos());
        w.u32(0);
    }
    // Element records (48 bytes), then that canvas's strings.
    for (size_t ci = 0; ci < canvases.size(); ci++) {
        const UICanvas& cv = canvases[ci];
        if (cv.elements.empty()) continue;
        w.align4();
        w.patchU32(dataPos[ci], uint32_t(w.pos()));
        std::vector<std::pair<size_t, std::string>> strings;
        for (size_t ei = 0; ei < cv.elements.size(); ei++) {
            const UIElement& e = cv.elements[ei];
            std::string name = truncateUtf16(e.name, 24);
            w.u8(uint8_t(e.type));
            w.u8(e.visible ? 1 : 0);
            w.u8(uint8_t(name.size()));
            w.u8(0);
            strings.push_back({w.pos(), name});
            w.u32(0);
            BakedAxis bx{}, by{};
            if (e.type == UIElementType::Line) {
                // Lines draw at their endpoints; the layout fields are unused.
                w.i16(int16_t(e.from[0]));
                w.i16(int16_t(e.from[1]));
                w.i16(int16_t(e.to[0]));
                w.i16(int16_t(e.to[1]));
                w.u32(0);
            } else {
                bx = bakeAxis(e.anchorMin[0], e.anchorMax[0], e.rect[0], e.rect[2], vs.resolutionX);
                by = bakeAxis(e.anchorMin[1], e.anchorMax[1], e.rect[1], e.rect[3], vs.resolutionY);
                w.i16(bx.pos);
                w.i16(by.pos);
                w.i16(bx.size);
                w.i16(by.size);
                w.u8(bx.amin);
                w.u8(by.amin);
                w.u8(bx.amax);
                w.u8(by.amax);
            }
            for (float c : e.color) w.u8(rgbByte(c));
            w.u8(0);
            size_t typeStart = w.pos();
            switch (e.type) {
            case UIElementType::Image: {
                const int si = uiSheets[ci][ei];
                const PsxTexture* t = uiTextures[ci][ei];
                if (!t && si >= 0 && size_t(si) < spriteTextures.size()) t = spriteTextures[size_t(si)];
                if (t) {
                    int expander = 16 / int(t->bitDepth);
                    const int baseU = t->packingX * expander, baseV = t->packingY;
                    int u0 = baseU, v0 = baseV, u1 = baseU + t->width - 1, v1 = baseV + t->height - 1;
                    const SpriteSheet* sh = si >= 0 ? &spriteSheets[size_t(si)] : nullptr;
                    const int cols = sh ? t->width / sh->cellW : 0;
                    if (sh) {
                        // One cell of the sheet, its last texel inclusive; the grid
                        // after it lets UI.SetFrame/GetFrame step through the cells
                        // (uisystem.cpp recovers the cell from u0/v0).
                        u0 = baseU + (e.cell % cols) * sh->cellW;
                        v0 = baseV + (e.cell / cols) * sh->cellH;
                        u1 = u0 + sh->cellW - 1;
                        v1 = v0 + sh->cellH - 1;
                    }
                    w.u8(t->texpageX);
                    w.u8(t->texpageY);
                    w.u16(t->clutPackingX);
                    w.u16(t->clutPackingY);
                    w.u8(uint8_t(u0));
                    w.u8(uint8_t(v0));
                    w.u8(uint8_t(u1));
                    w.u8(uint8_t(v1));
                    w.u8(t->bitDepth == BitDepth::Bpp4 ? 0 : t->bitDepth == BitDepth::Bpp8 ? 1 : 2);
                    if (sh) {
                        w.u8(uint8_t(sh->cellW));
                        w.u8(uint8_t(sh->cellH));
                        w.u8(uint8_t(cols));
                        w.u8(uint8_t(baseU));
                        w.u8(uint8_t(baseV));
                    }
                }
                break;
            }
            case UIElementType::Progress:
                for (float c : e.background) w.u8(rgbByte(c));
                w.u8(uint8_t(e.value));
                break;
            case UIElementType::Text:
                if (const std::string& font = effectiveFont(cv, e); !font.empty()) w.u8(uint8_t(fontIndex.at(font)));
                else w.u8(0);
                break;
            case UIElementType::Line:
                w.i16(int16_t(e.from[0]));
                w.i16(int16_t(e.from[1]));
                w.i16(int16_t(e.to[0]));
                w.i16(int16_t(e.to[1]));
                break;
            default:
                break;
            }
            while (w.pos() < typeStart + 16) w.u8(0);
            std::string text = e.type == UIElementType::Text ? truncateBytes(e.text, kUiTextMax) : "";
            if (!text.empty()) strings.push_back({w.pos(), text});
            w.u32(0);
            w.u32(0);
        }
        for (auto& [at, str] : strings) {
            w.patchU32(at, uint32_t(w.pos()));
            w.bytes(str);
            w.u8(0);
        }
    }
    for (size_t ci = 0; ci < canvases.size(); ci++) {
        w.patchU32(namePos[ci], uint32_t(w.pos()));
        w.bytes(names[ci]);
        w.u8(0);
    }
}

// ---- cutscenes and animations (cutscene.hh, animation.hh, splashpack.cpp in psxsplash)

constexpr int kMaxCutscenes = 16, kMaxTracks = 8, kMaxKeyframes = 64, kMaxAudioEvents = 64, kMaxSkinEvents = 16;

// A cutscene or an animation. The two share the track, keyframe and skin event
// layouts; an animation has no audio events and a shorter header.
struct Sequence {
    const std::string& name;
    int durationFrames;
    const std::vector<CutsceneTrack>& tracks;
    const std::vector<CutsceneAudioEvent>* audioEvents;  // null for an animation
    const std::vector<SkinAnimEvent>& skinEvents;
};

// A skinned mesh in skin table order, with its clip names in clip order.
struct SkinTarget {
    std::string object;
    std::vector<std::string> clips;
};

void writeSequences(BinWriter& w, size_t tableOffsetPos, const std::vector<Sequence>& cutscenes, bool animation,
                    const std::vector<std::string>& objectNames, const std::set<std::string>& offObjectNames,
                    const std::vector<std::string>& clipNames, const std::vector<SkinTarget>& skinTargets, const std::vector<UICanvas>& canvases,
                    const std::vector<std::string>& lightNames, const VramSettings& vs, float gte,
                    ExportResult& res) {
    const std::string kind = animation ? "animation" : "cutscene";
    if (cutscenes.size() > size_t(kMaxCutscenes))
        res.errors.push_back(std::to_string(cutscenes.size()) + " " + kind + "s, psxsplash loads at most " +
                             std::to_string(kMaxCutscenes));
    // Skin event target -> (skin table index, clip index), or an error.
    auto resolveSkin = [&](const SkinAnimEvent& e, std::string& err) -> std::pair<int, int> {
        int found = -1;
        for (size_t i = 0; i < skinTargets.size(); i++)
            if (skinTargets[i].object == e.object) {
                if (found >= 0) {
                    err = "two skinned objects are named '" + e.object + "'";
                    return {-1, -1};
                }
                found = int(i);
            }
        if (found < 0) {
            err = "no skinned object named '" + e.object + "'";
            return {-1, -1};
        }
        const auto& clips = skinTargets[size_t(found)].clips;
        auto it = std::find(clips.begin(), clips.end(), e.clip);
        if (it == clips.end()) {
            err = "'" + e.object + "' has no clip named '" + e.clip + "'";
            return {-1, -1};
        }
        return {found, int(it - clips.begin())};
    };
    auto findElement = [&](const std::string& path) -> const UIElement* {
        size_t slash = path.find('/');
        if (slash == std::string::npos) return nullptr;
        for (const UICanvas& cv : canvases)
            if (cv.name == path.substr(0, slash))
                for (const UIElement& e : cv.elements)
                    if (e.name == path.substr(slash + 1)) return &e;
        return nullptr;
    };
    auto hasCanvas = [&](const std::string& n) {
        return std::any_of(canvases.begin(), canvases.end(), [&](const UICanvas& c) { return c.name == n; });
    };
    // Light track target -> index in the runtime light table, or -1.
    auto lightIndex = [&](const std::string& name) {
        auto it = std::find(lightNames.begin(), lightNames.end(), name);
        return it == lightNames.end() ? -1 : int(it - lightNames.begin());
    };
    auto isLight = [](TrackType t) { return int(t) >= 14 && int(t) <= 18; };
    std::vector<std::string> seen;
    for (const Sequence& c : cutscenes) {
        std::string where = kind + " '" + c.name + "'";
        if (c.name.empty() || truncateUtf16(c.name, 24) != c.name)
            res.errors.push_back(where + ": the name must be 1..24 characters (Lua plays it by name)");
        if (std::find(seen.begin(), seen.end(), c.name) != seen.end()) res.errors.push_back("two " + kind + "s are named '" + c.name + "'");
        seen.push_back(c.name);
        if (c.tracks.size() > size_t(kMaxTracks))
            res.errors.push_back(where + ": " + std::to_string(c.tracks.size()) + " tracks, at most " + std::to_string(kMaxTracks));
        if (c.audioEvents && c.audioEvents->size() > size_t(kMaxAudioEvents))
            res.errors.push_back(where + ": more than " + std::to_string(kMaxAudioEvents) + " audio events");
        if (c.skinEvents.size() > size_t(kMaxSkinEvents))
            res.errors.push_back(where + ": more than " + std::to_string(kMaxSkinEvents) + " skin events");
        for (const SkinAnimEvent& e : c.skinEvents) {
            std::string err;
            resolveSkin(e, err);
            if (!err.empty()) res.errors.push_back(where + ": " + err);
        }
        for (const CutsceneTrack& t : c.tracks) {
            if (t.keyframes.size() > size_t(kMaxKeyframes))
                res.errors.push_back(where + ": a track has more than " + std::to_string(kMaxKeyframes) + " keyframes");
            int ty = int(t.type);
            bool camera = t.type == TrackType::CameraPosition || t.type == TrackType::CameraRotation ||
                          t.type == TrackType::CameraH;
            if (animation && camera)
                res.errors.push_back(where + ": camera tracks only play in cutscenes");
            bool object = (ty >= 2 && ty <= 4) || t.type == TrackType::ObjectUVOffset;
            if (object && std::find(objectNames.begin(), objectNames.end(), truncateUtf16(t.target, 24)) == objectNames.end()) {
                if (offObjectNames.count(truncateUtf16(t.target, 24)))
                    res.errors.push_back(where + ": '" + t.target +
                                         "' is switched off (or under an off parent), so it is not exported");
                else
                    res.errors.push_back(where + ": no exported object named '" + t.target + "' (objects need a mesh)");
            }
            if (t.type == TrackType::UICanvasVisible && !hasCanvas(t.target))
                res.errors.push_back(where + ": no canvas named '" + t.target + "'");
            if (ty >= 6 && ty <= 9 && !findElement(t.target))
                res.errors.push_back(where + ": no UI element '" + t.target + "' (write it as canvas/element)");
            if (isLight(t.type)) {
                if (lightIndex(t.target) < 0)
                    res.errors.push_back(where + ": no runtime point light named '" + t.target +
                                         "' (an active point light with runtime on, one of the first " +
                                         std::to_string(kMaxSceneLights) + ")");
                else if (std::count(lightNames.begin(), lightNames.end(), t.target) > 1)
                    res.errors.push_back(where + ": two runtime lights are named '" + t.target + "'");
            }
        }
        if (c.audioEvents)
            for (const CutsceneAudioEvent& a : *c.audioEvents)
                if (std::find(clipNames.begin(), clipNames.end(), a.clip) == clipNames.end())
                    res.errors.push_back(where + ": no audio clip named '" + a.clip + "'");
    }
    if (!res.ok()) return;

    w.align4();
    w.patchU32(tableOffsetPos, uint32_t(w.pos()));
    std::vector<size_t> dataPos, namePos;
    std::vector<std::string> names;
    for (const Sequence& c : cutscenes) {
        names.push_back(c.name);
        dataPos.push_back(w.pos());
        w.u32(0);
        w.u8(uint8_t(names.back().size()));
        w.u8(0);
        w.u16(0);
        namePos.push_back(w.pos());
        w.u32(0);
    }
    for (size_t ci = 0; ci < cutscenes.size(); ci++) {
        const Sequence& c = cutscenes[ci];
        std::string where = kind + " '" + c.name + "'";
        w.align4();
        w.patchU32(dataPos[ci], uint32_t(w.pos()));
        w.u16(uint16_t(c.durationFrames));
        w.u8(uint8_t(c.tracks.size()));
        w.u8(uint8_t(c.audioEvents ? c.audioEvents->size() : 0));
        size_t tracksPos = w.pos();
        w.u32(0);
        size_t audioPos = 0;
        if (!animation) {
            audioPos = w.pos();
            w.u32(0);
        }
        w.u8(uint8_t(c.skinEvents.size()));
        w.u8(0);
        w.u16(0);
        size_t skinPos = w.pos();
        w.u32(0);

        w.align4();
        w.patchU32(tracksPos, uint32_t(w.pos()));
        std::vector<size_t> trackNamePos, keyPos;
        std::vector<std::string> trackNames;
        for (const CutsceneTrack& t : c.tracks) {
            int ty = int(t.type);
            bool named = (ty >= 2 && ty <= 9) || t.type == TrackType::ObjectUVOffset;
            std::string n = named ? (ty >= 5 && ty <= 9 ? truncateBytes(t.target, 255) : truncateUtf16(t.target, 24)) : "";
            w.u8(uint8_t(t.type));
            w.u8(uint8_t(t.keyframes.size()));
            w.u8(uint8_t(n.size()));
            w.u8(isLight(t.type) ? uint8_t(lightIndex(t.target)) : 0);
            trackNamePos.push_back(w.pos());
            w.u32(0);
            keyPos.push_back(w.pos());
            w.u32(0);
            trackNames.push_back(n);
        }
        bool clamped = false, brightClamped = false;
        auto coord = [&](float v) {
            int f = roundToInt((v / gte) * 4096.f);
            if (f < -32768 || f > 32767) clamped = true;
            return int16_t(clampv(f, -32768, 32767));
        };
        auto angle = [&](float deg) {
            int f = roundToInt(deg * 1024.f / 180.f);
            return int16_t(clampv(f, -32768, 32767));
        };
        auto byteOf = [](float v, int lo, int hi) { return int16_t(clampv(roundToInt(v), lo, hi)); };
        for (size_t ti = 0; ti < c.tracks.size(); ti++) {
            const CutsceneTrack& t = c.tracks[ti];
            std::vector<Keyframe> keys = t.keyframes;
            std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
            // UI positions are offsets from the element's anchor: the same
            // anchor rounding correction as the element itself.
            int dx = 0, dy = 0;
            if (t.type == TrackType::UIPosition)
                if (const UIElement* e = findElement(t.target)) {
                    dx = bakeAxis(e->anchorMin[0], e->anchorMax[0], 0, 0, vs.resolutionX).pos;
                    dy = bakeAxis(e->anchorMin[1], e->anchorMax[1], 0, 0, vs.resolutionY).pos;
                }
            w.align4();
            w.patchU32(keyPos[ti], keys.empty() ? 0 : uint32_t(w.pos()));
            for (const Keyframe& k : keys) {
                w.u16(uint16_t((int(k.interp) << 13) | (k.frame & 0x1FFF)));
                const auto& v = k.value;
                int16_t o[3] = {0, 0, 0};
                switch (t.type) {
                case TrackType::CameraPosition:
                case TrackType::ObjectPosition:
                    o[0] = coord(v[0]), o[1] = coord(-v[1]), o[2] = coord(v[2]);
                    break;
                case TrackType::CameraRotation:
                case TrackType::ObjectRotation:
                    o[0] = angle(-v[0]), o[1] = angle(v[1]), o[2] = angle(-v[2]);
                    break;
                case TrackType::ObjectActive:
                case TrackType::UICanvasVisible:
                case TrackType::UIElementVisible:
                case TrackType::RumbleSmall:
                    o[0] = v[0] > 0.5f ? 1 : 0;
                    break;
                case TrackType::UIProgress:
                    o[0] = byteOf(v[0], 0, 100);
                    break;
                case TrackType::UIPosition:
                    o[0] = int16_t(clampv(roundToInt(v[0]) + dx, -32768, 32767));
                    o[1] = int16_t(clampv(roundToInt(v[1]) + dy, -32768, 32767));
                    break;
                case TrackType::UIColor:
                    for (int i = 0; i < 3; i++) o[i] = byteOf(v[size_t(i)] * 255.f, 0, 255);
                    break;
                case TrackType::CameraH:
                    o[0] = byteOf(v[0], 1, 1024);
                    break;
                case TrackType::RumbleLarge:
                    o[0] = byteOf(v[0], 0, 255);
                    break;
                case TrackType::ObjectUVOffset:
                    o[0] = byteOf(v[0], 0, 255), o[1] = byteOf(v[1], 0, 255);
                    break;
                case TrackType::LightPosition:
                    o[0] = coord(v[0]), o[1] = coord(-v[1]), o[2] = coord(v[2]);
                    break;
                case TrackType::LightColor:
                    for (int i = 0; i < 3; i++) o[i] = byteOf(v[size_t(i)] * 255.f, 0, 255);
                    break;
                case TrackType::LightIntensity:
                    if (roundToInt(v[0] * 4096.f) > 32767) brightClamped = true;
                    o[0] = byteOf(v[0] * 4096.f, 0, 32767);
                    break;
                case TrackType::LightRadius:
                    o[0] = std::max<int16_t>(0, coord(v[0]));
                    break;
                case TrackType::LightEnabled:
                    o[0] = v[0] > 0.5f ? 1 : 0;
                    break;
                }
                for (int16_t x : o) w.i16(x);
            }
        }
        if (clamped)
            res.warnings.push_back(where + ": a position or radius key is farther than 8 PSX units (" +
                                   std::to_string(int(8 * gte)) + " scene units) from the origin and was clamped");
        if (brightClamped)
            res.warnings.push_back(where + ": a light intensity key above 8 was clamped to 8 (tracks hold 4.12)");
        for (size_t ti = 0; ti < c.tracks.size(); ti++) {
            if (trackNames[ti].empty()) continue;
            w.patchU32(trackNamePos[ti], uint32_t(w.pos()));
            w.bytes(trackNames[ti]);
            w.u8(0);
        }
        if (c.audioEvents && !c.audioEvents->empty()) {
            std::vector<CutsceneAudioEvent> ev = *c.audioEvents;
            std::stable_sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
            w.align4();
            w.patchU32(audioPos, uint32_t(w.pos()));
            for (const CutsceneAudioEvent& a : ev) {
                w.u16(uint16_t(a.frame));
                w.u8(uint8_t(std::find(clipNames.begin(), clipNames.end(), a.clip) - clipNames.begin()));
                w.u8(uint8_t(a.volume));
                w.u8(uint8_t(a.pan));
                w.u8(0);
                w.u16(0);
            }
        }
        if (!c.skinEvents.empty()) {
            std::vector<SkinAnimEvent> ev = c.skinEvents;
            std::stable_sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
            w.align4();
            w.patchU32(skinPos, uint32_t(w.pos()));
            for (const SkinAnimEvent& e : ev) {
                std::string err;
                auto [skin, clip] = resolveSkin(e, err);
                w.u16(uint16_t(e.frame));
                w.u8(uint8_t(skin));
                w.u8(uint8_t(clip));
                w.u8(e.loop ? 1 : 0);
                w.u8(0);
                w.u16(0);
            }
        }
        w.patchU32(namePos[ci], uint32_t(w.pos()));
        w.bytes(names[ci]);
        w.u8(0);
    }
}
}  // namespace

std::array<int, 2> uiPositionTrackStart(const UIElement& e, const VramSettings& vs) {
    // writeUi's stored layout (a line keeps its endpoint and no anchors), then
    // UISystem::resolveLayout: anchor byte * resolution >> 8 plus the offset,
    // clamped at 0.
    const bool line = e.type == UIElementType::Line;
    const int res[2] = {vs.resolutionX, vs.resolutionY};
    std::array<int, 2> out{};
    for (int i = 0; i < 2; i++) {
        BakedAxis b = line ? BakedAxis{0, 0, int16_t(e.from[size_t(i)]), 0}
                           : bakeAxis(e.anchorMin[size_t(i)], e.anchorMax[size_t(i)], e.rect[size_t(i)], e.rect[size_t(i) + 2], res[i]);
        int resolved = std::max(int(int16_t(((b.amin * res[i]) >> 8) + b.pos)), 0);
        // writeSequences adds this to every key of the track.
        int keyOffset = bakeAxis(e.anchorMin[size_t(i)], e.anchorMax[size_t(i)], 0, 0, res[i]).pos;
        out[size_t(i)] = resolved - keyOffset;
    }
    return out;
}

// psxsplash render buffer sizing. Fragment bytes are a 4-byte chain word plus
// the psyqo primitive: GouraudTriangle 6 words, GouraudTexturedTriangle 9,
// Rectangle 3, Sprite 4, Line 3, TPage 1.
constexpr uint32_t kFragGouraudTri = 28, kFragTexturedTri = 40, kFragRect = 16, kFragSprite = 20, kFragLine = 16,
                   kFragTPage = 8;
// A triangle crossing the near plane is split up to MAX_SUBDIV_DEPTH (5) times,
// up to 32 pieces. How many cross it in one frame is not known here; this
// allowance (16 such triangles, textured and fogged) is a guess, which is why
// the editor also shows the peak psxsplash measures while playing.
constexpr uint32_t kNearPlaneAllowance = 16 * 31 * (kFragGouraudTri + kFragTexturedTri);

// Ordering table: the renderer uses a triangle's largest screen Z as its
// bucket and drops it at or past the table size, so the table is the far clip
// in GTE units (1 m = 4096 / gteScaling). The farthest any vertex can be from
// the camera is the diagonal of everything in the scene; fog culls past
// 20000 / density.
static uint32_t orderingTableNeed(const Scene& scene, const std::vector<ExpObject>& objs, float gte) {
    bool any = false;
    Vec3 mn{}, mx{};
    for (const ExpObject& e : objs)
        for (const Vec3& v : e.mesh->positions) {
            Vec3 p = e.flat->localToWorld.point(v);
            mn = any ? vmin(mn, p) : p;
            mx = any ? vmax(mx, p) : p;
            any = true;
        }
    Vec3 d = mx - mn;
    double far = std::sqrt(double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z) * 4096.0 / gte;
    if (scene.settings.fog.enabled && scene.settings.fog.density > 0)
        far = std::min(far, 20000.0 / scene.settings.fog.density);
    // A streamed world is bigger than what the camera should see: drawing all
    // of it would need all of it loaded.
    if (scene.settings.streamWorld) far = std::min(far, double(kStreamDrawDistance));
    return uint32_t(std::clamp(std::ceil(far) + 1, double(kOtMin), double(kOtMax)));
}

// Bump allocator: every triangle in the scene drawn in one frame (textured and
// fogged ones take two fragments), the near-plane allowance, the dither TPage,
// and the UI with every text at its longest.
// Streamed objects count as the most of their triangles within the draw
// distance of one camera position (`streamTris`, whole regions, all around the
// camera), each at the costliest fragment size.
static uint32_t bumpAllocatorNeed(const Scene& scene, const std::vector<ExpObject>& objs, const std::vector<bool>& streamed,
                                  uint64_t streamTris, const std::vector<UICanvas>& canvases) {
    const bool fog = scene.settings.fog.enabled;
    uint64_t n = kFragTPage + kNearPlaneAllowance;
    for (size_t i = 0; i < objs.size(); i++) {
        if (streamed[i]) continue;
        for (const PsxTri& t : objs[i].tris)
            n += t.textureIndex < 0 ? kFragGouraudTri : kFragTexturedTri + (fog ? kFragGouraudTri : 0);
    }
    n += streamTris * (kFragTexturedTri + (fog ? kFragGouraudTri : 0));
    for (const UICanvas& cv : canvases)
        for (const UIElement& el : cv.elements) switch (el.type) {
                case UIElementType::Box: n += kFragRect; break;
                case UIElementType::Progress: n += 2 * kFragRect; break;
                case UIElementType::Line: n += kFragLine; break;
                case UIElementType::Image: n += 2 * kFragTexturedTri; break;
                case UIElementType::Text:
                    if (!effectiveFont(cv, el).empty()) n += kFragTPage + uint64_t(kUiTextMax) * kFragSprite;
                    break;
            }
    return uint32_t(std::min<uint64_t>((n + 3) & ~uint64_t(3), UINT32_MAX));
}

// ---- memory card table (v21; psxsplash splashpack.cpp SPLASHPACKMemcard)

void checkMemCard(const MemCardSettings& mc, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    if (!mc.enabled) return;
    auto printable = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
    };
    if (mc.region.size() != kMemCardRegionLen || !printable(mc.region))
        errors.push_back("Memory card region '" + mc.region +
                         "' must be exactly 2 ASCII letters: BA (America), BE (Europe) or BI (Japan)");
    else if (mc.region != "BA" && mc.region != "BE" && mc.region != "BI")
        warnings.push_back("Memory card region '" + mc.region +
                           "' is not one the BIOS knows: use BA (America), BE (Europe) or BI (Japan)");
    if (mc.product.empty())
        errors.push_back("Memory card product code is empty: set the game's code, e.g. SLUS-00000; it starts every "
                         "save's filename");
    else if (mc.product.size() > kMemCardProductMax)
        errors.push_back("Memory card product code '" + mc.product + "' is " + std::to_string(mc.product.size()) +
                         " characters, the splashpack stores 10: shorten it, e.g. SLUS-00000");
    else if (!printable(mc.product))
        errors.push_back("Memory card product code '" + mc.product +
                         "' must be plain ASCII letters, digits and '-', e.g. SLUS-00000");
    if (mc.title.size() > kMemCardTitleMax)
        errors.push_back("Memory card save title '" + mc.title + "' is " + std::to_string(mc.title.size()) +
                         " characters, the BIOS title holds 32: shorten it");
    else if (!printable(mc.title))
        errors.push_back("Memory card save title '" + mc.title +
                         "' has a character that is not plain ASCII: the console writes ASCII titles only, replace it");
    else if (mc.title.empty())
        warnings.push_back("Memory card save title is empty, so the BIOS memory card screen shows saves with no name: "
                           "set a title");
    else if (mc.title.find_first_of("\"'-\\~") != std::string::npos)
        warnings.push_back("Memory card save title '" + mc.title +
                           "' shows a '?' in the BIOS for each \" ' - \\ or ~ (no full-width form): leave them out");
    if (mc.icons.size() > kMemCardIconsMax)
        errors.push_back(std::to_string(mc.icons.size()) +
                         " memory card icon frames, the BIOS animates at most 3: remove the extra frames");
    else if (mc.icons.empty())
        warnings.push_back("Memory card saves have no icon, so the BIOS shows a blank square: add a 16x16 image");
}

// ---- sprite sheets (v22; psxsplash spritesystem.cpp SPLASHPACKSpriteSheet/Anim)

void checkSpriteSheets(const std::vector<SpriteSheet>& sheets,
                       const std::function<SpriteTextureSize(const SpriteSheet&)>& sizeOf, std::vector<std::string>& errors,
                       std::vector<std::string>& warnings) {
    if (sheets.size() > size_t(kMaxSpriteSheets))
        errors.push_back(std::to_string(sheets.size()) + " sprite sheets, psxsplash loads at most " +
                         std::to_string(kMaxSpriteSheets) + ": delete some or merge them into one texture");
    size_t anims = 0;
    for (const SpriteSheet& s : sheets) anims += s.anims.size();
    if (anims > size_t(kMaxSpriteAnims))
        errors.push_back(std::to_string(anims) + " sprite animations, psxsplash loads at most " +
                         std::to_string(kMaxSpriteAnims) + ": delete some");
    std::vector<std::string> sheetNames;
    std::map<std::string, std::string> animSheet;  // stored animation name -> the sheet it was first seen in
    for (size_t i = 0; i < sheets.size(); i++) {
        const SpriteSheet& s = sheets[i];
        const std::string where = s.name.empty() ? "sprite sheet " + std::to_string(i + 1) : "sprite sheet '" + s.name + "'";
        const std::string stored = truncateUtf16(s.name, 24);
        if (s.name.empty()) errors.push_back(where + " has no name: Lua finds sheets by name (Sprite.SheetIndex)");
        else if (stored != s.name) warnings.push_back(where + ": the name is cut to 24 characters, '" + stored + "'");
        if (!s.name.empty()) {
            if (std::find(sheetNames.begin(), sheetNames.end(), stored) != sheetNames.end())
                errors.push_back("two sprite sheets are named '" + stored + "': rename one");
            sheetNames.push_back(stored);
        }
        const bool cellOk = s.cellW >= 1 && s.cellW <= 255 && s.cellH >= 1 && s.cellH <= 255;
        if (!cellOk)
            errors.push_back(where + ": the cell is " + std::to_string(s.cellW) + "x" + std::to_string(s.cellH) +
                             ", each side must be 1..255 pixels");
        int cells = -1;
        if (s.texture.empty()) {
            errors.push_back(where + " has no texture: pick an image");
        } else if (SpriteTextureSize sz = sizeOf(s); !sz.error.empty()) {
            errors.push_back(where + ": " + sz.error);
        } else if (cellOk) {
            if (sz.width % s.cellW || sz.height % s.cellH) {
                errors.push_back(where + ": the texture is " + std::to_string(sz.width) + "x" + std::to_string(sz.height) +
                                 ", not a whole number of " + std::to_string(s.cellW) + "x" + std::to_string(s.cellH) +
                                 " cells: change the cell size or the image");
            } else {
                const int cols = sz.width / s.cellW, rows = sz.height / s.cellH;
                cells = cols * rows;
                if (cells > 255)
                    errors.push_back(where + ": " + std::to_string(cols) + "x" + std::to_string(rows) + " = " +
                                     std::to_string(cells) + " cells, psxsplash numbers at most 255: use bigger cells");
            }
        }
        for (size_t k = 0; k < s.anims.size(); k++) {
            const SpriteAnim& a = s.anims[k];
            const std::string at = where + ", animation " + (a.name.empty() ? std::to_string(k + 1) : "'" + a.name + "'");
            const std::string an = truncateUtf16(a.name, 24);
            if (a.name.empty()) errors.push_back(at + " has no name: Lua plays animations by name (Sprite.AnimIndex)");
            else if (an != a.name) warnings.push_back(at + ": the name is cut to 24 characters, '" + an + "'");
            if (!a.name.empty()) {
                auto [it, fresh] = animSheet.emplace(an, s.name);
                if (!fresh)
                    errors.push_back("two sprite animations are named '" + an + "' (sheets '" + it->second + "' and '" + s.name +
                                     "'): Sprite.AnimIndex searches every sheet, so names must differ across the scene");
            }
            if (a.frameCount < 1) errors.push_back(at + ": " + std::to_string(a.frameCount) + " frames, it needs at least 1");
            if (a.frameDuration < 1 || a.frameDuration > 255)
                errors.push_back(at + ": each frame lasts " + std::to_string(a.frameDuration) + " vsyncs, it must be 1..255");
            if (a.firstFrame < 0 || (cells >= 0 && a.frameCount >= 1 && a.firstFrame + a.frameCount > cells))
                errors.push_back(at + ": cells " + std::to_string(a.firstFrame) + ".." +
                                 std::to_string(a.firstFrame + std::max(a.frameCount, 1) - 1) + " are outside the sheet" +
                                 (cells >= 0 ? "'s " + std::to_string(cells) + " cells (0.." + std::to_string(cells - 1) + ")" : ""));
        }
    }
    checkSpriteDirectionSets(sheets, warnings);
}

const std::vector<std::string>& spriteDirectionSuffixes(int n) {
    // Around the circle from yaw 0, the way yaw increases.
    static const std::vector<std::string> four = {"n", "e", "s", "w"}, eight = {"n", "ne", "e", "se", "s", "sw", "w", "nw"},
                                          none;
    return n == 4 ? four : n == 8 ? eight : none;
}

void checkSpriteDirectionSets(const std::vector<SpriteSheet>& sheets, std::vector<std::string>& warnings) {
    // The table psxsplash loads: every sheet's animations, sheet by sheet.
    struct Entry {
        size_t sheet;
        std::string suffix;
    };
    std::vector<std::string> bases;                         // in order of first appearance
    std::map<std::string, std::vector<std::pair<size_t, Entry>>> byBase;  // base -> (table index, entry)
    const std::vector<std::string>& eight = spriteDirectionSuffixes(8);
    size_t index = 0;
    for (size_t i = 0; i < sheets.size(); i++)
        for (const SpriteAnim& a : sheets[i].anims) {
            const size_t us = a.name.rfind('_');
            if (us != std::string::npos && us > 0) {
                const std::string base = a.name.substr(0, us), suffix = a.name.substr(us + 1);
                if (std::find(eight.begin(), eight.end(), suffix) != eight.end()) {
                    if (!byBase.count(base)) bases.push_back(base);
                    byBase[base].push_back({index, {i, suffix}});
                }
            }
            index++;
        }
    for (const std::string& base : bases) {
        const auto& got = byBase[base];
        if (got.size() < 2) continue;  // one walk_n alone is just a name
        bool eightWay = false;
        for (const auto& g : got)
            if (g.second.suffix.size() == 2) eightWay = true;
        const int n = eightWay ? 8 : 4;
        const std::vector<std::string>& want = spriteDirectionSuffixes(n);
        std::vector<std::string> order;
        bool consecutive = true, oneSheet = true;
        for (size_t k = 0; k < got.size(); k++) {
            order.push_back(got[k].second.suffix);
            if (k && got[k].first != got[k - 1].first + 1) consecutive = false;
            if (got[k].second.sheet != got[0].second.sheet) oneSheet = false;
        }
        std::string missing;
        for (const std::string& s : want)
            if (std::find(order.begin(), order.end(), s) == order.end()) missing += (missing.empty() ? "" : ", ") + base + "_" + s;
        auto join = [&](const std::vector<std::string>& v) {
            std::string o;
            for (const std::string& s : v) o += (o.empty() ? "" : " ") + s;
            return o;
        };
        const std::string call = "Sprite.SetFacingFromYaw(sprite, \"" + base + "_n\", " + std::to_string(n) + ", yaw)";
        const std::string head = "sprite animations " + base + "_*: ";
        if (!missing.empty())
            warnings.push_back(head + "no " + missing + ", so " + call +
                               " would play the wrong animation for some directions: add the missing ones");
        else if (order.size() != want.size())
            warnings.push_back(head + "a direction appears more than once, so " + call + " would play the wrong animation");
        else if (order != want)
            warnings.push_back(head + "listed in the order " + join(order) + ", so " + call +
                               " would play the wrong animation: list them " + join(want));
        else if (!consecutive)
            warnings.push_back(head + "other animations sit between them in the table" +
                               (oneSheet ? "" : " (they are split across sprite sheets)") + ", so " + call +
                               " would play the wrong animation: keep them one after another in one sheet");
        else if (!oneSheet)
            warnings.push_back(head + "split across sprite sheets '" + sheets[got.front().second.sheet].name + "' and '" +
                               sheets[got.back().second.sheet].name + "'. " + call +
                               " works only while nothing is added between them: keep a set in one sheet");
    }
}

std::string spriteSheetPageError(const std::string& name, int u0, int v0, int width, int height) {
    if (u0 >= 0 && v0 >= 0 && u0 + width <= 256 && v0 + height <= 256) return {};
    return "sprite sheet '" + name + "': packed at " + std::to_string(u0) + "," + std::to_string(v0) +
           " in its texture page, its " + std::to_string(width) + "x" + std::to_string(height) +
           " texels run past the page's 256x256: use a smaller image or a lower bit depth";
}

// ---- tilemap (v23; psxsplash tilesystem.cpp SPLASHPACKTilemap, tilemath.hh TileCell/TileObject)

int tilesetIndex(const Tilemap& t, const std::vector<SpriteSheet>& sheets) {
    if (t.tileset.empty()) return -1;
    for (size_t i = 0; i < sheets.size(); i++)
        if (sheets[i].name == t.tileset) return int(i);
    return -1;
}

FlatTilemap flattenTilemap(const Tilemap& t) {
    FlatTilemap out;
    out.cells.assign(size_t(t.width * t.height) * 2, 0);
    std::map<int, int> nextId;  // kind -> the id the next auto-numbered object of it gets
    for (int y = 0; y < t.height; y++)
        for (int x = 0; x < t.width; x++) {
            const int bi = t.at(x, y);
            const size_t c = size_t(y * t.width + x) * 2;
            const TileBrush* b = bi >= 0 && bi < int(t.brushes.size()) ? &t.brushes[size_t(bi)] : nullptr;
            if (!b || b->cell < 0) {
                out.cells[c] = 0xFF;
                continue;
            }
            out.cells[c] = uint8_t(std::clamp(b->cell, 0, 254));
            out.cells[c + 1] = b->walkable ? 1 : 0;
            if (b->kind == 0) continue;
            const int kind = std::clamp(b->kind, 0, 255);
            int id = b->id;
            if (id < 0) {
                auto [it, fresh] = nextId.emplace(kind, 1);
                id = it->second++;
            }
            out.objects.push_back({uint8_t(kind), uint8_t(std::clamp(id, 0, 255)), uint16_t(x), uint16_t(y)});
        }
    return out;
}

void checkTilemap(const Tilemap& t, const std::vector<SpriteSheet>& sheets, const std::function<int(size_t)>& sheetCells,
                  std::vector<std::string>& errors) {
    const int sheet = tilesetIndex(t, sheets);
    if (t.tileset.empty()) errors.push_back("The tilemap has no tileset: pick the sprite sheet its tiles are drawn from");
    else if (sheet < 0) errors.push_back("The tilemap's tileset '" + t.tileset + "' is not one of the scene's sprite sheets: pick one");
    const int cells = sheet >= 0 ? sheetCells(size_t(sheet)) : -1;
    for (size_t i = 0; i < t.brushes.size(); i++) {
        const TileBrush& b = t.brushes[i];
        const std::string where = "Tile brush " + (b.name.empty() ? std::to_string(i + 1) : "'" + b.name + "'");
        if (b.cell < -1) errors.push_back(where + " draws tileset cell " + std::to_string(b.cell) + ": use -1 for a hole");
        else if (cells >= 0 && b.cell >= cells)
            errors.push_back(where + " draws tileset cell " + std::to_string(b.cell) + ", but '" + t.tileset + "' has " +
                             std::to_string(cells) + " (0.." + std::to_string(cells - 1) + ")");
    }
    size_t missing = 0;
    for (int c : t.cells)
        if (c >= int(t.brushes.size())) missing++;
    if (missing)
        errors.push_back(std::to_string(missing) + (missing == 1 ? " tilemap cell is" : " tilemap cells are") +
                         " painted with a brush that no longer exists: repaint or erase them");
    const size_t objects = flattenTilemap(t).objects.size();
    if (objects > size_t(kMaxTileObjects))
        errors.push_back(std::to_string(objects) + " tilemap object tiles, psxsplash holds at most " + std::to_string(kMaxTileObjects) +
                         ": paint fewer cells with brushes that have a kind");
}

namespace {

// The sprite table: `count` 20-byte sheet records, then 12-byte animation
// records, then their NUL-terminated names (absolute offsets). Each sheet's
// texture is placed already. Returns the table's size from its start.
size_t writeSpriteTable(BinWriter& w, size_t tableOffsetPos, const std::vector<SpriteSheet>& sheets,
                        const std::vector<PsxTexture*>& textures) {
    w.align4();
    const size_t start = w.pos();
    w.patchU32(tableOffsetPos, uint32_t(start));
    std::vector<size_t> namePos;
    std::vector<std::string> names;
    for (size_t i = 0; i < sheets.size(); i++) {
        const SpriteSheet& s = sheets[i];
        const PsxTexture& t = *textures[i];
        const int u0 = t.packingX * (16 / int(t.bitDepth)), v0 = t.packingY;
        namePos.push_back(w.pos());
        names.push_back(truncateUtf16(s.name, 24));
        w.u32(0);
        w.u8(t.texpageX);
        w.u8(t.texpageY);
        w.u8(uint8_t(u0));
        w.u8(uint8_t(v0));
        w.u16(t.clutPackingX);
        w.u16(t.clutPackingY);
        w.u8(uint8_t(s.cellW));
        w.u8(uint8_t(s.cellH));
        w.u8(uint8_t(t.width / s.cellW));
        w.u8(uint8_t(t.height / s.cellH));
        w.u8(t.bitDepth == BitDepth::Bpp4 ? 0 : t.bitDepth == BitDepth::Bpp8 ? 1 : 2);
        w.u8(0);
        w.u16(0);
    }
    for (size_t i = 0; i < sheets.size(); i++)
        for (const SpriteAnim& a : sheets[i].anims) {
            namePos.push_back(w.pos());
            names.push_back(truncateUtf16(a.name, 24));
            w.u32(0);
            w.u8(uint8_t(i));
            w.u8(uint8_t(a.firstFrame));
            w.u8(uint8_t(a.frameCount));
            w.u8(uint8_t(a.frameDuration));
            w.u8(a.loop ? 1 : 0);
            w.u8(0);
            w.u16(0);
        }
    for (size_t i = 0; i < names.size(); i++) {
        w.patchU32(namePos[i], uint32_t(w.pos()));
        w.bytes(names[i].data(), names[i].size());
        w.u8(0);
    }
    return w.pos() - start;
}

// The 464-byte SPLASHPACKMemcard record: region[2], product[10], title[32]
// (zero padded), u8 frameCount, u8 0, u16 0, u16 clut[16], u8 pixels[3][128]
// (16x16 4bpp, top row first, low nibble = left pixel). Empty on errors.
template <typename ImageFor>
std::vector<uint8_t> memCardTable(const MemCardSettings& mc, ImageFor& imageFor, ExportResult& res) {
    checkMemCard(mc, res.errors, res.warnings);
    // The frames are quantised as one image stacked top to bottom, so they
    // share the one palette the record holds.
    const int frames = int(std::min(mc.icons.size(), kMemCardIconsMax));
    const int n = kMemCardIconSize;
    Image stack;
    stack.width = n;
    stack.height = n * frames;
    stack.pixels.resize(size_t(n) * size_t(stack.height));
    for (int f = 0; f < frames; f++) {
        const std::string& path = mc.icons[size_t(f)];
        try {
            const Image& img = imageFor(path);
            if (img.width != n || img.height != n) {
                res.errors.push_back("Memory card icon " + path + " is " + std::to_string(img.width) + "x" +
                                     std::to_string(img.height) + ", the BIOS icon is 16x16: resize it");
                continue;
            }
            // Image rows are bottom-up: frame f's top row r lands at stack row f*16 + r from the top.
            for (int r = 0; r < n; r++)
                for (int x = 0; x < n; x++) {
                    const Image::Px& p = img.pixels[size_t(n - 1 - r) * n + x];
                    stack.pixels[size_t(stack.height - 1 - (f * n + r)) * n + x] = p;
                }
        } catch (const std::exception& ex) {
            res.errors.push_back("Memory card icon: " + std::string(ex.what()) + ": pick an image that exists");
        }
    }
    if (!res.ok()) return {};

    BinWriter t;
    auto fixed = [&](const std::string& s, size_t len) {
        for (size_t i = 0; i < len; i++) t.u8(i < s.size() ? uint8_t(s[i] & 0x7f) : 0);
    };
    fixed(mc.region, kMemCardRegionLen);
    fixed(mc.product, kMemCardProductMax);
    fixed(mc.title, kMemCardTitleMax);
    t.u8(uint8_t(std::max(frames, 1)));
    t.u8(0);
    t.u16(0);
    PsxTexture icon;
    // Transparent texels take palette entry 0 (word 0, which the GPU skips),
    // and only when the icon has any; otherwise all 16 entries are colours.
    if (frames) icon = convertTexture(stack, BitDepth::Bpp4, hasCutoutTexels(stack));
    for (size_t i = 0; i < 16; i++) t.u16(i < icon.palette.size() ? icon.palette[i] : 0);
    for (int f = 0; f < int(kMemCardIconsMax); f++)
        for (int row = 0; row < n; row++)
            for (int g = 0; g < n / 4; g++) t.u16(f < frames ? icon.word(g, f * n + row) : 0);
    return t.data();
}

}  // namespace

ExportResult exportSplashpack(const Scene& scene, const fs::path& root, const fs::path& out,
                              const ExportOptions& options) {
    ExportResult res;
    const float gte = scene.settings.gteScaling;
    std::vector<FlatObject> flat = flatten(scene);

    std::map<std::string, std::unique_ptr<Mesh>> meshes;
    std::map<std::string, Image> images;
    std::deque<PsxTexture> textureStore;

    auto meshFor = [&](const std::string& p) -> const Mesh* {
        auto it = meshes.find(p);
        if (it == meshes.end()) it = meshes.emplace(p, std::make_unique<Mesh>(loadMesh(root / p))).first;
        return it->second.get();
    };
    auto imageFor = [&](const std::string& p) -> const Image& {
        auto it = images.find(p);
        if (it == images.end()) it = images.emplace(p, loadImage(root / p)).first;
        return it->second;
    };

    // Memory card table; its errors stop the export with the others below.
    std::vector<uint8_t> memCard;
    if (scene.settings.memCard.enabled) memCard = memCardTable(scene.settings.memCard, imageFor, res);

    // Every light bakes into meshes that are not lit at runtime; a runtime-lit
    // mesh bakes only the non-runtime ones, so nothing is counted twice.
    std::vector<SceneLight> lights, bakedOnlyLights;
    std::vector<RuntimeLight> runtimeLights;
    for (const FlatObject& fo : flat) {
        // Lights on an off object, or under an off parent, neither bake nor ship
        // (Unity's FindObjectsByType<Light> skips them).
        if (!fo.object->light || !fo.activeInHierarchy) continue;
        const LightComponent& l = *fo.object->light;
        // Disabled runtime lights on an active object are still exported so Lua
        // can switch them on.
        if (l.runtime) runtimeLights.push_back({&fo, &l});
        if (!l.enabled) continue;
        SceneLight sl = sceneLight(fo);
        lights.push_back(sl);
        if (!l.runtime) bakedOnlyLights.push_back(sl);
    }
    if (runtimeLights.size() > kMaxSceneLights) {
        std::string dropped;
        for (size_t i = kMaxSceneLights; i < runtimeLights.size(); i++)
            dropped += (dropped.empty() ? "" : ", ") + runtimeLights[i].flat->object->name;
        res.warnings.push_back("the scene has " + std::to_string(runtimeLights.size()) +
                               " runtime point lights and the PS1 holds " + std::to_string(kMaxSceneLights) +
                               "; left out: " + dropped);
        runtimeLights.resize(kMaxSceneLights);
    }

    std::vector<ExpObject> exporters;
    for (const FlatObject& fo : flat) {
        // An object that is off, or under an off parent, is not in the pack at
        // all (Unity's FindObjectsByType skips it), so its components are not
        // checked either.
        if (!fo.activeInHierarchy) continue;
        if (fo.object->skin && !fo.object->mesh)
            res.errors.push_back(fo.object->name + ": a skin component needs a mesh component");
        if (!fo.object->mesh) {
            // A script needs a game object to run on, and an agent is one (its
            // actor id is its game object index + 1). Without a mesh it ships as
            // one with no triangles, which the engine draws as nothing.
            if (fo.object->script || fo.object->agent) {
                static const MeshComponent noMeshComponent = [] {
                    MeshComponent mc;
                    mc.dynamicLighting = DynamicLighting::Off;
                    return mc;
                }();
                static const Mesh noMesh{};
                exporters.push_back(ExpObject{&fo, fo.object, &noMeshComponent, &noMesh, {}, {}, {}, {}, {}, {}});
            }
            continue;
        }
        ExpObject e{&fo, fo.object, &*fo.object->mesh, nullptr, {}, {}, {}, {}, {}, {}};
        try {
            e.mesh = meshFor(e.mc->mesh);
        } catch (const std::exception& ex) {
            res.errors.push_back(ex.what());
            continue;
        }
        // PSXObjectExporter.CreatePSXTextures2D: one texture per textured
        // material, reusing the object's own copy for a repeated source.
        std::map<std::string, PsxTexture*> cache;
        for (const Material& mat : e.mc->materials) {
            if (mat.texture.empty()) continue;
            auto it = cache.find(mat.texture);
            if (it != cache.end()) {
                e.textures.push_back(it->second);
                continue;
            }
            try {
                // Texels under half alpha become word 0, which the GPU
                // skips; an image without any converts exactly as before.
                const Image& img = imageFor(mat.texture);
                textureStore.push_back(convertTexture(img, e.mc->bitDepth, hasCutoutTexels(img)));
            } catch (const std::exception& ex) {
                res.errors.push_back(ex.what());
                continue;
            }
            textureStore.back().source = mat.texture;
            cache[mat.texture] = &textureStore.back();
            e.textures.push_back(&textureStore.back());
        }
        exporters.push_back(std::move(e));
    }
    if (!res.ok()) return res;

    if (!options.objectOrder.empty()) {
        auto rank = [&](const ExpObject& e) {
            auto it = std::find(options.objectOrder.begin(), options.objectOrder.end(), e.obj->name);
            return it - options.objectOrder.begin();
        };
        std::stable_sort(exporters.begin(), exporters.end(),
                         [&](const ExpObject& a, const ExpObject& b) { return rank(a) < rank(b); });
    }

    // Skinned meshes: validate, pick a bone per vertex, bake every clip.
    struct SkinOut {
        size_t exporter;
        std::vector<BakedClip> clips;
        size_t boneCount;
        std::vector<std::array<int16_t, 3>> bind;
        std::vector<std::string> names;
    };
    std::vector<SkinOut> skins;
    for (size_t i = 0; i < exporters.size(); i++) {
        ExpObject& e = exporters[i];
        if (!e.obj->skin) continue;
        const SkinComponent& sc = *e.obj->skin;
        std::string where = e.obj->name + ": ";
        if (!e.mesh->skin) {
            res.errors.push_back(where + "mesh " + e.mc->mesh + " has no skeleton");
            continue;
        }
        const MeshSkin& sk = *e.mesh->skin;
        if (sk.joints.size() > 64) {
            res.errors.push_back(where + std::to_string(sk.joints.size()) + " joints, psxsplash draws at most 64");
            continue;
        }
        if (sc.clips.empty()) {
            // psxsplash skips a skinned object with no clips in both render passes.
            res.errors.push_back(where + "a skinned mesh needs at least one clip, or it is never drawn");
            continue;
        }
        if (sc.clips.size() > 16) {
            res.errors.push_back(where + std::to_string(sc.clips.size()) + " clips, at most 16");
            continue;
        }
        if (skins.size() == 16) {
            res.errors.push_back(where + "more than 16 skinned meshes in the scene");
            continue;
        }
        SkinOut so{i, {}, sk.joints.size(), {}, {}};
        bool clamped = false;
        std::vector<std::string> names;
        for (const std::string& path : sc.clips) {
            AnimClip clip;
            try {
                clip = loadAnim(root / path);
            } catch (const std::exception& ex) {
                res.errors.push_back(ex.what());
                continue;
            }
            // The engine reads the name back for SkinnedAnim.Play; 2.4.0 cut it
            // at 24 characters, which made longer names unplayable.
            if (clip.name.empty() || truncateUtf16(clip.name, 24) != clip.name)
                res.errors.push_back(where + path + ": clip name must be 1..24 characters");
            if (std::find(names.begin(), names.end(), clip.name) != names.end())
                res.errors.push_back(where + "two clips are named '" + clip.name + "'");
            names.push_back(clip.name);
            std::vector<std::string> errs;
            BakedClip b = bakeClip(sk, clip, sc.fps, e.flat->lossyScale, gte, errs, clamped);
            for (auto& m : errs) res.errors.push_back(where + m);
            so.clips.push_back(std::move(b));
        }
        if (clamped)
            res.warnings.push_back(where + "bone values outside the 4.12 range were clamped (bone moves more than " +
                                   std::to_string(int(8 * gte)) + " units from its bind position?)");
        {
            std::vector<std::string> errs;
            so.bind = bindPositions(sk, e.flat->lossyScale, gte, errs, clamped);
            for (auto& m : errs) res.errors.push_back(where + m);
            for (const Joint& j : sk.joints) so.names.push_back(truncateUtf16(j.name, 24));
        }
        e.vertexBone = dominantJoints(sk);
        skins.push_back(std::move(so));
    }
    if (!res.ok()) return res;

    for (ExpObject& e : exporters) {
        if (runtimeLights.empty() || e.obj->skin) continue;  // the engine does not light skinned meshes
        size_t reaching = 0;
        std::string names;
        for (const RuntimeLight& rl : runtimeLights)
            if (lightReaches(rl, e.flat->localToWorld, e.mesh->bounds())) {
                reaching++;
                names += (names.empty() ? "" : ", ") + rl.flat->object->name;
            }
        switch (e.mc->dynamicLighting) {
            case DynamicLighting::On:
            case DynamicLighting::Smooth: e.dynamicLit = true; break;
            case DynamicLighting::Off: e.dynamicLit = false; break;
            case DynamicLighting::Auto: e.dynamicLit = reaching > 0; break;
        }
        e.dynamicLitSmooth = e.dynamicLit && e.mc->dynamicLighting == DynamicLighting::Smooth;
        if (e.dynamicLit && reaching > kMaxLightsPerMesh)
            res.warnings.push_back(e.obj->name + ": " + std::to_string(reaching) +
                                   " runtime point lights reach it and the PS1 lights a mesh with " +
                                   std::to_string(kMaxLightsPerMesh) + " (the first in scene order): " + names);
    }
    for (ExpObject& e : exporters) buildTris(e, gte, e.dynamicLit ? bakedOnlyLights : lights, res);
    if (!res.ok()) return res;

    // UI: image textures (packed after the object textures, as 2.4.0 does),
    // font sheets, and the checks the loader would otherwise fail silently.
    // Elements of a type this build does not know are left out.
    // The loading canvas goes in the loader pack (exportLoaderPack) instead.
    std::vector<UICanvas> canvases = scene.canvases;
    if (const std::string& lc = scene.settings.loadingCanvas; !lc.empty()) {
        auto it = std::find_if(canvases.begin(), canvases.end(), [&](const UICanvas& c) { return c.name == lc; });
        if (it == canvases.end())
            res.errors.push_back("loading screen: no canvas named '" + lc + "'");
        else
            canvases.erase(it);
    }
    stripUnknownElements(canvases, res);
    std::vector<std::vector<PsxTexture*>> uiTextures(canvases.size());
    std::vector<std::vector<int>> uiSheets(canvases.size());  // sheet-backed images: the sprite sheet's index, else -1
    std::vector<FontSheet> fontSheets;
    std::vector<std::array<int, 2>> fontVram;
    std::map<std::string, int> fontIndex;  // name -> 1-based index (0 = system font)
    prepareUi(scene, canvases, root, options.vram, imageFor, textureStore, uiTextures, uiSheets, true, fontSheets, fontVram,
              fontIndex, res);
    for (size_t ci = 0; ci < canvases.size(); ci++)
        for (const UIElement& e : canvases[ci].elements)
            if (e.type == UIElementType::Image) res.stats.uiImages.push_back({canvases[ci].name, e.name, e.sheet, e.cell});

    // Sprite sheets: checked, then each texture converted with cutout on (the
    // engine draws them as sprites over the scene), packed after the UI's.
    std::vector<PsxTexture*> spriteTextures(scene.spriteSheets.size(), nullptr);
    {
        auto sizeOf = [&](const SpriteSheet& s) -> SpriteTextureSize {
            try {
                const Image& img = imageFor(s.texture);
                return {img.width, img.height, {}};
            } catch (const std::exception& ex) {
                return {0, 0, ex.what()};
            }
        };
        const size_t before = res.errors.size();
        checkSpriteSheets(scene.spriteSheets, sizeOf, res.errors, res.warnings);
        if (scene.tilemap)
            checkTilemap(
                *scene.tilemap, scene.spriteSheets,
                [&](size_t i) {
                    const SpriteSheet& s = scene.spriteSheets[i];
                    const SpriteTextureSize sz = sizeOf(s);
                    if (!sz.error.empty() || s.cellW < 1 || s.cellH < 1) return -1;
                    return (sz.width / s.cellW) * (sz.height / s.cellH);
                },
                res.errors);
        if (res.errors.size() == before)
            for (size_t i = 0; i < scene.spriteSheets.size(); i++) {
                const SpriteSheet& s = scene.spriteSheets[i];
                try {
                    textureStore.push_back(convertTexture(imageFor(s.texture), s.bitDepth, true));
                } catch (const std::exception& ex) {
                    res.errors.push_back("sprite sheet '" + s.name + "': " + ex.what());
                    continue;
                }
                textureStore.back().source = s.texture;
                spriteTextures[i] = &textureStore.back();
            }
    }
    if (!res.ok()) return res;

    // VRAM packing over every object's texture list in object order.
    std::vector<PsxTexture*> all;
    for (ExpObject& e : exporters) all.insert(all.end(), e.textures.begin(), e.textures.end());
    for (auto& cv : uiTextures)
        for (PsxTexture* t : cv)
            if (t) all.push_back(t);
    for (PsxTexture* t : spriteTextures) all.push_back(t);
    // With fog on, psxsplash draws the fog over a 4bpp or 8bpp cutout texture
    // through a mask CLUT, so the texture's holes get no fog.
    bool fogMask4 = false, fogMask8 = false;
    if (scene.settings.fog.enabled)
        for (ExpObject& e : exporters)
            for (const PsxTexture* t : e.textures) {
                if (!t || !t->cutout || !t->hasPalette) continue;
                if (t->bitDepth == BitDepth::Bpp4) fogMask4 = true;
                if (t->bitDepth == BitDepth::Bpp8) fogMask8 = true;
            }
    VramLayout vram = packVram(all, options.vram, res.errors, fogMask4, fogMask8);
    // psxsplash steps through a sheet's cells with 8-bit texture coordinates
    // from its top-left corner, so the whole grid must sit in one 256x256 page.
    const bool packed = res.ok();  // a texture that did not fit keeps 0,0: no page to check
    for (size_t i = 0; packed && i < spriteTextures.size(); i++) {
        const PsxTexture& t = *spriteTextures[i];
        const std::string e = spriteSheetPageError(scene.spriteSheets[i].name, t.packingX * (16 / int(t.bitDepth)),
                                                   t.packingY, t.width, t.height);
        if (!e.empty()) res.errors.push_back(e);
    }
    {
        using K = ExportStats::VramKind;
        auto& map = res.stats.vramMap;
        const VramSettings& vs = options.vram;
        map.push_back({K::Framebuffer, 0, 0, vs.resolutionX, vs.resolutionY, "Framebuffer 0 (320x240)"});
        if (vs.verticalBuffering)
            map.push_back({K::Framebuffer, 0, 256, vs.resolutionX, vs.resolutionY, "Framebuffer 1 (320x240)"});
        else
            map.push_back({K::Framebuffer, vs.resolutionX, 0, vs.resolutionX, vs.resolutionY, "Framebuffer 1 (320x240)"});
        map.push_back({K::Reserved, 960, 0, 64, kVramHeight, "Font column (x 960, reserved)"});
        for (size_t i = 0; i < vs.prohibited.size(); i++) {
            const auto& a = vs.prohibited[i];
            map.push_back({K::Prohibited, a.x, a.y, a.w, a.h, "Prohibited area " + std::to_string(i + 1)});
        }
        for (const Atlas& a : vram.atlases) {
            K k = a.bitDepth == BitDepth::Bpp16 ? K::Atlas16 : a.bitDepth == BitDepth::Bpp8 ? K::Atlas8 : K::Atlas4;
            const char* bpp = a.bitDepth == BitDepth::Bpp16 ? "16" : a.bitDepth == BitDepth::Bpp8 ? "8" : "4";
            size_t n = a.textures.size();
            map.push_back({k, a.positionX, a.positionY, a.width, Atlas::kHeight,
                           std::string(bpp) + " bpp atlas, " + std::to_string(n) + (n == 1 ? " texture" : " textures")});
        }
        std::set<std::array<int, 3>> cluts;
        for (const Atlas& a : vram.atlases)
            for (const PsxTexture* t : a.textures) {
                if (!t->hasPalette || t->palette.empty()) continue;
                std::array<int, 3> key{t->clutPackingX * 16, int(t->clutPackingY), int(t->palette.size())};
                if (!cluts.insert(key).second) continue;
                // A CLUT that did not fit keeps (0,0), inside framebuffer 0.
                if (key[0] < vs.resolutionX && key[1] < vs.resolutionY) continue;
                map.push_back({K::Clut, key[0], key[1], key[2], 1, "CLUT of " + t->source});
            }
        if (vram.fogMaskClut4 >= 0)
            map.push_back({K::Clut, (vram.fogMaskClut4 & 63) * 16, vram.fogMaskClut4 >> 6, 16, 1, "Fog mask CLUT (4 bpp)"});
        if (vram.fogMaskClut8 >= 0)
            map.push_back({K::Clut, (vram.fogMaskClut8 & 63) * 16, vram.fogMaskClut8 >> 6, 256, 1, "Fog mask CLUT (8 bpp)"});
        std::vector<std::string> fontNames(fontSheets.size());
        for (const auto& [name, idx] : fontIndex)
            if (idx >= 1 && size_t(idx) <= fontNames.size()) fontNames[idx - 1] = name;
        for (size_t i = 0; i < fontSheets.size() && i < fontVram.size(); i++)
            map.push_back({K::Font, fontVram[i][0], fontVram[i][1], 64, fontSheets[i].height,
                           "Font '" + fontNames[i] + "'"});
    }
    if (!res.ok()) return res;

    // Remap each object's texture list to the packed (deduplicated) textures.
    std::map<std::string, PsxTexture*> packedBySource;
    for (Atlas& a : vram.atlases)
        for (PsxTexture* t : a.textures) packedBySource[t->source + "#" + std::to_string(int(t->bitDepth))] = t;
    for (ExpObject& e : exporters) {
        std::vector<int> remap(e.textures.size(), -1);
        for (size_t i = 0; i < e.textures.size(); i++) {
            auto it = packedBySource.find(e.textures[i]->source + "#" + std::to_string(int(e.textures[i]->bitDepth)));
            if (it == packedBySource.end()) continue;
            auto f = std::find(e.finalTextures.begin(), e.finalTextures.end(), it->second);
            if (f == e.finalTextures.end()) {
                e.finalTextures.push_back(it->second);
                f = e.finalTextures.end() - 1;
            }
            remap[i] = int(f - e.finalTextures.begin());
        }
        for (PsxTri& t : e.tris)
            if (t.textureIndex >= 0) t.textureIndex = remap[size_t(t.textureIndex)];
    }

    // Lua files: object scripts in object order, then the scene script.
    std::vector<std::string> luaFiles;
    auto addLua = [&](const std::string& p) {
        if (!p.empty() && std::find(luaFiles.begin(), luaFiles.end(), p) == luaFiles.end()) luaFiles.push_back(p);
    };
    // Trigger boxes and interactables: active objects only (FindObjectsByType
    // skips inactive ones), in canonical order.
    std::vector<const FlatObject*> triggers, interactables, audioSources, agents;
    for (const FlatObject& fo : flat) {
        if (!fo.activeInHierarchy || !fo.object->active) continue;
        if (fo.object->trigger) triggers.push_back(&fo);
        if (fo.object->interactable) interactables.push_back(&fo);
        if (fo.object->agent) {
            agents.push_back(&fo);
            // The skin's clips as baked above, for the plays-once warning.
            std::vector<AgentClipLoop> loaded;
            for (const SkinOut& so : skins)
                if (exporters[so.exporter].obj == fo.object)
                    for (const BakedClip& c : so.clips) loaded.push_back({c.name, c.loop});
            for (std::string& wmsg : agentWarnings(*fo.object, loaded)) res.warnings.push_back(std::move(wmsg));
        }
        if (fo.object->audio) audioSources.push_back(&fo);
    }
    // Audio clips: SPU-ADPCM as `psxavenc -t spu -f <rate> [-L]` writes it.
    std::vector<std::vector<uint8_t>> audioData;
    for (const FlatObject* fo : audioSources) {
        const AudioComponent& a = *fo->object->audio;
        std::vector<uint8_t> data;
        if (!a.clip.empty()) {
            try {
                MonoAudio src = loadAudioMono(root / a.clip);
                if (a.trimLeadingSilence) trimLeadingSilence(src);
                data = encodeSpuAdpcm(toPcm16(resample(src, a.sampleRate).samples), a.loop);
            } catch (const std::exception& ex) {
                res.errors.push_back(ex.what());
            }
        }
        audioData.push_back(std::move(data));
    }
    for (ExpObject& e : exporters)
        if (e.obj->script) addLua(e.obj->script->lua);
    addLua(scene.settings.script);
    for (const FlatObject* t : triggers) addLua(t->object->trigger->lua);
    std::vector<std::string> luaData, luaSources;
    for (const std::string& p : luaFiles) {
        try {
            std::string src = readFile(root / p);
            if (fs::path(p).extension() == ".blocks") src = blocksFileToLua(src, p);
            luaSources.push_back(src);
            if (options.luaBytecode) {
                std::vector<uint8_t> bc = compileLua(src, p);
                src.assign(bc.begin(), bc.end());
            }
            luaData.push_back(std::move(src));
        } catch (const std::exception& ex) {
            res.errors.push_back(ex.what());
        }
    }
    if (!res.ok()) return res;

    // Render buffer sizes go first: with streaming on, the ordering table is
    // the draw distance, which sets how near a region has to be loaded.
    ExportStats& st = res.stats;
    st.orderingTableNeed = orderingTableNeed(scene, exporters, gte);
    st.orderingTableSize = scene.settings.orderingTableSize > 0
                               ? uint32_t(std::clamp(scene.settings.orderingTableSize, int(kOtMin), int(kOtMax)))
                               : st.orderingTableNeed;

    // World streaming: splits exporters, so it runs before anything indexes them.
    const size_t sceneObjects = exporters.size();  // streaming appends the cut parts after these
    std::vector<bool> streamed(exporters.size(), false);
    std::vector<StreamRegionOut> streamRegions;
    if (scene.settings.streamWorld)
        planStreaming(scene, exporters, streamed, streamRegions, luaSources, interactables, gte, res);

    // BVH over active objects' exported triangles.
    std::vector<BvhInputObject> bvhIn;
    for (ExpObject& e : exporters) {
        // Over the exported Tris, which the engine's TriangleRef indexes into.
        BvhInputObject b{e.obj->active, e.flat->localToWorld, &e.triPositions, {}};
        for (size_t i = 0; i < e.triPositions.size(); i++) b.triangles.push_back(int(i));
        bvhIn.push_back(std::move(b));
    }
    Bvh bvh = buildBvh(bvhIn);
    st.bvhBoxes.clear();
    st.bvhDepth = 0;
    st.bvhBoxes.resize(bvh.nodes.size());
    for (size_t i = 0; i < bvh.nodes.size(); i++) {
        const BvhNode& n = bvh.nodes[i];
        ExportStats::BvhBox& b = st.bvhBoxes[i];
        b.lo = n.bounds.min();
        b.hi = n.bounds.max();
        b.leaf = n.left < 0 && n.right < 0;
        b.tris = n.triangleCount;
        st.bvhDepth = std::max(st.bvhDepth, b.depth);
        // Children come after their parent, so their depth is set before they are visited.
        for (int c : {n.left, n.right})
            if (c > int(i) && size_t(c) < bvh.nodes.size()) st.bvhBoxes[size_t(c)].depth = b.depth + 1;
    }

    // Rooms and portals, over the same triangles as the BVH.
    RoomObjects roomObjs = collectRoomObjects(flat, res.errors);
    if (!roomObjs.rooms.empty() && scene.settings.streamWorld)
        res.errors.push_back("Rooms and world streaming cannot be used together: remove the Room objects or turn "
                             "streaming off");
    if (!res.ok()) return res;
    std::vector<RoomInputObject> roomIn;
    for (ExpObject& e : exporters) roomIn.push_back({e.obj->active, e.flat->localToWorld, &e.triPositions});
    RoomSet rooms = buildRooms(roomObjs, roomIn, res.errors);
    if (!res.ok()) return res;
    for (size_t r = 0; r < rooms.realRooms(); r++)
        st.rooms.push_back({rooms.rooms[r].name, int(rooms.rooms[r].triRefCount), int(rooms.rooms[r].cellCount),
                            int(rooms.rooms[r].portalRefCount)});
    if (!rooms.empty()) st.roomCatchAllTris = int(rooms.rooms.back().triRefCount);
    st.roomPortals = int(rooms.portals.size());

    auto luaIndex = [&](const std::string& p) -> int16_t {
        auto it = std::find(luaFiles.begin(), luaFiles.end(), p);
        return it == luaFiles.end() ? int16_t(-1) : int16_t(it - luaFiles.begin());
    };

    int clutCount = 0;
    for (Atlas& a : vram.atlases)
        for (PsxTexture* t : a.textures)
            if (t->hasPalette) clutCount++;
    int colliderCount = 0;
    for (ExpObject& e : exporters)
        if (e.obj->collider && e.obj->collider->kind == ColliderKind::Dynamic) colliderCount++;

    // PSXSceneExporter: the first active PSXPlayer and PSXNavigationSettings
    // (FindObjectsByType skips inactive objects), in canonical order.
    const FlatObject* playerObj = nullptr;
    const FlatObject* navObj = nullptr;
    for (const FlatObject& fo : flat) {
        if (!fo.activeInHierarchy || !fo.object->active) continue;
        if (fo.object->player && !playerObj) playerObj = &fo;
        if (fo.object->navigation && !navObj) navObj = &fo;
    }
    Vec3 navSpawn{};
    if (navObj) {
        navSpawn = navObj->worldPosition;
        const std::string& anchor = navObj->object->navigation->spawnAnchor;
        if (!anchor.empty()) {
            auto it = std::find_if(flat.begin(), flat.end(),
                                   [&](const FlatObject& fo) { return fo.object->name == anchor; });
            if (it == flat.end()) {
                res.errors.push_back(navObj->object->name + ": spawnAnchor '" + anchor + "' not found");
                return res;
            }
            navSpawn = it->worldPosition;
        }
    }
    Vec3 playerPos{};
    Quat playerRot{};
    float playerHeight = 1.8f, playerRadius = 0.5f, moveSpeed = 3.f, sprintSpeed = 8.f, jumpHeight = 2.f,
          gravity = 20.f;
    if (playerObj) {
        const PlayerComponent& pc = *playerObj->object->player;
        playerPos = findCamPoint(playerObj->worldPosition, pc.playerHeight, exporters);
        playerRot = playerObj->worldRotation;
        playerHeight = pc.playerHeight;
        playerRadius = pc.playerRadius;
        moveSpeed = pc.moveSpeed;
        sprintSpeed = pc.sprintSpeed;
        jumpHeight = pc.jumpHeight;
        gravity = pc.gravity;
    } else if (navObj) {
        playerPos = navSpawn;
        playerHeight = navObj->object->navigation->agentHeight;
        playerRadius = navObj->object->navigation->agentRadius;
    }

    // Nav regions. Agent size comes from the player (or its defaults); the
    // navigation settings override everything when present.
    NavBuildParams np;
    np.agentRadius = playerRadius;
    np.agentHeight = playerHeight;
    const NavBakeSettings* bake = nullptr;
    if (navObj) {
        np.agentRadius = navObj->object->navigation->agentRadius;
        np.agentHeight = navObj->object->navigation->agentHeight;
        bake = &navObj->object->navigation->nav;
    } else if (playerObj) {
        bake = &playerObj->object->player->nav;
    }
    if (bake) {
        np.maxStepHeight = bake->maxStepHeight;
        np.walkableSlopeAngle = bake->walkableSlopeAngle;
        np.cellSize = bake->cellSize;
        np.cellHeight = bake->cellHeight;
        np.minRegionArea = bake->minRegionArea;
        np.mergeRegionArea = bake->mergeRegionArea;
        np.maxSimplifyError = bake->maxSimplifyError;
        np.maxEdgeLength = bake->maxEdgeLength;
        np.partition = bake->partition;
        np.detailSampleDist = bake->detailSampleDist;
        np.detailMaxError = bake->detailMaxError;
        np.maxPlaneError = bake->maxPlaneError;
    }
    std::vector<NavInputObject> navIn;
    // Over the scene's objects; parts cut out for streaming share their mesh.
    for (size_t oi = 0; oi < sceneObjects; oi++) {
        const ExpObject& e = exporters[oi];
        NavInputObject n{e.obj->collider && e.obj->collider->kind == ColliderKind::Static,
                         e.obj->collider && e.obj->collider->platform,
                         e.flat->localToWorld,
                         &e.mesh->positions,
                         {},
                         e.mesh->bounds()};
        for (const auto& sm : e.mesh->submeshes) n.triangles.insert(n.triangles.end(), sm.begin(), sm.end());
        navIn.push_back(std::move(n));
    }
    // Walk-off zones: active objects only, like trigger boxes.
    std::vector<Bounds> walkoffBoxes;
    for (size_t i = 0; i < flat.size(); i++) {
        const FlatObject& fo = flat[i];
        if (!fo.activeInHierarchy || !fo.object->active || !fo.object->walkoff) continue;
        Vec3 lo, hi;
        walkoffWorldBox(fo, &lo, &hi);
        Bounds b;
        b.setMinMax(lo, hi);
        walkoffBoxes.push_back(b);
        st.walkoffZones.push_back({fo.object->name, int(i), 0});
    }
    NavMesh nav = buildNavRegions(navIn, np, playerObj ? playerPos : navObj ? navSpawn : playerPos, res.errors,
                                  roomBoxes(roomObjs), walkoffBoxes);
    for (size_t z = 0; z < st.walkoffZones.size() && z < nav.walkoffEdgesPerZone.size(); z++)
        st.walkoffZones[z].edges = nav.walkoffEdgesPerZone[z];
    if (!res.ok()) return res;
    if (nav.regions.size() > 65535 || nav.portals.size() > 65535) {
        res.errors.push_back("too many nav regions or portals");
        return res;
    }
    st.navWanted = playerObj || navObj;
    st.navRegions = int(nav.regions.size());
    st.navCellSizeAsked = np.cellSize;
    st.navMaxPlaneError = np.maxPlaneError;
    st.navCellSize = nav.cellSize;
    st.navExtentX = nav.extentX;
    st.navExtentZ = nav.extentZ;
    for (const NavRegion& r : nav.regions) {
        std::vector<Vec3>& o = st.navOutlines.emplace_back();
        for (const Vec2& v : r.verts) o.push_back({v.x, r.planeA * v.x + r.planeB * v.y + r.planeD, v.y});
        ExportStats::NavRegionInfo& info = st.navInfo.emplace_back();
        float cx = 0, cz = 0;
        for (const Vec2& v : r.verts) {
            cx += v.x;
            cz += v.y;
        }
        if (!r.verts.empty()) {
            cx /= float(r.verts.size());
            cz /= float(r.verts.size());
        }
        info.centroid = {cx, r.planeA * cx + r.planeB * cz + r.planeD, cz};
        info.floorY = info.centroid.y;
        info.room = r.roomIndex == 0xFF ? -1 : int(r.roomIndex);
        info.surface = int(r.surfaceType);
        info.platform = (r.flags & 1) != 0;
        info.walkoffEdges = r.walkoffEdgeMask;
        info.planeDeviation = r.maxPlaneDeviation;
        for (int p = r.portalStart; p < r.portalStart + r.portalCount; p++) {
            const NavPortal& np2 = nav.portals[size_t(p)];
            info.portals.push_back({np2.neighborRegion, np2.heightDelta,
                                    {np2.a.x, r.planeA * np2.a.x + r.planeB * np2.a.y + r.planeD, np2.a.y},
                                    {np2.b.x, r.planeA * np2.b.x + r.planeB * np2.b.y + r.planeD, np2.b.y}});
        }
    }
    st.navPortals = int(nav.portals.size());
    st.navIslands = roomBoxes(roomObjs).empty();
    if (st.navWanted && nav.regions.empty())
        res.warnings.push_back(navEmptyWarning());
    if (!agents.empty() && nav.regions.empty())
        res.warnings.push_back(agentNoNavWarning(agents.size() == 1 ? agents[0]->object->name : std::string()));

    BinWriter w;
    st.bumpAllocatorNeed = bumpAllocatorNeed(scene, exporters, streamed, st.streamDrawTris, canvases);
    st.trianglesInView = st.streamDrawTris;
    for (size_t i = 0; i < exporters.size(); i++)
        if (!streamed[i]) st.trianglesInView += exporters[i].tris.size();
    st.bumpAllocatorSize = scene.settings.bumpAllocatorSize > 0 ? (uint32_t(scene.settings.bumpAllocatorSize) + 3) & ~3u
                                                                 : st.bumpAllocatorNeed;

    // ---- header (156 bytes, v25)
    bool hasLights = !runtimeLights.empty();
    w.u8('S');
    w.u8('P');
    w.u16(27);
    w.u16(uint16_t(luaFiles.size()));
    w.u16(uint16_t(exporters.size()));
    res.stats.gameObjects = int(exporters.size());
    w.u16(uint16_t(vram.atlases.size()));
    w.u16(uint16_t(clutCount));
    w.u16(uint16_t(colliderCount));
    w.u16(uint16_t(interactables.size()));
    w.i16(toPsxCoord(playerPos.x, gte));
    w.i16(toPsxCoord(-playerPos.y, gte));
    w.i16(toPsxCoord(playerPos.z, gte));
    // Euler angles in 4.12 units of pi: the engine casts these to psyqo::Angle.
    // (2.4.0 writes radians, so any start yaw other than 0 faces the wrong way.)
    Vec3 euler = eulerAngles(playerRot);
    w.i16(toFixed12(euler.x / 180.f));
    w.i16(toFixed12(euler.y / 180.f));
    w.i16(toFixed12(euler.z / 180.f));
    w.u16(uint16_t(toPsxCoord(playerHeight, gte)));
    w.i16(scene.settings.script.empty() ? int16_t(-1) : luaIndex(scene.settings.script));
    w.u16(uint16_t(std::min<size_t>(bvh.nodes.size(), 65535)));
    res.stats.bvhTriangleRefs = uint32_t(std::min<size_t>(bvh.refs.size(), 65535));
    w.u16(uint16_t(res.stats.bvhTriangleRefs));
    w.u16(uint16_t(scene.settings.sceneType));
    w.u16(uint16_t(triggers.size()));
    w.u16(0);  // world collision mesh count (removed)
    w.u16(0);  // world collision tri count (removed)
    w.u16(uint16_t(nav.regions.size()));
    w.u16(uint16_t(nav.portals.size()));
    {
        const float fps = 30.f;
        float movePerFrame = moveSpeed / fps / gte;
        float sprintPerFrame = sprintSpeed / fps / gte;
        w.u16(uint16_t(clampv(roundToInt(movePerFrame * 4096.f), 0, 65535)));
        w.u16(uint16_t(clampv(roundToInt(sprintPerFrame * 4096.f), 0, 65535)));
        float jumpVel = float(std::sqrt(double(2.f * gravity * jumpHeight))) / gte;
        w.u16(uint16_t(clampv(roundToInt(jumpVel * 4096.f), 0, 65535)));
        float grav = gravity / gte;
        w.u16(uint16_t(clampv(roundToInt(grav * 4096.f), 0, 65535)));
        w.u16(uint16_t(toPsxCoord(playerRadius, gte)));
        w.u16(uint16_t(std::max(vram.fogMaskClut4, 0)));  // was padding: 0 = no 4bpp fog mask
    }
    size_t nameTableOffsetPos = w.pos();
    w.u32(0);
    w.u16(uint16_t(audioSources.size()));
    w.u16(uint16_t(std::max(vram.fogMaskClut8, 0)));  // was padding: 0 = no 8bpp fog mask
    size_t audioTableOffsetPos = w.pos();
    w.u32(0);
    const FogSettings& fog = scene.settings.fog;
    w.u8(fog.enabled ? 1 : 0);
    for (float c : fog.color) w.u8(uint8_t(clampv(roundToInt(c * 255.f), 0, 255)));
    w.u8(uint8_t(clampv(fog.density, 1, 10)));
    w.u8(0);
    w.u16(uint16_t(rooms.rooms.size()));  // real rooms + the catch-all, or 0
    w.u16(uint16_t(rooms.portals.size()));
    w.u16(uint16_t(rooms.refs.size()));
    w.u16(uint16_t(scene.cutscenes.size()));
    w.u16(uint16_t(rooms.cells.size()));
    size_t cutsceneTableOffsetPos = w.pos();
    w.u32(0);
    w.u16(uint16_t(canvases.size()));
    w.u8(uint8_t(fontSheets.size()));
    w.u8(0);
    size_t uiTableOffsetPos = w.pos();
    w.u32(0);
    w.u32(0);  // pixel data offset
    w.u16(uint16_t(scene.animations.size()));
    w.u16(uint16_t(rooms.portalRefs.size()));
    size_t animationTableOffsetPos = w.pos();
    w.u32(0);
    w.u16(uint16_t(skins.size()));
    w.u16(uint16_t(agents.size()));
    size_t skinTableOffsetPos = w.pos();
    w.u32(0);
    size_t memcardTableOffsetPos = w.pos();
    w.u32(0);
    size_t streamTableOffsetPos = w.pos();
    w.u32(0);
    const size_t spriteTableOffsetPos = w.pos();
    w.u32(0);  // sprite table offset, patched when the scene has sheets
    size_t spriteAnimCount = 0;
    for (const SpriteSheet& s : scene.spriteSheets) spriteAnimCount += s.anims.size();
    w.u16(uint16_t(scene.spriteSheets.size()));
    w.u16(uint16_t(spriteAnimCount));
    w.u32(hashSceneId(scene.settings.networkId));
    const size_t tilemapTableOffsetPos = w.pos();
    w.u32(0);  // tilemap table offset, patched when the scene has a tilemap
    size_t lightTableOffsetPos = w.pos();
    w.u32(0);
    w.u32(st.orderingTableSize);
    w.u32(st.bumpAllocatorSize);

    // ---- Lua metadata
    std::vector<size_t> luaOffsetPos;
    for (const std::string& d : luaData) {
        luaOffsetPos.push_back(w.pos());
        w.u32(0);
        w.u32(uint32_t(d.size()));
    }

    // ---- game objects (92 bytes)
    std::vector<size_t> meshOffsetPos;
    for (size_t oi = 0; oi < exporters.size(); oi++) {
        ExpObject& e = exporters[oi];
        meshOffsetPos.push_back(w.pos());
        w.u32(0);
        Vec3 pos = e.flat->localToWorld.position();
        w.i32(toWorldFixed12(pos.x / gte));
        w.i32(toWorldFixed12(-pos.y / gte));
        w.i32(toWorldFixed12(pos.z / gte));
        writeRotation(w, e.flat->worldRotation);
        // A streamed object draws nothing until the stream table attaches its region.
        w.u16(streamed[oi] ? 0 : uint16_t(e.tris.size()));
        w.i16(e.obj->script ? luaIndex(e.obj->script->lua) : int16_t(-1));
        w.u32((e.obj->startActive ? 1u : 0u) | (e.obj->skin ? 0x10u : 0u) | (hasLights && e.dynamicLit ? 0x10000u : 0u) |
              (hasLights && e.dynamicLitSmooth ? 0x20000u : 0u));
        {
            auto it = std::find_if(interactables.begin(), interactables.end(),
                                   [&](const FlatObject* fo) { return fo->object == e.obj; });
            w.u16(it == interactables.end() ? 0xFFFF : uint16_t(it - interactables.begin()));
        }
        w.u16(0);       // uv offset (legacy)
        w.u32(0);       // event mask
        writeWorldAabb(w, e.flat->localToWorld, e.pieceBounds ? *e.pieceBounds : e.mesh->bounds(), gte);
    }

    // ---- colliders (32 bytes, dynamic only)
    for (size_t i = 0; i < exporters.size(); i++) {
        ExpObject& e = exporters[i];
        if (!e.obj->collider || e.obj->collider->kind != ColliderKind::Dynamic) continue;
        writeWorldAabb(w, e.flat->localToWorld, e.mesh->bounds(), gte);
        w.u8(1);
        w.u8(0xFF);
        w.u16(uint16_t(i));
        w.u32(0);
    }

    // ---- trigger boxes (32 bytes): world AABB of the transformed box
    for (const FlatObject* fo : triggers) {
        const TriggerComponent& t = *fo->object->trigger;
        Vec3 mn, mx;
        triggerWorldBox(*fo, &mn, &mx);
        w.i32(toWorldFixed12(mn.x / gte));
        w.i32(toWorldFixed12(-mx.y / gte));
        w.i32(toWorldFixed12(mn.z / gte));
        w.i32(toWorldFixed12(mx.x / gte));
        w.i32(toWorldFixed12(-mn.y / gte));
        w.i32(toWorldFixed12(mx.z / gte));
        w.i16(t.lua.empty() ? int16_t(-1) : luaIndex(t.lua));
        w.u16(0);
        w.u32(0);
    }

    // ---- BVH
    w.align4();
    for (const BvhNode& n : bvh.nodes) {
        Vec3 mn = n.bounds.min(), mx = n.bounds.max();
        w.i32(toWorldFixed12(mn.x / gte));
        w.i32(toWorldFixed12(-mx.y / gte));
        w.i32(toWorldFixed12(mn.z / gte));
        w.i32(toWorldFixed12(mx.x / gte));
        w.i32(toWorldFixed12(-mn.y / gte));
        w.i32(toWorldFixed12(mx.z / gte));
        w.u16(n.left >= 0 ? uint16_t(n.left) : 0xFFFF);
        w.u16(n.right >= 0 ? uint16_t(n.right) : 0xFFFF);
        w.u16(n.firstTriangle >= 0 ? uint16_t(n.firstTriangle) : 0);
        w.u16(uint16_t(n.triangleCount));
    }
    for (const TriangleRef& r : bvh.refs) {
        w.u16(r.objectIndex);
        w.u16(r.triangleIndex);
    }

    // ---- interactables (32 bytes)
    w.align4();
    for (const FlatObject* fo : interactables) {
        const InteractableComponent& it = *fo->object->interactable;
        // 2.4.0 needs a PSXObjectExporter on the same object; without a mesh
        // here the index is 0xFFFF.
        auto ex = std::find_if(exporters.begin(), exporters.end(),
                               [&](const ExpObject& e) { return e.obj == fo->object; });
        w.i32(toWorldFixed12(it.radius * it.radius / (gte * gte)));
        w.u8(uint8_t(it.button));
        w.u8(uint8_t((it.repeatable ? 1 : 0) | (it.showPrompt ? 2 : 0) | (it.lineOfSight ? 4 : 0)));
        w.u16(it.cooldownFrames);
        w.u16(0);  // current cooldown (runtime)
        w.u16(ex == exporters.end() ? 0xFFFF : uint16_t(ex - exporters.begin()));
        char name[16] = {};
        std::memcpy(name, it.promptCanvas.data(), std::min<size_t>(it.promptCanvas.size(), 15));
        for (char ch : name) w.u8(uint8_t(ch));
        // v27: fp12 cosine of the facing half-angle, then 2 reserved bytes.
        float facing = clampv(it.facingAngle, 0.0f, 180.0f) * 3.14159265358979f / 180.0f;
        w.i16(int16_t(roundToInt(std::cos(facing) * 4096.0f)));
        w.u16(0);
    }

    // ---- agents (28 bytes each, right after the interactables), then every
    // agent's waypoints in the same order
    {
        std::vector<AgentRecord> recs;
        for (const FlatObject* fo : agents) {
            auto ex = std::find_if(exporters.begin(), exporters.end(), [&](const ExpObject& e) { return e.obj == fo->object; });
            const uint16_t go = uint16_t(ex - exporters.begin());
            const int clips = fo->object->skin ? int(fo->object->skin->clips.size()) : 0;
            recs.push_back(agentRecord(*fo->object->agent, go, clips, gte));
            w.bytes(recs.back().bytes.data(), recs.back().bytes.size());
            st.agents.push_back({fo->object->name, int(fo - flat.data()), int(go), int(recs.back().waypoints.size()),
                                 agentBytes(*fo->object->agent)});
            std::copy_n(recs.back().bytes.begin() + 16, kAgentStates, st.agents.back().clips.begin());
        }
        for (const AgentRecord& r : recs)
            for (const auto& p : r.waypoints)
                for (int32_t v : p) w.i32(v);
    }

    // ---- nav regions
    if (!nav.regions.empty()) {
        w.align4();
        writeNavRegions(w, nav, gte);
    }

    // ---- rooms and portals
    if (!rooms.empty()) {
        w.align4();
        st.roomBlockOffset = w.pos();
        writeRooms(w, rooms, gte);
    }

    // ---- atlas + CLUT metadata (the reader skips the contents)
    for (Atlas& a : vram.atlases) {
        w.u32(0);
        w.u16(uint16_t(a.width));
        w.u16(uint16_t(Atlas::kHeight));
        w.u16(uint16_t(a.positionX));
        w.u16(uint16_t(a.positionY));
    }
    for (Atlas& a : vram.atlases)
        for (PsxTexture* t : a.textures) {
            if (!t->hasPalette) continue;
            w.u32(0);
            w.u16(t->clutPackingX);
            w.u16(t->clutPackingY);
            w.u16(uint16_t(t->palette.size()));
            w.u16(0);
        }

    // ---- Lua data
    for (size_t i = 0; i < luaData.size(); i++) {
        w.align4();
        w.patchU32(luaOffsetPos[i], uint32_t(w.pos()));
        w.bytes(luaData[i]);
    }

    // ---- mesh data (52-byte Tri). Streamed objects' triangles go to the .geo
    // file instead and their offset stays 0.
    // Tri flag 0x2: a paletted cutout texture whose depth has a fog mask CLUT.
    auto writeTri = [&vram](BinWriter& w, const ExpObject& e, const PsxTri& t) {
        for (const PsxVert& v : t.v) {
            w.i16(v.vx);
            w.i16(v.vy);
            w.i16(v.vz);
        }
        w.i16(t.v[0].nx);
        w.i16(t.v[0].ny);
        w.i16(t.v[0].nz);
        for (const PsxVert& v : t.v) {
            w.u8(v.r);
            w.u8(v.g);
            w.u8(v.b);
            w.u8(0);
        }
        uint16_t flags = e.mc->uvOffsetMaterial == t.textureIndex ? 1 : 0;
        if (t.textureIndex < 0) {
            for (int i = 0; i < 6; i++) w.u8(0);
            w.u16(0);
            w.u16(0xFFFF);
            w.u16(0);
            w.u16(0);
            w.u16(flags);
        } else {
            const PsxTexture* tex = e.finalTextures[size_t(t.textureIndex)];
            int expander = tex->bitDepth == BitDepth::Bpp4 ? 4 : tex->bitDepth == BitDepth::Bpp8 ? 2 : 1;
            for (const PsxVert& v : t.v) {
                w.u8(uint8_t(v.u + tex->packingX * expander));
                w.u8(uint8_t(v.v + tex->packingY));
            }
            w.u16(0);
            uint16_t mode = tex->bitDepth == BitDepth::Bpp4 ? 0 : tex->bitDepth == BitDepth::Bpp8 ? 1 : 2;
            uint16_t tpage = uint16_t((tex->texpageX & 0xF) | ((tex->texpageY & 1) << 4) | (mode << 7) | 0x200);
            w.u16(tpage);
            w.u16(tex->clutPackingX);
            w.u16(tex->clutPackingY);
            if (tex->cutout && tex->hasPalette &&
                ((tex->bitDepth == BitDepth::Bpp4 && vram.fogMaskClut4 >= 0) ||
                 (tex->bitDepth == BitDepth::Bpp8 && vram.fogMaskClut8 >= 0)))
                flags |= 2;
            w.u16(flags);
        }
    };
    for (size_t oi = 0; oi < exporters.size(); oi++) {
        if (streamed[oi]) continue;
        w.align4();
        w.patchU32(meshOffsetPos[oi], uint32_t(w.pos()));
        for (const PsxTri& t : exporters[oi].tris) writeTri(w, exporters[oi], t);
    }

    // ---- object name table
    w.align4();
    w.patchU32(nameTableOffsetPos, uint32_t(w.pos()));
    for (ExpObject& e : exporters) {
        std::string n = truncateUtf16(e.obj->name, 24);
        w.u8(uint8_t(n.size()));
        w.bytes(n);
        w.u8(0);
    }

    // ---- audio clip table (16 bytes each, then the names). The ADPCM goes in
    // the .spu file, so dataOffset stays 0. Names are UTF-8 (2.4.0: ASCII).
    if (!audioSources.empty()) {
        w.align4();
        w.patchU32(audioTableOffsetPos, uint32_t(w.pos()));
        std::vector<std::string> names;
        std::vector<size_t> nameOffsetPos;
        for (size_t i = 0; i < audioSources.size(); i++) {
            const AudioComponent& a = *audioSources[i]->object->audio;
            std::string n = a.clipName;
            if (n.size() > 255) {
                size_t cut = 255;
                while (cut > 0 && (uint8_t(n[cut]) & 0xC0) == 0x80) cut--;
                n.resize(cut);
            }
            w.u32(0);
            w.u32(uint32_t(audioData[i].size()));
            w.u16(uint16_t(a.sampleRate));
            w.u8(a.loop ? 1 : 0);
            w.u8(uint8_t(n.size()));
            nameOffsetPos.push_back(w.pos());
            w.u32(0);
            names.push_back(std::move(n));
        }
        for (size_t i = 0; i < names.size(); i++) {
            w.patchU32(nameOffsetPos[i], uint32_t(w.pos()));
            w.bytes(names[i]);
            w.u8(0);
        }
    }

    if (!scene.cutscenes.empty() || !scene.animations.empty()) {
        std::vector<std::string> objectNames, clipNames;
        for (ExpObject& e : exporters) objectNames.push_back(truncateUtf16(e.obj->name, 24));
        std::set<std::string> offObjectNames;
        for (const FlatObject& fo : flat)
            if (!fo.activeInHierarchy) offObjectNames.insert(truncateUtf16(fo.object->name, 24));
        std::vector<std::string> lightNames;
        for (const RuntimeLight& rl : runtimeLights) lightNames.push_back(rl.flat->object->name);
        for (const FlatObject* fo : audioSources) clipNames.push_back(fo->object->audio->clipName);
        std::vector<SkinTarget> skinTargets;
        for (const SkinOut& so : skins) {
            SkinTarget t{exporters[so.exporter].obj->name, {}};
            for (const BakedClip& b : so.clips) t.clips.push_back(b.name);
            skinTargets.push_back(std::move(t));
        }
        std::vector<Sequence> cs, as;
        for (const Cutscene& c : scene.cutscenes)
            cs.push_back({c.name, c.durationFrames, c.tracks, &c.audioEvents, c.skinEvents});
        for (const Animation& a : scene.animations)
            as.push_back({a.name, a.durationFrames, a.tracks, nullptr, a.skinEvents});
        if (!cs.empty())
            writeSequences(w, cutsceneTableOffsetPos, cs, false, objectNames, offObjectNames, clipNames, skinTargets, canvases,
                           lightNames, options.vram, gte, res);
        if (res.ok() && !as.empty())
            writeSequences(w, animationTableOffsetPos, as, true, objectNames, offObjectNames, clipNames, skinTargets, canvases,
                           lightNames, options.vram, gte, res);
        if (!res.ok()) return res;
    }
    if (!skins.empty()) {
        // PSXSkinnedMeshExporter.ExportSkinData
        w.align4();
        w.patchU32(skinTableOffsetPos, uint32_t(w.pos()));
        std::vector<size_t> dataPos, namePos;
        for (size_t i = 0; i < skins.size(); i++) {
            dataPos.push_back(w.pos());
            w.u32(0);
            w.u8(uint8_t(truncateUtf16(exporters[skins[i].exporter].obj->name, 24).size()));
            w.u8(0);
            w.u16(0);
            namePos.push_back(w.pos());
            w.u32(0);
        }
        for (size_t i = 0; i < skins.size(); i++) {
            const SkinOut& so = skins[i];
            const ExpObject& e = exporters[so.exporter];
            ExportStats::SkinInfo& info = st.skins.emplace_back();
            info.object = e.obj->name;
            info.bones = int(so.boneCount);
            info.triangles = e.triBones.size() / 3;
            size_t blockStart = w.pos();
            w.align4();
            w.patchU32(dataPos[i], uint32_t(w.pos()));
            w.u16(uint16_t(so.exporter));
            w.u8(uint8_t(so.boneCount));
            w.u8(uint8_t(so.clips.size()));
            for (uint8_t b : e.triBones) w.u8(b);
            w.align4();
            for (const BakedClip& c : so.clips) {
                size_t clipStart = w.pos();
                w.u8(uint8_t(c.name.size()));
                w.bytes(c.name);
                w.u8(0);
                w.u8(c.loop ? 1 : 0);
                w.u8(uint8_t(c.fps));
                if (w.pos() & 1) w.u8(0);
                w.u16(uint16_t(c.frameCount));
                for (const BoneMatrix& m : c.frames)
                    for (int16_t v : m) w.i16(v);
                info.clips.push_back({c.name, c.fps, c.frameCount, w.pos() - clipStart, c.loop});
            }
            // v26: bind joint positions, then joint names.
            if (w.pos() & 1) w.u8(0);
            for (const auto& p : so.bind)
                for (int16_t v : p) w.i16(v);
            for (const std::string& n : so.names) {
                w.u8(uint8_t(n.size()));
                w.bytes(n);
                w.u8(0);
            }
            w.patchU32(namePos[i], uint32_t(w.pos()));
            w.bytes(truncateUtf16(e.obj->name, 24));
            w.u8(0);
            info.bytes = 12 + (w.pos() - blockStart);
        }
    }
    if (!canvases.empty() || !fontSheets.empty())
        writeUi(w, uiTableOffsetPos, canvases, uiTextures, fontSheets, fontVram, fontIndex, options.vram, uiSheets,
                scene.spriteSheets, spriteTextures);

    // ---- sprite sheets and animations (v22)
    if (!scene.spriteSheets.empty()) {
        res.stats.spriteTableBytes = writeSpriteTable(w, spriteTableOffsetPos, scene.spriteSheets, spriteTextures);
        res.stats.spriteAnims = int(spriteAnimCount);
        for (size_t i = 0; i < scene.spriteSheets.size(); i++) {
            const SpriteSheet& s = scene.spriteSheets[i];
            const PsxTexture& t = *spriteTextures[i];
            res.stats.spriteSheets.push_back(
                {s.name, int(s.bitDepth), s.cellW, s.cellH, t.width / s.cellW, t.height / s.cellH, int(s.anims.size())});
        }
    }

    // ---- tilemap (v23): the 20-byte header, then 2-byte cells, then 6-byte
    // objects. psxsplash points straight into these (u16 loads), so the chunk
    // starts 4-aligned; cells are an even count of bytes, which keeps the objects 2-aligned.
    if (scene.tilemap) {
        const Tilemap& tm = *scene.tilemap;
        const SpriteSheet& sheet = scene.spriteSheets[size_t(tilesetIndex(tm, scene.spriteSheets))];
        const FlatTilemap ft = flattenTilemap(tm);
        w.align4();
        const size_t start = w.pos();
        w.patchU32(tilemapTableOffsetPos, uint32_t(start));
        auto& info = res.stats.tilemap;
        info.width = tm.width;
        info.height = tm.height;
        info.tileW = std::clamp(sheet.cellW, 1, 255);
        info.tileH = std::clamp(sheet.cellH, 1, 255);
        info.sheet = tilesetIndex(tm, scene.spriteSheets);
        info.objects = int(ft.objects.size());
        w.u16(uint16_t(tm.width));
        w.u16(uint16_t(tm.height));
        w.u8(uint8_t(info.tileW));
        w.u8(uint8_t(info.tileH));
        w.u8(uint8_t(info.sheet));
        w.u8(0);
        w.u16(uint16_t(ft.objects.size()));
        w.u16(0);
        const size_t cellsPos = w.pos();
        w.u32(0);
        const size_t objectsPos = w.pos();
        w.u32(0);
        w.patchU32(cellsPos, uint32_t(w.pos()));
        w.bytes(ft.cells.data(), ft.cells.size());
        if (!ft.objects.empty()) w.patchU32(objectsPos, uint32_t(w.pos()));
        for (const TileObjectRecord& o : ft.objects) {
            w.u8(o.kind);
            w.u8(o.id);
            w.u16(o.x);
            w.u16(o.y);
        }
        info.bytes = w.pos() - start;
    }

    // ---- point lights (v24): {u16 count, u16 0}, 28-byte records, then names
    if (hasLights) {
        w.align4();
        w.patchU32(lightTableOffsetPos, uint32_t(w.pos()));
        w.u16(uint16_t(runtimeLights.size()));
        w.u16(0);
        std::vector<size_t> namePos;
        for (const RuntimeLight& rl : runtimeLights) {
            const LightComponent& l = *rl.light;
            Vec3 p = rl.flat->worldPosition;
            w.i32(toWorldFixed12(p.x / gte));
            w.i32(toWorldFixed12(-p.y / gte));
            w.i32(toWorldFixed12(p.z / gte));
            w.i32(toWorldFixed12(std::max(0.f, l.range) / gte));
            w.u16(uint16_t(clampv(roundToInt(l.intensity * 4096.f), 0, 65535)));
            for (int c = 0; c < 3; c++) w.u8(uint8_t(clampv(roundToInt(l.color[c] * 255.f), 0, 255)));
            w.u8(l.enabled ? 1 : 0);
            w.u16(0);
            namePos.push_back(w.pos());
            w.u32(0);
        }
        for (size_t i = 0; i < runtimeLights.size(); i++) {
            std::string name = truncateUtf16(runtimeLights[i].flat->object->name, 24);
            if (name.empty()) continue;
            w.patchU32(namePos[i], uint32_t(w.pos()));
            w.bytes(name.data(), name.size());
            w.u8(0);
        }
    }

    // ---- memory card (v21): one 464-byte record, 0 in the header when off
    if (!memCard.empty()) {
        w.align4();
        w.patchU32(memcardTableOffsetPos, uint32_t(w.pos()));
        w.bytes(memCard.data(), memCard.size());
    }

    // ---- stream table (psxsplash streamplanner.hh): header, regions, object refs
    if (!streamRegions.empty()) {
        w.align4();
        w.patchU32(streamTableOffsetPos, uint32_t(w.pos()));
        uint16_t refs = 0;
        for (const StreamRegionOut& r : streamRegions) refs = uint16_t(refs + r.objects.size());
        w.u16(uint16_t(streamRegions.size()));
        w.u16(refs);
        w.u16(uint16_t(st.streamSlots));
        w.u16(0);
        w.u32(st.streamSlotBytes);
        w.i32(st.streamLoadRadius);
        w.i32(int32_t(std::min<int64_t>(int64_t(st.streamLoadRadius) * 3 / 2, INT32_MAX)));
        uint16_t first = 0;
        for (const StreamRegionOut& r : streamRegions) {
            w.i32(r.bounds.minX);
            w.i32(r.bounds.minZ);
            w.i32(r.bounds.maxX);
            w.i32(r.bounds.maxZ);
            w.u32(r.firstSector);
            w.u32(r.byteSize);
            w.u16(first);
            w.u16(uint16_t(r.objects.size()));
            w.u32(0);
            first = uint16_t(first + r.objects.size());
        }
        for (const StreamRegionOut& r : streamRegions) {
            uint32_t offset = 0;
            for (auto [object, tris] : r.objects) {
                w.u16(object);
                w.u16(tris);
                w.u32(offset);
                offset += uint32_t(tris) * stream::kBytesPerTri;
            }
        }
    }

    fs::path outPath = out;
    if (!options.dryRun) w.save(outPath);
    res.stats.splashpackBytes = w.pos();

    // ---- .geo: each region's triangles back to back, padded to a sector. A
    // scene that does not stream removes it, so a stale one is not shipped.
    {
        const fs::path geoPath = fs::path(outPath).replace_extension(".geo");
        if (!streamRegions.empty()) {
            BinWriter g;
            for (const StreamRegionOut& r : streamRegions) {
                const size_t start = g.pos();
                for (auto [object, tris] : r.objects)
                    for (const PsxTri& t : exporters[object].tris) writeTri(g, exporters[object], t);
                while (g.pos() < start + r.byteSize) g.u8(0);
            }
            if (!options.dryRun) g.save(geoPath);
            st.geoFileBytes = g.pos();
        } else if (!options.dryRun) {
            std::error_code ec;
            fs::remove(geoPath, ec);
        }
    }

    // ---- .vram
    BinWriter v;
    v.u8('V');
    v.u8('R');
    v.u16(uint16_t(vram.atlases.size()));
    v.u16(uint16_t(clutCount + (vram.fogMaskClut4 >= 0) + (vram.fogMaskClut8 >= 0)));
    v.u8(uint8_t(fontSheets.size()));
    v.u8(0);
    for (Atlas& a : vram.atlases) {
        v.u16(uint16_t(a.positionX));
        v.u16(uint16_t(a.positionY));
        v.u16(uint16_t(a.width));
        v.u16(uint16_t(Atlas::kHeight));
        for (uint16_t px : a.pixels) v.u16(px);
        v.align4();
    }
    for (Atlas& a : vram.atlases)
        for (PsxTexture* t : a.textures) {
            if (!t->hasPalette) continue;
            v.u16(t->clutPackingX);
            v.u16(t->clutPackingY);
            v.u16(uint16_t(t->palette.size()));
            v.u16(0);
            for (uint16_t c : t->palette) v.u16(c);
            v.align4();
        }
    // Fog masks: entry 0 transparent, every other entry white with STP set.
    for (auto [id, length] : {std::pair{vram.fogMaskClut4, 16}, std::pair{vram.fogMaskClut8, 256}}) {
        if (id < 0) continue;
        v.u16(uint16_t(id & 63));
        v.u16(uint16_t(id >> 6));
        v.u16(uint16_t(length));
        v.u16(0);
        v.u16(0x0000);
        for (int i = 1; i < length; i++) v.u16(0xFFFF);
        v.align4();
    }
    for (size_t i = 0; i < fontSheets.size(); i++) {
        std::vector<uint8_t> px = fontSheets[i].packed4bpp();
        v.u8(uint8_t(fontSheets[i].glyphWidth));
        v.u8(uint8_t(fontSheets[i].glyphHeight));
        v.u16(uint16_t(fontVram[i][0]));
        v.u16(uint16_t(fontVram[i][1]));
        v.u16(uint16_t(fontSheets[i].height));
        v.u32(uint32_t(px.size()));
        v.bytes(px.data(), px.size());
        v.align4();
    }
    if (!options.dryRun) v.save(fs::path(outPath).replace_extension(".vram"));
    res.stats.vramFileBytes = v.pos();

    // ---- .spu
    BinWriter s;
    s.u8('S');
    s.u8('A');
    s.u16(uint16_t(audioSources.size()));
    for (size_t i = 0; i < audioSources.size(); i++) {
        const AudioComponent& a = *audioSources[i]->object->audio;
        s.u32(uint32_t(audioData[i].size()));
        s.u16(uint16_t(a.sampleRate));
        s.u8(a.loop ? 1 : 0);
        s.u8(0);
        for (uint8_t b : audioData[i]) s.u8(b);
        s.align4();
    }
    if (!options.dryRun) s.save(fs::path(outPath).replace_extension(".spu"));
    res.stats.spuFileBytes = s.pos();

    const VramSettings& vs = options.vram;
    st.framebufferBytes = size_t(vs.resolutionX) * size_t(vs.resolutionY) * 2 * (vs.dualBuffering ? 2 : 1);
    for (const Atlas& a : vram.atlases) {
        st.atlasBytes += size_t(a.width) * Atlas::kHeight * 2;
        for (const PsxTexture* t : a.textures) {
            if (t->hasPalette) st.clutBytes += t->palette.size() * 2;
            st.textures.push_back({t->source, int(t->bitDepth), t->cutout, t->width, t->height, a.positionX + t->packingX,
                                   a.positionY + t->packingY, t->clutPackingX * 16, t->clutPackingY,
                                   t->hasPalette ? int(t->palette.size()) : 0});
        }
    }
    for (const FontSheet& f : fontSheets) st.fontBytes += f.packed4bpp().size();
    st.spuEnd = kSpuStart;
    for (const std::vector<uint8_t>& d : audioData) st.spuEnd = ((st.spuEnd + 15) & ~size_t(15)) + ((d.size() + 15) & ~size_t(15));
    for (const ExpObject& e : exporters) st.triangles += int(e.tris.size());
    if (objectsBindRam(st)) res.warnings.push_back(objectRamWarning(st));
    return res;
}

ExportResult exportLoaderPack(const Scene& scene, const fs::path& root, const fs::path& out,
                              const ExportOptions& options) {
    ExportResult res;
    const std::string& lc = scene.settings.loadingCanvas;
    if (lc.empty()) {
        if (!options.dryRun) {
            std::error_code ec;
            fs::remove(out, ec);
        }
        return res;
    }
    auto found = std::find_if(scene.canvases.begin(), scene.canvases.end(), [&](const UICanvas& c) { return c.name == lc; });
    if (found == scene.canvases.end()) {
        res.errors.push_back("loading screen: no canvas named '" + lc + "'");
        return res;
    }
    std::vector<UICanvas> canvases{*found};
    stripUnknownElements(canvases, res);
    for (const UIElement& e : canvases[0].elements)
        if (e.type == UIElementType::Line)
            res.warnings.push_back("loading screen '" + lc + "', element '" + e.name +
                                   "': psxsplash does not draw lines on the loading screen");

    // Only the fonts this canvas uses, in scene order.
    Scene fontScene;
    fontScene.spriteSheets = scene.spriteSheets;  // for sheet-backed images
    for (const UIFont& f : scene.fonts)
        for (const UIElement& e : canvases[0].elements)
            if (e.type == UIElementType::Text && effectiveFont(canvases[0], e) == f.name) {
                fontScene.fonts.push_back(f);
                break;
            }

    std::map<std::string, Image> images;
    auto imageFor = [&](const std::string& p) -> const Image& {
        auto it = images.find(p);
        if (it == images.end()) it = images.emplace(p, loadImage(root / p)).first;
        return it->second;
    };
    std::deque<PsxTexture> store;
    std::vector<std::vector<PsxTexture*>> uiTextures(1);
    std::vector<std::vector<int>> uiSheets(1);
    std::vector<FontSheet> sheets;
    std::vector<std::array<int, 2>> fontVram;
    std::map<std::string, int> fontIndex;
    prepareUi(fontScene, canvases, root, options.vram, imageFor, store, uiTextures, uiSheets, false, sheets, fontVram, fontIndex,
              res);
    if (!res.ok()) return res;
    std::vector<PsxTexture*> all;
    for (PsxTexture* t : uiTextures[0])
        if (t) all.push_back(t);
    VramLayout vram = packVram(all, options.vram, res.errors);
    if (!res.ok()) return res;
    // A sheet-backed image steps through its cells with 8-bit texture
    // coordinates, as a sprite does: its copy of the sheet must sit in one page.
    for (size_t ei = 0; ei < uiTextures[0].size(); ei++)
        if (const PsxTexture* t = uiTextures[0][ei]; t && uiSheets[0][ei] >= 0) {
            const std::string e = spriteSheetPageError(fontScene.spriteSheets[size_t(uiSheets[0][ei])].name,
                                                       t->packingX * (16 / int(t->bitDepth)), t->packingY, t->width, t->height);
            if (!e.empty()) res.errors.push_back("loading screen: " + e);
        }
    if (!res.ok()) return res;
    std::vector<const PsxTexture*> cluts;
    for (const Atlas& a : vram.atlases)
        for (const PsxTexture* t : a.textures)
            if (t->hasPalette) cluts.push_back(t);
    if (vram.atlases.size() > 255 || cluts.size() > 255 || sheets.size() > 255) {
        res.errors.push_back("loading screen '" + lc + "': too many textures");
        return res;
    }

    // Header (LoaderPackHeader, 16 bytes), atlas and CLUT records, then the
    // 4-aligned pixel, CLUT and font blobs, then the splashpack's UI table.
    BinWriter w;
    w.u8('L');
    w.u8('P');
    w.u16(2);
    w.u8(uint8_t(sheets.size()));
    w.u8(1);
    w.u16(uint16_t(options.vram.resolutionX));
    w.u16(uint16_t(options.vram.resolutionY));
    w.u8(uint8_t(vram.atlases.size()));
    w.u8(uint8_t(cluts.size()));
    const size_t tableOffsetPos = w.pos();
    w.u32(0);
    std::vector<size_t> atlasPos, clutPos;
    for (const Atlas& a : vram.atlases) {
        atlasPos.push_back(w.pos());
        w.u32(0);
        w.u16(uint16_t(a.width));
        w.u16(uint16_t(Atlas::kHeight));
        w.u16(uint16_t(a.positionX));
        w.u16(uint16_t(a.positionY));
    }
    for (const PsxTexture* t : cluts) {
        clutPos.push_back(w.pos());
        w.u32(0);
        w.u16(t->clutPackingX);
        w.u16(t->clutPackingY);
        w.u16(uint16_t(t->palette.size()));
        w.u16(0);
    }
    for (size_t i = 0; i < vram.atlases.size(); i++) {
        w.align4();
        w.patchU32(atlasPos[i], uint32_t(w.pos()));
        for (uint16_t px : vram.atlases[i].pixels) w.u16(px);
    }
    for (size_t i = 0; i < cluts.size(); i++) {
        w.align4();
        w.patchU32(clutPos[i], uint32_t(w.pos()));
        for (uint16_t c : cluts[i]->palette) w.u16(c);
    }
    std::vector<uint32_t> fontOffsets;
    for (const FontSheet& sh : sheets) {
        w.align4();
        fontOffsets.push_back(uint32_t(w.pos()));
        std::vector<uint8_t> px = sh.packed4bpp();
        w.bytes(px.data(), px.size());
    }
    writeUi(w, tableOffsetPos, canvases, uiTextures, sheets, fontVram, fontIndex, options.vram, uiSheets, fontScene.spriteSheets, {},
            &fontOffsets);
    // The pack sits in RAM next to the old scene until it is drawn.
    if (w.pos() > 128 * 1024)
        res.warnings.push_back("loading screen '" + lc + "' is " + std::to_string(w.pos() / 1024) +
                               " KB; it shares RAM with the previous scene while loading, keep it small");
    if (!options.dryRun) w.save(out);
    return res;
}

std::vector<Vec3> exportVertexColors(const std::vector<FlatObject>& flat, const FlatObject& object,
                                     const Mesh& mesh) {
    std::vector<Vec3> out;
    if (!object.object->mesh || mesh.normals.size() != mesh.positions.size()) return out;
    std::vector<SceneLight> lights;
    for (const FlatObject& fo : flat)
        if (fo.activeInHierarchy && fo.object->light && fo.object->light->enabled) lights.push_back(sceneLight(fo));
    for (const Color& c : meshColors(object, *object.object->mesh, mesh, lights)) out.push_back({c.r, c.g, c.b});
    return out;
}

}  // namespace splash
