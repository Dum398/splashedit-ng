#include "import.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "blend.hh"
#include "simplify.hh"
#include "cgltf.h"
#include "ufbx.h"
#include "unitymath.hh"
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace splash {

namespace {

constexpr int kMaxTextureSide = 256;  // PS1 UVs are 8 bits

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

// The running importModel's ImportProgress on this thread, null for none.
thread_local ImportProgress* t_progress = nullptr;

// A point where the import may stop: throws ImportCancelled once it is
// cancelled, else moves the fraction done up to `fraction` (none: unchanged).
void checkpoint(float fraction = -1) {
    ImportProgress* p = t_progress;
    if (!p) return;
    if (p->cancel.load(std::memory_order_relaxed)) throw ImportCancelled();
    p->checks.fetch_add(1, std::memory_order_relaxed);
    if (fraction > p->fraction.load(std::memory_order_relaxed)) p->fraction.store(fraction, std::memory_order_relaxed);
}
// Every this many triangles of one mesh, the import checks for a cancel.
constexpr size_t kCheckEvery = 8192;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A file name stem safe on every platform the editor runs on.
std::string safeStem(const fs::path& p) {
    std::u8string u = p.stem().u8string();
    std::string s;
    for (char8_t c : u) s += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? char(c) : '_';
    return s.empty() ? std::string("model") : s;
}

std::string projectPath(const fs::path& rel) {
    std::u8string u = rel.generic_u8string();
    return std::string(u.begin(), u.end());
}

const char* resultText(cgltf_result r) {
    switch (r) {
        case cgltf_result_data_too_short: return "the file is cut short";
        case cgltf_result_unknown_format: return "not a glTF file";
        case cgltf_result_invalid_json: return "the JSON is not valid";
        case cgltf_result_invalid_gltf: return "the glTF is not valid";
        case cgltf_result_file_not_found: return "a file it refers to is missing";
        case cgltf_result_io_error: return "it could not be read";
        case cgltf_result_out_of_memory: return "out of memory";
        default: return "it could not be read";
    }
}

struct Pixels {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};

// The encoded bytes of a glTF image: a buffer view (.glb), a data: URI or a
// file next to the model.
std::vector<unsigned char> imageBytes(const cgltf_options& opt, const cgltf_image& img, const fs::path& dir) {
    if (img.buffer_view) {
        const auto* p = static_cast<const unsigned char*>(cgltf_buffer_view_data(img.buffer_view));
        if (!p) fail("an embedded image has no data");
        return {p, p + img.buffer_view->size};
    }
    if (!img.uri) fail("an image has neither a URI nor embedded data");
    std::string uri = img.uri;
    if (uri.rfind("data:", 0) == 0) {
        size_t comma = uri.find(";base64,");
        if (comma == std::string::npos) fail("an image data URI is not base64");
        std::string b64 = uri.substr(comma + 8);
        size_t pad = b64.size() >= 2 && b64[b64.size() - 1] == '=' ? (b64[b64.size() - 2] == '=' ? 2 : 1) : 0;
        size_t size = b64.size() / 4 * 3 - pad;
        void* out = nullptr;
        if (cgltf_load_buffer_base64(&opt, size, b64.c_str(), &out) != cgltf_result_success) fail("an image data URI does not decode");
        std::vector<unsigned char> v(static_cast<unsigned char*>(out), static_cast<unsigned char*>(out) + size);
        std::free(out);
        return v;
    }
    std::vector<char> name(uri.begin(), uri.end());
    name.push_back(0);
    cgltf_decode_uri(name.data());
    fs::path file = dir / fs::path(reinterpret_cast<const char8_t*>(name.data()));
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("missing texture " + projectPath(file.filename()));
    return {std::istreambuf_iterator<char>(in), {}};
}

Pixels decode(const std::vector<unsigned char>& bytes, const std::string& what) {
    Pixels px;
    int n;
    unsigned char* d = stbi_load_from_memory(bytes.data(), int(bytes.size()), &px.w, &px.h, &n, 4);
    if (!d) fail(what + " is not a PNG or JPEG image (" + stbi_failure_reason() + ")");
    px.rgba.assign(d, d + size_t(px.w) * px.h * 4);
    stbi_image_free(d);
    return px;
}

// Scales the image down so neither side exceeds kMaxTextureSide.
bool fit(Pixels& px) {
    if (px.w <= kMaxTextureSide && px.h <= kMaxTextureSide) return false;
    float s = float(kMaxTextureSide) / float(std::max(px.w, px.h));
    int w = std::max(1, int(std::lround(px.w * s))), h = std::max(1, int(std::lround(px.h * s)));
    std::vector<unsigned char> out(size_t(w) * h * 4);
    stbir_resize_uint8_srgb(px.rgba.data(), px.w, px.h, 0, out.data(), w, h, 0, STBIR_RGBA);
    px.w = w;
    px.h = h;
    px.rgba = std::move(out);
    return true;
}

struct Accessor {
    const cgltf_accessor* a = nullptr;
    int comps = 0;
    float at(size_t i, int c) const {
        float v[16] = {};
        cgltf_accessor_read_float(a, i, v, 16);
        return v[c];
    }
};

Accessor attribute(const cgltf_primitive& p, cgltf_attribute_type type, int index) {
    for (size_t i = 0; i < p.attributes_count; i++)
        if (p.attributes[i].type == type && p.attributes[i].index == index)
            return {p.attributes[i].data, int(cgltf_num_components(p.attributes[i].data->type))};
    return {};
}

// Unit length; up for a degenerate vector, so a sliver triangle still lights.
Vec3 unit(Vec3 v) {
    float l = std::sqrt(dot(v, v));
    return l > 0 ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0, 1, 0};
}

void collectNodes(const cgltf_node* n, std::vector<const cgltf_node*>& out) {
    out.push_back(n);
    for (size_t i = 0; i < n->children_count; i++) collectNodes(n->children[i], out);
}


// The bones moving one vertex as the file gives them: (joint node, weight),
// any number, not normalised. `skin` is -1 for a vertex no skin moves. The
// joint is the node's index in the tree (for FBX first its ufbx typed id).
struct Influence {
    int skin = -1;
    std::vector<std::pair<int, float>> weights;
};

// Builds the .mesh vertices every format ends up in. Callers pass right-handed,
// Y-up, +Z-front world space values (what glTF gives, and what ufbx gives when
// told to convert) with UVs already in the .mesh's bottom-left origin; X is
// mirrored here. Each vertex takes `influence` along.
struct Emitter {
    Mesh& mesh;
    std::vector<Influence>& influences;
    bool anyUv, anyColor;
    Influence influence;
    int vertex(Vec3 w, Vec3 n, Vec2 uv, std::array<float, 4> color) {
        mesh.positions.push_back({-w.x, w.y, w.z});
        mesh.normals.push_back({-n.x, n.y, n.z});
        if (anyUv) mesh.uv.push_back(uv);
        if (anyColor) mesh.colors.push_back(color);
        influences.push_back(influence);
        return int(mesh.positions.size() - 1);
    }
};

// Mirroring, and a node with a negative determinant, each flip which way a
// triangle faces, so the winding is reversed once for the mirror and again for
// each negative scale: `reverse` is true for a node with a non-negative one.
void addTriangle(std::vector<int>& sm, int i0, int i1, int i2, bool reverse) {
    if (reverse) std::swap(i1, i2);
    sm.insert(sm.end(), {i0, i1, i2});
}

// Flat shading for a triangle made of three fresh vertices: every corner gets
// the face normal.
void addFlatTriangle(Mesh& mesh, std::vector<int>& sm, int c0, int c1, int c2, bool reverse) {
    if (reverse) std::swap(c1, c2);
    const Vec3 &p0 = mesh.positions[size_t(c0)], &p1 = mesh.positions[size_t(c1)], &p2 = mesh.positions[size_t(c2)];
    Vec3 fn = unit(cross(p1 - p0, p2 - p0));
    for (int ci : {c0, c1, c2}) mesh.normals[size_t(ci)] = fn;
    sm.insert(sm.end(), {c0, c1, c2});
}

// One submesh's material as a format reader found it. `image` identifies the
// texture so an image shared by several materials is written once; `load`
// returns its encoded bytes, or nothing when the file cannot be found.
struct SourceMaterial {
    std::string name;  // as the file names it
    std::array<float, 4> color{1, 1, 1, 1};
    const void* image = nullptr;
    std::function<std::optional<std::vector<unsigned char>>()> load;
};

// A mesh being built in one space. Its submeshes are keyed by the model's
// material index (the order materials are first met in the whole file).
struct Sink {
    Mesh mesh;
    std::vector<Influence> influences;  // one per vertex
    std::vector<int> materials;  // model material index of each submesh
    std::vector<int>& submesh(int material) {
        for (size_t i = 0; i < materials.size(); i++)
            if (materials[i] == material) return mesh.submeshes[i];
        materials.push_back(material);
        mesh.submeshes.emplace_back();
        return mesh.submeshes.back();
    }
    int triangles() const {
        int t = 0;
        for (const auto& sm : mesh.submeshes) t += int(sm.size() / 3);
        return t;
    }
};

// Affine 3x4, row-major, in double: skeleton matrices compose down whole joint
// chains before they are written.
struct Aff {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

Aff mul(const Aff& a, const Aff& b) {
    Aff r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0);
    return r;
}

// The inverse; identity for a singular matrix.
Aff inverse(const Aff& a) {
    const auto& m = a.m;
    double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
           c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    Aff out;
    if (std::fabs(det) < 1e-300) return out;
    double id = 1.0 / det;
    double r[3][3] = {{c00 * id, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id},
                      {c01 * id, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id},
                      {c02 * id, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out.m[i][j] = r[i][j];
        out.m[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]);
    }
    return out;
}

// From a column-major 4x4 (glTF, cgltf).
Aff affOf(const float* c) {
    Aff a;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) a.m[i][j] = c[j * 4 + i];
    return a;
}

Aff affOf(const ufbx_matrix& u) {
    Aff a;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) a.m[i][j] = u.cols[j].v[i];
    return a;
}

// Translation, rotation, scale composed, as Matrix4x4.TRS.
Aff affOf(const Transform& t) {
    double x = t.rotation.x, y = t.rotation.y, z = t.rotation.z, w = t.rotation.w;
    double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 0) x /= n, y /= n, z /= n, w /= n;
    const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    const double s[3] = {t.scale.x, t.scale.y, t.scale.z}, p[3] = {t.position.x, t.position.y, t.position.z};
    Aff a;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) a.m[i][j] = R[i][j] * s[j];
        a.m[i][3] = p[i];
    }
    return a;
}

// S M S, S the X mirror: right-handed to the .mesh space and back.
Aff mirrored(const Aff& a) {
    Aff r = a;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            if ((i == 0) != (j == 0)) r.m[i][j] = -r.m[i][j];
    return r;
}

bool isIdentity(const Aff& a) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            if (std::fabs(a.m[i][j] - (i == j ? 1.0 : 0.0)) > 1e-9) return false;
    return true;
}

// A node of the tree while the file is read: its transform in the .mesh space
// and, when it has geometry, that geometry in the node's own space.
struct NodeBuild {
    std::string name;
    int parent = -1;
    Transform transform;
    std::optional<Sink> local;
    // For skins, right-handed in root space (the file's, before the mirror):
    // the node's rest transform, and the space its own animation keys are in
    // (its parent's world; for FBX's top-level nodes, ufbx's root).
    Aff world, keySpace;
    bool joint = false;      // a joint of the skeleton
    bool rootSpace = false;  // skinned: `local` is in root space, like the baked mesh
};

// One skin: per joint node (tree index), root space to joint space at bind,
// right-handed. A joint it does not list binds at its rest transform.
struct SkinSource {
    std::map<int, Aff> inverseBind;
};

// An animation as the file has it: channels on joint nodes, keys in the
// node's local space, right-handed.
struct ChannelSource {
    int node;
    AnimChannel channel;  // `joint` unused
};
struct ClipSource {
    std::string name;
    float length = 0;
    std::vector<ChannelSource> channels;
};

struct RigSource {
    std::vector<SkinSource> skins;
    std::vector<ClipSource> clips;
};

// Splits a right-handed affine matrix, basis columns c[0..2] and translation
// t, into translation, rotation and scale (a negative determinant goes to the
// X scale; shear is dropped), then takes it to the .mesh space. With S the X
// mirror, the node's matrix there is S M S: the translation's X negated, the
// scale unchanged, and the rotation S R S, which is the quaternion
// (x, -y, -z, w).
Transform mirroredTrs(const double c[3][3], const double t[3]) {
    auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto len = [&](const double* a) { return std::sqrt(dot3(a, a)); };
    double cr[3] = {c[1][1] * c[2][2] - c[1][2] * c[2][1], c[1][2] * c[2][0] - c[1][0] * c[2][2],
                    c[1][0] * c[2][1] - c[1][1] * c[2][0]};
    double det = dot3(c[0], cr);
    double sx = len(c[0]);
    if (det < 0) sx = -sx;
    double r0[3] = {1, 0, 0}, r1[3] = {0, 1, 0}, r2[3];
    if (sx != 0)
        for (int k = 0; k < 3; k++) r0[k] = c[0][k] / sx;
    double p = dot3(c[1], r0);
    double u[3] = {c[1][0] - p * r0[0], c[1][1] - p * r0[1], c[1][2] - p * r0[2]};
    double sy = len(u);
    if (sy > 0)
        for (int k = 0; k < 3; k++) r1[k] = u[k] / sy;
    r2[0] = r0[1] * r1[2] - r0[2] * r1[1];
    r2[1] = r0[2] * r1[0] - r0[0] * r1[2];
    r2[2] = r0[0] * r1[1] - r0[1] * r1[0];
    double sz = dot3(c[2], r2);
    // R[row][col], the columns r0 r1 r2; then the quaternion of R.
    double R[3][3] = {{r0[0], r1[0], r2[0]}, {r0[1], r1[1], r2[1]}, {r0[2], r1[2], r2[2]}};
    double qx, qy, qz, qw, tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        qw = 0.25 * s, qx = (R[2][1] - R[1][2]) / s, qy = (R[0][2] - R[2][0]) / s, qz = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        double s = std::sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2;
        qw = (R[2][1] - R[1][2]) / s, qx = 0.25 * s, qy = (R[0][1] + R[1][0]) / s, qz = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        double s = std::sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2;
        qw = (R[0][2] - R[2][0]) / s, qx = (R[0][1] + R[1][0]) / s, qy = 0.25 * s, qz = (R[1][2] + R[2][1]) / s;
    } else {
        double s = std::sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2;
        qw = (R[1][0] - R[0][1]) / s, qx = (R[0][2] + R[2][0]) / s, qy = (R[1][2] + R[2][1]) / s, qz = 0.25 * s;
    }
    double ql = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (qw < 0) ql = -ql;  // one sign for the same rotation, w >= 0
    Transform out;
    out.position = {float(-t[0]) + 0.0f, float(t[1]), float(t[2])};  // + 0 turns -0 into 0
    out.rotation = {float(qx / ql), float(-qy / ql) + 0.0f, float(-qz / ql) + 0.0f, float(qw / ql)};
    out.scale = {float(sx), float(sy), float(sz)};
    return out;
}

// A node name as a file name: letters, digits, '-' and '_' kept, the rest '_'.
std::string safeName(const std::string& name) {
    std::string s;
    for (char c : name) s += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return s.empty() ? std::string("node") : s;
}

// Normals from the faces: each corner gets the sum of the face normals at its
// position that are within `degrees` of its own face's, each weighted by the
// angle its triangle makes there, so a cube corner takes its three sides
// evenly. Corners holding the same vertex and normal share one vertex.
void recomputeNormals(Mesh& mesh, float degrees) {
    std::vector<std::array<int, 3>> tris;
    for (const std::vector<int>& sm : mesh.submeshes)
        for (size_t i = 0; i + 2 < sm.size(); i += 3) tris.push_back({sm[i], sm[i + 1], sm[i + 2]});
    std::vector<Vec3> face(tris.size());
    std::vector<std::array<float, 3>> angle(tris.size());
    std::map<std::array<float, 3>, std::vector<std::pair<size_t, int>>> around;  // position -> (triangle, corner)
    auto key = [&](int v) {
        const Vec3& p = mesh.positions[size_t(v)];
        return std::array<float, 3>{p.x + 0.0f, p.y + 0.0f, p.z + 0.0f};  // + 0 turns -0 into 0
    };
    for (size_t t = 0; t < tris.size(); t++) {
        const Vec3 p[3] = {mesh.positions[size_t(tris[t][0])], mesh.positions[size_t(tris[t][1])], mesh.positions[size_t(tris[t][2])]};
        face[t] = unit(cross(p[1] - p[0], p[2] - p[0]));
        for (int c = 0; c < 3; c++) {
            Vec3 a = p[(c + 1) % 3] - p[c], b = p[(c + 2) % 3] - p[c];
            float la = magnitude(a), lb = magnitude(b);
            angle[t][size_t(c)] = la > 0 && lb > 0 ? std::acos(std::clamp(dot(a, b) / (la * lb), -1.0f, 1.0f)) : 0.0f;
            around[key(tris[t][size_t(c)])].push_back({t, c});
        }
    }
    const float limit = float(std::cos(double(degrees) * 3.14159265358979 / 180.0)) - 1e-5f;
    Mesh out;
    out.skin = mesh.skin;
    if (out.skin) out.skin->vertexJoints.clear(), out.skin->vertexWeights.clear();
    std::map<std::tuple<int, float, float, float>, int> made;  // (vertex, normal) -> new vertex
    size_t t = 0;
    for (std::vector<int>& sm : mesh.submeshes) {
        std::vector<int> outSm;
        for (size_t i = 0; i + 2 < sm.size(); i += 3, t++)
            for (int c = 0; c < 3; c++) {
                const int v = tris[t][size_t(c)];
                Vec3 sum{};
                for (auto [u, cu] : around[key(v)])
                    if (dot(face[u], face[t]) >= limit) sum = sum + face[u] * angle[u][size_t(cu)];
                Vec3 n = sqrMagnitude(sum) > 0 ? unit(sum) : face[t];
                auto [it, fresh] = made.try_emplace({v, n.x, n.y, n.z}, int(out.positions.size()));
                if (fresh) {
                    out.positions.push_back(mesh.positions[size_t(v)]);
                    out.normals.push_back(n);
                    if (!mesh.uv.empty()) out.uv.push_back(mesh.uv[size_t(v)]);
                    if (!mesh.colors.empty()) out.colors.push_back(mesh.colors[size_t(v)]);
                    if (out.skin) {
                        out.skin->vertexJoints.push_back(mesh.skin->vertexJoints[size_t(v)]);
                        out.skin->vertexWeights.push_back(mesh.skin->vertexWeights[size_t(v)]);
                    }
                }
                outSm.push_back(it->second);
            }
        out.submeshes.push_back(std::move(outSm));
    }
    mesh = std::move(out);
}

// Quaternion product a * b: b's rotation, then a's.
Quat mulQuat(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The settings that change geometry, in the .mesh space. Z up turns the model
// -90 degrees about X, (x, y, z) -> (x, z, -y), which the X mirror leaves as
// it is: the baked mesh and the top-level nodes turn. The scale multiplies the
// baked mesh, every node translation and every node mesh, which scales the
// composed tree by the same amount. A root-space node mesh turns with the
// baked one. Skeletons and clips live in that space too: with A the turn and
// scale, every joint's rest and keys become A L A^-1 and every inverse bind
// A M A^-1, which skins A v the way the file skinned v.
void applySettings(Sink& baked, std::vector<NodeBuild>& nodes, std::vector<AnimClip>& clips, const ImportSettings& s) {
    const bool turn = s.up == UpAxis::ZUp;
    const float k = s.scale;
    auto turned = [&](Vec3 v) { return turn ? Vec3{v.x, v.z, -v.y + 0.0f} : v; };
    const float h = std::sqrt(0.5f);
    auto turnedQuat = [&](Quat q) { return turn ? mulQuat(mulQuat({-h, 0, 0, h}, q), {h, 0, 0, h}) : q; };
    auto turnedScale = [&](Vec3 v) { return turn ? Vec3{v.x, v.z, v.y} : v; };
    auto skin = [&](Mesh& mesh) {
        if (!mesh.skin) return;
        for (Joint& j : mesh.skin->joints) {
            j.position = turned(j.position) * k;
            j.rotation = turnedQuat(j.rotation);
            j.scale = turnedScale(j.scale);
        }
        // T = rows (1 0 0) (0 0 1) (0 -1 0): T L T^T, then k T t.
        for (std::array<float, 12>& m : mesh.skin->inverseBind) {
            if (turn) {
                const int src[3] = {0, 2, 1};
                const float sign[3] = {1, 1, -1};
                std::array<float, 12> r = m;
                for (int i = 0; i < 3; i++) {
                    for (int j = 0; j < 3; j++) r[size_t(i * 4 + j)] = sign[i] * sign[j] * m[size_t(src[i] * 4 + src[j])] + 0.0f;
                    r[size_t(i * 4 + 3)] = sign[i] * m[size_t(src[i] * 4 + 3)] + 0.0f;
                }
                m = r;
            }
            for (int i = 0; i < 3; i++) m[size_t(i * 4 + 3)] *= k;
        }
    };
    for (Vec3& p : baked.mesh.positions) p = turned(p) * k;
    for (Vec3& n : baked.mesh.normals) n = turned(n);
    skin(baked.mesh);
    for (NodeBuild& nb : nodes) {
        if (nb.parent < 0 && turn) {
            nb.transform.position = turned(nb.transform.position);
            nb.transform.rotation = mulQuat({-h, 0, 0, h}, nb.transform.rotation);
        }
        nb.transform.position = nb.transform.position * k;
        if (!nb.local) continue;
        for (Vec3& p : nb.local->mesh.positions) p = (nb.rootSpace ? turned(p) : p) * k;
        if (nb.rootSpace)
            for (Vec3& n : nb.local->mesh.normals) n = turned(n);
        skin(nb.local->mesh);
    }
    for (AnimClip& c : clips)
        for (AnimChannel& ch : c.channels)
            for (size_t i = 0; i < ch.times.size(); i++) {
                float* v = &ch.values[i * (ch.property == AnimProperty::Rotation ? 4 : 3)];
                if (ch.property == AnimProperty::Rotation) {
                    Quat q = turnedQuat({v[0], v[1], v[2], v[3]});
                    v[0] = q.x, v[1] = q.y, v[2] = q.z, v[3] = q.w;
                } else {
                    Vec3 p = ch.property == AnimProperty::Position ? turned({v[0], v[1], v[2]}) * k : turnedScale({v[0], v[1], v[2]});
                    v[0] = p.x, v[1] = p.y, v[2] = p.z;
                }
            }
    if (s.normals == Normals::Recompute) {
        recomputeNormals(baked.mesh, s.smoothingAngle);
        for (NodeBuild& nb : nodes)
            if (nb.local) recomputeNormals(nb.local->mesh, s.smoothingAngle);
    }
}

// targetTriangles: the baked mesh is simplified toward the whole target, each
// node mesh toward its share of it, in proportion to its triangles (at least
// 4, at most all it has). A simplified mesh gets its normals recomputed: the
// file's are per corner and do not survive vertices moving away.
void reduce(ImportedModel& out, Sink& baked, std::vector<NodeBuild>& nodes, const ImportSettings& s) {
    const int target = s.targetTriangles, total = baked.triangles();
    if (target <= 0 || target >= total) return;
    bool any = false;
    auto simplify = [&](Mesh& mesh, int t) {
        checkpoint();
        const SimplifyResult r = simplifyToTriangles(mesh, t);
        if (r.after == r.before) return r;
        recomputeNormals(mesh, s.smoothingAngle);
        any = true;
        return r;
    };
    const SimplifyResult r = simplify(baked.mesh, target);
    long long nodeTotal = 0;
    for (const NodeBuild& nb : nodes)
        if (nb.local) nodeTotal += nb.local->triangles();
    for (NodeBuild& nb : nodes) {
        if (!nb.local || nodeTotal <= 0) continue;
        const int own = nb.local->triangles();
        const int share = int((long long)target * own / nodeTotal);
        simplify(nb.local->mesh, std::clamp(share, std::min(own, 4), own));
    }
    if (r.after > target)
        out.warnings.push_back("simplified to " + std::to_string(r.after) + " triangles, not the target " +
                               std::to_string(target) + ": UV seams, material edges and open borders keep the rest");
    if (r.partsRemoved > 0)
        out.warnings.push_back(std::to_string(r.partsRemoved) + (r.partsRemoved == 1 ? " small part was" : " small parts were") +
                               " removed whole by simplifying: raise the target triangles to keep them");
    if (any && s.normals == Normals::Import)
        out.warnings.push_back("the simplified meshes have recomputed normals (smoothing angle " +
                               std::to_string(int(std::lround(s.smoothingAngle))) + "), not the file's");
}

// Applies the settings, counts triangles, then writes the textures, the baked
// .mesh and one .mesh per node with geometry, all named after `stem`.
// A right-handed matrix as a .mesh space transform.
Transform trsOf(const Aff& a) {
    double c[3][3], t[3];
    for (int k = 0; k < 3; k++) {
        for (int r = 0; r < 3; r++) c[k][r] = a.m[r][k];
        t[k] = a.m[k][3];
    }
    return mirroredTrs(c, t);
}

constexpr size_t kMaxJoints = 64;  // SKINMESH_MAX_BONES in psxsplash skinmesh.hh
constexpr size_t kMaxClips = 16;   // SKINMESH_MAX_CLIPS

// Turns what a reader found into the .mesh space, before the settings: the
// skeleton onto every sink whose vertices one skin moves, and the clips.
// Joints are the joint nodes in tree order, each parented to the nearest
// joint above it; a root joint's rest is its root-space transform. A joint's
// keys are in its node's parent's space, so they are taken through whatever
// lies between that and the parent joint (`prefix`).
std::vector<AnimClip> buildRig(ImportedModel& out, Sink& baked, std::vector<NodeBuild>& nodes, const RigSource& rig,
                               const ImportSettings& settings) {
    std::vector<int> jointOf(nodes.size(), -1), jointNodes;
    std::vector<Joint> joints;
    std::vector<Aff> prefix;
    std::set<std::string> names;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!nodes[i].joint) continue;
        int p = nodes[i].parent;
        while (p >= 0 && jointOf[size_t(p)] < 0) p = nodes[size_t(p)].parent;
        Joint j;
        const std::string base = nodes[i].name.empty() ? std::string("Joint") : nodes[i].name;
        j.name = base;
        for (int k = 2; names.count(j.name); k++) j.name = base + "_" + std::to_string(k);
        names.insert(j.name);
        j.parent = p < 0 ? -1 : jointOf[size_t(p)];
        const Aff toParent = p < 0 ? Aff{} : inverse(nodes[size_t(p)].world);
        const Transform t = trsOf(mul(toParent, nodes[i].world));
        j.position = t.position, j.rotation = t.rotation, j.scale = t.scale;
        prefix.push_back(mul(toParent, nodes[i].keySpace));
        jointOf[i] = int(joints.size());
        jointNodes.push_back(int(i));
        joints.push_back(std::move(j));
    }
    if (joints.empty()) return {};
    if (joints.size() > kMaxJoints)
        out.warnings.push_back("the skeleton has " + std::to_string(joints.size()) + " joints, psxsplash draws at most " +
                               std::to_string(kMaxJoints) + ": delete helper bones (IK targets, poles, twist bones) in the modelling tool");

    // A sink takes the skeleton when one skin moves every vertex of it.
    int blended = 0, unweighted = 0;
    auto attach = [&](Sink& sink, bool count) {
        if (sink.influences.empty()) return false;
        const int s = sink.influences[0].skin;
        if (s < 0 || size_t(s) >= rig.skins.size()) return false;
        for (const Influence& inf : sink.influences)
            if (inf.skin != s) return false;
        MeshSkin sk;
        sk.joints = joints;
        for (int node : jointNodes) {
            auto it = rig.skins[size_t(s)].inverseBind.find(node);
            const Aff ib = mirrored(it != rig.skins[size_t(s)].inverseBind.end() ? it->second : inverse(nodes[size_t(node)].world));
            std::array<float, 12> a;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) a[size_t(r * 4 + c)] = float(ib.m[r][c]) + 0.0f;
            sk.inverseBind.push_back(a);
        }
        for (const Influence& inf : sink.influences) {
            std::vector<std::pair<int, float>> w;  // (joint, weight), one entry per joint
            for (auto [node, weight] : inf.weights) {
                if (!(weight > 0) || node < 0 || size_t(node) >= nodes.size() || jointOf[size_t(node)] < 0) continue;
                const int j = jointOf[size_t(node)];
                auto it = std::find_if(w.begin(), w.end(), [&](const auto& e) { return e.first == j; });
                if (it != w.end()) it->second += weight;
                else w.push_back({j, weight});
            }
            std::stable_sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (w.size() > 4) w.resize(4);
            float sum = 0;
            for (const auto& e : w) sum += e.second;
            std::array<int, 4> vj{0, 0, 0, 0};
            std::array<float, 4> vw{0, 0, 0, 0};
            if (w.empty() || !(sum > 0)) {
                vw[0] = 1;
                unweighted += count;
            } else {
                for (size_t k = 0; k < w.size(); k++) vj[k] = w[k].first, vw[k] = w[k].second / sum;
                blended += count && vw[1] > 0.001f;
            }
            sk.vertexJoints.push_back(vj);
            sk.vertexWeights.push_back(vw);
        }
        sink.mesh.skin = std::move(sk);
        return true;
    };
    bool anySkinned = false;
    std::string loose;  // a mesh hanging off a joint that no skin moves
    for (NodeBuild& nb : nodes) {
        if (!nb.local) continue;
        if (attach(*nb.local, true)) {
            anySkinned = true;
            continue;
        }
        int p = nb.parent;
        while (p >= 0 && jointOf[size_t(p)] < 0) p = nodes[size_t(p)].parent;
        if (p >= 0 && loose.empty()) loose = "'" + nb.name + "' hangs off joint '" + joints[size_t(jointOf[size_t(p)])].name + "'";
    }
    out.skinned = attach(baked, false);
    if (anySkinned && !out.skinned && settings.mergeMeshes)
        out.warnings.push_back("the merged mesh mixes skinned and unskinned parts, so it has no skeleton: turn off Merge meshes to animate it");
    if (!loose.empty())
        out.warnings.push_back(loose + " but no skin moves it, so it will not follow the animation: weight it to that joint");
    if (blended)
        out.warnings.push_back(std::to_string(blended) +
                               " vertices are weighted to more than one joint; psxsplash skins rigidly, one bone per vertex: the heaviest "
                               "joint wins; weight-paint to single bones for exact deformation");
    if (unweighted)
        out.warnings.push_back(std::to_string(unweighted) + " vertices have no bone weights and follow joint '" + joints[0].name +
                               "': weight them in the modelling tool");

    // Clips, keys mirrored and taken through the joint's prefix.
    std::vector<AnimClip> clips;
    bool uneven = false;
    for (const ClipSource& cs : rig.clips) {
        AnimClip c;
        c.name = cs.name;
        c.length = std::max(0.0f, cs.length);
        // Cycles by name (Walk, Idle, ...) loop; the inspector flips it.
        c.loop = defaultClipLoop(cs.name);
        for (const ChannelSource& src : cs.channels) {
            if (src.node < 0 || size_t(src.node) >= nodes.size() || jointOf[size_t(src.node)] < 0 || src.channel.times.empty()) continue;
            const size_t j = size_t(jointOf[size_t(src.node)]);
            AnimChannel ch = src.channel;
            ch.joint = joints[j].name;
            const bool through = !isIdentity(prefix[j]);
            const Aff x = mirrored(prefix[j]);
            const Transform xt = trsOf(prefix[j]);
            const float smin = std::min({std::fabs(xt.scale.x), std::fabs(xt.scale.y), std::fabs(xt.scale.z)});
            const float smax = std::max({std::fabs(xt.scale.x), std::fabs(xt.scale.y), std::fabs(xt.scale.z)});
            if (through && ch.property != AnimProperty::Position && smax - smin > 1e-4f * smax) uneven = true;
            const size_t stride = ch.property == AnimProperty::Rotation ? 4 : 3;
            for (size_t i = 0; i < ch.times.size(); i++) {
                float* v = &ch.values[i * stride];
                if (ch.property == AnimProperty::Position) {
                    const double p[3] = {-v[0], v[1], v[2]};
                    for (int r = 0; r < 3; r++)
                        v[r] = float(through ? x.m[r][0] * p[0] + x.m[r][1] * p[1] + x.m[r][2] * p[2] + x.m[r][3] : p[r]) + 0.0f;
                } else if (ch.property == AnimProperty::Rotation) {
                    Quat q{v[0], -v[1] + 0.0f, -v[2] + 0.0f, v[3]};
                    if (through) q = mulQuat(xt.rotation, q);
                    v[0] = q.x, v[1] = q.y, v[2] = q.z, v[3] = q.w;
                } else if (through) {
                    v[0] *= xt.scale.x, v[1] *= xt.scale.y, v[2] *= xt.scale.z;
                }
            }
            c.length = std::max(c.length, ch.times.back());
            c.channels.push_back(std::move(ch));
        }
        if (c.channels.empty()) {
            out.warnings.push_back("animation '" + cs.name + "' moves no joint and was left out: only skeleton animation is imported");
            continue;
        }
        clips.push_back(std::move(c));
    }
    if (uneven)
        out.warnings.push_back("a joint sits under a non-uniform scale its keys cannot carry exactly: apply the armature's scale in the modelling tool");
    if (!anySkinned && !out.skinned) return clips;
    if (clips.empty())
        out.warnings.push_back("the file has no skeleton animation; psxsplash only draws a skinned mesh that has a clip, so the model gets no "
                               "skin component and stays in its rest pose");
    else if (clips.size() > kMaxClips)
        out.warnings.push_back(std::to_string(clips.size()) + " animations, a skin plays at most " + std::to_string(kMaxClips) +
                               ": the first " + std::to_string(kMaxClips) + " go on the skin component, swap in others by hand");
    return clips;
}

// A skinned node's mesh is in root space: its transform becomes the inverse of
// its parent's world, so it sits at the model's root, and each child keeps its
// world. Runs on the transforms after the settings.
void placeRootSpaceNodes(std::vector<NodeBuild>& nodes) {
    std::vector<Aff> world(nodes.size()), placed(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++) {
        const int p = nodes[i].parent;
        const Aff local = affOf(nodes[i].transform);
        world[i] = p < 0 ? local : mul(world[size_t(p)], local);
        placed[i] = nodes[i].rootSpace ? Aff{} : world[i];
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        const int p = nodes[i].parent;
        if (!nodes[i].rootSpace && (p < 0 || !nodes[size_t(p)].rootSpace)) continue;
        // In the .mesh space; trsOf takes a right-handed matrix.
        nodes[i].transform = trsOf(mirrored(mul(p < 0 ? Aff{} : inverse(placed[size_t(p)]), placed[i])));
    }
}

void finishImport(ImportedModel& out, Sink& baked, std::vector<NodeBuild>& nodes, const std::vector<SourceMaterial>& mats,
                  const RigSource& rig, const fs::path& source, const fs::path& project, const ImportSettings& settings,
                  const std::string& stem) {
    const std::string what = projectPath(source.filename());
    checkpoint(0.55f);
    std::vector<AnimClip> clips = buildRig(out, baked, nodes, rig, settings);
    applySettings(baked, nodes, clips, settings);
    placeRootSpaceNodes(nodes);
    checkpoint(0.6f);
    out.sourceTriangles = baked.triangles();
    reduce(out, baked, nodes, settings);
    checkpoint(0.65f);
    const Mesh& mesh = baked.mesh;
    out.triangles = baked.triangles();
    if (out.triangles == 0) fail(what + " has no triangles to import");

    const fs::path dir = "models";
    std::error_code ec;
    fs::create_directories(project / dir, ec);
    if (ec) fail("cannot create " + projectPath(project / dir) + ": " + ec.message());

    // Textures: each image used as a base colour, written once.
    std::map<const void*, std::string> written;
    std::set<const void*> tried;
    int imagesUsed = 0;
    for (const SourceMaterial& mat : mats)
        if (mat.image && !written.count(mat.image)) written[mat.image] = {}, imagesUsed++;
    int n = 0;
    std::vector<Material> byIndex;  // per model material index
    for (const SourceMaterial& mat : mats) {
        checkpoint(0.65f + 0.15f * float(byIndex.size()) / float(mats.size()));
        Material outMat;
        outMat.color = mat.color;
        if (mat.image) {
            std::string& rel = written[mat.image];
            if (tried.insert(mat.image).second) {
                if (std::optional<std::vector<unsigned char>> bytes = mat.load()) {
                    Pixels px = decode(*bytes, what + " texture");
                    if (fit(px))
                        out.warnings.push_back("a texture was scaled down to " + std::to_string(px.w) + " x " + std::to_string(px.h) +
                                               ", the most a PS1 texture can be");
                    fs::path file = dir / (imagesUsed == 1 ? stem + ".png" : stem + "_" + std::to_string(n++) + ".png");
                    if (!stbi_write_png(projectPath(project / file).c_str(), px.w, px.h, 4, px.rgba.data(), px.w * 4))
                        fail("cannot write " + projectPath(file));
                    rel = projectPath(file);
                    out.textures.push_back(rel);
                }
            }
            outMat.texture = rel;
        }
        byIndex.push_back(outMat);
    }
    for (int i : baked.materials) {
        out.materials.push_back(byIndex[size_t(i)]);
        out.materialNames.push_back(mats[size_t(i)].name);
    }

    checkpoint(0.8f);
    fs::path meshFile = dir / (stem + ".mesh");
    saveMesh(mesh, project / meshFile);
    out.mesh = projectPath(meshFile);
    checkpoint(0.85f);

    // The tree: models/<stem>/<node>.mesh, and the clips beside them as
    // models/<stem>/<clip>.anim, the folder cleared of an earlier import's
    // meshes and clips first.
    const fs::path nodeDir = dir / stem;
    for (fs::directory_iterator it(project / nodeDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".mesh" || it->path().extension() == ".anim") fs::remove(it->path(), ec);
    ec.clear();
    bool madeDir = false;
    auto makeDir = [&] {
        if (madeDir) return;
        fs::create_directories(project / nodeDir, ec);
        if (ec) fail("cannot create " + projectPath(project / nodeDir) + ": " + ec.message());
        madeDir = true;
    };
    // Clip names are what SkinnedAnim.Play takes: file-safe, unique, and at
    // most 24 characters, which psxsplash keeps.
    std::set<std::string> clipNames;
    for (AnimClip& c : clips) {
        const std::string base = safeName(c.name).substr(0, 24);
        std::string name = base;
        for (int k = 2; clipNames.count(lower(name)); k++) {
            const std::string suffix = "_" + std::to_string(k);
            name = base.substr(0, 24 - suffix.size()) + suffix;
        }
        clipNames.insert(lower(name));
        out.clipTakes.push_back(c.name);
        c.name = name;
        makeDir();
        const fs::path file = nodeDir / (name + ".anim");
        saveAnim(c, project / file);
        out.clips.push_back(projectPath(file));
    }
    std::set<std::string> taken;
    for (NodeBuild& nb : nodes) {
        checkpoint(0.85f + 0.15f * float(out.nodes.size()) / float(nodes.size()));
        ImportedNode node;
        node.name = nb.name.empty() ? std::string("Node") : nb.name;
        node.parent = nb.parent;
        node.transform = nb.transform;
        if (nb.local && nb.local->triangles() > 0) {
            makeDir();
            node.skinned = nb.local->mesh.skin.has_value();
            std::string base = safeName(nb.name), name = base;
            for (int k = 2; taken.count(lower(name)); k++) name = base + "_" + std::to_string(k);
            taken.insert(lower(name));
            fs::path file = nodeDir / (name + ".mesh");
            saveMesh(nb.local->mesh, project / file);
            node.mesh = projectPath(file);
            for (int i : nb.local->materials) {
                node.materials.push_back(byIndex[size_t(i)]);
                node.materialNames.push_back(mats[size_t(i)].name);
            }
        }
        out.nodes.push_back(std::move(node));
    }
}

// A UTF-8 string as a path, Windows separators turned into slashes.
fs::path pathFrom(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::optional<std::vector<unsigned char>> readFile(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {});
}

// Directories directly under `base` named like a Unity-style texture folder.
std::vector<fs::path> textureFolders(const fs::path& base) {
    static const char* const names[] = {"textures", "tex", "materials"};
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string n = lower(projectPath(it->path().filename()));
        for (const char* want : names)
            if (n == want) found.push_back(it->path());
    }
    return found;
}

// A file called `name` in `folder`, ignoring case.
std::optional<std::vector<unsigned char>> findByName(const fs::path& folder, const std::string& name) {
    std::string want = lower(name);
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (lower(projectPath(it->path().filename())) == want)
            if (auto bytes = readFile(it->path())) return bytes;
    return std::nullopt;
}

// The encoded bytes of an FBX/OBJ texture, searched the way Unity does:
// embedded data, the relative path from the model, the absolute path, and last
// the bare file name in the model's folder and in texture folders inside and
// beside it.
std::optional<std::vector<unsigned char>> textureBytes(const ufbx_texture& t, const fs::path& dir) {
    if (t.content.size) {
        const auto* p = static_cast<const unsigned char*>(t.content.data);
        return std::vector<unsigned char>(p, p + t.content.size);
    }
    std::string rel(t.relative_filename.data, t.relative_filename.length);
    std::string abs(t.absolute_filename.data, t.absolute_filename.length);
    if (!rel.empty())
        if (auto bytes = readFile(dir / pathFrom(rel))) return bytes;
    if (!abs.empty())
        if (auto bytes = readFile(pathFrom(abs))) return bytes;
    std::string bare = projectPath(pathFrom(rel.empty() ? abs : rel).filename());
    if (bare.empty()) return std::nullopt;
    std::vector<fs::path> folders{dir};
    for (const fs::path& f : textureFolders(dir)) folders.push_back(f);
    for (const fs::path& f : textureFolders(dir.parent_path())) folders.push_back(f);
    for (const fs::path& f : folders)
        if (auto bytes = findByName(f, bare)) return bytes;
    return std::nullopt;
}

ImportedModel importGltf(const fs::path& source, const fs::path& project, const ImportSettings& settings, const std::string& stem) {
    const std::string what = projectPath(source.filename());
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    std::u8string src8 = source.u8string();
    std::string src(src8.begin(), src8.end());
    cgltf_result r = cgltf_parse_file(&opt, src.c_str(), &data);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&opt, data, src.c_str());
    if (r == cgltf_result_success) r = cgltf_validate(data);
    if (r != cgltf_result_success) {
        cgltf_free(data);
        fail(what + ": " + resultText(r));
    }
    std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free);
    checkpoint(0.15f);

    ImportedModel out;
    std::vector<const cgltf_node*> nodes;
    const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene)
        for (size_t i = 0; i < scene->nodes_count; i++) collectNodes(scene->nodes[i], nodes);
    else
        for (size_t i = 0; i < data->nodes_count; i++)
            if (!data->nodes[i].parent) collectNodes(&data->nodes[i], nodes);

    // The node tree, in the .mesh space.
    std::vector<NodeBuild> tree;
    std::map<const cgltf_node*, int> indexOf;
    for (const cgltf_node* n : nodes) {
        NodeBuild nb;
        nb.name = n->name ? n->name : "";
        auto pit = n->parent ? indexOf.find(n->parent) : indexOf.end();
        nb.parent = pit == indexOf.end() ? -1 : pit->second;
        float lm[16];
        cgltf_node_transform_local(n, lm);
        const double basis[3][3] = {{lm[0], lm[1], lm[2]}, {lm[4], lm[5], lm[6]}, {lm[8], lm[9], lm[10]}};
        const double t[3] = {lm[12], lm[13], lm[14]};
        nb.transform = mirroredTrs(basis, t);
        float wm[16];
        cgltf_node_transform_world(n, wm);
        nb.world = affOf(wm);
        if (n->parent) {
            cgltf_node_transform_world(n->parent, wm);
            nb.keySpace = affOf(wm);
        }
        indexOf[n] = int(tree.size());
        tree.push_back(std::move(nb));
    }

    // Skins used by the scene's meshes; their joints make the skeleton.
    RigSource rig;
    std::map<const cgltf_skin*, int> skinIndex;
    for (const cgltf_node* n : nodes) {
        if (!n->mesh || !n->skin || skinIndex.count(n->skin)) continue;
        const cgltf_skin* sk = n->skin;
        SkinSource src;
        bool inScene = true;
        for (size_t j = 0; j < sk->joints_count; j++) {
            auto it = indexOf.find(sk->joints[j]);
            if (it == indexOf.end()) {
                inScene = false;
                break;
            }
            Aff ib;
            if (sk->inverse_bind_matrices) {
                float m[16];
                cgltf_accessor_read_float(sk->inverse_bind_matrices, j, m, 16);
                ib = affOf(m);
            }
            src.inverseBind[it->second] = ib;
        }
        if (!inScene || sk->joints_count == 0) {
            out.warnings.push_back("a skin whose joints are not all in the scene was left out, its mesh stays in its rest pose");
            skinIndex[sk] = -1;
            continue;
        }
        for (auto [node, ib] : src.inverseBind) tree[size_t(node)].joint = true;
        skinIndex[sk] = int(rig.skins.size());
        rig.skins.push_back(std::move(src));
    }

    Sink baked;
    std::map<const cgltf_material*, int> materialIndex;
    std::vector<const cgltf_material*> materialOf;
    bool anyUv = false, anyColor = false, skipped = false;
    for (const cgltf_node* n : nodes)
        for (size_t pi = 0; n->mesh && pi < n->mesh->primitives_count; pi++) {
            const cgltf_primitive& p = n->mesh->primitives[pi];
            anyUv |= attribute(p, cgltf_attribute_type_texcoord, 0).a != nullptr;
            anyColor |= attribute(p, cgltf_attribute_type_color, 0).a != nullptr;
        }

    // Writes one primitive into `sink` through the column-major matrix `m`.
    // `skin` (index into rig.skins, -1 for none) moves the vertices of `node`.
    auto emitPrimitive = [&](Sink& sink, int material, const float* m, const cgltf_primitive& p, const std::vector<int>& idx,
                             const cgltf_node* node, int skin) {
        Mesh& mesh = sink.mesh;
        Emitter emit{mesh, sink.influences, anyUv, anyColor, {}};
        const Accessor joints[2] = {attribute(p, cgltf_attribute_type_joints, 0), attribute(p, cgltf_attribute_type_joints, 1)};
        const Accessor weights[2] = {attribute(p, cgltf_attribute_type_weights, 0), attribute(p, cgltf_attribute_type_weights, 1)};
        Accessor pos = attribute(p, cgltf_attribute_type_position, 0);
        Accessor nrm = attribute(p, cgltf_attribute_type_normal, 0);
        Accessor uv = attribute(p, cgltf_attribute_type_texcoord, 0);
        Accessor col = attribute(p, cgltf_attribute_type_color, 0);
        // Normals take the inverse transpose of the upper 3x3.
        float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
        float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
        float it[9] = {e * k - f * h, f * g - d * k, d * h - e * g, c * h - b * k, a * k - c * g,
                       b * g - a * h, b * f - c * e, c * d - a * f, a * e - b * d};  // cofactor matrix, rows
        std::vector<int>& sm = sink.submesh(material);

        // glTF (right-handed, +Z forward) to the .mesh space (left-handed):
        // mirror X, reverse the winding, and take V from a top-left origin
        // to the .mesh's bottom-left one.
        bool reverse = det >= 0;
        auto vertex = [&](size_t i, bool flat) {
            float x = pos.at(i, 0), y = pos.at(i, 1), z = pos.at(i, 2);
            Vec3 w{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                   m[2] * x + m[6] * y + m[10] * z + m[14]};
            Vec3 nn{};
            if (!flat) {
                float nx = nrm.at(i, 0), ny = nrm.at(i, 1), nz = nrm.at(i, 2);
                Vec3 t{it[0] * nx + it[1] * ny + it[2] * nz, it[3] * nx + it[4] * ny + it[5] * nz,
                       it[6] * nx + it[7] * ny + it[8] * nz};
                if (det < 0) t = {-t.x, -t.y, -t.z};
                nn = unit(t);
            }
            emit.influence.skin = skin;
            emit.influence.weights.clear();
            for (int set = 0; skin >= 0 && set < 2; set++) {
                if (!joints[set].a || !weights[set].a) continue;
                for (int k = 0; k < 4 && k < joints[set].comps; k++) {
                    const size_t jn = size_t(joints[set].at(i, k));
                    if (jn < node->skin->joints_count)
                        emit.influence.weights.push_back({indexOf[node->skin->joints[jn]], weights[set].at(i, k)});
                }
            }
            return emit.vertex(w, nn, uv.a ? Vec2{uv.at(i, 0), 1.0f - uv.at(i, 1)} : Vec2{0, 0},
                               col.a ? std::array<float, 4>{col.at(i, 0), col.at(i, 1), col.at(i, 2), col.comps == 4 ? col.at(i, 3) : 1.0f}
                                     : std::array<float, 4>{1, 1, 1, 1});
        };
        if (nrm.a) {
            int base = int(mesh.positions.size());
            for (size_t i = 0; i < pos.a->count; i++) {
                if (i % kCheckEvery == 0) checkpoint();
                vertex(i, false);
            }
            for (size_t t = 0; t < idx.size(); t += 3) {
                if (t % (3 * kCheckEvery) == 0) checkpoint();
                addTriangle(sm, base + idx[t], base + idx[t + 1], base + idx[t + 2], reverse);
            }
        } else {
            // No normals: flat shading, each triangle gets its own corners.
            for (size_t t = 0; t < idx.size(); t += 3) {
                if (t % (3 * kCheckEvery) == 0) checkpoint();
                int c0 = vertex(size_t(idx[t]), true), c1 = vertex(size_t(idx[t + 1]), true), c2 = vertex(size_t(idx[t + 2]), true);
                addFlatTriangle(mesh, sm, c0, c1, c2, reverse);
            }
        }
    };

    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const size_t meshNodes = size_t(std::count_if(nodes.begin(), nodes.end(), [](const cgltf_node* n) { return n->mesh != nullptr; }));
    size_t meshesDone = 0;
    for (const cgltf_node* n : nodes) {
        if (!n->mesh) continue;
        checkpoint(0.15f + 0.35f * float(meshesDone++) / float(meshNodes));
        // A skinned node's transform is ignored (glTF 2.0, 3.7.3.3): the
        // joints place its vertices, which are in root space.
        const int skin = n->skin ? skinIndex[n->skin] : -1;
        float m[16];
        cgltf_node_transform_world(n, m);
        if (skin >= 0) std::copy(identity, identity + 16, m);
        NodeBuild& nb = tree[size_t(indexOf[n])];
        nb.rootSpace = skin >= 0;
        for (size_t pi = 0; pi < n->mesh->primitives_count; pi++) {
            const cgltf_primitive& p = n->mesh->primitives[pi];
            if (p.type != cgltf_primitive_type_triangles) {
                skipped = true;
                continue;
            }
            Accessor pos = attribute(p, cgltf_attribute_type_position, 0);
            if (!pos.a) continue;
            std::vector<int> idx;
            if (p.indices)
                for (size_t i = 0; i < p.indices->count; i++) idx.push_back(int(cgltf_accessor_read_index(p.indices, i)));
            else
                for (size_t i = 0; i < pos.a->count; i++) idx.push_back(int(i));
            idx.resize(idx.size() / 3 * 3);

            auto mi = materialIndex.find(p.material);
            if (mi == materialIndex.end()) {
                mi = materialIndex.emplace(p.material, int(materialOf.size())).first;
                materialOf.push_back(p.material);
            }
            // Baked into root space, and again in the node's own space.
            emitPrimitive(baked, mi->second, m, p, idx, n, skin);
            if (!nb.local) nb.local.emplace();
            emitPrimitive(*nb.local, mi->second, identity, p, idx, n, skin);
        }
    }
    if (skipped) out.warnings.push_back("points and lines were left out, only triangles are imported");

    checkpoint(0.5f);
    // Animations: the channels on joints. Cubic-spline samplers keep their
    // values (the middle of each in-tangent, value, out-tangent triple).
    bool cubic = false, morph = false;
    for (size_t a = 0; a < data->animations_count; a++) {
        const cgltf_animation& an = data->animations[a];
        ClipSource clip;
        clip.name = an.name && *an.name ? an.name : "Clip" + std::to_string(a + 1);
        for (size_t c = 0; c < an.channels_count; c++) {
            const cgltf_animation_channel& ch = an.channels[c];
            auto it = ch.target_node ? indexOf.find(ch.target_node) : indexOf.end();
            if (ch.target_path == cgltf_animation_path_type_weights) morph = true;
            if (it == indexOf.end() || !tree[size_t(it->second)].joint || !ch.sampler) continue;
            ChannelSource src;
            src.node = it->second;
            AnimChannel& out_ = src.channel;
            if (ch.target_path == cgltf_animation_path_type_translation) out_.property = AnimProperty::Position;
            else if (ch.target_path == cgltf_animation_path_type_rotation) out_.property = AnimProperty::Rotation;
            else if (ch.target_path == cgltf_animation_path_type_scale) out_.property = AnimProperty::Scale;
            else continue;
            const cgltf_animation_sampler& sm = *ch.sampler;
            const bool spline = sm.interpolation == cgltf_interpolation_type_cubic_spline;
            cubic |= spline;
            out_.interp = sm.interpolation == cgltf_interpolation_type_step ? AnimInterp::Step : AnimInterp::Linear;
            const int stride = out_.property == AnimProperty::Rotation ? 4 : 3;
            const size_t keys = sm.input->count;
            if (sm.output->count < keys * (spline ? 3 : 1)) continue;
            for (size_t k = 0; k < keys; k++) {
                float t = 0, v[4] = {0, 0, 0, 1};
                cgltf_accessor_read_float(sm.input, k, &t, 1);
                cgltf_accessor_read_float(sm.output, spline ? k * 3 + 1 : k, v, size_t(stride));
                if (!out_.times.empty() && t < out_.times.back()) t = out_.times.back();
                out_.times.push_back(std::max(0.0f, t));
                out_.values.insert(out_.values.end(), v, v + stride);
            }
            if (!out_.times.empty()) clip.channels.push_back(std::move(src));
        }
        rig.clips.push_back(std::move(clip));
    }

    std::vector<SourceMaterial> mats;
    for (const cgltf_material* mat : materialOf) {
        SourceMaterial sm;
        if (mat && mat->name) sm.name = mat->name;
        const cgltf_texture* t = nullptr;
        if (mat && mat->has_pbr_metallic_roughness) {
            const float* c = mat->pbr_metallic_roughness.base_color_factor;
            sm.color = {c[0], c[1], c[2], c[3]};
            t = mat->pbr_metallic_roughness.base_color_texture.texture;
        } else if (mat && mat->has_pbr_specular_glossiness) {
            const float* c = mat->pbr_specular_glossiness.diffuse_factor;
            sm.color = {c[0], c[1], c[2], c[3]};
            t = mat->pbr_specular_glossiness.diffuse_texture.texture;
        }
        if (t && t->image) {
            const cgltf_image* image = t->image;
            sm.image = image;
            sm.load = [&opt, image, dir = source.parent_path()] { return std::make_optional(imageBytes(opt, *image, dir)); };
        }
        mats.push_back(std::move(sm));
    }
    finishImport(out, baked, tree, mats, rig, source, project, settings, stem);
    // After the skeleton's warnings, which matter more.
    if (cubic) out.warnings.push_back("cubic-spline keys were read by their values only and play linearly: bake the animation to linear keys for its exact curve");
    if (morph) out.warnings.push_back("morph target (shape key) animation was left out, psxsplash has no morph targets");
    return out;
}

ImportedModel importUfbx(const fs::path& source, const fs::path& project, const ImportSettings& settings, const std::string& stem) {
    const std::string what = projectPath(source.filename());
    ufbx_load_opts opts{};
    // Let ufbx turn the file's axes and units into glTF's: right-handed, +X
    // right, +Y up, +Z front, one unit a metre. From there on it is glTF. An
    // up axis set in the settings stands in for the file's: ufbx then only
    // converts units, the data is taken as Y up as it is, and Z up is turned
    // in applySettings like a glTF.
    if (settings.up == UpAxis::File) opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1;
    opts.load_external_files = true;  // the .mtl of an .obj
    opts.ignore_missing_external_files = true;
    opts.obj_search_mtl_by_filename = true;
    // Reading a large file is most of the time: ufbx reports how far it is
    // and stops when the import is cancelled.
    opts.progress_cb.fn = [](void*, const ufbx_progress* pr) -> ufbx_progress_result {
        ImportProgress* p = t_progress;
        if (!p) return UFBX_PROGRESS_CONTINUE;
        if (p->cancel.load(std::memory_order_relaxed)) return UFBX_PROGRESS_CANCEL;
        p->checks.fetch_add(1, std::memory_order_relaxed);
        if (pr->bytes_total > 0) {
            const float f = 0.15f * float(double(pr->bytes_read) / double(pr->bytes_total));
            if (f > p->fraction.load(std::memory_order_relaxed)) p->fraction.store(f, std::memory_order_relaxed);
        }
        return UFBX_PROGRESS_CONTINUE;
    };
    ufbx_error err;
    std::u8string src8 = source.u8string();
    std::string src(src8.begin(), src8.end());
    ufbx_scene* loaded = ufbx_load_file(src.c_str(), &opts, &err);
    if (!loaded && err.type == UFBX_ERROR_CANCELLED) throw ImportCancelled();
    if (!loaded) fail(what + ": " + std::string(err.description.data, err.description.length));
    checkpoint(0.15f);
    std::unique_ptr<ufbx_scene, void (*)(ufbx_scene*)> scene(loaded, ufbx_free_scene);

    ImportedModel out;
    Sink baked;
    std::map<const ufbx_material*, int> materialIndex;
    std::vector<const ufbx_material*> materialOf;
    bool anyUv = false, anyColor = false, skipped = false;
    for (const ufbx_node* n : scene->nodes)
        if (n->mesh) {
            anyUv |= n->mesh->vertex_uv.exists;
            anyColor |= n->mesh->vertex_color.exists;
        }

    // Writes the node's mesh into `sink` through `toSpace`. ufbx_matrix_for_normals
    // already flips normals under a negative determinant.
    std::vector<uint32_t> tri;
    // With `skin` (index into rig.skins) set, each vertex takes the weights of
    // the mesh's first skin deformer, joints by ufbx typed id.
    auto emitMesh = [&](Sink& sink, const ufbx_node* n, const ufbx_matrix& toSpace, int skin) {
        const ufbx_mesh* me = n->mesh;
        Mesh& mesh = sink.mesh;
        Emitter emit{mesh, sink.influences, anyUv, anyColor, {}};
        const ufbx_skin_deformer* deformer = skin >= 0 ? me->skin_deformers.data[0] : nullptr;
        ufbx_matrix forNormals = ufbx_matrix_for_normals(&toSpace);
        bool reverse = ufbx_matrix_determinant(&toSpace) >= 0;
        tri.resize(me->max_face_triangles * 3);
        for (size_t fi = 0; fi < me->faces.count; fi++) {
            if (fi % kCheckEvery == 0) checkpoint();
            ufbx_face face = me->faces.data[fi];
            if (face.num_indices < 3) {
                skipped = true;
                continue;
            }
            size_t slot = me->face_material.count ? me->face_material.data[fi] : 0;
            const ufbx_material* material = slot < n->materials.count ? n->materials.data[slot]
                                            : slot < me->materials.count ? me->materials.data[slot] : nullptr;
            auto it = materialIndex.find(material);
            if (it == materialIndex.end()) {
                it = materialIndex.emplace(material, int(materialOf.size())).first;
                materialOf.push_back(material);
            }
            std::vector<int>& sm = sink.submesh(it->second);

            // Right-handed Y-up from here on, like glTF: mirror X and reverse
            // the winding. OBJ and FBX already put V at the bottom, which is
            // where the .mesh has it, so UVs are kept as they are.
            auto vertex = [&](uint32_t ix, bool flat) {
                Vec3 w = {}, nn = {};
                ufbx_vec3 p = ufbx_transform_position(&toSpace, me->vertex_position.values.data[me->vertex_position.indices.data[ix]]);
                w = {float(p.x), float(p.y), float(p.z)};
                if (!flat) {
                    ufbx_vec3 q = ufbx_transform_direction(&forNormals, me->vertex_normal.values.data[me->vertex_normal.indices.data[ix]]);
                    nn = unit({float(q.x), float(q.y), float(q.z)});
                }
                Vec2 uv{0, 0};
                if (me->vertex_uv.exists) {
                    ufbx_vec2 u = me->vertex_uv.values.data[me->vertex_uv.indices.data[ix]];
                    uv = {float(u.x), float(u.y)};
                }
                std::array<float, 4> col{1, 1, 1, 1};
                if (me->vertex_color.exists) {
                    ufbx_vec4 c = me->vertex_color.values.data[me->vertex_color.indices.data[ix]];
                    col = {float(c.x), float(c.y), float(c.z), float(c.w)};
                }
                emit.influence.skin = skin;
                emit.influence.weights.clear();
                const uint32_t vi = me->vertex_indices.data[ix];
                if (deformer && vi < deformer->vertices.count) {
                    const ufbx_skin_vertex& sv = deformer->vertices.data[vi];
                    for (uint32_t k = 0; k < sv.num_weights; k++) {
                        const ufbx_skin_weight& sw = deformer->weights.data[sv.weight_begin + k];
                        const ufbx_node* bone = deformer->clusters.data[sw.cluster_index]->bone_node;
                        if (bone) emit.influence.weights.push_back({int(bone->typed_id), float(sw.weight)});
                    }
                }
                return emit.vertex(w, nn, uv, col);
            };
            uint32_t count = ufbx_triangulate_face(tri.data(), tri.size(), me, face);
            for (uint32_t t = 0; t < count; t++) {
                uint32_t i0 = tri[t * 3], i1 = tri[t * 3 + 1], i2 = tri[t * 3 + 2];
                bool flat = !me->vertex_normal.exists;
                int c0 = vertex(i0, flat), c1 = vertex(i1, flat), c2 = vertex(i2, flat);
                if (flat) addFlatTriangle(mesh, sm, c0, c1, c2, reverse);
                else addTriangle(sm, c0, c1, c2, reverse);
            }
        }
    };

    // The tree below ufbx's root node, whose transform (the axis and unit
    // conversion) goes into each top-level node. Every instance is baked into
    // root space, and written again in its node's own space.
    std::vector<NodeBuild> tree;
    std::vector<int> treeOf(scene->nodes.count, -1);  // by typed id
    std::vector<const ufbx_node*> skinnedNodes;       // by skin index
    size_t meshNodes = 0, meshesDone = 0;
    for (const ufbx_node* n : scene->nodes) meshNodes += n->mesh != nullptr;
    std::function<void(const ufbx_node*, int)> walk = [&](const ufbx_node* n, int parent) {
        NodeBuild nb;
        nb.name.assign(n->name.data, n->name.length);
        nb.parent = parent;
        const ufbx_matrix& local = parent < 0 ? n->node_to_world : n->node_to_parent;
        double basis[3][3], t[3];
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) basis[c][r] = local.cols[c].v[r];
        for (int r = 0; r < 3; r++) t[r] = local.cols[3].v[r];
        nb.transform = mirroredTrs(basis, t);
        nb.world = affOf(n->node_to_world);
        if (n->parent) nb.keySpace = affOf(n->parent->node_to_world);
        if (n->mesh) {
            // A skinned mesh stays in root space, where its bones put it.
            int skin = -1;
            if (n->mesh->skin_deformers.count > 0 && n->mesh->skin_deformers.data[0]->clusters.count > 0) {
                skin = int(skinnedNodes.size());
                skinnedNodes.push_back(n);
            }
            checkpoint(0.15f + 0.35f * float(meshesDone++) / float(meshNodes));
            emitMesh(baked, n, n->geometry_to_world, skin);
            nb.local.emplace();
            nb.rootSpace = skin >= 0;
            emitMesh(*nb.local, n, skin >= 0 ? n->geometry_to_world : n->geometry_to_node, skin);
        }
        int self = int(tree.size());
        treeOf[n->typed_id] = self;
        tree.push_back(std::move(nb));
        for (const ufbx_node* c : n->children) walk(c, self);
    };
    if (scene->root_node->mesh)
        walk(scene->root_node, -1);
    else
        for (const ufbx_node* c : scene->root_node->children) walk(c, -1);
    checkpoint(0.5f);
    if (skipped) out.warnings.push_back("points and lines were left out, only triangles are imported");

    // The skeleton: every bone a skin uses, and the bones above it. Each
    // skin binds root space through the mesh's geometry_to_world.
    RigSource rig;
    for (const ufbx_node* n : skinnedNodes) {
        SkinSource src;
        const Aff toGeometry = inverse(affOf(n->geometry_to_world));
        for (const ufbx_skin_cluster* c : n->mesh->skin_deformers.data[0]->clusters) {
            if (!c->bone_node || treeOf[c->bone_node->typed_id] < 0) continue;
            for (const ufbx_node* b = c->bone_node; b && treeOf[b->typed_id] >= 0 && (b == c->bone_node || b->bone); b = b->parent)
                tree[size_t(treeOf[b->typed_id])].joint = true;
            src.inverseBind[treeOf[c->bone_node->typed_id]] = mul(affOf(c->geometry_to_bone), toGeometry);
        }
        rig.skins.push_back(std::move(src));
    }
    auto remap = [&](Sink& sink) {
        for (Influence& inf : sink.influences)
            for (auto& w : inf.weights) w.first = w.first >= 0 && size_t(w.first) < treeOf.size() ? treeOf[size_t(w.first)] : -1;
    };
    remap(baked);
    for (NodeBuild& nb : tree)
        if (nb.local) remap(*nb.local);

    // Animation stacks, baked by ufbx to linear keys at 30 Hz or less.
    if (!skinnedNodes.empty())
        for (const ufbx_anim_stack* stack : scene->anim_stacks) {
            ufbx_bake_opts bo{};
            bo.resample_rate = 30;
            bo.trim_start_time = true;
            ufbx_error berr;
            ufbx_baked_anim* bakedAnim = ufbx_bake_anim(scene.get(), stack->anim, &bo, &berr);
            if (!bakedAnim) {
                out.warnings.push_back("animation '" + std::string(stack->name.data, stack->name.length) +
                                       "' could not be read: " + std::string(berr.description.data, berr.description.length));
                continue;
            }
            std::unique_ptr<ufbx_baked_anim, void (*)(ufbx_baked_anim*)> guard(bakedAnim, ufbx_free_baked_anim);
            ClipSource clip;
            clip.name.assign(stack->name.data, stack->name.length);
            if (clip.name.empty()) clip.name = "Clip" + std::to_string(rig.clips.size() + 1);
            clip.length = float(std::max(0.0, bakedAnim->playback_duration));
            for (const ufbx_baked_node& bn : bakedAnim->nodes) {
                const int node = bn.typed_id < treeOf.size() ? treeOf[bn.typed_id] : -1;
                if (node < 0 || !tree[size_t(node)].joint) continue;
                auto vec3s = [&](const ufbx_baked_vec3_list& keys, AnimProperty prop) {
                    ChannelSource src;
                    src.node = node;
                    src.channel.property = prop;
                    for (const ufbx_baked_vec3& k : keys) {
                        src.channel.times.push_back(float(std::max(0.0, k.time)));
                        src.channel.values.insert(src.channel.values.end(), {float(k.value.x), float(k.value.y), float(k.value.z)});
                    }
                    if (!src.channel.times.empty()) clip.channels.push_back(std::move(src));
                };
                vec3s(bn.translation_keys, AnimProperty::Position);
                vec3s(bn.scale_keys, AnimProperty::Scale);
                ChannelSource rot;
                rot.node = node;
                rot.channel.property = AnimProperty::Rotation;
                for (const ufbx_baked_quat& k : bn.rotation_keys) {
                    rot.channel.times.push_back(float(std::max(0.0, k.time)));
                    rot.channel.values.insert(rot.channel.values.end(),
                                              {float(k.value.x), float(k.value.y), float(k.value.z), float(k.value.w)});
                }
                if (!rot.channel.times.empty()) clip.channels.push_back(std::move(rot));
            }
            rig.clips.push_back(std::move(clip));
        }

    std::vector<SourceMaterial> mats;
    for (const ufbx_material* mat : materialOf) {
        SourceMaterial sm;
        if (mat) {
            sm.name.assign(mat->name.data, mat->name.length);
            const ufbx_material_map& base = mat->pbr.base_color;
            if (base.has_value) {
                float f = mat->pbr.base_factor.has_value ? float(mat->pbr.base_factor.value_real) : 1.0f;
                sm.color = {float(base.value_vec4.x) * f, float(base.value_vec4.y) * f, float(base.value_vec4.z) * f,
                            base.value_components >= 4 ? float(base.value_vec4.w) : 1.0f};
            }
            const ufbx_texture* t = base.texture;
            if (t && t->type != UFBX_TEXTURE_FILE && t->file_textures.count) t = t->file_textures.data[0];
            if (t && t->type == UFBX_TEXTURE_FILE) {
                sm.image = t;
                sm.load = [t, dir = source.parent_path(), &warnings = out.warnings]() -> std::optional<std::vector<unsigned char>> {
                    auto bytes = textureBytes(*t, dir);
                    if (!bytes) {
                        std::string rel(t->relative_filename.data, t->relative_filename.length);
                        if (rel.empty()) rel.assign(t->absolute_filename.data, t->absolute_filename.length);
                        warnings.push_back("texture " + projectPath(pathFrom(rel).filename()) + " was not found, the material is left untextured");
                    }
                    return bytes;
                };
            }
        }
        mats.push_back(std::move(sm));
    }
    finishImport(out, baked, tree, mats, rig, source, project, settings, stem);
    return out;
}

// Blender exports the .blend as a GLB into a fresh temporary folder, removed
// again whatever happens, and that is imported under the .blend's name.
ImportedModel importBlend(const fs::path& source, const fs::path& project, const ImportSettings& settings,
                          const std::string& stem) {
    BlendExport blender(source);
    return importGltf(blender.get(), project, settings, stem);
}

}  // namespace

bool canImportModel(const fs::path& file) {
    std::string e = lower(projectPath(file.extension()));
    return e == ".gltf" || e == ".glb" || e == ".fbx" || e == ".obj" || e == ".blend";
}

std::string importName(const fs::path& file) { return safeStem(file); }

ImportedModel importModel(const fs::path& source, const fs::path& project, const ImportSettings& settings, const fs::path& as,
                          ImportProgress* progress) {
    if (!canImportModel(source))
        fail(projectPath(source.filename()) + ": only .glb, .gltf, .fbx, .obj and .blend models can be imported");
    if (!(settings.scale > 0) || !std::isfinite(settings.scale)) fail("the import scale must be above 0");
    std::string e = lower(projectPath(source.extension()));
    const std::string stem = safeStem(as.empty() ? source : as);
    struct Scope {
        ImportProgress* old;
        ~Scope() { t_progress = old; }
    } scope{t_progress};
    t_progress = progress;
    checkpoint();
    ImportedModel out = e == ".blend"                ? importBlend(source, project, settings, stem)
                        : e == ".fbx" || e == ".obj" ? importUfbx(source, project, settings, stem)
                                                     : importGltf(source, project, settings, stem);
    checkpoint(1.0f);
    return out;
}

namespace {

// Same float handling as the scene files.
using json = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, std::int64_t, std::uint64_t, float>;

const std::pair<const char*, UpAxis> kUpAxes[] = {{"file", UpAxis::File}, {"y", UpAxis::YUp}, {"z", UpAxis::ZUp}};
const std::pair<const char*, Normals> kNormals[] = {{"import", Normals::Import}, {"recompute", Normals::Recompute}};
const std::pair<const char*, DynamicLighting> kLighting[] = {
    {"auto", DynamicLighting::Auto}, {"on", DynamicLighting::On}, {"off", DynamicLighting::Off}, {"smooth", DynamicLighting::Smooth}};

template <typename E, size_t N>
E enumFrom(const json& j, const std::pair<const char*, E> (&table)[N], const std::string& key) {
    std::string s = j.get<std::string>();
    for (auto& [name, value] : table)
        if (s == name) return value;
    fail("unknown " + key + " '" + s + "'");
}
template <typename E, size_t N>
const char* enumTo(E v, const std::pair<const char*, E> (&table)[N]) {
    for (auto& [name, value] : table)
        if (v == value) return name;
    return table[0].first;
}

json settingsJson(const ImportSettings& s) {
    json j = {{"scale", s.scale},
              {"up", enumTo(s.up, kUpAxes)},
              {"normals", enumTo(s.normals, kNormals)},
              {"smoothingAngle", s.smoothingAngle},
              {"mergeMeshes", s.mergeMeshes},
              {"bitDepth", int(s.bitDepth)},
              {"lighting", enumTo(s.lighting, kLighting)},
              {"source", s.source}};
    // Only when set, so a sidecar without a target stays as it was.
    if (s.targetTriangles > 0) j["targetTriangles"] = s.targetTriangles;
    if (s.targetTriangles > 0 && s.sourceTriangles > 0) j["sourceTriangles"] = s.sourceTriangles;
    return j;
}

}  // namespace

fs::path importSettingsPath(const fs::path& model) {
    fs::path p = model;
    p += ".import.json";
    return p;
}

ImportSettings loadImportSettings(const fs::path& model) {
    ImportSettings s;
    const fs::path file = importSettingsPath(model);
    std::error_code ec;
    if (!fs::exists(file, ec)) return s;
    const std::string what = projectPath(file.filename());
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("cannot open " + what);
    try {
        json j = json::parse(in);
        if (!j.is_object()) fail(what + ": not a JSON object");
        s.scale = j.value("scale", s.scale);
        if (j.contains("up")) s.up = enumFrom(j["up"], kUpAxes, "up axis");
        if (j.contains("normals")) s.normals = enumFrom(j["normals"], kNormals, "normals");
        s.smoothingAngle = j.value("smoothingAngle", s.smoothingAngle);
        s.mergeMeshes = j.value("mergeMeshes", s.mergeMeshes);
        if (j.contains("bitDepth")) {
            int b = j["bitDepth"].get<int>();
            if (b != 4 && b != 8 && b != 16) fail(what + ": bit depth must be 4, 8 or 16");
            s.bitDepth = BitDepth(b);
        }
        if (j.contains("lighting")) s.lighting = enumFrom(j["lighting"], kLighting, "lighting");
        s.source = j.value("source", s.source);
        auto count = [&](const char* key, const char* name) {
            if (!j.contains(key)) return 0;
            const json& t = j[key];
            if (!t.is_number_integer() || t.get<std::int64_t>() < 0 || t.get<std::int64_t>() > 1000000000)
                fail(what + ": " + name + " must be a whole number, 0 or more");
            return t.get<int>();
        };
        s.targetTriangles = count("targetTriangles", "target triangles");
        s.sourceTriangles = count("sourceTriangles", "source triangles");
        json written = settingsJson(s);
        for (auto& [k, v] : j.items())
            if (!written.contains(k) && k != "targetTriangles" && k != "sourceTriangles") s.extra.push_back({k, v.dump()});
    } catch (const json::exception& e) {
        fail(what + ": " + e.what());
    } catch (const std::runtime_error& e) {
        fail(std::string(e.what()).rfind(what, 0) == 0 ? e.what() : what + ": " + e.what());
    }
    return s;
}

void saveImportSettings(const fs::path& model, const ImportSettings& settings) {
    json j = settingsJson(settings);
    for (auto& [k, v] : settings.extra)
        if (!j.contains(k)) j[k] = json::parse(v);
    const std::string text = j.dump(2) + "\n";  // before the file is opened, so a failure leaves it as it was
    const fs::path file = importSettingsPath(model);
    std::ofstream out(file, std::ios::binary);
    out << text;
    if (!out) fail("cannot write " + projectPath(file.filename()));
}

}  // namespace splash
