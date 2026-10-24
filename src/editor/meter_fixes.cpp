#include "editor/meter_fixes.hh"

#include <algorithm>
#include <exception>
#include <map>
#include <numeric>
#include <set>
#include <unordered_set>

#include "budget.hh"
#include "editor/project.hh"
#include "simplify.hh"
#include "texture.hh"

namespace editor {

size_t meterUsed(FixMeter m, const splash::ExportStats& s) {
    switch (m) {
        case FixMeter::Vram: return splash::vramBudget(s).used;
        case FixMeter::Ram: return splash::ramBudget(s).used;
        case FixMeter::Tris: return splash::triangleBudget(s).used;
    }
    return 0;
}

const char* meterName(FixMeter m) {
    switch (m) {
        case FixMeter::Vram: return "vram";
        case FixMeter::Ram: return "ram";
        case FixMeter::Tris: return "tris";
    }
    return "";
}

namespace {

int bits(splash::BitDepth d) { return int(d); }

splash::Object* objectAt(splash::Scene& scene, const ObjectPath& path) {
    std::vector<splash::Object>* level = &scene.objects;
    splash::Object* o = nullptr;
    for (int i : path) {
        if (i < 0 || size_t(i) >= level->size()) return nullptr;
        o = &(*level)[size_t(i)];
        level = &o->children;
    }
    return o;
}

// The one edit each kind makes, shared by the dry runs and the document.
void fixObject(splash::Object& o, const MeterFix& fix) {
    if (fix.kind == MeterFix::Kind::TextureDepth && o.mesh) o.mesh->bitDepth = fix.to;
}
void fixSettings(splash::SceneSettings& s, const MeterFix& fix) {
    if (fix.kind == MeterFix::Kind::ResetOrderingTable) s.orderingTableSize = 0;
    if (fix.kind == MeterFix::Kind::ResetBumpAllocator) s.bumpAllocatorSize = 0;
}

// An exported object the editor can change: active down from the top, not a
// prefab instance or inside one (their content comes from the prefab file).
struct Exported {
    ObjectPath path;
    const splash::Object* object;
};
void walk(const std::vector<splash::Object>& level, ObjectPath& path, std::vector<Exported>& out) {
    for (size_t i = 0; i < level.size(); i++) {
        const splash::Object& o = level[i];
        if (!o.active || !o.prefab.empty()) continue;
        path.push_back(int(i));
        out.push_back({path, &o});
        walk(o.children, path, out);
        path.pop_back();
    }
}

std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Distinct colours after reduction to the PS1's 15 bits (a fully
// transparent texel counts once), and the texel count. 0, 0 when unreadable.
std::pair<int, size_t> colours15(const std::filesystem::path& file) {
    try {
        const splash::Image img = splash::loadImage(file);
        std::unordered_set<uint32_t> seen;
        for (const splash::Image::Px& p : img.pixels) {
            if (p.a <= 0) {
                seen.insert(0x10000u);
                continue;
            }
            auto q = [](float v) { return uint32_t(std::clamp(int(v * 31.0f + 0.5f), 0, 31)); };
            seen.insert(q(p.r) | q(p.g) << 5 | q(p.b) << 10);
        }
        return {int(seen.size()), img.pixels.size()};
    } catch (const std::exception&) {
        return {0, 0};
    }
}

splash::ExportResult dryRun(splash::Scene scene, const std::filesystem::path& root, const splash::VramSettings& vram) {
    splash::ExportOptions opt;
    opt.dryRun = true;
    opt.vram = vram;
    splash::ExportResult r;
    try {
        r.errors = splash::resolvePrefabs(scene, root);
        if (r.ok()) r = splash::exportSplashpack(scene, root, root / "scene.splashpack", opt);
    } catch (const std::exception& e) {
        r = {};
        r.errors.push_back(e.what());
    }
    return r;
}

std::array<size_t, kFixMeters> readings(const splash::ExportStats& s) {
    return {meterUsed(FixMeter::Vram, s), meterUsed(FixMeter::Ram, s), meterUsed(FixMeter::Tris, s)};
}

}  // namespace

void applyFix(splash::Scene& scene, const MeterFix& fix) {
    fixSettings(scene.settings, fix);
    if (fix.kind != MeterFix::Kind::TextureDepth) return;
    for (const ObjectPath& p : fix.objects)
        if (splash::Object* o = objectAt(scene, p)) fixObject(*o, fix);
}

bool applyFix(Document& doc, const MeterFix& fix) {
    switch (fix.kind) {
        case MeterFix::Kind::TextureDepth:
            return doc.editMany(fix.objects, [&](splash::Object& o) { fixObject(o, fix); }) > 0;
        case MeterFix::Kind::ResetOrderingTable:
        case MeterFix::Kind::ResetBumpAllocator: {
            splash::SceneSettings after = doc.scene().settings;
            fixSettings(after, fix);
            if (after.orderingTableSize == doc.scene().settings.orderingTableSize &&
                after.bumpAllocatorSize == doc.scene().settings.bumpAllocatorSize)
                return false;
            doc.editSettings([&](splash::SceneSettings& s) { fixSettings(s, fix); });
            return true;
        }
        case MeterFix::Kind::SimplifyMesh: return false;
    }
    return false;
}

MeterFixResult findMeterFixes(const splash::Scene& scene, const std::filesystem::path& root, const splash::VramSettings& vram,
                              const std::function<bool()>& stop, const std::function<void(int, int)>& progress,
                              int maxTextureTries) {
    MeterFixResult out;
    const splash::ExportResult base = dryRun(scene, root, vram);
    out.ok = base.ok();
    out.errors = base.errors;
    if (!out.ok) return out;
    out.base = base.stats;
    const std::array<size_t, kFixMeters> before = readings(base.stats);

    std::vector<Exported> objs;
    {
        ObjectPath p;
        walk(scene.objects, p, objs);
    }

    // --- Candidates ---
    std::vector<MeterFix> cands;
    // Textures: meshes at one depth that share a texture drop together (one
    // left behind would keep the old copy in VRAM).
    struct Group {
        splash::BitDepth depth;
        std::vector<size_t> members;  // into objs
        std::set<std::string> textures;
    };
    std::vector<Group> groups;
    for (size_t i = 0; i < objs.size(); i++) {
        const splash::Object& o = *objs[i].object;
        if (!o.mesh || o.mesh->bitDepth == splash::BitDepth::Bpp4) continue;
        std::set<std::string> tex;
        for (const splash::Material& m : o.mesh->materials)
            if (!m.texture.empty()) tex.insert(m.texture);
        if (tex.empty()) continue;
        std::vector<size_t> hit;
        for (size_t g = 0; g < groups.size(); g++) {
            if (groups[g].depth != o.mesh->bitDepth) continue;
            for (const std::string& t : tex)
                if (groups[g].textures.count(t)) {
                    hit.push_back(g);
                    break;
                }
        }
        if (hit.empty()) {
            groups.push_back({o.mesh->bitDepth, {i}, tex});
            continue;
        }
        Group& into = groups[hit[0]];
        into.members.push_back(i);
        into.textures.insert(tex.begin(), tex.end());
        for (size_t k = hit.size(); k-- > 1;) {
            Group& g = groups[hit[k]];
            into.members.insert(into.members.end(), g.members.begin(), g.members.end());
            into.textures.insert(g.textures.begin(), g.textures.end());
            groups.erase(groups.begin() + long(hit[k]));
        }
    }
    struct TexCand {
        MeterFix fix;
        double guess;  // texels x bits saved, to pick which to export
    };
    std::vector<TexCand> texCands;
    for (Group& g : groups) {
        std::sort(g.members.begin(), g.members.end());
        size_t texels = 0;
        int colors = 0;
        std::vector<std::pair<size_t, std::string>> bySize;
        for (const std::string& t : g.textures) {
            const auto [c, n] = colours15(root / t);
            texels += n;
            colors = std::max(colors, c);
            bySize.push_back({n, t});
        }
        std::stable_sort(bySize.begin(), bySize.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (splash::BitDepth to : {splash::BitDepth::Bpp8, splash::BitDepth::Bpp4}) {
            if (bits(to) >= bits(g.depth)) continue;
            MeterFix f;
            f.kind = MeterFix::Kind::TextureDepth;
            f.from = g.depth;
            f.to = to;
            f.colors = colors;
            for (const auto& [n, t] : bySize) f.textures.push_back(t);
            for (size_t m : g.members) f.objects.push_back(objs[m].path);
            f.title = baseName(f.textures[0]) +
                      (f.textures.size() > 1 ? " and " + std::to_string(f.textures.size() - 1) + " more" : std::string()) + ": " +
                      std::to_string(bits(f.from)) + " \xe2\x86\x92 " + std::to_string(bits(f.to)) + " bpp";
            const int fits = 1 << bits(to);
            f.detail = std::to_string(f.objects.size()) + (f.objects.size() == 1 ? " object" : " objects") + " \xc2\xb7 " +
                       (colors > 0 && colors <= fits ? std::to_string(colors) + " colours, fits " + std::to_string(fits) + " exactly"
                                                     : std::to_string(colors) + " colours reduced to " + std::to_string(fits));
            texCands.push_back({std::move(f), double(texels) * (bits(g.depth) - bits(to))});
        }
    }
    std::stable_sort(texCands.begin(), texCands.end(), [](const TexCand& a, const TexCand& b) { return a.guess > b.guess; });
    if (int(texCands.size()) > maxTextureTries) texCands.resize(size_t(std::max(maxTextureTries, 0)));
    for (TexCand& c : texCands) cands.push_back(std::move(c.fix));

    // Render buffers set above what the scene needs.
    const splash::ExportStats& s = base.stats;
    if (scene.settings.orderingTableSize > 0 && s.orderingTableSize > s.orderingTableNeed) {
        MeterFix f;
        f.kind = MeterFix::Kind::ResetOrderingTable;
        f.title = "Ordering table: " + std::to_string(s.orderingTableSize) + " \xe2\x86\x92 " +
                  std::to_string(s.orderingTableNeed) + " buckets";
        f.detail = "Back to Auto: what the scene can draw at its depth";
        cands.push_back(std::move(f));
    }
    if (scene.settings.bumpAllocatorSize > 0 && s.bumpAllocatorSize > s.bumpAllocatorNeed) {
        MeterFix f;
        f.kind = MeterFix::Kind::ResetBumpAllocator;
        f.title = "Bump allocator: " + std::to_string(s.bumpAllocatorSize / 1024) + " \xe2\x86\x92 " +
                  std::to_string((s.bumpAllocatorNeed + 1023) / 1024) + " KB";
        f.detail = "Back to Auto: the most the scene draws in one frame";
        cands.push_back(std::move(f));
    }

    // --- Measure each by exporting with it ---
    const int total = int(cands.size());
    int done = 0;
    if (progress) progress(0, total);
    for (MeterFix& f : cands) {
        if (stop && stop()) return out;
        splash::Scene with = scene;
        applyFix(with, f);
        const splash::ExportResult r = dryRun(std::move(with), root, vram);
        if (progress) progress(++done, total);
        if (!r.ok()) continue;
        f.before = before;
        f.after = readings(r.stats);
        bool lowers = false;
        for (int m = 0; m < kFixMeters; m++) lowers |= f.after[size_t(m)] < f.before[size_t(m)];
        if (lowers) out.fixes.push_back(std::move(f));
    }

    // --- Triangles: the meshes that put the most in view, simplified to half ---
    std::map<std::string, std::vector<ObjectPath>> byMesh;
    for (const Exported& e : objs)
        if (e.object->mesh && !e.object->mesh->mesh.empty()) byMesh[e.object->mesh->mesh].push_back(e.path);
    struct MeshCand {
        std::string mesh;
        std::vector<ObjectPath> paths;
        int tris;
    };
    std::vector<MeshCand> meshes;
    for (auto& [m, paths] : byMesh) {
        try {
            const splash::Mesh mesh = splash::loadMesh(root / m);
            int t = 0;
            for (const auto& sm : mesh.submeshes) t += int(sm.size() / 3);
            if (t > 0) meshes.push_back({m, paths, t});
        } catch (const std::exception&) {
        }
    }
    std::stable_sort(meshes.begin(), meshes.end(), [](const MeshCand& a, const MeshCand& b) {
        return size_t(a.tris) * a.paths.size() > size_t(b.tris) * b.paths.size();
    });
    if (meshes.size() > 5) meshes.resize(5);
    // A streamed world's view count is not the sum of its meshes.
    const bool streamed = scene.settings.streamWorld;
    for (MeshCand& mc : meshes) {
        if (stop && stop()) return out;
        MeterFix f;
        f.kind = MeterFix::Kind::SimplifyMesh;
        f.mesh = mc.mesh;
        f.objects = mc.paths;
        f.triangles = mc.tris;
        f.simplified = mc.tris;
        if (mc.tris >= 8) {
            try {
                splash::Mesh mesh = splash::loadMesh(root / mc.mesh);
                f.simplified = splash::simplifyToTriangles(mesh, mc.tris / 2).after;
            } catch (const std::exception&) {
            }
        }
        f.before = before;
        f.after = before;
        const size_t cut = size_t(mc.tris - f.simplified) * mc.paths.size();
        if (!streamed) f.after[int(FixMeter::Tris)] = before[int(FixMeter::Tris)] - std::min(cut, before[int(FixMeter::Tris)]);
        f.title = baseName(mc.mesh) + ": " + std::to_string(mc.tris) + " triangles" +
                  (mc.paths.size() > 1 ? " x " + std::to_string(mc.paths.size()) : std::string());
        f.detail = f.simplified < mc.tris ? "Simplified to half it keeps " + std::to_string(f.simplified) +
                                                " (Triangles in its import settings)"
                                          : std::string("Too few to simplify");
        out.fixes.push_back(std::move(f));
    }
    return out;
}

std::vector<const MeterFix*> fixesFor(const MeterFixResult& r, FixMeter m) {
    std::vector<const MeterFix*> v;
    for (const MeterFix& f : r.fixes) {
        if (m == FixMeter::Tris ? f.kind == MeterFix::Kind::SimplifyMesh : f.saving(m) > 0 && f.kind != MeterFix::Kind::SimplifyMesh)
            v.push_back(&f);
    }
    std::stable_sort(v.begin(), v.end(), [m](const MeterFix* a, const MeterFix* b) { return a->saving(m) > b->saving(m); });
    return v;
}

MeterFixFinder::~MeterFixFinder() { join(); }

void MeterFixFinder::join() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_stop = false;
}

void MeterFixFinder::request(const Document& doc) {
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_state.started && m_state.revision == doc.revision()) return;
    }
    join();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_state = {};
        m_state.revision = doc.revision();
        m_state.started = true;
    }
    m_thread = std::thread([this, scene = doc.scene(), root = doc.projectRoot()]() {
        MeterFixResult r = findMeterFixes(
            scene, root, loadVramSettings(root), [this] { return m_stop.load(); },
            [this](int d, int t) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_state.measured = d;
                m_state.total = t;
            });
        if (m_stop) return;
        std::lock_guard<std::mutex> lk(m_mu);
        m_state.result = std::move(r);
        m_state.done = true;
    });
}

MeterFixFinder::State MeterFixFinder::state() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_state;
}

void MeterFixFinder::wait() {
    if (m_thread.joinable()) m_thread.join();
}

}  // namespace editor
