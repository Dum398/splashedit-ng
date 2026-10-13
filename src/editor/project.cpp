#include "editor/project.hh"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
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

AdoptedAsset adoptAsset(const fs::path& project, const fs::path& file, const std::string& folder) {
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
    fs::create_directories(dir, ec);
    if (ec) {
        r.error = "could not create " + utf8(dir) + ": " + ec.message();
        return r;
    }
    const std::string stem = utf8(src.stem()), ext = utf8(src.extension());
    fs::path dest = dir / src.filename();
    for (int n = 2; fs::exists(dest, ec) && !sameBytes(src, dest); ++n) dest = dir / fromUtf8(stem + "_" + std::to_string(n) + ext);
    if (!fs::exists(dest, ec)) {
        fs::copy_file(src, dest, ec);
        if (ec) {
            r.error = "could not copy " + utf8(src.filename()) + ": " + ec.message();
            return r;
        }
        r.copied = true;
    }
    r.path = genericUtf8(dest.lexically_relative(root));
    return r;
}

splash::Object modelObject(const splash::ImportedModel& model, const std::string& name) {
    splash::Object root;
    root.name = name;
    if (model.nodes.size() <= 1) {
        root.mesh.emplace();
        root.mesh->mesh = model.mesh;
        root.mesh->materials = model.materials;
        return root;
    }
    std::vector<splash::Object> objs(model.nodes.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        const splash::ImportedNode& n = model.nodes[i];
        objs[i].name = n.name;
        objs[i].transform = n.transform;
        if (!n.mesh.empty()) {
            objs[i].mesh.emplace();
            objs[i].mesh->mesh = n.mesh;
            objs[i].mesh->materials = n.materials;
        }
    }
    // Parents come before their children, so walking backwards finishes every
    // object's subtree before it moves into its parent (at the front, which
    // keeps the file's order).
    for (size_t i = objs.size(); i-- > 0;) {
        const int p = model.nodes[i].parent;
        if (p >= 0 && static_cast<size_t>(p) < i)
            objs[static_cast<size_t>(p)].children.insert(objs[static_cast<size_t>(p)].children.begin(), std::move(objs[i]));
    }
    for (size_t i = 0; i < objs.size(); ++i) {
        const int p = model.nodes[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= i) root.children.push_back(std::move(objs[i]));
    }
    return root;
}

PlacedModel placeModel(Document& doc, const fs::path& file, const std::optional<ObjectPath>& replace, const ObjectPath& at,
                       splash::Vec3 position) {
    PlacedModel r;
    if (doc.projectRoot().empty()) {
        r.error = "no project is open";
        return r;
    }
    // Imported from where it was found, so a .gltf's buffers and textures
    // beside it are still at hand; adopted once it has imported, so a file
    // that is not a model never lands in the project.
    try {
        r.model = splash::importModel(file, doc.projectRoot());
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    AdoptedAsset adopted = adoptAsset(doc.projectRoot(), file, "models");
    if (!adopted.error.empty()) {
        r.error = adopted.error;
        return r;
    }
    std::vector<std::string> written = r.model.textures;
    written.push_back(r.model.mesh);
    for (const splash::ImportedNode& n : r.model.nodes)
        if (!n.mesh.empty()) written.push_back(n.mesh);
    doc.refreshAssets(written);

    const splash::Object* target = replace ? doc.object(*replace) : nullptr;
    if (target && target->mesh) {
        doc.edit(*replace, [&](splash::Object& o) {
            o.mesh->mesh = r.model.mesh;
            o.mesh->materials = r.model.materials;
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
    splash::Object obj = modelObject(r.model, uniqueName(utf8(file.stem()), parent ? parent->children : doc.scene().objects));
    obj.transform.position = position;
    doc.insertObject(at, std::move(obj));
    r.path = at;
    return r;
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
