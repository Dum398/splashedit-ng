#include "editor/meter_fixes.hh"

#include <algorithm>
#include <exception>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_set>

#include "budget.hh"
#include "editor/file_swaps.hh"
#include "editor/project.hh"
#include "editor/ui_canvas.hh"
#include "import.hh"
#include "simplify.hh"
#include "texture.hh"

namespace fs = std::filesystem;

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
// A plain image only: a sheet-backed one draws at its sheet's depth.
void fixImage(splash::UIElement& e, const MeterFix& fix) {
    if (fix.kind == MeterFix::Kind::TextureDepth && e.type == splash::UIElementType::Image && e.sheet.empty()) e.bitDepth = fix.to;
}
void fixSheets(std::vector<splash::SpriteSheet>& sheets, const MeterFix& fix) {
    if (fix.kind != MeterFix::Kind::TextureDepth) return;
    for (int i : fix.sheets)
        if (i >= 0 && size_t(i) < sheets.size()) sheets[size_t(i)].bitDepth = fix.to;
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

std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string projectPath(const fs::path& root, const fs::path& file) {
    std::u8string u = file.lexically_relative(root).generic_u8string();
    return std::string(u.begin(), u.end());
}

int triangleCount(const splash::Mesh& m) {
    int t = 0;
    for (const auto& sm : m.submeshes) t += int(sm.size() / 3);
    return t;
}

// How many times each mesh is drawn by an exported object, prefab
// instances' content too (resolved as the export resolves it).
void countDrawn(const std::vector<splash::Object>& level, std::map<std::string, size_t>& out) {
    for (const splash::Object& o : level) {
        if (!o.active) continue;
        if (o.mesh && !o.mesh->mesh.empty()) out[o.mesh->mesh]++;
        countDrawn(o.children, out);
    }
}
std::map<std::string, size_t> drawnMeshes(splash::Scene scene, const fs::path& root) {
    std::map<std::string, size_t> out;
    try {
        splash::resolvePrefabs(scene, root);
    } catch (const std::exception&) {
    }
    countDrawn(scene.objects, out);
    return out;
}

// What the importer gives a node mesh of `own` triangles out of `nodeTotal`
// for a model target of `target` (core/import.cpp, reduce).
int nodeShare(int target, int own, long long nodeTotal) {
    if (nodeTotal <= 0) return own;
    return std::clamp(int((long long)target * own / nodeTotal), std::min(own, 4), own);
}

// Fills a SimplifyMesh fix for `mesh` (`tris` triangles, the saving
// counted over `drawn`): when it came from an imported model, the model and
// the Triangles target that brings it to half, and what the importer's
// simplifier then leaves of every mesh of the model in view. Otherwise
// `model` stays empty and `detail` says why.
void planSimplify(MeterFix& f, const fs::path& root, const std::map<std::string, size_t>& drawn, bool skinned, bool terrain,
                  bool streamed) {
    const std::string& path = f.mesh;
    const int tris = f.triangles;
    f.simplified = tris;
    const size_t cutBase = f.before[int(FixMeter::Tris)];
    auto halfOfDisk = [&] {
        try {
            splash::Mesh mesh = splash::loadMesh(root / fromUtf8(path));
            f.simplified = splash::simplifyToTriangles(mesh, tris / 2).after;
        } catch (const std::exception&) {
        }
        auto it = drawn.find(path);
        const size_t cut = size_t(tris - f.simplified) * (it == drawn.end() ? 0 : it->second);
        if (!streamed) f.after[int(FixMeter::Tris)] = cutBase - std::min(cut, cutBase);
    };
    auto refuse = [&](const std::string& why) {
        halfOfDisk();
        f.detail = (f.simplified < tris ? "Half keeps " + std::to_string(f.simplified) + " \xc2\xb7 " : std::string()) + why;
    };
    if (tris < 8) {
        f.detail = "Too few to simplify";
        return;
    }
    if (terrain) return refuse("a terrain: lower its resolution instead");
    const fs::path model = modelOfMesh(root, path);
    if (model.empty()) return refuse("not from an imported model: simplify it where it was made");
    if (skinned) return refuse("skinned: set Triangles in its import settings");
    splash::ImportSettings settings;
    try {
        settings = splash::loadImportSettings(model);
    } catch (const std::exception& e) {
        return refuse(std::string("its import settings do not read: ") + e.what());
    }
    // The model's meshes as its last import wrote them: the baked one and a
    // node mesh per node with geometry, all in its own folder.
    const std::string name = splash::importName(model), baked = "models/" + name + ".mesh";
    struct Part {
        std::string path;
        splash::Mesh mesh;
        int tris = 0;
    };
    std::vector<Part> parts;  // baked first
    try {
        Part b{baked, splash::loadMesh(root / fromUtf8(baked)), 0};
        b.tris = triangleCount(b.mesh);
        parts.push_back(std::move(b));
        std::vector<fs::path> nodes;
        std::error_code ec;
        for (fs::directory_iterator it(root / "models" / fromUtf8(name), ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".mesh") nodes.push_back(it->path());
        std::sort(nodes.begin(), nodes.end());
        for (const fs::path& n : nodes) {
            Part p{projectPath(root, n), splash::loadMesh(n), 0};
            p.tris = triangleCount(p.mesh);
            parts.push_back(std::move(p));
        }
    } catch (const std::exception& e) {
        return refuse(std::string("its model's meshes do not read: ") + e.what());
    }
    const auto self = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.path == path; });
    if (self == parts.end()) return refuse("not one of " + utf8(model.filename()) + "'s meshes");
    long long nodeTotal = 0;
    for (size_t i = 1; i < parts.size(); i++) nodeTotal += parts[i].tris;
    const int total = parts[0].tris, half = tris / 2;
    // The baked mesh is simplified toward the whole target, a node mesh
    // toward its share of it: the smallest target giving it half.
    const bool isBaked = self == parts.begin();
    const int target = isBaked ? half : int((half * nodeTotal + tris - 1) / std::max(1, tris));
    if (target <= 0 || target >= total || (!isBaked && nodeShare(target, tris, nodeTotal) != half))
        return refuse("no Triangles target of " + utf8(model.filename()) + " halves it");
    // Simplified already: the next import starts from the file's triangles,
    // which the project does not hold, so what it keeps is estimated from
    // the meshes as they are.
    f.approximate = settings.targetTriangles > 0 && (settings.sourceTriangles <= 0 || settings.targetTriangles < settings.sourceTriangles);
    size_t cut = 0;
    int others = 0;
    for (Part& p : parts) {
        auto it = drawn.find(p.path);
        const bool mine = &p == &*self;
        if (it == drawn.end() && !mine) continue;
        const int t = &p == &parts[0] ? target : nodeShare(target, p.tris, nodeTotal);
        const int left = t < p.tris ? splash::simplifyToTriangles(p.mesh, t).after : p.tris;
        if (mine) f.simplified = left;
        else if (left < p.tris) others++;
        if (it != drawn.end()) cut += size_t(p.tris - left) * it->second;
    }
    if (!streamed) f.after[int(FixMeter::Tris)] = cutBase - std::min(cut, cutBase);
    if (f.simplified >= tris) {
        f.detail = "Simplifying keeps all " + std::to_string(tris) + ": UV seams, material edges and open borders hold them";
        return;
    }
    f.model = projectPath(root, model);
    f.target = target;
    f.detail = (f.approximate ? "Keeps about " : "Keeps ") + std::to_string(f.simplified) + " at Triangles " +
               std::to_string(target) + " in " + utf8(model.filename()) +
               (others > 0 ? ", and " + std::to_string(others) + (others == 1 ? " other mesh" : " other meshes") + " in view"
                           : std::string());
}

// Every file importing `model` again can write, as the project has them:
// its sidecar, models/<name>.mesh and its textures (models/<name>[_<n>].png),
// and everything under models/<name>/ (node meshes and clips). Others
// named alike come along; they do not change.
std::map<fs::path, std::optional<std::string>> modelFiles(const fs::path& root, const fs::path& model) {
    std::map<fs::path, std::optional<std::string>> out;
    const std::string name = splash::importName(model);
    out[splash::importSettingsPath(model)];
    std::error_code ec;
    for (fs::directory_iterator it(root / "models", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string f = utf8(it->path().filename()), ext = utf8(it->path().extension());
        std::error_code sec;
        if (f.size() > name.size() && f.compare(0, name.size(), name) == 0 && (f[name.size()] == '.' || f[name.size()] == '_') &&
            (ext == ".mesh" || ext == ".png") && it->is_regular_file(sec))
            out[it->path()];
    }
    ec.clear();
    for (fs::recursive_directory_iterator it(root / "models" / fromUtf8(name), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_regular_file(sec)) out[it->path()];
    }
    for (auto& [p, bytes] : out) {
        std::string b;
        if (readBytes(p, &b)) bytes = std::move(b);
    }
    return out;
}

// The import warnings of a model, kept with the files the import wrote.
class ImportWarningsCommand : public Command {
  public:
    fs::path model;
    std::vector<std::string> before, after;
    void apply(Document& doc) override { doc.setImportWarnings(model, after); }
    void revert(Document& doc) override { doc.setImportWarnings(model, before); }
};

bool applySimplify(Document& doc, const MeterFix& fix, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    if (fix.model.empty() || fix.target <= 0) return fail(fix.detail);
    const fs::path root = doc.projectRoot(), model = root / fromUtf8(fix.model);
    splash::ImportSettings settings;
    try {
        settings = splash::loadImportSettings(model);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (settings.targetTriangles == fix.target) return false;
    const auto before = modelFiles(root, model);
    // Everything as it was, for an import that did not work out.
    auto putBack = [&] {
        std::vector<std::string> recheck;
        for (auto& [p, bytes] : modelFiles(root, model)) {
            auto it = before.find(p);
            std::error_code ec;
            if (it == before.end() || !it->second) fs::remove(p, ec);
            recheck.push_back(projectPath(root, p));
        }
        for (const auto& [p, bytes] : before)
            if (bytes) writeBytes(p, *bytes);
        std::erase_if(recheck, [](const std::string& r) { return !r.ends_with(".mesh") && !r.ends_with(".png") && !r.ends_with(".anim"); });
        doc.refreshAssets(recheck);
    };
    settings.targetTriangles = fix.target;
    try {
        splash::saveImportSettings(model, settings);
    } catch (const std::exception& e) {
        putBack();
        return fail(std::string("the import settings were not saved: ") + e.what());
    }
    auto warnings = std::make_unique<ImportWarningsCommand>();
    warnings->model = model;
    warnings->before = doc.importWarnings(model);
    // The scene step the import makes (materials and node transforms, when
    // they changed), the files it wrote and its warnings: one undo step.
    // Undo goes last to first: warnings, files, then the scene.
    doc.beginBatch();
    const Reimported r = reimportModel(doc, model);
    if (!r.error.empty()) {
        doc.endBatch();
        putBack();
        doc.setImportWarnings(model, warnings->before);
        return fail(r.error);
    }
    auto swaps = std::make_unique<FileSwapsCommand>();
    auto after = modelFiles(root, model);
    for (const auto& [p, bytes] : before) after.try_emplace(p);  // removed by the import
    for (auto& [p, now] : after) {
        auto it = before.find(p);
        std::optional<std::string> was = it == before.end() ? std::nullopt : it->second;
        if (was == now) continue;
        const std::string rel = projectPath(root, p);
        if (rel.ends_with(".mesh") || rel.ends_with(".png") || rel.ends_with(".anim")) swaps->recheck.push_back(rel);
        swaps->files.push_back({p, std::move(was), std::move(now)});
    }
    warnings->after = doc.importWarnings(model);
    doc.execute(std::move(swaps));
    doc.execute(std::move(warnings));
    doc.endBatch();
    return true;
}

}  // namespace

void applyFix(splash::Scene& scene, const MeterFix& fix) {
    fixSettings(scene.settings, fix);
    if (fix.kind != MeterFix::Kind::TextureDepth) return;
    for (const ObjectPath& p : fix.objects)
        if (splash::Object* o = objectAt(scene, p)) fixObject(*o, fix);
    for (const auto& [c, e] : fix.uiImages)
        if (c >= 0 && size_t(c) < scene.canvases.size() && e >= 0 && size_t(e) < scene.canvases[size_t(c)].elements.size())
            fixImage(scene.canvases[size_t(c)].elements[size_t(e)], fix);
    fixSheets(scene.spriteSheets, fix);
}

bool applyFix(Document& doc, const MeterFix& fix, std::string* error) {
    switch (fix.kind) {
        case MeterFix::Kind::TextureDepth: {
            // Objects, UI images and sheets through their own edits, undone
            // as one step: one carrier left behind would keep the old copy.
            std::map<int, std::vector<int>> byCanvas;
            for (const auto& [c, e] : fix.uiImages) byCanvas[c].push_back(e);
            std::vector<ObjectPath> objects;  // editMany makes a step for each it is given, changed or not
            for (const ObjectPath& p : fix.objects)
                if (const splash::Object* o = doc.object(p); o && o->mesh && o->mesh->bitDepth != fix.to) objects.push_back(p);
            bool changed = false;
            doc.beginBatch();
            if (!objects.empty()) changed |= doc.editMany(objects, [&](splash::Object& o) { fixObject(o, fix); }) > 0;
            for (const auto& [c, idx] : byCanvas)
                changed |= uic::editElements(doc, c, idx, [&](splash::UIElement& e, size_t) { fixImage(e, fix); });
            if (!fix.sheets.empty()) changed |= doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& ss) { fixSheets(ss, fix); });
            doc.endBatch();
            return changed;
        }
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
        case MeterFix::Kind::SimplifyMesh: return applySimplify(doc, fix, error);
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
    // Textures: every carrier of a texture at one depth drops together (one
    // left behind would keep the old copy in VRAM). packVram
    // (core/vrampacker.cpp) packs the meshes', the plain UI images' and the
    // sprite sheets' textures as one list and places one copy per source
    // file, depth and cutout, so carriers of all three kinds share copies. A
    // group joins every carrier on a file at a depth; cutout is not looked
    // at, so a group may hold carriers whose copies differ but never leaves
    // one that shares a copy behind.
    struct Group {
        splash::BitDepth depth;
        std::vector<size_t> objects;              // into objs
        std::vector<std::pair<int, int>> images;  // canvas, element
        std::vector<int> sheets;
        std::set<std::string> textures;
    };
    std::vector<Group> groups;
    auto absorb = [](Group& into, const Group& g) {
        into.objects.insert(into.objects.end(), g.objects.begin(), g.objects.end());
        into.images.insert(into.images.end(), g.images.begin(), g.images.end());
        into.sheets.insert(into.sheets.end(), g.sheets.begin(), g.sheets.end());
        into.textures.insert(g.textures.begin(), g.textures.end());
    };
    auto join = [&](Group one) {
        if (one.depth == splash::BitDepth::Bpp4 || one.textures.empty()) return;
        std::vector<size_t> hit;
        for (size_t g = 0; g < groups.size(); g++) {
            if (groups[g].depth != one.depth) continue;
            for (const std::string& t : one.textures)
                if (groups[g].textures.count(t)) {
                    hit.push_back(g);
                    break;
                }
        }
        if (hit.empty()) {
            groups.push_back(std::move(one));
            return;
        }
        absorb(groups[hit[0]], one);
        for (size_t k = hit.size(); k-- > 1;) {
            absorb(groups[hit[0]], groups[hit[k]]);
            groups.erase(groups.begin() + long(hit[k]));
        }
    };
    for (size_t i = 0; i < objs.size(); i++) {
        const splash::Object& o = *objs[i].object;
        if (!o.mesh) continue;
        Group one{o.mesh->bitDepth, {i}, {}, {}, {}};
        for (const splash::Material& m : o.mesh->materials)
            if (!m.texture.empty()) one.textures.insert(m.texture);
        join(std::move(one));
    }
    // Every canvas the export writes, hidden ones too, but the loading
    // screen's (the first of that name), which goes in the loader pack.
    // Elements of a type this build does not know are not exported.
    int loading = -1;
    if (const std::string& lc = scene.settings.loadingCanvas; !lc.empty())
        for (size_t c = 0; c < scene.canvases.size() && loading < 0; c++)
            if (scene.canvases[c].name == lc) loading = int(c);
    for (size_t c = 0; c < scene.canvases.size(); c++) {
        if (int(c) == loading) continue;
        const std::vector<splash::UIElement>& els = scene.canvases[c].elements;
        for (size_t e = 0; e < els.size(); e++)
            if (els[e].type == splash::UIElementType::Image && els[e].unknown.empty() && els[e].sheet.empty() &&
                !els[e].texture.empty())
                join({els[e].bitDepth, {}, {{int(c), int(e)}}, {}, {els[e].texture}});
    }
    for (size_t i = 0; i < scene.spriteSheets.size(); i++) {
        const splash::SpriteSheet& sh = scene.spriteSheets[i];
        if (!sh.texture.empty()) join({sh.bitDepth, {}, {}, {int(i)}, {sh.texture}});
    }
    struct TexCand {
        MeterFix fix;
        double guess;  // texels x bits saved, to pick which to export
    };
    std::vector<TexCand> texCands;
    for (Group& g : groups) {
        std::sort(g.objects.begin(), g.objects.end());
        std::sort(g.images.begin(), g.images.end());
        std::sort(g.sheets.begin(), g.sheets.end());
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
            for (size_t m : g.objects) f.objects.push_back(objs[m].path);
            f.uiImages = g.images;
            f.sheets = g.sheets;
            f.title = baseName(f.textures[0]) +
                      (f.textures.size() > 1 ? " and " + std::to_string(f.textures.size() - 1) + " more" : std::string()) + ": " +
                      std::to_string(bits(f.from)) + " \xe2\x86\x92 " + std::to_string(bits(f.to)) + " bpp";
            const int fits = 1 << bits(to);
            std::string who;
            auto count = [&](size_t n, const char* one, const char* many) {
                if (n > 0) who += std::to_string(n) + " " + (n == 1 ? one : many) + " \xc2\xb7 ";
            };
            count(f.objects.size(), "object", "objects");
            count(f.uiImages.size(), "UI image", "UI images");
            count(f.sheets.size(), "sprite sheet", "sprite sheets");
            f.detail = who + (colors > 0 && colors <= fits ? std::to_string(colors) + " colours, fits " + std::to_string(fits) + " exactly"
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
    std::set<std::string> skinned, terrain;
    for (const Exported& e : objs)
        if (e.object->mesh && !e.object->mesh->mesh.empty()) {
            const std::string& m = e.object->mesh->mesh;
            byMesh[m].push_back(e.path);
            if (e.object->skin) skinned.insert(m);
            if (e.object->terrain) terrain.insert(m);
        }
    // A reimport rewrites the files prefab instances draw as well.
    const std::map<std::string, size_t> drawn = drawnMeshes(scene, root);
    auto drawnCount = [&](const std::string& m, size_t paths) {
        auto it = drawn.find(m);
        return std::max(paths, it == drawn.end() ? size_t(0) : it->second);
    };
    struct MeshCand {
        std::string mesh;
        std::vector<ObjectPath> paths;
        int tris;
        size_t drawn;
    };
    std::vector<MeshCand> meshes;
    for (auto& [m, paths] : byMesh) {
        try {
            const splash::Mesh mesh = splash::loadMesh(root / m);
            if (mesh.skin) skinned.insert(m);
            const int t = triangleCount(mesh);
            if (t > 0) meshes.push_back({m, paths, t, drawnCount(m, paths.size())});
        } catch (const std::exception&) {
        }
    }
    std::stable_sort(meshes.begin(), meshes.end(),
                     [](const MeshCand& a, const MeshCand& b) { return size_t(a.tris) * a.drawn > size_t(b.tris) * b.drawn; });
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
        f.before = before;
        f.after = before;
        planSimplify(f, root, drawn, skinned.count(mc.mesh) > 0, terrain.count(mc.mesh) > 0, streamed);
        // Another mesh of a model already listed whose half takes the same
        // target: the same edit, listed once (its detail counts this one).
        if (!f.model.empty() && std::any_of(out.fixes.begin(), out.fixes.end(), [&](const MeterFix& g) {
                return g.kind == MeterFix::Kind::SimplifyMesh && g.model == f.model && g.target == f.target;
            }))
            continue;
        f.title = baseName(mc.mesh) + ": " + std::to_string(mc.tris) + " triangles" +
                  (mc.drawn > 1 ? " x " + std::to_string(mc.drawn) : std::string());
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
