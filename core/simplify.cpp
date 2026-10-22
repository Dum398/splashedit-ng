#include "simplify.hh"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

#include <meshoptimizer.h>

namespace splash {

namespace {

// One welded vertex: everything a .mesh vertex carries but its normal, which
// the caller rebuilds, plus the submesh it belongs to. Welding compares these
// bytewise, so every field is a 4-byte value and nothing is padding.
struct Vertex {
    float p[3], uv[2], c[4];
    int joints[4];
    float weights[4];
    int submesh;
};

// The simplifier's input: welded vertices, one index buffer over every
// submesh, positions and attributes as meshopt reads them.
struct Welded {
    std::vector<Vertex> verts;
    std::vector<Vec3> normals;  // one per welded vertex: the first corner's
    std::vector<unsigned int> indices;
    std::vector<float> positions, attrs, weights;
};

Welded weld(const Mesh& mesh, int triangles) {
    const bool hasUv = !mesh.uv.empty(), hasColor = !mesh.colors.empty(), hasSkin = mesh.skin.has_value();
    auto zero = [](float f) { return f + 0.0f; };  // -0 -> 0, so it welds with 0
    std::vector<Vertex> corners;
    std::vector<Vec3> cornerNormals;
    corners.reserve(size_t(triangles) * 3);
    for (size_t s = 0; s < mesh.submeshes.size(); s++) {
        const std::vector<int>& sm = mesh.submeshes[s];
        for (size_t i = 0; i + 2 < sm.size(); i += 3)
            for (size_t k = 0; k < 3; k++) {
                const size_t v = size_t(sm[i + k]);
                Vertex c;
                std::memset(&c, 0, sizeof c);
                const Vec3 p = mesh.positions[v];
                c.p[0] = zero(p.x), c.p[1] = zero(p.y), c.p[2] = zero(p.z);
                if (hasUv) c.uv[0] = zero(mesh.uv[v].x), c.uv[1] = zero(mesh.uv[v].y);
                if (hasColor)
                    for (size_t j = 0; j < 4; j++) c.c[j] = zero(mesh.colors[v][j]);
                if (hasSkin)
                    for (size_t j = 0; j < 4; j++) {
                        c.joints[j] = mesh.skin->vertexJoints[v][j];
                        c.weights[j] = zero(mesh.skin->vertexWeights[v][j]);
                    }
                c.submesh = int(s);
                corners.push_back(c);
                cornerNormals.push_back(v < mesh.normals.size() ? mesh.normals[v] : Vec3{});
            }
    }
    Welded w;
    std::vector<unsigned int> remap(corners.size());
    const size_t unique =
        meshopt_generateVertexRemap(remap.data(), nullptr, corners.size(), corners.data(), corners.size(), sizeof(Vertex));
    w.verts.resize(unique);
    meshopt_remapVertexBuffer(w.verts.data(), corners.data(), corners.size(), sizeof(Vertex), remap.data());
    w.indices.resize(corners.size());
    meshopt_remapIndexBuffer(w.indices.data(), nullptr, corners.size(), remap.data());
    w.normals.assign(unique, Vec3{});
    for (size_t i = corners.size(); i-- > 0;) w.normals[remap[i]] = cornerNormals[i];

    // Attributes, weighted as the README suggests: about 1 for normalised
    // values (normals, 0..1 colours); for UVs the reciprocal of their average
    // density, 1 / sqrt(mean |UV area|) over the triangles.
    w.weights = {0.5f, 0.5f, 0.5f};
    size_t uvAt = 0, colorAt = 0;
    if (hasUv) {
        double area = 0;
        for (size_t t = 0; t < w.indices.size(); t += 3) {
            const float *a = w.verts[w.indices[t]].uv, *b = w.verts[w.indices[t + 1]].uv, *c = w.verts[w.indices[t + 2]].uv;
            area += std::fabs(double(b[0] - a[0]) * (c[1] - a[1]) - double(c[0] - a[0]) * (b[1] - a[1])) * 0.5;
        }
        const double mean = area / double(w.indices.size() / 3);
        const float uw = mean > 0 ? float(std::clamp(1.0 / std::sqrt(mean), 1.0, 1000.0)) : 1.0f;
        uvAt = w.weights.size();
        w.weights.insert(w.weights.end(), {uw, uw});
    }
    if (hasColor) {
        colorAt = w.weights.size();
        w.weights.insert(w.weights.end(), {1.0f, 1.0f, 1.0f, 1.0f});
    }
    const size_t stride = w.weights.size();
    w.attrs.resize(unique * stride);
    w.positions.resize(unique * 3);
    for (size_t v = 0; v < unique; v++) {
        float* a = &w.attrs[v * stride];
        std::memcpy(&w.positions[v * 3], w.verts[v].p, sizeof(float) * 3);
        a[0] = w.normals[v].x, a[1] = w.normals[v].y, a[2] = w.normals[v].z;
        if (hasUv) std::memcpy(a + uvAt, w.verts[v].uv, sizeof(float) * 2);
        if (hasColor) std::memcpy(a + colorAt, w.verts[v].c, sizeof(float) * 4);
    }
    return w;
}

std::vector<unsigned int> simplify(const Welded& w, int target, bool permissive, float& error) {
    const size_t n = w.verts.size(), stride = w.weights.size();
    std::vector<unsigned char> locks;
    unsigned int options = 0;
    if (permissive) {
        // README, "Permissive simplification": vertices sharing a position
        // that differ in UV, colour or submesh are protected, so collapses
        // still cannot cross those seams; only normal seams (rebuilt after) give.
        options |= meshopt_SimplifyPermissive;
        std::vector<unsigned int> byPos(n);
        meshopt_generatePositionRemap(byPos.data(), w.positions.data(), n, sizeof(float) * 3);
        locks.assign(n, 0);
        for (size_t v = 0; v < n; v++) {
            const Vertex &a = w.verts[v], &b = w.verts[byPos[v]];
            if (byPos[v] != v &&
                (std::memcmp(a.uv, b.uv, sizeof a.uv) || std::memcmp(a.c, b.c, sizeof a.c) || a.submesh != b.submesh))
                locks[v] = locks[byPos[v]] = meshopt_SimplifyVertex_Protect;
        }
    }
    std::vector<unsigned int> out(w.indices.size());
    out.resize(meshopt_simplifyWithAttributes(out.data(), w.indices.data(), w.indices.size(), w.positions.data(), n,
                                              sizeof(float) * 3, w.attrs.data(), sizeof(float) * stride, w.weights.data(),
                                              stride, permissive ? locks.data() : nullptr, size_t(target) * 3, FLT_MAX,
                                              options, &error));
    return out;
}

// Connected parts (triangles joined through shared positions, whatever their
// attributes) of more than kPartTriangles triangles in `indices` that have no
// triangle left in `out`.
constexpr int kPartTriangles = 4;
int partsRemoved(const Welded& w, const std::vector<unsigned int>& out) {
    const size_t n = w.verts.size();
    std::vector<unsigned int> byPos(n), parent(n);
    meshopt_generatePositionRemap(byPos.data(), w.positions.data(), n, sizeof(float) * 3);
    for (size_t v = 0; v < n; v++) parent[v] = unsigned(v);
    auto find = [&](unsigned int v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    for (size_t t = 0; t + 2 < w.indices.size(); t += 3)
        for (size_t k = 1; k < 3; k++) {
            const unsigned int a = find(byPos[w.indices[t]]), b = find(byPos[w.indices[t + k]]);
            if (a != b) parent[a] = b;
        }
    std::vector<int> before(n, 0);
    std::vector<char> kept(n, 0);
    for (size_t t = 0; t + 2 < w.indices.size(); t += 3) before[find(byPos[w.indices[t]])]++;
    for (size_t t = 0; t + 2 < out.size(); t += 3) kept[find(byPos[out[t]])] = 1;
    int removed = 0;
    for (size_t v = 0; v < n; v++) removed += before[v] > kPartTriangles && !kept[v];
    return removed;
}

}  // namespace

// The recipe is meshoptimizer's README ("Simplification", "Attribute-aware
// simplification", "Permissive simplification"). Identical vertices are
// welded so the simplifier sees the real topology; the normal is left out of
// the weld (README: "weld the vertices without taking into account vertex
// attributes that aren't critical and can be rebuilt later"), since a
// faceted mesh would otherwise be all seams. A vertex sharing its position
// with another but differing in UV, colour or submesh keeps its own index,
// which the simplifier reads as a discontinuity and preserves.
SimplifyResult simplifyToTriangles(Mesh& mesh, int target) {
    SimplifyResult r;
    for (const auto& sm : mesh.submeshes) r.before += int(sm.size() / 3);
    r.after = r.before;
    if (target <= 0 || target >= r.before) return r;

    const Welded w = weld(mesh, r.before);
    std::vector<unsigned int> out = simplify(w, target, false, r.error);
    if (out.size() / 3 > size_t(target) + size_t(target) / 10) {
        float error = 0;
        std::vector<unsigned int> loose = simplify(w, target, true, error);
        if (loose.size() < out.size()) out = std::move(loose), r.error = error, r.permissive = true;
    }

    r.partsRemoved = partsRemoved(w, out);

    // Back to a Mesh: the vertices still used, in order of first use, and each
    // triangle to the submesh its corners came from.
    const bool hasUv = !mesh.uv.empty(), hasColor = !mesh.colors.empty(), hasSkin = mesh.skin.has_value();
    Mesh res;
    res.skin = mesh.skin;
    if (res.skin) res.skin->vertexJoints.clear(), res.skin->vertexWeights.clear();
    res.submeshes.resize(mesh.submeshes.size());
    std::vector<int> made(w.verts.size(), -1);
    for (size_t i = 0; i < out.size(); i++) {
        const unsigned int v = out[i];
        if (made[v] < 0) {
            const Vertex& c = w.verts[v];
            made[v] = int(res.positions.size());
            res.positions.push_back({c.p[0], c.p[1], c.p[2]});
            res.normals.push_back(w.normals[v]);
            if (hasUv) res.uv.push_back({c.uv[0], c.uv[1]});
            if (hasColor) res.colors.push_back({c.c[0], c.c[1], c.c[2], c.c[3]});
            if (hasSkin) {
                res.skin->vertexJoints.push_back({c.joints[0], c.joints[1], c.joints[2], c.joints[3]});
                res.skin->vertexWeights.push_back({c.weights[0], c.weights[1], c.weights[2], c.weights[3]});
            }
        }
        res.submeshes[size_t(w.verts[out[i - i % 3]].submesh)].push_back(made[v]);
    }
    mesh = std::move(res);
    r.after = int(out.size() / 3);
    return r;
}

}  // namespace splash
