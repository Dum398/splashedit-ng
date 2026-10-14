#include "editor/project.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#include "editor/catalog.hh"
#include "scene.hh"

namespace fs = std::filesystem;

namespace editor {

static std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
static fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path firstScene(const fs::path& project) {
    fs::path best;
    std::error_code ec;
    for (fs::directory_iterator it(project, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".scene" && (best.empty() || it->path().filename() < best))
            best = it->path().filename();
    return best;
}

NewProject createProject(const fs::path& dir) {
    NewProject r;
    if (dir.empty()) {
        r.error = "no folder given";
        return r;
    }
    if (fs::path existing = firstScene(dir); !existing.empty()) {
        r.error = utf8(dir.filename()) + " already holds " + utf8(existing) + "; open it instead";
        return r;
    }
    std::error_code ec;
    for (const char* sub : {"meshes", "textures", "scripts"}) {
        fs::create_directories(dir / sub, ec);
        if (ec) {
            r.error = "could not create " + utf8(dir / sub) + ": " + ec.message();
            return r;
        }
    }
    std::string stem = utf8(dir.filename());
    if (stem.empty() || stem == "." || stem == "..") stem = "main";
    fs::path scene = fromUtf8(stem + ".scene");
    try {
        splash::saveScene(splash::Scene{}, dir / scene);
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    r.scene = scene;
    return r;
}

std::optional<fs::path> sceneInProject(const fs::path& project, const fs::path& file) {
    if (project.empty() || file.empty()) return std::nullopt;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(project, ec);
    if (ec) return std::nullopt;
    fs::path target = fs::weakly_canonical(file, ec);
    if (ec) return std::nullopt;
    fs::path rel = target.lexically_relative(root);
    if (rel.empty() || rel == "." || *rel.begin() == "..") return std::nullopt;
    if (rel.extension() != ".scene") rel += ".scene";
    return rel;
}

static bool sameBytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::equal(std::istreambuf_iterator<char>(fa), {}, std::istreambuf_iterator<char>(fb), {});
}
static std::string genericUtf8(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// adoptAsset, or with `dryRun` where it would put the file, `copied` telling
// whether it would copy it, without changing anything.
static AdoptedAsset adopt(const fs::path& project, const fs::path& file, const std::string& folder, bool dryRun) {
    AdoptedAsset r;
    std::error_code ec;
    if (project.empty() || file.empty()) {
        r.error = "no project is open";
        return r;
    }
    if (!fs::is_regular_file(file, ec)) {
        r.error = utf8(file) + " is not a file";
        return r;
    }
    fs::path root = fs::weakly_canonical(project, ec);
    fs::path src = fs::weakly_canonical(file, ec);
    if (ec) {
        r.error = ec.message();
        return r;
    }
    fs::path rel = src.lexically_relative(root);
    if (!rel.empty() && rel != "." && *rel.begin() != "..") {
        r.path = genericUtf8(rel);
        return r;
    }
    fs::path dir = root / fromUtf8(folder);
    if (!dryRun) fs::create_directories(dir, ec);
    if (ec) {
        r.error = "could not create " + utf8(dir) + ": " + ec.message();
        return r;
    }
    const std::string stem = utf8(src.stem()), ext = utf8(src.extension());
    fs::path dest = dir / src.filename();
    for (int n = 2; fs::exists(dest, ec) && !sameBytes(src, dest); ++n) dest = dir / fromUtf8(stem + "_" + std::to_string(n) + ext);
    if (!fs::exists(dest, ec)) {
        if (!dryRun) fs::copy_file(src, dest, ec);
        if (ec) {
            r.error = "could not copy " + utf8(src.filename()) + ": " + ec.message();
            return r;
        }
        r.copied = true;
    }
    r.path = genericUtf8(dest.lexically_relative(root));
    return r;
}

AdoptedAsset adoptAsset(const fs::path& project, const fs::path& file, const std::string& folder) {
    return adopt(project, file, folder, false);
}

std::string scriptFileStem(const std::string& name) {
    std::string out;
    bool gap = false;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
            if (gap && !out.empty()) out += '_';
            gap = false;
            out += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else {
            gap = true;
        }
    }
    return out.empty() ? "script" : out;
}

std::string scriptTemplate(const std::string& name, ScriptKind kind) {
    std::string title;
    for (char c : name) title += c == '\n' || c == '\r' ? ' ' : c;
    if (kind == ScriptKind::Trigger)
        return "-- " + title +
               ": the trigger's script.\n"
               "\n"
               "-- The player walked into the box. index is the trigger's number.\n"
               "function onTriggerEnter(index)\n"
               "end\n"
               "\n"
               "-- The player left it.\n"
               "function onTriggerExit(index)\n"
               "end\n";
    if (kind == ScriptKind::Scene)
        return "-- " + title +
               ": the scene's script.\n"
               "\n"
               "-- Before the scene's objects are created.\n"
               "function onSceneCreationStart()\n"
               "end\n"
               "\n"
               "-- After every object is created and its onCreate has run.\n"
               "function onSceneCreationEnd()\n"
               "end\n";
    return "-- " + title +
           "\n"
           "\n"
           "-- Once, after the scene has loaded. self is this object.\n"
           "function onCreate(self)\n"
           "end\n"
           "\n"
           "-- Every frame; dt is 4096 for one 30 fps frame. Leave it out unless needed.\n"
           "-- function onUpdate(self, dt)\n"
           "-- end\n";
}

NewScript createScript(const fs::path& project, const std::string& name, ScriptKind kind) {
    NewScript r;
    std::error_code ec;
    if (project.empty()) {
        r.error = "no project is open";
        return r;
    }
    const fs::path dir = project / "scripts";
    fs::create_directories(dir, ec);
    if (ec) {
        r.error = "could not create " + utf8(dir) + ": " + ec.message();
        return r;
    }
    const std::string stem = scriptFileStem(kind == ScriptKind::Trigger ? name + " trigger" : name);
    fs::path dest = dir / fromUtf8(stem + ".lua");
    for (int n = 2; fs::exists(dest, ec); ++n) dest = dir / fromUtf8(stem + "_" + std::to_string(n) + ".lua");
    std::ofstream out(dest, std::ios::binary);
    out << scriptTemplate(name, kind);
    if (!out.flush()) {
        r.error = "could not write " + utf8(dest);
        return r;
    }
    r.path = "scripts/" + utf8(dest.filename());
    return r;
}

// A mesh component for an object made from a model.
static splash::MeshComponent modelMesh(const std::string& mesh, const std::vector<splash::Material>& materials,
                                       const splash::ImportSettings& settings) {
    splash::MeshComponent m;
    m.mesh = mesh;
    m.materials = materials;
    m.bitDepth = settings.bitDepth;
    m.dynamicLighting = settings.lighting;
    return m;
}

// A skin component playing the model's clips, the first 16; none when the
// model has no clips, since psxsplash never draws a skinned mesh without one.
static std::optional<splash::SkinComponent> modelSkin(const splash::ImportedModel& model) {
    if (model.clips.empty()) return std::nullopt;
    splash::SkinComponent s;
    s.clips.assign(model.clips.begin(), model.clips.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(model.clips.size(), 16)));
    return s;
}

// An object per node, each holding its children: the model's top-level nodes
// in file order.
static std::vector<splash::Object> nodeObjects(const splash::ImportedModel& model, const splash::ImportSettings& settings) {
    std::vector<splash::Object> objs(model.nodes.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        const splash::ImportedNode& n = model.nodes[i];
        objs[i].name = n.name;
        objs[i].transform = n.transform;
        if (!n.mesh.empty()) objs[i].mesh = modelMesh(n.mesh, n.materials, settings);
        if (!n.mesh.empty() && n.skinned) objs[i].skin = modelSkin(model);
    }
    // Parents come before their children, so walking backwards finishes every
    // object's subtree before it moves into its parent (at the front, which
    // keeps the file's order).
    for (size_t i = objs.size(); i-- > 0;) {
        const int p = model.nodes[i].parent;
        if (p >= 0 && static_cast<size_t>(p) < i)
            objs[static_cast<size_t>(p)].children.insert(objs[static_cast<size_t>(p)].children.begin(), std::move(objs[i]));
    }
    std::vector<splash::Object> top;
    for (size_t i = 0; i < objs.size(); ++i) {
        const int p = model.nodes[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= i) top.push_back(std::move(objs[i]));
    }
    return top;
}

splash::Object modelObject(const splash::ImportedModel& model, const std::string& name, const splash::ImportSettings& settings) {
    splash::Object root;
    root.name = name;
    if (model.nodes.size() <= 1 || settings.mergeMeshes) {
        root.mesh = modelMesh(model.mesh, model.materials, settings);
        if (model.skinned) root.skin = modelSkin(model);
        return root;
    }
    root.children = nodeObjects(model, settings);
    return root;
}

// Every file an import wrote, project-relative.
static std::vector<std::string> writtenFiles(const splash::ImportedModel& m) {
    std::vector<std::string> written = m.textures;
    written.push_back(m.mesh);
    for (const splash::ImportedNode& n : m.nodes)
        if (!n.mesh.empty()) written.push_back(n.mesh);
    written.insert(written.end(), m.clips.begin(), m.clips.end());
    return written;
}

StagedModel stagePlace(const Document& doc, const fs::path& file, const fs::path& converted) {
    StagedModel s;
    s.project = doc.projectRoot();
    s.file = file;
    s.read = converted.empty() ? file : converted;
    if (s.project.empty()) {
        s.error = "no project is open";
        return s;
    }
    if (!splash::canImportModel(file)) {
        s.error = utf8(file.filename()) + ": only .glb, .gltf, .fbx, .obj and .blend models can be imported";
        return s;
    }
    // Named after where the copy goes, but imported from where it was found,
    // so a .gltf's buffers and textures beside it are still at hand. The copy
    // is only made once the import worked, so a file that is not a model (or
    // an import cancelled) never lands in the project.
    const AdoptedAsset plan = adopt(s.project, file, "models", true);
    if (!plan.error.empty()) {
        s.error = plan.error;
        return s;
    }
    s.copy = s.project / fromUtf8(plan.path);
    s.adopt = plan.copied;
    std::error_code ec;
    s.inside = !s.adopt && fs::equivalent(file, s.copy, ec);
    s.hadSidecar = fs::exists(splash::importSettingsPath(s.copy), ec);
    try {
        s.settings = splash::loadImportSettings(s.copy);
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    return s;
}

StagedModel stageReimport(const Document& doc, const fs::path& model, const fs::path& converted) {
    StagedModel s;
    s.reimport = true;
    s.project = doc.projectRoot();
    if (s.project.empty()) {
        s.error = "no project is open";
        return s;
    }
    s.file = s.copy = model.is_absolute() ? model : s.project / model;
    try {
        s.settings = splash::loadImportSettings(s.file);
        s.read = s.file;
        std::error_code ec;
        if (!s.settings.source.empty() && fs::is_regular_file(fromUtf8(s.settings.source), ec)) s.read = fromUtf8(s.settings.source);
        if (!converted.empty()) s.read = converted;
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    return s;
}

void buildStaged(StagedModel& s) {
    if (!s.error.empty()) return;
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        s.error = "no temporary folder: " + ec.message();
        return;
    }
    const auto now = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    s.staging.reset(tmp / ("splashedit-import-" + std::to_string(now) + "-" + std::to_string(counter++)));
    fs::create_directories(s.staging.path(), ec);
    if (ec) {
        s.error = "could not create " + utf8(s.staging.path()) + ": " + ec.message();
        return;
    }
    try {
        s.model = splash::importModel(s.read, s.staging.path(), s.settings, s.copy);
    } catch (const std::exception& e) {
        s.error = e.what();
    }
}

StagingFolder::StagingFolder(StagingFolder&& o) noexcept : m_path(std::move(o.m_path)) { o.m_path.clear(); }

StagingFolder& StagingFolder::operator=(StagingFolder&& o) noexcept {
    if (this != &o) {
        reset(std::move(o.m_path));
        o.m_path.clear();
    }
    return *this;
}

void StagingFolder::reset(fs::path p) {
    if (!m_path.empty()) {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    m_path = std::move(p);
}

void discardStaged(StagedModel& s) { s.staging.reset(); }

namespace {
// Removes the staging folder however applying ends.
struct Discard {
    StagedModel& s;
    ~Discard() { discardStaged(s); }
};
}  // namespace

// Moves a built import's outputs into the project, replacing an earlier
// import's: the node folder is cleared of its meshes and clips first, as
// importModel does when it writes into the project itself.
static std::string commitStaged(const StagedModel& s) {
    std::error_code ec;
    const fs::path nodeDir = s.project / "models" / fromUtf8(splash::importName(s.copy));
    for (fs::directory_iterator it(nodeDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".mesh" || it->path().extension() == ".anim") fs::remove(it->path(), ec);
    for (const std::string& rel : writtenFiles(s.model)) {
        ec.clear();
        const fs::path from = s.staging.path() / fromUtf8(rel), to = s.project / fromUtf8(rel);
        fs::create_directories(to.parent_path(), ec);
        ec.clear();
        fs::rename(from, to, ec);
        if (!ec) continue;
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) return "could not write " + rel + ": " + ec.message();
    }
    return {};
}

PlacedModel applyPlace(Document& doc, StagedModel& s, const std::optional<ObjectPath>& replace, const ObjectPath& at,
                       splash::Vec3 position) {
    PlacedModel r;
    Discard discard{s};
    if (!s.error.empty()) {
        r.error = s.error;
        return r;
    }
    if (doc.projectRoot() != s.project) {
        r.error = "another project was opened";
        return r;
    }
    if (s.adopt) {
        const AdoptedAsset adopted = adoptAsset(s.project, s.file, "models");
        if (!adopted.error.empty()) {
            r.error = adopted.error;
            return r;
        }
        if (s.project / fromUtf8(adopted.path) != s.copy) {
            std::error_code ec;
            if (adopted.copied) fs::remove(s.project / fromUtf8(adopted.path), ec);
            r.error = utf8(s.file.filename()) + ": the models folder changed while it was importing, import it again";
            return r;
        }
    }
    if (std::string e = commitStaged(s); !e.empty()) {
        r.error = e;
        return r;
    }
    r.model = std::move(s.model);
    const fs::path& copy = s.copy;
    const fs::path& file = s.file;
    splash::ImportSettings& settings = s.settings;
    std::error_code ec;
    const std::string source = s.inside ? std::string() : utf8(fs::absolute(file, ec));
    if (!s.hadSidecar || (!s.inside && settings.source != source)) {
        settings.source = source;
        try {
            splash::saveImportSettings(copy, settings);
        } catch (const std::exception& e) {
            r.model.warnings.push_back(std::string("the import settings were not saved: ") + e.what());
        }
    }
    doc.setImportWarnings(copy, r.model.warnings);
    doc.refreshAssets(writtenFiles(r.model));

    const splash::Object* target = replace ? doc.object(*replace) : nullptr;
    if (target && target->mesh) {
        doc.edit(*replace, [&](splash::Object& o) {
            o.mesh->mesh = r.model.mesh;
            o.mesh->materials = r.model.materials;
            if (r.model.skinned && !o.skin) o.skin = modelSkin(r.model);
        });
        doc.endMerge();
        r.path = *replace;
        return r;
    }
    const ObjectPath parentPath = at.empty() ? ObjectPath{} : ObjectPath(at.begin(), at.end() - 1);
    const splash::Object* parent = doc.object(parentPath);
    if (at.empty() || (!parentPath.empty() && !parent)) {
        r.error = "there is no object to put it in";
        return r;
    }
    splash::Object obj = modelObject(r.model, uniqueName(utf8(file.stem()), parent ? parent->children : doc.scene().objects), settings);
    obj.transform.position = position;
    doc.insertObject(at, std::move(obj));
    r.path = at;
    return r;
}

PlacedModel placeModel(Document& doc, const fs::path& file, const std::optional<ObjectPath>& replace, const ObjectPath& at,
                       splash::Vec3 position, const fs::path& converted) {
    StagedModel s = stagePlace(doc, file, converted);
    buildStaged(s);
    return applyPlace(doc, s, replace, at, position);
}

fs::path modelOfMesh(const fs::path& project, const std::string& mesh) {
    fs::path rel = fromUtf8(mesh);
    auto it = rel.begin();
    if (project.empty() || it == rel.end() || *it != "models" || rel.extension() != ".mesh") return {};
    ++it;
    if (it == rel.end()) return {};
    const fs::path first = *it;
    const size_t depth = static_cast<size_t>(std::distance(rel.begin(), rel.end()));
    if (depth != 2 && depth != 3) return {};
    const std::string name = utf8(depth == 2 ? first.stem() : first);
    fs::path best;
    std::error_code ec;
    for (fs::directory_iterator d(project / "models", ec), end; !ec && d != end; d.increment(ec)) {
        const fs::path& p = d->path();
        if (!splash::canImportModel(p) || splash::importName(p) != name || (!best.empty() && p > best)) continue;
        std::error_code sec;
        if (fs::is_regular_file(p, sec) && fs::exists(splash::importSettingsPath(p), sec)) best = p;
    }
    return best;
}

static bool sameMaterials(const std::vector<splash::Material>& a, const std::vector<splash::Material>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].texture != b[i].texture || a[i].color != b[i].color) return false;
    return true;
}
static bool sameTransform(const splash::Transform& a, const splash::Transform& b) {
    return a.position == b.position && a.scale == b.scale && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
           a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}

namespace {

// Brings the scene's objects in line with a model imported again.
struct Resync {
    const splash::ImportedModel& model;
    const splash::ImportSettings& settings;
    std::string nodePrefix;  // models/<name>/
    bool toMerged = false, toTree = false;
    std::map<std::string, size_t> nodeOfMesh;
    std::vector<std::vector<size_t>> childNodes;  // per node; the last entry holds the top-level nodes

    Resync(const splash::ImportedModel& m, const splash::ImportSettings& s, std::string prefix)
        : model(m), settings(s), nodePrefix(std::move(prefix)) {
        childNodes.assign(model.nodes.size() + 1, {});
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            const int p = model.nodes[i].parent;
            childNodes[p < 0 ? model.nodes.size() : static_cast<size_t>(p)].push_back(i);
            if (!model.nodes[i].mesh.empty()) nodeOfMesh[model.nodes[i].mesh] = i;
        }
    }

    // The children of `o` standing for `nodes`, matched by name in order;
    // nothing when one is missing.
    std::optional<std::vector<size_t>> match(const splash::Object& o, const std::vector<size_t>& nodes) const {
        std::vector<size_t> found;
        size_t from = 0;
        for (size_t n : nodes) {
            size_t k = from;
            while (k < o.children.size() && o.children[k].name != model.nodes[n].name) ++k;
            if (k == o.children.size()) return std::nullopt;
            found.push_back(k);
            from = k + 1;
        }
        return found;
    }
    bool drawsNodeMesh(const splash::Object& o) const {
        if (o.mesh && o.mesh->mesh.rfind(nodePrefix, 0) == 0) return true;
        for (const splash::Object& c : o.children)
            if (drawsNodeMesh(c)) return true;
        return false;
    }
    // The top object of an instance of the node tree: its children are the
    // model's top-level nodes and something below draws a node mesh.
    std::optional<std::vector<size_t>> instanceTop(const splash::Object& o) const {
        if (model.nodes.size() <= 1 || (o.mesh && o.mesh->mesh == model.mesh)) return std::nullopt;
        auto m = match(o, childNodes.back());
        if (!m) return std::nullopt;
        for (size_t k : *m)
            if (drawsNodeMesh(o.children[k])) return m;
        return std::nullopt;
    }
    bool setMaterials(splash::MeshComponent& m, const std::vector<splash::Material>& mats) {
        if (sameMaterials(m.materials, mats)) return false;
        m.materials = mats;
        return true;
    }
    // The skin of an object drawing a skinned mesh of the model: the model's
    // clips (those under models/<name>/) swapped for the new ones, others
    // kept after them. An object without a skin is left without one, so a
    // skin taken off stays off and the mesh draws in its rest pose.
    bool syncSkin(splash::Object& o, bool skinned) {
        if (!skinned || !o.skin) return false;
        std::optional<splash::SkinComponent> fresh = modelSkin(model);
        if (!fresh) return false;
        std::vector<std::string> clips = fresh->clips;
        for (const std::string& c : o.skin->clips)
            if (c.rfind(nodePrefix, 0) != 0 && clips.size() < 16) clips.push_back(c);
        if (clips == o.skin->clips) return false;
        o.skin->clips = std::move(clips);
        return true;
    }
    // An object made from node `n`: transform, mesh and materials, then its
    // children by name.
    bool syncNode(splash::Object& o, size_t n) {
        const splash::ImportedNode& node = model.nodes[n];
        bool changed = false;
        if (!sameTransform(o.transform, node.transform)) o.transform = node.transform, changed = true;
        if (!node.mesh.empty()) {
            if (!o.mesh) o.mesh = modelMesh(node.mesh, node.materials, settings), changed = true;
            if (o.mesh->mesh != node.mesh) o.mesh->mesh = node.mesh, changed = true;
            changed |= setMaterials(*o.mesh, node.materials);
            changed |= syncSkin(o, node.skinned);
        }
        std::vector<bool> matched(o.children.size());
        if (auto m = match(o, childNodes[n]))
            for (size_t i = 0; i < m->size(); ++i) {
                changed |= syncNode(o.children[(*m)[i]], childNodes[n][i]);
                matched[(*m)[i]] = true;
            }
        for (size_t k = 0; k < o.children.size(); ++k)
            if (!matched[k]) changed |= walk(o.children[k]);
        return changed;
    }
    bool walk(splash::Object& o) {
        bool changed = false;
        std::vector<bool> handled(o.children.size());
        if (auto top = instanceTop(o)) {
            if (toMerged) {
                for (auto k = top->rbegin(); k != top->rend(); ++k) o.children.erase(o.children.begin() + static_cast<std::ptrdiff_t>(*k));
                o.mesh = modelMesh(model.mesh, model.materials, settings);
                if (model.skinned && !o.skin) o.skin = modelSkin(model);
                handled.assign(o.children.size(), false);
                changed = true;
            } else {
                for (size_t i = 0; i < top->size(); ++i) {
                    changed |= syncNode(o.children[(*top)[i]], childNodes.back()[i]);
                    handled[(*top)[i]] = true;
                }
            }
        } else if (o.mesh && o.mesh->mesh == model.mesh) {
            if (toTree && model.nodes.size() > 1) {
                o.mesh.reset();
                o.skin.reset();  // a skin cannot exist without its mesh
                std::vector<splash::Object> made = nodeObjects(model, settings);
                o.children.insert(o.children.begin(), std::make_move_iterator(made.begin()), std::make_move_iterator(made.end()));
                handled.assign(o.children.size(), false);
                for (size_t i = 0; i < made.size(); ++i) handled[i] = true;
                changed = true;
            } else {
                changed |= setMaterials(*o.mesh, model.materials);
                changed |= syncSkin(o, model.skinned);
            }
        } else if (o.mesh) {
            auto it = nodeOfMesh.find(o.mesh->mesh);
            if (it != nodeOfMesh.end()) {
                changed |= setMaterials(*o.mesh, model.nodes[it->second].materials);
                changed |= syncSkin(o, model.nodes[it->second].skinned);
            }
        }
        for (size_t k = 0; k < o.children.size(); ++k)
            if (!handled[k]) changed |= walk(o.children[k]);
        return changed;
    }
};

}  // namespace

fs::path modelSource(const Document& doc, const fs::path& model) {
    const fs::path file = model.is_absolute() ? model : doc.projectRoot() / model;
    std::error_code ec;
    try {
        const splash::ImportSettings settings = splash::loadImportSettings(file);
        if (!settings.source.empty() && fs::is_regular_file(fromUtf8(settings.source), ec)) return fromUtf8(settings.source);
    } catch (const std::exception&) {
    }
    return file;
}

Reimported applyReimport(Document& doc, StagedModel& s, std::optional<bool> wasMerged) {
    Reimported r;
    Discard discard{s};
    if (!s.error.empty()) {
        r.error = s.error;
        return r;
    }
    if (doc.projectRoot() != s.project) {
        r.error = "another project was opened";
        return r;
    }
    if (std::string e = commitStaged(s); !e.empty()) {
        r.error = e;
        return r;
    }
    r.model = std::move(s.model);
    const fs::path& file = s.file;
    const splash::ImportSettings& settings = s.settings;
    doc.refreshAssets(writtenFiles(r.model));

    Resync sync(r.model, settings, "models/" + splash::importName(file) + "/");
    sync.toMerged = wasMerged && !*wasMerged && settings.mergeMeshes;
    sync.toTree = wasMerged && *wasMerged && !settings.mergeMeshes;
    std::vector<std::pair<ObjectPath, splash::Object>> trees;
    for (size_t i = 0; i < doc.scene().objects.size(); ++i) {
        splash::Object copy = doc.scene().objects[i];
        if (sync.walk(copy)) trees.push_back({ObjectPath{int(i)}, std::move(copy)});
    }
    r.objects = doc.replaceTrees(std::move(trees));
    doc.setImportWarnings(file, r.model.warnings);
    return r;
}

Reimported reimportModel(Document& doc, const fs::path& model, std::optional<bool> wasMerged, const fs::path& converted) {
    StagedModel s = stageReimport(doc, model, converted);
    buildStaged(s);
    return applyReimport(doc, s, wasMerged);
}

ImportWarning explainImportWarning(const std::string& warning) {
    // `sep`: the importer already ends the warning with its fix after the
    // last `sep`. `fix`: the fix to show (instead of that tail, when both).
    struct Rule {
        const char* needle;
        const char* sep;
        const char* fix;
    };
    static const Rule rules[] = {
        {" joints, psxsplash draws at most ", ": ",
         "Delete helper bones (IK targets, poles, twist bones) in the modelling tool so the skeleton has at most 64 joints, then "
         "export again"},
        {"mixes skinned and unskinned parts", ": ", nullptr},
        {" but no skin moves it", ": ", nullptr},
        {" weighted to more than one joint", "; ", nullptr},
        {" have no bone weights", ": ", nullptr},
        {"a non-uniform scale", ": ", nullptr},
        {"cubic-spline keys", ": ", nullptr},
        {" animations, a skin plays at most ", ": ",
         "Keep at most 16 animations in the file (delete unused actions before exporting); the first 16 are on the Skin component, "
         "swap others in by hand"},
        {"' moves no joint and was left out", nullptr,
         "Key the armature's bones instead of the object, or delete the animation in the modelling tool"},
        {"has no skeleton animation", nullptr,
         "Add an animation (action) to the armature in the modelling tool and export it with the model"},
        {"' could not be read: ", nullptr, "Bake the animation in the exporter (Bake Animation) and export again"},
        {"a texture was scaled down to ", nullptr, "Resize the texture to at most 256 x 256 in an image editor to choose how it is scaled"},
        {" was not found, the material is left untextured", nullptr,
         "Put the texture beside the model or in a textures/ folder next to it, then reimport"},
        {"a skin whose joints are not all in the scene", nullptr, "Export the whole armature together with the mesh"},
        {"points and lines were left out", nullptr, "Turn them into faces in the modelling tool if they should be drawn"},
        {"morph target (shape key) animation", nullptr, "Drive the motion with bones instead of shape keys"},
        {"the import settings were not saved", nullptr, "Make the models/ folder writable, then change a setting to save it again"},
    };
    ImportWarning w{warning, {}};
    for (const Rule& r : rules) {
        if (warning.find(r.needle) == std::string::npos) continue;
        if (r.sep) {
            const size_t at = warning.rfind(r.sep);
            if (at != std::string::npos && at > warning.find(r.needle)) {
                w.problem = warning.substr(0, at);
                w.fix = warning.substr(at + std::string(r.sep).size());
            }
        }
        if (r.fix) w.fix = r.fix;
        if (!w.fix.empty() && w.fix[0] >= 'a' && w.fix[0] <= 'z') w.fix[0] = char(w.fix[0] - 'a' + 'A');
        break;
    }
    return w;
}

std::vector<fs::path> ModelWatcher::poll(const fs::path& project) {
    if (project != m_project) {
        m_seen.clear();
        m_project = project;
    }
    std::vector<fs::path> changed;
    if (project.empty()) return changed;
    std::set<fs::path> present;
    std::error_code ec;
    for (fs::directory_iterator d(project / "models", ec), end; !ec && d != end; d.increment(ec)) {
        const fs::path p = d->path();
        std::error_code sec;
        if (!splash::canImportModel(p) || !d->is_regular_file(sec)) continue;
        const fs::file_time_type sidecar = fs::last_write_time(splash::importSettingsPath(p), sec);
        if (sec) continue;  // no sidecar, not an imported model
        present.insert(p);
        const bool first = !m_seen.count(p);
        Seen& seen = m_seen[p];
        if (first || sidecar != seen.sidecar) {
            seen.sidecar = sidecar;
            std::string source;
            try {
                source = splash::loadImportSettings(p).source;
            } catch (const std::exception&) {
                source = seen.sourcePath;  // a sidecar being written; read it next time
            }
            if (source != seen.sourcePath) {
                seen.sourcePath = source;
                seen.source = source.empty() ? fs::file_time_type{} : fs::last_write_time(fromUtf8(source), sec);
            }
        }
        bool report = false;
        if (!seen.sourcePath.empty()) {
            const fs::file_time_type t = fs::last_write_time(fromUtf8(seen.sourcePath), sec);
            if (!sec && t != seen.source) {
                seen.source = t;
                std::error_code cec;
                if (!first) fs::copy_file(fromUtf8(seen.sourcePath), p, fs::copy_options::overwrite_existing, cec);
                report = !first && !cec;
            }
        }
        const fs::file_time_type t = fs::last_write_time(p, sec);
        if (!sec && t != seen.model) {
            report |= !first;
            seen.model = t;
        }
        if (report) changed.push_back(p);
    }
    for (auto it = m_seen.begin(); it != m_seen.end();) it = present.count(it->first) ? std::next(it) : m_seen.erase(it);
    return changed;
}

void RecentScenes::load() {
    m_items.clear();
    if (m_file.empty()) return;
    std::ifstream in(m_file);
    std::string line;
    while (std::getline(in, line) && m_items.size() < kMax) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) m_items.push_back(fromUtf8(line));
    }
}

void RecentScenes::add(const fs::path& scene) {
    remove(scene);
    m_items.insert(m_items.begin(), scene);
    if (m_items.size() > kMax) m_items.resize(kMax);
    save();
}

void RecentScenes::remove(const fs::path& scene) {
    auto it = std::remove(m_items.begin(), m_items.end(), scene);
    if (it == m_items.end()) return;
    m_items.erase(it, m_items.end());
    save();
}

void RecentScenes::save() const {
    if (m_file.empty()) return;
    std::error_code ec;
    if (m_file.has_parent_path()) fs::create_directories(m_file.parent_path(), ec);
    std::ofstream out(m_file, std::ios::binary);
    for (const fs::path& p : m_items) out << utf8(p) << "\n";
}

}  // namespace editor
