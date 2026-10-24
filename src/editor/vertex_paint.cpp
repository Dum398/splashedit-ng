#include "editor/vertex_paint.hh"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

#include "editor/pick.hh"
#include "splashpack.hh"

namespace editor {

namespace {

const splash::FlatObject* flatOf(const std::vector<splash::FlatObject>& flats, const splash::Object* o) {
    for (const splash::FlatObject& fo : flats)
        if (fo.object == o) return &fo;
    return nullptr;
}

Rgb toBytes(splash::Vec3 c) { return {splash::colorToPsx(c.x), splash::colorToPsx(c.y), splash::colorToPsx(c.z)}; }

uint8_t mix(uint8_t a, uint8_t b, float t) {
    return uint8_t(std::clamp(std::lround(float(a) + (float(b) - float(a)) * t), 0l, 255l));
}

}  // namespace

float paintWeight(const PaintBrush& b, float dist) {
    if (b.radius <= 0 || dist > b.radius) return 0;
    const float strength = std::clamp(b.strength, 0.f, 1.f);
    const float falloff = std::clamp(b.falloff, 0.f, 1.f);
    const float x = dist / b.radius, inner = 1 - falloff;
    if (x <= inner || falloff <= 0) return strength;
    const float t = std::clamp((x - inner) / falloff, 0.f, 1.f);
    return strength * (1 - t * t * (3 - 2 * t));
}

const splash::Mesh* paintMesh(const Document& doc, const std::string& meshPath) {
    struct Entry {
        std::filesystem::file_time_type time;
        std::optional<splash::Mesh> mesh;
    };
    static std::map<std::filesystem::path, Entry> cache;
    if (meshPath.empty()) return nullptr;
    const std::filesystem::path file = doc.resolve(meshPath);
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) return nullptr;
    auto it = cache.find(file);
    if (it == cache.end() || it->second.time != time) {
        Entry e{time, std::nullopt};
        try {
            e.mesh = splash::loadMesh(file);
        } catch (const std::exception&) {
        }
        it = cache.insert_or_assign(file, std::move(e)).first;
    }
    return it->second.mesh ? &*it->second.mesh : nullptr;
}

std::string paintRefusal(const Document& doc, const ObjectPath& path) {
    const splash::Object* o = doc.object(path);
    if (!o || !o->mesh) return "it has no mesh";
    if (o->terrain) return "a terrain is painted with its own tools";
    if (o->skin) return "skinned meshes are not painted";
    if (!o->prefab.empty() || doc.inInstance(path)) return "it is part of a prefab instance: paint it in the prefab";
    const splash::Mesh* m = paintMesh(doc, o->mesh->mesh);
    if (!m || m->positions.empty()) return "its model does not load";
    if (m->normals.size() != m->positions.size()) return "its model has no normals";
    return {};
}

std::vector<ObjectPath> paintTargets(const Document& doc) {
    std::vector<ObjectPath> out;
    for (const ObjectPath& p : doc.selectedPaths())
        if (paintRefusal(doc, p).empty()) out.push_back(p);
    return out;
}

std::vector<Rgb> currentPaint(const Document& doc, const ObjectPath& path) {
    if (!paintRefusal(doc, path).empty()) return {};
    const splash::Object* o = doc.object(path);
    const splash::Mesh* m = paintMesh(doc, o->mesh->mesh);
    if (o->mesh->vertexColors == splash::VertexColorMode::Painted && o->mesh->paint.size() == m->positions.size())
        return o->mesh->paint;
    const std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    const splash::FlatObject* fo = flatOf(flats, o);
    if (!fo) return {};
    std::vector<Rgb> out;
    for (const splash::Vec3& c : splash::exportVertexColors(flats, *fo, *m)) out.push_back(toBytes(c));
    return out;
}

std::optional<PaintHit> paintHit(const Document& doc, const std::vector<ObjectPath>& paths, splash::Vec3 origin, splash::Vec3 dir) {
    const std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    const Ray ray{origin, dir};
    std::optional<PaintHit> best;
    float bestT = 0;
    for (const ObjectPath& path : paths) {
        if (!paintRefusal(doc, path).empty()) continue;
        const splash::Object* o = doc.object(path);
        const splash::FlatObject* fo = flatOf(flats, o);
        const splash::Mesh* m = paintMesh(doc, o->mesh->mesh);
        if (!fo || !m) continue;
        std::vector<splash::Vec3> wp(m->positions.size());
        for (size_t i = 0; i < wp.size(); i++) wp[i] = fo->localToWorld.point(m->positions[i]);
        int hitTri[3] = {-1, -1, -1};
        float hitT = 0;
        for (const std::vector<int>& sub : m->submeshes)
            for (size_t k = 0; k + 2 < sub.size(); k += 3) {
                const size_t a = size_t(sub[k]), b = size_t(sub[k + 1]), c = size_t(sub[k + 2]);
                if (a >= wp.size() || b >= wp.size() || c >= wp.size()) continue;
                std::optional<float> t = rayTriangle(ray, wp[a], wp[b], wp[c]);
                if (t && *t > 0 && (hitTri[0] < 0 || *t < hitT)) {
                    hitT = *t;
                    hitTri[0] = int(a), hitTri[1] = int(b), hitTri[2] = int(c);
                }
            }
        if (hitTri[0] < 0 || (best && hitT >= bestT)) continue;
        const splash::Vec3 p = origin + dir * hitT;
        // Barycentric weights of the hit point, for the colour under it.
        const splash::Vec3 A = wp[size_t(hitTri[0])], B = wp[size_t(hitTri[1])], C = wp[size_t(hitTri[2])];
        const splash::Vec3 v0 = B - A, v1 = C - A, v2 = p - A;
        const float d00 = splash::dot(v0, v0), d01 = splash::dot(v0, v1), d11 = splash::dot(v1, v1);
        const float d20 = splash::dot(v2, v0), d21 = splash::dot(v2, v1), den = d00 * d11 - d01 * d01;
        float wb = 0, wc = 0;
        if (std::fabs(den) > 1e-12f) wb = (d11 * d20 - d01 * d21) / den, wc = (d00 * d21 - d01 * d20) / den;
        wb = std::clamp(wb, 0.f, 1.f), wc = std::clamp(wc, 0.f, 1.f - wb);
        const float wa = 1 - wb - wc;
        const std::vector<Rgb> cols = currentPaint(doc, path);
        Rgb col{128, 128, 128};
        if (cols.size() == wp.size())
            for (int ch = 0; ch < 3; ch++)
                col[size_t(ch)] = uint8_t(std::clamp(
                    std::lround(wa * cols[size_t(hitTri[0])][size_t(ch)] + wb * cols[size_t(hitTri[1])][size_t(ch)] +
                                wc * cols[size_t(hitTri[2])][size_t(ch)]),
                    0l, 255l));
        best = PaintHit{path, p, col};
        bestT = hitT;
    }
    return best;
}

void beginPaintStroke(PaintStroke& s, Document& doc, const std::vector<ObjectPath>& paths, const PaintBrush& brush) {
    doc.endMerge();
    s = {};
    s.active = true;
    s.brush = brush;
    for (const ObjectPath& p : paths) {
        std::vector<Rgb> c = currentPaint(doc, p);
        if (c.empty()) continue;
        s.paths.push_back(p);
        s.weight.emplace_back(c.size(), 0.f);
        s.start.push_back(std::move(c));
        s.touched.push_back(0);
    }
}

bool stepPaintStroke(PaintStroke& s, Document& doc, splash::Vec3 worldHit) {
    if (!s.active || s.paths.empty()) return false;
    const std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    bool changed = false;
    for (size_t i = 0; i < s.paths.size(); i++) {
        const splash::Object* o = doc.object(s.paths[i]);
        const splash::FlatObject* fo = o ? flatOf(flats, o) : nullptr;
        const splash::Mesh* m = o && o->mesh ? paintMesh(doc, o->mesh->mesh) : nullptr;
        if (!fo || !m || m->positions.size() != s.weight[i].size()) continue;
        for (size_t v = 0; v < m->positions.size(); v++) {
            const float w = paintWeight(s.brush, splash::magnitude(fo->localToWorld.point(m->positions[v]) - worldHit));
            if (w <= s.weight[i][v]) continue;
            const Rgb& a = s.start[i][v];
            const Rgb before{mix(a[0], s.brush.color[0], s.weight[i][v]), mix(a[1], s.brush.color[1], s.weight[i][v]),
                             mix(a[2], s.brush.color[2], s.weight[i][v])};
            s.weight[i][v] = w;
            const Rgb after{mix(a[0], s.brush.color[0], w), mix(a[1], s.brush.color[1], w), mix(a[2], s.brush.color[2], w)};
            if (before != after) changed = true, s.touched[i] = 1;
        }
    }
    if (!changed) return false;
    doc.editEach(s.paths, [&](const ObjectPath& p, splash::Object& ob) {
        const size_t i = size_t(std::find(s.paths.begin(), s.paths.end(), p) - s.paths.begin());
        if (i >= s.paths.size() || !s.touched[i] || !ob.mesh) return;
        std::vector<Rgb> layer(s.start[i].size());
        for (size_t v = 0; v < layer.size(); v++) {
            const Rgb& a = s.start[i][v];
            const float w = s.weight[i][v];
            layer[v] = {mix(a[0], s.brush.color[0], w), mix(a[1], s.brush.color[1], w), mix(a[2], s.brush.color[2], w)};
        }
        ob.mesh->paint = std::move(layer);
        ob.mesh->vertexColors = splash::VertexColorMode::Painted;
    }, kPaintStrokeKey);
    s.steps++;
    return true;
}

void endPaintStroke(PaintStroke& s, Document& doc) {
    if (s.active) doc.endMerge();
    s.active = false;
}

int fillPaint(Document& doc, const std::vector<ObjectPath>& paths, Rgb color) {
    std::vector<ObjectPath> targets;
    std::vector<size_t> counts;
    for (const ObjectPath& p : paths) {
        if (!paintRefusal(doc, p).empty()) continue;
        const splash::Object* o = doc.object(p);
        const size_t n = paintMesh(doc, o->mesh->mesh)->positions.size();
        if (o->mesh->vertexColors == splash::VertexColorMode::Painted && o->mesh->paint == std::vector<Rgb>(n, color)) continue;
        targets.push_back(p);
        counts.push_back(n);
    }
    if (targets.empty()) return 0;
    doc.endMerge();
    return doc.editEach(targets, [&](const ObjectPath& p, splash::Object& ob) {
        const size_t i = size_t(std::find(targets.begin(), targets.end(), p) - targets.begin());
        if (i >= targets.size() || !ob.mesh) return;
        ob.mesh->paint.assign(counts[i], color);
        ob.mesh->vertexColors = splash::VertexColorMode::Painted;
    });
}

bool setPaintedMode(Document& doc, const ObjectPath& path) {
    const splash::Object* o = doc.object(path);
    if (!o || !o->mesh || o->mesh->vertexColors == splash::VertexColorMode::Painted) return false;
    // A layer that fits is kept (painted before, then another mode picked).
    std::vector<Rgb> layer = o->mesh->paint;
    const splash::Mesh* m = paintRefusal(doc, path).empty() ? paintMesh(doc, o->mesh->mesh) : nullptr;
    if (m && layer.size() != m->positions.size()) layer = currentPaint(doc, path);
    return doc.edit(path, [&](splash::Object& ob) {
        if (!ob.mesh) return;
        ob.mesh->vertexColors = splash::VertexColorMode::Painted;
        ob.mesh->paint = layer;
    });
}

bool clearPaint(Document& doc, const ObjectPath& path) {
    const splash::Object* o = doc.object(path);
    if (!o || !o->mesh || (o->mesh->paint.empty() && o->mesh->vertexColors != splash::VertexColorMode::Painted)) return false;
    return doc.edit(path, [](splash::Object& ob) {
        if (!ob.mesh) return;
        ob.mesh->paint.clear();
        if (ob.mesh->vertexColors == splash::VertexColorMode::Painted) ob.mesh->vertexColors = splash::VertexColorMode::Baked;
    });
}

}  // namespace editor
