#include "terrain.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;

namespace splash {

namespace {

size_t vid(const TerrainComponent& t, int x, int z) { return size_t(z) * size_t(t.resolution + 1) + size_t(x); }

float hv(const TerrainComponent& t, int x, int z) {
    x = std::clamp(x, 0, t.resolution);
    z = std::clamp(z, 0, t.resolution);
    return t.heights[vid(t, x, z)];
}

float cellX(const TerrainComponent& t) { return t.sizeX / float(t.resolution); }
float cellZ(const TerrainComponent& t) { return t.sizeZ / float(t.resolution); }

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
}
uint32_t getU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void eachTerrain(std::vector<Object>& objs, const std::function<void(Object&)>& fn) {
    for (Object& o : objs) {
        if (o.terrain) fn(o);
        eachTerrain(o.children, fn);
    }
}
void eachTerrain(const std::vector<Object>& objs, const std::function<void(const Object&)>& fn) {
    for (const Object& o : objs) {
        if (o.terrain) fn(o);
        eachTerrain(o.children, fn);
    }
}

// 3x3 inverse of the linear part.
bool invert3(const Mat34& m, float r[3][3]) {
    const float(&a)[3][4] = m.m;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::fabs(det) < 1e-12f) return false;
    const float id = 1.f / det;
    r[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
    r[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id;
    r[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    r[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id;
    r[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
    r[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
    r[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
    r[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
    r[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    return true;
}

}  // namespace

void initTerrain(TerrainComponent& t, int resolution) {
    t.resolution = std::clamp(resolution, 1, kTerrainMaxResolution);
    t.heights.assign(size_t(t.resolution + 1) * size_t(t.resolution + 1), 0.f);
    t.cellLayers.assign(size_t(t.resolution) * size_t(t.resolution), 0);
}

bool terrainLoaded(const TerrainComponent& t) {
    return t.resolution >= 1 && t.resolution <= kTerrainMaxResolution &&
           t.heights.size() == size_t(t.resolution + 1) * size_t(t.resolution + 1) &&
           t.cellLayers.size() == size_t(t.resolution) * size_t(t.resolution);
}

void loadTerrainData(TerrainComponent& t, const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read terrain " + file.string());
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "SPTR", 4) != 0) throw std::runtime_error(file.string() + ": not a terrain file");
    const uint32_t version = getU32(&b[4]);
    if (version != kTerrainFileVersion)
        throw std::runtime_error(file.string() + ": terrain version " + std::to_string(version) + " is not supported");
    const uint32_t n = getU32(&b[8]);
    if (n < 1 || n > uint32_t(kTerrainMaxResolution))
        throw std::runtime_error(file.string() + ": terrain resolution " + std::to_string(n) + " is out of range");
    const size_t nv = size_t(n + 1) * (n + 1), nc = size_t(n) * n;
    if (b.size() != 12 + nv * 4 + nc) throw std::runtime_error(file.string() + ": terrain file is truncated");
    TerrainComponent g = t;
    g.resolution = int(n);
    g.heights.resize(nv);
    for (size_t i = 0; i < nv; i++) {
        uint32_t u = getU32(&b[12 + i * 4]);
        float f;
        std::memcpy(&f, &u, 4);
        if (!std::isfinite(f)) throw std::runtime_error(file.string() + ": terrain height is not a number");
        g.heights[i] = f;
    }
    g.cellLayers.assign(b.begin() + std::ptrdiff_t(12 + nv * 4), b.end());
    for (uint8_t& l : g.cellLayers) l = uint8_t(std::min<int>(l, kTerrainMaxLayers - 1));
    t = std::move(g);
}

void saveTerrainData(const TerrainComponent& t, const fs::path& file) {
    if (!terrainLoaded(t)) throw std::runtime_error("terrain " + t.data + " has no grid to save");
    std::vector<uint8_t> b{'S', 'P', 'T', 'R'};
    putU32(b, kTerrainFileVersion);
    putU32(b, uint32_t(t.resolution));
    for (float h : t.heights) {
        uint32_t u;
        std::memcpy(&u, &h, 4);
        putU32(b, u);
    }
    b.insert(b.end(), t.cellLayers.begin(), t.cellLayers.end());
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write terrain " + file.string());
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    if (!out) throw std::runtime_error("cannot write terrain " + file.string());
}

std::vector<std::string> loadTerrains(Scene& scene, const fs::path& projectRoot) {
    std::vector<std::string> errs;
    eachTerrain(scene.objects, [&](Object& o) {
        TerrainComponent& t = *o.terrain;
        if (terrainLoaded(t)) return;
        try {
            if (t.data.empty()) throw std::runtime_error("no data file set");
            loadTerrainData(t, projectRoot / t.data);
        } catch (const std::exception& e) {
            errs.push_back(o.name + ": " + e.what());
        }
    });
    return errs;
}

std::vector<std::string> saveTerrains(const Scene& scene, const fs::path& projectRoot) {
    std::vector<std::string> errs;
    eachTerrain(scene.objects, [&](const Object& o) {
        const TerrainComponent& t = *o.terrain;
        if (!terrainLoaded(t) || t.data.empty()) return;
        try {
            saveTerrainData(t, projectRoot / t.data);
        } catch (const std::exception& e) {
            errs.push_back(o.name + ": " + e.what());
        }
    });
    return errs;
}

std::string uniqueTerrainData(const Scene& scene, const fs::path& projectRoot, const std::string& want) {
    std::vector<std::string> used;
    eachTerrain(scene.objects, [&](const Object& o) { used.push_back(o.terrain->data); });
    const fs::path w(want);
    const std::string stem = (w.parent_path() / w.stem()).generic_string(), ext = w.extension().string();
    for (int i = 1;; i++) {
        const std::string p = i == 1 ? want : stem + "_" + std::to_string(i) + ext;
        std::error_code ec;
        if (std::find(used.begin(), used.end(), p) != used.end()) continue;
        if (!projectRoot.empty() && fs::exists(projectRoot / p, ec)) continue;
        return p;
    }
}

Mesh buildTerrainMesh(const TerrainComponent& t, int layers) {
    Mesh m;
    if (!terrainLoaded(t)) return m;
    const int n = t.resolution;
    const float dx = cellX(t), dz = cellZ(t);
    const float x0 = -t.sizeX * 0.5f, z0 = -t.sizeZ * 0.5f;
    const float tile = float(std::max(1, t.tileCells));
    m.positions.reserve(t.heights.size());
    for (int z = 0; z <= n; z++)
        for (int x = 0; x <= n; x++) {
            m.positions.push_back({x0 + float(x) * dx, hv(t, x, z), z0 + float(z) * dz});
            // Central differences inside, one-sided at the edges.
            const int xa = std::max(x - 1, 0), xb = std::min(x + 1, n), za = std::max(z - 1, 0), zb = std::min(z + 1, n);
            const float sx = (hv(t, xb, z) - hv(t, xa, z)) / (float(xb - xa) * dx);
            const float sz = (hv(t, x, zb) - hv(t, x, za)) / (float(zb - za) * dz);
            m.normals.push_back(normalized(Vec3{-sx, 1.f, -sz}));
            m.uv.push_back({float(x) / tile, float(z) / tile});
        }
    layers = std::clamp(layers, 1, kTerrainMaxLayers);
    m.submeshes.assign(size_t(layers), {});
    for (int z = 0; z < n; z++)
        for (int x = 0; x < n; x++) {
            const int a = int(vid(t, x, z)), b = int(vid(t, x + 1, z)), c = int(vid(t, x, z + 1)), d = int(vid(t, x + 1, z + 1));
            const int l = std::min<int>(t.cellLayers[size_t(z) * size_t(n) + size_t(x)], layers - 1);
            std::vector<int>& sm = m.submeshes[size_t(l)];
            // Same horizontal length both ways: the shorter diagonal is the flatter one.
            const float ad = std::fabs(t.heights[size_t(a)] - t.heights[size_t(d)]);
            const float bc = std::fabs(t.heights[size_t(b)] - t.heights[size_t(c)]);
            const bool useAD = ad < bc - 1e-6f ? true : bc < ad - 1e-6f ? false : ((x + z) & 1) == 0;
            // Clockwise seen from above (Unity's front face), normal +Y.
            if (useAD)
                sm.insert(sm.end(), {a, c, d, a, d, b});
            else
                sm.insert(sm.end(), {a, c, b, b, c, d});
        }
    return m;
}

float terrainHeightAt(const TerrainComponent& t, float x, float z) {
    if (!terrainLoaded(t)) return 0;
    const int n = t.resolution;
    const float fx = std::clamp((x + t.sizeX * 0.5f) / cellX(t), 0.f, float(n));
    const float fz = std::clamp((z + t.sizeZ * 0.5f) / cellZ(t), 0.f, float(n));
    const int ix = std::min(int(fx), n - 1), iz = std::min(int(fz), n - 1);
    const float u = fx - float(ix), v = fz - float(iz);
    const float h0 = hv(t, ix, iz) * (1 - u) + hv(t, ix + 1, iz) * u;
    const float h1 = hv(t, ix, iz + 1) * (1 - u) + hv(t, ix + 1, iz + 1) * u;
    return h0 * (1 - v) + h1 * v;
}

void resampleTerrain(TerrainComponent& t, int resolution) {
    resolution = std::clamp(resolution, 1, kTerrainMaxResolution);
    if (!terrainLoaded(t)) {
        initTerrain(t, resolution);
        return;
    }
    if (resolution == t.resolution) return;
    TerrainComponent g = t;
    initTerrain(g, resolution);
    const float x0 = -t.sizeX * 0.5f, z0 = -t.sizeZ * 0.5f;
    for (int z = 0; z <= resolution; z++)
        for (int x = 0; x <= resolution; x++)
            g.heights[vid(g, x, z)] = terrainHeightAt(t, x0 + t.sizeX * float(x) / float(resolution),
                                                       z0 + t.sizeZ * float(z) / float(resolution));
    for (int z = 0; z < resolution; z++)
        for (int x = 0; x < resolution; x++) {
            const int ox = std::min(int((float(x) + 0.5f) * float(t.resolution) / float(resolution)), t.resolution - 1);
            const int oz = std::min(int((float(z) + 0.5f) * float(t.resolution) / float(resolution)), t.resolution - 1);
            g.cellLayers[size_t(z) * size_t(resolution) + size_t(x)] = t.cellLayers[size_t(oz) * size_t(t.resolution) + size_t(ox)];
        }
    t = std::move(g);
}

std::optional<Vec3> terrainRayHit(const TerrainComponent& t, Vec3 o, Vec3 d) {
    if (!terrainLoaded(t) || sqrMagnitude(d) <= 0) return std::nullopt;
    d = normalized(d);
    float lo = *std::min_element(t.heights.begin(), t.heights.end()) - 1e-3f;
    float hi = *std::max_element(t.heights.begin(), t.heights.end()) + 1e-3f;
    const float mn[3] = {-t.sizeX * 0.5f, lo, -t.sizeZ * 0.5f}, mx[3] = {t.sizeX * 0.5f, hi, t.sizeZ * 0.5f};
    float t0 = 0, t1 = 1e30f;
    for (int ax = 0; ax < 3; ax++) {
        const float oa = o[ax], da = d[ax];
        if (std::fabs(da) < 1e-12f) {
            if (oa < mn[ax] || oa > mx[ax]) return std::nullopt;
            continue;
        }
        float a = (mn[ax] - oa) / da, b = (mx[ax] - oa) / da;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return std::nullopt;
    }
    const float step = std::min(cellX(t), cellZ(t)) * 0.25f;
    auto above = [&](float s) {
        const Vec3 p = o + d * s;
        return p.y - terrainHeightAt(t, p.x, p.z);
    };
    float prev = t0, fprev = above(t0);
    if (fprev <= 0) return o + d * t0;  // starts under the surface (inside the box)
    for (float s = t0 + step;; s += step) {
        s = std::min(s, t1);
        const float f = above(s);
        if (f <= 0) {
            float a = prev, b = s;
            for (int i = 0; i < 24; i++) {
                const float m = 0.5f * (a + b);
                (above(m) > 0 ? a : b) = m;
            }
            return o + d * b;
        }
        prev = s;
        if (s >= t1) break;
    }
    return std::nullopt;
}

Vec3 inverseTransformDirection(const Mat34& m, Vec3 d) {
    float r[3][3];
    if (!invert3(m, r)) return {};
    return {r[0][0] * d.x + r[0][1] * d.y + r[0][2] * d.z, r[1][0] * d.x + r[1][1] * d.y + r[1][2] * d.z,
            r[2][0] * d.x + r[2][1] * d.y + r[2][2] * d.z};
}

Vec3 inverseTransformPoint(const Mat34& m, Vec3 p) { return inverseTransformDirection(m, p - m.position()); }

float terrainBrushWeight(const TerrainBrush& b, float d) {
    if (b.radius <= 0 || d >= b.radius) return 0;
    const float inner = b.radius * (1 - std::clamp(b.falloff, 0.f, 1.f));
    if (d <= inner) return 1;
    const float s = 1 - (d - inner) / (b.radius - inner);
    return s * s * (3 - 2 * s);
}

bool applyTerrainBrush(TerrainComponent& t, float px, float pz, const TerrainBrush& b, float dt) {
    if (!terrainLoaded(t) || b.radius <= 0) return false;
    const int n = t.resolution;
    const float dx = cellX(t), dz = cellZ(t), x0 = -t.sizeX * 0.5f, z0 = -t.sizeZ * 0.5f;
    bool changed = false;
    if (b.tool == TerrainTool::Paint) {
        const uint8_t layer = uint8_t(std::clamp(b.layer, 0, kTerrainMaxLayers - 1));
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                const float cx = x0 + (float(x) + 0.5f) * dx, cz = z0 + (float(z) + 0.5f) * dz;
                if (std::hypot(cx - px, cz - pz) >= b.radius) continue;
                uint8_t& l = t.cellLayers[size_t(z) * size_t(n) + size_t(x)];
                if (l != layer) {
                    l = layer;
                    changed = true;
                }
            }
        return changed;
    }
    const int xa = std::max(0, int(std::floor((px - b.radius - x0) / dx))), xb = std::min(n, int(std::ceil((px + b.radius - x0) / dx)));
    const int za = std::max(0, int(std::floor((pz - b.radius - z0) / dz))), zb = std::min(n, int(std::ceil((pz + b.radius - z0) / dz)));
    const std::vector<float> before = b.tool == TerrainTool::Smooth ? t.heights : std::vector<float>{};
    for (int z = za; z <= zb; z++)
        for (int x = xa; x <= xb; x++) {
            const float w = terrainBrushWeight(b, std::hypot(x0 + float(x) * dx - px, z0 + float(z) * dz - pz));
            if (w <= 0) continue;
            float& h = t.heights[vid(t, x, z)];
            const float old = h;
            const float k = std::clamp(b.strength * w * dt, 0.f, 1.f);
            switch (b.tool) {
                case TerrainTool::Raise: h += b.strength * w * dt; break;
                case TerrainTool::Lower: h -= b.strength * w * dt; break;
                case TerrainTool::Smooth: {
                    auto at = [&](int ix, int iz) {
                        return before[vid(t, std::clamp(ix, 0, n), std::clamp(iz, 0, n))];
                    };
                    const float avg = (at(x - 1, z) + at(x + 1, z) + at(x, z - 1) + at(x, z + 1) + at(x, z)) / 5.f;
                    h += (avg - h) * k;
                    break;
                }
                case TerrainTool::Flatten: h += (b.target - h) * k; break;
                case TerrainTool::Paint: break;
            }
            if (h != old) changed = true;
        }
    return changed;
}

}  // namespace splash
