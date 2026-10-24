#include "editor/document.hh"

#include <optional>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <system_error>
#include <unordered_set>

#include "audio.hh"
#include "editor/catalog.hh"
#include "terrain.hh"
#include "texture.hh"

namespace fs = std::filesystem;

namespace editor {

namespace {

int countObjects(const std::vector<splash::Object>& objs) {
    int n = 0;
    for (const splash::Object& o : objs) n += 1 + countObjects(o.children);
    return n;
}

bool findByName(const std::vector<splash::Object>& objs, const std::string& name, ObjectPath& path) {
    for (size_t i = 0; i < objs.size(); ++i) {
        path.push_back(static_cast<int>(i));
        if (objs[i].name == name || findByName(objs[i].children, name, path)) return true;
        path.pop_back();
    }
    return false;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

int vramBytes(const splash::PsxTexture& t) {
    return static_cast<int>((t.imageData.size() + t.palette.size()) * sizeof(uint16_t));
}

TextureInfo analyseTexture(const fs::path& file) {
    TextureInfo info;
    if (!isFile(file)) return info;
    try {
        splash::Image img = splash::loadImage(file);
        info.width = img.width;
        info.height = img.height;
        std::unordered_set<uint32_t> colors;
        for (const splash::Image::Px& p : img.pixels) {
            if (p.a <= 0.0f) {
                colors.insert(0x10000u);  // all fully transparent texels count once
                continue;
            }
            auto c5 = [](float v) { return static_cast<uint32_t>(splash::clampv(v, 0.0f, 1.0f) * 31.0f + 0.5f); };
            colors.insert(c5(p.r) | c5(p.g) << 5 | c5(p.b) << 10);
        }
        info.colors15 = static_cast<int>(colors.size());
        info.holes = splash::hasCutoutTexels(img);
        info.vramBytes4 = vramBytes(splash::convertTexture(img, splash::BitDepth::Bpp4, info.holes));
        info.vramBytes8 = vramBytes(splash::convertTexture(img, splash::BitDepth::Bpp8, info.holes));
        info.vramBytes16 = vramBytes(splash::convertTexture(img, splash::BitDepth::Bpp16, info.holes));
        info.status = AssetStatus::Ok;
    } catch (const std::exception& e) {
        info.status = AssetStatus::Unreadable;
        info.error = e.what();
    }
    return info;
}

MeshInfo analyseMesh(const fs::path& file) {
    MeshInfo info;
    if (!isFile(file)) return info;
    try {
        splash::Mesh m = splash::loadMesh(file);
        size_t indices = 0;
        for (const auto& sm : m.submeshes) indices += sm.size();
        info.triangles = static_cast<int>(indices / 3);
        info.submeshes = static_cast<int>(m.submeshes.size());
        info.hasColors = !m.positions.empty() && m.colors.size() == m.positions.size();
        info.status = AssetStatus::Ok;
    } catch (const std::exception& e) {
        info.status = AssetStatus::Unreadable;
        info.error = e.what();
    }
    return info;
}

AudioInfo analyseAudio(const fs::path& file) {
    AudioInfo info;
    if (!isFile(file)) return info;
    try {
        splash::AudioFileInfo w = splash::audioFileInfo(file);
        info.rate = w.rate;
        info.channels = w.channels;
        info.frames = w.frames;
        info.status = AssetStatus::Ok;
    } catch (const std::exception& e) {
        info.status = AssetStatus::Unreadable;
        info.error = e.what();
    }
    return info;
}

}  // namespace

std::optional<std::string> Document::load(const fs::path& project, const fs::path& scene) {
    if (project != m_project) m_importWarnings.clear();
    m_outer.reset();  // another scene leaves prefab mode
    m_prefabFile.clear();
    m_prefabExtra.clear();
    m_prefabErrors.clear();
    m_project = project;
    m_sceneFile = scene;
    m_stem = scene.stem().string();
    if (m_stem.empty()) m_stem = "untitled";
    m_scene = {};
    clearHistory();
    ++m_loadId;
    m_selection.reset();
    m_more.clear();
    m_uiSelection = {};
    m_pinned.reset();
    m_collapsed.clear();
    m_meshes.clear();
    m_textures.clear();
    m_audio.clear();
    m_files.clear();
    m_objectCount = 0;
    m_projectFiles = 0;
    ++m_revision;

    std::error_code ec;
    for (fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec)) ++m_projectFiles;

    try {
        m_scene = splash::loadScene(project / scene);
    } catch (const std::exception& e) {
        m_scene = {};
        return std::string(e.what());
    }
    resolvePrefabs();
    m_objectCount = countObjects(m_scene.objects);
    for (const splash::Object& o : m_scene.objects) cacheAssets(o);
    return std::nullopt;
}

void Document::reset(splash::Scene scene, const fs::path& project, const std::string& stem) {
    if (project != m_project) m_importWarnings.clear();
    m_outer.reset();
    m_prefabFile.clear();
    m_prefabExtra.clear();
    m_prefabErrors.clear();
    m_project = project;
    m_stem = stem;
    m_sceneFile = stem + ".scene";
    m_scene = std::move(scene);
    clearHistory();
    ++m_loadId;
    ++m_revision;
    m_selection.reset();
    m_more.clear();
    m_uiSelection = {};
    m_pinned.reset();
    m_collapsed.clear();
    m_meshes.clear();
    m_textures.clear();
    m_audio.clear();
    m_files.clear();
    m_projectFiles = 0;
    resolvePrefabs();
    recount();
    for (const splash::Object& o : m_scene.objects) cacheAssets(o);
}

void Document::refreshAssets(const std::vector<std::string>& projectPaths) {
    for (const std::string& p : projectPaths) {
        const fs::path ext = fs::path(p).extension();
        if (ext == ".mesh") m_meshes[p] = analyseMesh(resolve(p));
        else if (ext == ".lua" || ext == ".blocks" || ext == ".anim" || splash::isAudioFile(p)) {
            m_files[p] = isFile(resolve(p));
            if (splash::isAudioFile(p)) m_audio[p] = analyseAudio(resolve(p));
        }
        else m_textures[p] = analyseTexture(resolve(p));
    }
    ++m_assetsId;
    ++m_revision;
}

void Document::recount() { m_objectCount = countObjects(m_scene.objects); }

void Document::cacheAssets(const splash::Object& o) {
    if (o.mesh) {
        if (!o.mesh->mesh.empty() && !m_meshes.count(o.mesh->mesh)) m_meshes[o.mesh->mesh] = analyseMesh(resolve(o.mesh->mesh));
        for (const splash::Material& mat : o.mesh->materials)
            if (!mat.texture.empty() && !m_textures.count(mat.texture)) m_textures[mat.texture] = analyseTexture(resolve(mat.texture));
    }
    auto file = [&](const std::string& p) {
        if (!p.empty() && !m_files.count(p)) m_files[p] = isFile(resolve(p));
    };
    if (o.script) file(o.script->lua);
    if (o.trigger) file(o.trigger->lua);
    if (o.audio) {
        file(o.audio->clip);
        if (!o.audio->clip.empty() && !m_audio.count(o.audio->clip)) m_audio[o.audio->clip] = analyseAudio(resolve(o.audio->clip));
    }
    if (o.skin)
        for (const std::string& c : o.skin->clips) file(c);
    for (const splash::Object& c : o.children) cacheAssets(c);
}

const splash::Object* Document::object(const ObjectPath& path) const {
    if (path.empty()) return nullptr;
    const std::vector<splash::Object>* level = &m_scene.objects;
    const splash::Object* o = nullptr;
    for (int i : path) {
        if (i < 0 || static_cast<size_t>(i) >= level->size()) return nullptr;
        o = &(*level)[static_cast<size_t>(i)];
        level = &o->children;
    }
    return o;
}

const splash::Object* Document::parent(const ObjectPath& path) const {
    if (path.size() < 2) return nullptr;
    return object(ObjectPath(path.begin(), path.end() - 1));
}

splash::Object* Document::objectMut(const ObjectPath& path) { return const_cast<splash::Object*>(object(path)); }

std::vector<splash::Object>* Document::childrenMut(const ObjectPath& parentPath) {
    if (parentPath.empty()) return &m_scene.objects;
    splash::Object* o = objectMut(parentPath);
    return o ? &o->children : nullptr;
}

std::vector<ObjectPath> Document::flatPaths() const {
    std::vector<ObjectPath> out;
    ObjectPath p;
    std::function<void(const std::vector<splash::Object>&)> walk = [&](const std::vector<splash::Object>& objs) {
        for (size_t i = 0; i < objs.size(); ++i) {
            p.push_back(static_cast<int>(i));
            out.push_back(p);
            walk(objs[i].children);
            p.pop_back();
        }
    };
    walk(m_scene.objects);
    return out;
}

namespace {

// The object at `p` after a sibling was inserted at (delta +1) or removed
// from (delta -1) `at`; nullopt if `p` was the removed object or inside it.
std::optional<ObjectPath> shifted(ObjectPath p, const ObjectPath& at, int delta) {
    const size_t d = at.size() - 1;
    if (p.size() < at.size() || !std::equal(at.begin(), at.end() - 1, p.begin())) return p;
    if (delta < 0 && p[d] == at[d]) return std::nullopt;
    if (p[d] >= at[d]) p[d] += delta;
    return p;
}

// Copies an object without its children, which can be large and which an
// edit of the object itself never touches.
splash::Object shallowCopy(splash::Object& o) {
    std::vector<splash::Object> kids = std::move(o.children);
    o.children.clear();
    splash::Object copy = o;
    o.children = std::move(kids);
    return copy;
}

// Replaces everything but the children.
void assignShallow(splash::Object& dst, const splash::Object& src) {
    std::vector<splash::Object> kids = std::move(dst.children);
    dst = src;
    dst.children = std::move(kids);
}

class EditCommand : public Command {
public:
    EditCommand(ObjectPath path, splash::Object before, splash::Object after, std::string key)
        : m_path(std::move(path)), m_before(std::move(before)), m_after(std::move(after)), m_key(std::move(key)) {}
    void apply(Document& doc) override { set(doc, m_after); }
    void revert(Document& doc) override { set(doc, m_before); }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const EditCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key || n->m_path != m_path) return false;
        m_after = n->m_after;
        return true;
    }

private:
    void set(Document& doc, const splash::Object& v) {
        if (splash::Object* o = doc.objectMut(m_path)) assignShallow(*o, v);
    }
    ObjectPath m_path;
    splash::Object m_before, m_after;
    std::string m_key;
};

// Several commands that undo and redo as one step.
class GroupCommand : public Command {
public:
    explicit GroupCommand(std::vector<std::unique_ptr<Command>> parts) : m_parts(std::move(parts)) {}
    // A keyed group of edits (editMany with a merge key) on `paths`: a later
    // one with the same key on the same paths folds into it, part by part.
    GroupCommand(std::vector<std::unique_ptr<Command>> parts, std::string key, std::vector<ObjectPath> paths)
        : m_parts(std::move(parts)), m_key(std::move(key)), m_paths(std::move(paths)) {}
    void apply(Document& doc) override {
        for (auto& c : m_parts) c->apply(doc);
    }
    void revert(Document& doc) override {
        for (auto it = m_parts.rbegin(); it != m_parts.rend(); ++it) (*it)->revert(doc);
    }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const GroupCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key || n->m_paths != m_paths || n->m_parts.size() != m_parts.size()) return false;
        // Every part is an EditCommand with this key on the same path, so each merges.
        for (size_t i = 0; i < m_parts.size(); ++i)
            if (!m_parts[i]->merge(*n->m_parts[i])) return false;
        return true;
    }

private:
    std::vector<std::unique_ptr<Command>> m_parts;
    std::string m_key;
    std::vector<ObjectPath> m_paths;
};

// The selection as part of a batch: with `restore` (at the batch's front)
// undo puts it back; without (at the end) do and redo set it.
class SelectionCommand : public Command {
public:
    SelectionCommand(std::optional<ObjectPath> primary, std::vector<ObjectPath> more, bool restore)
        : m_primary(std::move(primary)), m_more(std::move(more)), m_restore(restore) {}
    void apply(Document& doc) override {
        if (!m_restore) doc.selectMany(m_primary, m_more);
    }
    void revert(Document& doc) override {
        if (m_restore) doc.selectMany(m_primary, m_more);
    }

private:
    std::optional<ObjectPath> m_primary;
    std::vector<ObjectPath> m_more;
    bool m_restore;
};

class SettingsCommand : public Command {
public:
    SettingsCommand(splash::SceneSettings before, splash::SceneSettings after, std::string key)
        : m_before(std::move(before)), m_after(std::move(after)), m_key(std::move(key)) {}
    void apply(Document& doc) override { doc.settingsMut() = m_after; }
    void revert(Document& doc) override { doc.settingsMut() = m_before; }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const SettingsCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key) return false;
        m_after = n->m_after;
        return true;
    }

private:
    splash::SceneSettings m_before, m_after;
    std::string m_key;
};

class SpriteSheetsCommand : public Command {
public:
    // `canvases`: the UI canvases before and after when a rename carried the
    // sheet field of the UI images that name the sheet (else not touched);
    // `tileset`: the tilemap's tileset name before and after, the same way.
    SpriteSheetsCommand(std::vector<splash::SpriteSheet> before, std::vector<splash::SpriteSheet> after, std::string key,
                        std::optional<std::pair<std::vector<splash::UICanvas>, std::vector<splash::UICanvas>>> canvases,
                        std::optional<std::pair<std::string, std::string>> tileset = {})
        : m_before(std::move(before)), m_after(std::move(after)), m_key(std::move(key)), m_canvases(std::move(canvases)),
          m_tileset(std::move(tileset)) {}
    void apply(Document& doc) override {
        doc.sceneMut().spriteSheets = m_after;
        if (m_canvases) doc.sceneMut().canvases = m_canvases->second;
        if (m_tileset && doc.sceneMut().tilemap) doc.sceneMut().tilemap->tileset = m_tileset->second;
    }
    void revert(Document& doc) override {
        doc.sceneMut().spriteSheets = m_before;
        if (m_canvases) doc.sceneMut().canvases = m_canvases->first;
        if (m_tileset && doc.sceneMut().tilemap) doc.sceneMut().tilemap->tileset = m_tileset->first;
    }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const SpriteSheetsCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key) return false;
        m_after = n->m_after;
        if (n->m_canvases) {
            if (m_canvases) m_canvases->second = n->m_canvases->second;
            else m_canvases = n->m_canvases;
        }
        if (n->m_tileset) {
            if (m_tileset) m_tileset->second = n->m_tileset->second;
            else m_tileset = n->m_tileset;
        }
        return true;
    }

private:
    std::vector<splash::SpriteSheet> m_before, m_after;
    std::string m_key;
    std::optional<std::pair<std::vector<splash::UICanvas>, std::vector<splash::UICanvas>>> m_canvases;
    std::optional<std::pair<std::string, std::string>> m_tileset;
};

// The scene's tilemap before and after (nullopt: none).
class TilemapCommand : public Command {
public:
    TilemapCommand(std::optional<splash::Tilemap> before, std::optional<splash::Tilemap> after, std::string key)
        : m_before(std::move(before)), m_after(std::move(after)), m_key(std::move(key)) {}
    void apply(Document& doc) override { doc.sceneMut().tilemap = m_after; }
    void revert(Document& doc) override { doc.sceneMut().tilemap = m_before; }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const TilemapCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key) return false;
        m_after = n->m_after;
        return true;
    }

private:
    std::optional<splash::Tilemap> m_before, m_after;
    std::string m_key;
};

// Inserts (forward) or removes (inverse) one object with its subtree.
class StructureCommand : public Command {
public:
    StructureCommand(ObjectPath path, splash::Object obj, bool insert) : m_path(std::move(path)), m_obj(std::move(obj)), m_insert(insert) {}
    void apply(Document& doc) override { m_insert ? insert(doc) : remove(doc); }
    void revert(Document& doc) override { m_insert ? remove(doc) : insert(doc); }

private:
    void insert(Document& doc) {
        std::vector<splash::Object>* level = doc.childrenMut(ObjectPath(m_path.begin(), m_path.end() - 1));
        if (!level) return;
        size_t i = std::min(static_cast<size_t>(m_path.back()), level->size());
        level->insert(level->begin() + static_cast<std::ptrdiff_t>(i), m_obj);
        doc.shiftPaths(m_path, +1);
        doc.recount();
        doc.select(m_path);
    }
    void remove(Document& doc) {
        std::vector<splash::Object>* level = doc.childrenMut(ObjectPath(m_path.begin(), m_path.end() - 1));
        if (!level || static_cast<size_t>(m_path.back()) >= level->size()) return;
        auto it = level->begin() + m_path.back();
        m_obj = std::move(*it);  // keeps the object as it is now for the inverse
        level->erase(it);
        doc.shiftPaths(m_path, -1);
        doc.recount();
    }
    ObjectPath m_path;
    splash::Object m_obj;
    bool m_insert;
};

// Swaps one object with its subtree for another.
class TreeCommand : public Command {
public:
    TreeCommand(ObjectPath path, splash::Object before, splash::Object after)
        : m_path(std::move(path)), m_before(std::move(before)), m_after(std::move(after)) {}
    void apply(Document& doc) override { set(doc, m_after); }
    void revert(Document& doc) override { set(doc, m_before); }

private:
    void set(Document& doc, const splash::Object& v) {
        splash::Object* o = doc.objectMut(m_path);
        if (!o) return;
        *o = v;
        for (std::optional<ObjectPath> p : {doc.selection(), doc.pinned()}) {
            if (!p || p->size() <= m_path.size() || !std::equal(m_path.begin(), m_path.end(), p->begin()) || doc.object(*p)) continue;
            if (p == doc.selection()) doc.selectMany(m_path, doc.moreSelected());
            if (p == doc.pinned()) doc.pin(m_path);
        }
        doc.recount();
    }
    ObjectPath m_path;
    splash::Object m_before, m_after;
};

// Moves one object with its subtree from `from` to `to` (a path in the tree
// as it is once the object has been taken out), swapping its transform.
class MoveCommand : public Command {
public:
    MoveCommand(ObjectPath from, ObjectPath to, splash::Transform before, splash::Transform after)
        : m_from(std::move(from)), m_to(std::move(to)), m_before(before), m_after(after) {}
    void apply(Document& doc) override { move(doc, m_from, m_to, m_after); }
    void revert(Document& doc) override { move(doc, m_to, m_from, m_before); }

private:
    static void move(Document& doc, const ObjectPath& a, const ObjectPath& b, const splash::Transform& t) {
        std::vector<splash::Object>* src = doc.childrenMut(ObjectPath(a.begin(), a.end() - 1));
        if (!src || static_cast<size_t>(a.back()) >= src->size()) return;
        const bool pinned = doc.pinned() && *doc.pinned() == a;
        splash::Object obj = std::move((*src)[a.back()]);
        src->erase(src->begin() + a.back());
        doc.shiftPaths(a, -1);
        std::vector<splash::Object>* dst = doc.childrenMut(ObjectPath(b.begin(), b.end() - 1));
        if (!dst) return;
        obj.transform = t;
        size_t i = std::min(static_cast<size_t>(b.back()), dst->size());
        dst->insert(dst->begin() + static_cast<std::ptrdiff_t>(i), std::move(obj));
        doc.shiftPaths(b, +1);
        doc.select(b);
        if (pinned) doc.pin(b);
    }
    ObjectPath m_from, m_to;
    splash::Transform m_before, m_after;
};

splash::Quat mulQ(splash::Quat a, splash::Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// World position, rotation and lossy scale composed the way splash::flatten does.
struct World {
    splash::Vec3 pos;
    splash::Quat rot;
    splash::Vec3 scale{1, 1, 1};
    splash::Mat34 m = splash::Mat34::trs({}, {}, {1, 1, 1});
};

World worldOf(const Document& doc, const ObjectPath& path) {
    World w;
    for (size_t n = 1; n <= path.size(); ++n) {
        const splash::Transform& t = doc.object(ObjectPath(path.begin(), path.begin() + n))->transform;
        if (n == 1) {
            w.pos = t.position;
            w.rot = t.rotation;
            w.scale = t.scale;
        } else {
            w.pos = w.m.point(t.position);
            w.rot = mulQ(w.rot, t.rotation);
            w.scale = splash::scale(w.scale, t.scale);
        }
        w.m = splash::Mat34::trs(w.pos, w.rot, w.scale);
    }
    return w;
}

// The local transform that puts an object at `w` under a parent at `p`.
splash::Transform localUnder(const World& p, const World& w) {
    splash::Transform t;
    const float(*m)[4] = p.m.m;
    splash::Vec3 d = w.pos - p.m.position();
    float a = m[0][0], b = m[0][1], c = m[0][2], e = m[1][0], f = m[1][1], g = m[1][2], h = m[2][0], i = m[2][1], k = m[2][2];
    float det = a * (f * k - g * i) - b * (e * k - g * h) + c * (e * i - f * h);
    if (std::fabs(det) < 1e-12f) {
        t.position = d;
    } else {
        float inv = 1.0f / det;
        t.position = {((f * k - g * i) * d.x + (c * i - b * k) * d.y + (b * g - c * f) * d.z) * inv,
                      ((g * h - e * k) * d.x + (a * k - c * h) * d.y + (c * e - a * g) * d.z) * inv,
                      ((e * i - f * h) * d.x + (b * h - a * i) * d.y + (a * f - b * e) * d.z) * inv};
    }
    t.rotation = mulQ({-p.rot.x, -p.rot.y, -p.rot.z, p.rot.w}, w.rot);
    auto div = [](float x, float y) { return y != 0 ? x / y : x; };
    t.scale = {div(w.scale.x, p.scale.x), div(w.scale.y, p.scale.y), div(w.scale.z, p.scale.z)};
    return t;
}

}  // namespace

void Document::shiftPaths(const ObjectPath& at, int delta) {
    if (at.empty()) return;
    if (m_selection) m_selection = shifted(*m_selection, at, delta);
    if (m_pinned) m_pinned = shifted(*m_pinned, at, delta);
    std::vector<ObjectPath> more;
    for (const ObjectPath& p : m_more)
        if (auto q = shifted(p, at, delta)) more.push_back(*q);
    m_more = std::move(more);
    // The primary went: the one selected before it takes over.
    if (!m_selection && !m_more.empty()) {
        m_selection = m_more.back();
        m_more.pop_back();
    }
    std::set<ObjectPath> collapsed;
    for (const ObjectPath& p : m_collapsed)
        if (p.empty()) collapsed.insert(p);
        else if (auto q = shifted(p, at, delta)) collapsed.insert(*q);
    m_collapsed = std::move(collapsed);
}

void Document::clearHistory() {
    m_history.clear();
    m_cursor = 0;
    m_savedCursor = 0;
    m_mergeOpen = false;
}

void Document::execute(std::unique_ptr<Command> cmd, bool mergeable) {
    cmd->apply(*this);
    ++m_revision;
    if (m_batch) {
        m_batch->push_back(std::move(cmd));
        return;
    }
    record(std::move(cmd), mergeable);
}

void Document::beginBatch() {
    m_batch.emplace();
}

void Document::endBatch() {
    if (!m_batch) return;
    std::vector<std::unique_ptr<Command>> parts = std::move(*m_batch);
    m_batch.reset();
    if (!parts.empty()) record(std::make_unique<GroupCommand>(std::move(parts)), false);
}

void Document::record(std::unique_ptr<Command> cmd, bool mergeable) {
    if (mergeable && m_mergeOpen && m_cursor > 0 && m_cursor == m_history.size() && m_history[m_cursor - 1]->merge(*cmd)) {
        // The merged step no longer matches what was saved, if it was the saved one.
        if (m_savedCursor == m_cursor) m_savedCursor = kNever;
        return;
    }
    if (m_savedCursor != kNever && m_savedCursor > m_cursor) m_savedCursor = kNever;  // the saved state is being dropped
    m_history.resize(m_cursor);
    m_history.push_back(std::move(cmd));
    m_cursor = m_history.size();
    m_mergeOpen = mergeable;
}

namespace {

// `after` differs from `before` (an instance, children left out) in more
// than what an instance overrides: its name, active flags and transform.
bool touchesContent(splash::Object before, splash::Object after) {
    if (before.prefab != after.prefab) return true;
    after.name = before.name;
    after.active = before.active;
    after.startActive = before.startActive;
    after.transform = before.transform;
    after.extra = before.extra;
    before.prefab.clear();
    after.prefab.clear();
    return splash::writeObjectsText({before}) != splash::writeObjectsText({after});
}

}  // namespace

bool Document::edit(const ObjectPath& path, const std::function<void(splash::Object&)>& fn, const std::string& mergeKey) {
    splash::Object* o = objectMut(path);
    if (!o || inInstance(path)) return false;
    splash::Object before = shallowCopy(*o);
    splash::Object after = before;
    fn(after);
    after.children.clear();
    if (!before.prefab.empty() && touchesContent(before, after)) return false;
    execute(std::make_unique<EditCommand>(path, std::move(before), std::move(after), mergeKey), !mergeKey.empty());
    return true;
}

int Document::editMany(const std::vector<ObjectPath>& paths, const std::function<void(splash::Object&)>& fn,
                       const std::string& mergeKey) {
    return editEach(paths, [&](const ObjectPath&, splash::Object& o) { fn(o); }, mergeKey);
}

int Document::editEach(const std::vector<ObjectPath>& paths, const std::function<void(const ObjectPath&, splash::Object&)>& fn,
                       const std::string& mergeKey) {
    std::vector<std::unique_ptr<Command>> parts;
    std::vector<ObjectPath> edited;
    for (const ObjectPath& path : paths) {
        splash::Object* o = objectMut(path);
        if (!o || inInstance(path)) continue;
        splash::Object before = shallowCopy(*o);
        splash::Object after = before;
        fn(path, after);
        after.children.clear();
        if (!before.prefab.empty() && touchesContent(before, after)) continue;
        parts.push_back(std::make_unique<EditCommand>(path, std::move(before), std::move(after), mergeKey));
        edited.push_back(path);
    }
    if (parts.empty()) return 0;
    const int n = int(parts.size());
    if (mergeKey.empty()) execute(std::make_unique<GroupCommand>(std::move(parts)));
    else execute(std::make_unique<GroupCommand>(std::move(parts), mergeKey, std::move(edited)), true);
    return n;
}

int Document::renameObject(const ObjectPath& path, const std::string& name) {
    const splash::Object* target = object(path);
    if (!target || name.empty() || target->name == name || inInstance(path)) return -1;
    const std::string oldName = target->name;
    bool retarget = target->room.has_value();
    std::vector<ObjectPath> portals;
    for (const ObjectPath& p : flatPaths()) {
        const splash::Object* o = object(p);
        if (p != path && o->room && o->name == oldName) retarget = false;
        if (o->portal && (o->portal->roomA == oldName || o->portal->roomB == oldName)) portals.push_back(p);
    }
    if (!retarget) portals.clear();
    auto follow = [&](splash::Object& ob) {
        if (ob.portal && ob.portal->roomA == oldName) ob.portal->roomA = name;
        if (ob.portal && ob.portal->roomB == oldName) ob.portal->roomB = name;
    };
    // One command per path: the renamed object retargets its own portal (if
    // it has one) in its rename edit.
    const bool selfPortal = std::find(portals.begin(), portals.end(), path) != portals.end();

    std::vector<std::unique_ptr<Command>> parts;
    auto add = [&](const ObjectPath& p, const std::function<void(splash::Object&)>& fn) {
        splash::Object before = shallowCopy(*objectMut(p));
        splash::Object after = before;
        fn(after);
        after.children.clear();
        parts.push_back(std::make_unique<EditCommand>(p, std::move(before), std::move(after), std::string()));
    };
    add(path, [&](splash::Object& ob) {
        ob.name = name;
        if (selfPortal) follow(ob);
    });
    for (const ObjectPath& p : portals)
        if (p != path) add(p, follow);
    const int n = int(portals.size());
    execute(std::make_unique<GroupCommand>(std::move(parts)));
    return n;
}

int Document::replaceTrees(std::vector<std::pair<ObjectPath, splash::Object>> trees) {
    std::vector<std::unique_ptr<Command>> parts;
    for (auto& [path, obj] : trees) {
        const splash::Object* o = object(path);
        if (!o || inInstance(path)) continue;
        parts.push_back(std::make_unique<TreeCommand>(path, *o, std::move(obj)));
    }
    if (parts.empty()) return 0;
    const int n = int(parts.size());
    execute(std::make_unique<GroupCommand>(std::move(parts)));
    return n;
}

void Document::editSettings(const std::function<void(splash::SceneSettings&)>& fn, const std::string& mergeKey) {
    splash::SceneSettings after = m_scene.settings;
    fn(after);
    execute(std::make_unique<SettingsCommand>(m_scene.settings, std::move(after), mergeKey), !mergeKey.empty());
}

bool Document::editSpriteSheets(const std::function<void(std::vector<splash::SpriteSheet>&)>& fn, const std::string& mergeKey) {
    std::vector<splash::SpriteSheet> after = m_scene.spriteSheets;
    fn(after);
    if (after == m_scene.spriteSheets) return false;
    // A rename (same sheets, one's name changed) carries the UI images that
    // show a cell of it, in every canvas, in the same undo step. Deleting a
    // sheet leaves them naming it: the element's problem marker shows that.
    std::optional<std::pair<std::vector<splash::UICanvas>, std::vector<splash::UICanvas>>> canvases;
    std::optional<std::pair<std::string, std::string>> tileset;
    if (after.size() == m_scene.spriteSheets.size()) {
        std::vector<splash::UICanvas> cv = m_scene.canvases;
        bool changed = false;
        for (size_t i = 0; i < after.size(); i++) {
            const std::string& from = m_scene.spriteSheets[i].name;
            const std::string& to = after[i].name;
            if (from == to || from.empty()) continue;
            // Another sheet still has the old name: the images keep pointing at it.
            if (std::any_of(after.begin(), after.end(), [&](const splash::SpriteSheet& s) { return s.name == from; })) continue;
            for (splash::UICanvas& c : cv)
                for (splash::UIElement& e : c.elements)
                    if (e.type == splash::UIElementType::Image && e.sheet == from) e.sheet = to, changed = true;
            if (m_scene.tilemap && m_scene.tilemap->tileset == from) tileset.emplace(from, to);
        }
        if (changed) canvases.emplace(m_scene.canvases, std::move(cv));
    }
    execute(std::make_unique<SpriteSheetsCommand>(m_scene.spriteSheets, std::move(after), mergeKey, std::move(canvases),
                                                  std::move(tileset)),
            !mergeKey.empty());
    return true;
}

bool Document::editTilemap(const std::function<void(std::optional<splash::Tilemap>&)>& fn, const std::string& mergeKey) {
    std::optional<splash::Tilemap> after = m_scene.tilemap;
    fn(after);
    if (after == m_scene.tilemap) return false;
    execute(std::make_unique<TilemapCommand>(m_scene.tilemap, std::move(after), mergeKey), !mergeKey.empty());
    return true;
}

bool Document::insertObject(const ObjectPath& path, splash::Object obj) {
    if (path.empty()) return false;
    ObjectPath at = path;
    // Prefab mode: the root is the prefab's only top-level object, so what
    // would go beside it goes in it, last.
    if (prefabRootLevel({}) && at.size() == 1 && !m_scene.objects.empty())
        at = {0, static_cast<int>(m_scene.objects[0].children.size())};
    const ObjectPath parentPath(at.begin(), at.end() - 1);
    if (!childrenMut(parentPath)) return false;
    // Nothing goes into an instance: its children come from its prefab.
    if (!parentPath.empty() && (inInstance(parentPath) || !object(parentPath)->prefab.empty())) return false;
    execute(std::make_unique<StructureCommand>(at, std::move(obj), true));
    return true;
}

bool Document::removeObject(const ObjectPath& path) {
    if (!object(path) || inInstance(path) || prefabRootLevel(ObjectPath(path.begin(), path.end() - 1))) return false;
    execute(std::make_unique<StructureCommand>(path, splash::Object{}, false));
    return true;
}

namespace {

// "Crate (3)" -> base "Crate" and 3; a name without a number gives 0.
int splitNumbered(const std::string& name, std::string* base) {
    size_t open = name.rfind(" (");
    if (open != std::string::npos && name.size() > open + 3 && name.back() == ')') {
        std::string num = name.substr(open + 2, name.size() - open - 3);
        if (num.find_first_not_of("0123456789") == std::string::npos) {
            *base = name.substr(0, open);
            return std::stoi(num);
        }
    }
    *base = name;
    return 0;
}

// The name Unity gives a copy of `name` among `siblings`: its base with the
// next free number, at least 2.
std::string numberedName(const std::string& name, const std::vector<std::string>& siblings) {
    std::string base;
    splitNumbered(name, &base);
    int next = 1;
    for (const std::string& sib : siblings) {
        std::string b;
        int n = splitNumbered(sib, &b);
        if (b == base) next = std::max(next, n + 1);
    }
    return base + " (" + std::to_string(std::max(next, 2)) + ")";
}

std::vector<std::string> namesOf(const std::vector<splash::Object>& objs) {
    std::vector<std::string> names;
    for (const splash::Object& o : objs) names.push_back(o.name);
    return names;
}

}  // namespace

namespace {
const splash::TerrainComponent* loadedTerrainWithData(const std::vector<splash::Object>& objs, const std::string& data) {
    for (const splash::Object& o : objs) {
        if (o.terrain && o.terrain->data == data && splash::terrainLoaded(*o.terrain)) return &*o.terrain;
        if (const splash::TerrainComponent* t = loadedTerrainWithData(o.children, data)) return t;
    }
    return nullptr;
}
bool terrainUses(const std::vector<splash::Object>& objs, const std::string& data) {
    for (const splash::Object& o : objs)
        if ((o.terrain && o.terrain->data == data) || terrainUses(o.children, data)) return true;
    return false;
}
}  // namespace

void Document::giveTerrainsOwnFiles(splash::Object& o, std::vector<std::string>& taken) const {
    if (o.terrain) {
        splash::TerrainComponent& t = *o.terrain;
        const bool shared = terrainUses(m_scene.objects, t.data) ||
                            std::find(taken.begin(), taken.end(), t.data) != taken.end();
        if (!splash::terrainLoaded(t)) {
            if (const splash::TerrainComponent* src = loadedTerrainWithData(m_scene.objects, t.data)) {
                t.resolution = src->resolution;
                t.heights = src->heights;
                t.cellLayers = src->cellLayers;
            } else if (!t.data.empty() && !m_project.empty()) {
                try {
                    splash::loadTerrainData(t, m_project / t.data);
                } catch (const std::exception&) {
                }
            }
        }
        if (shared) {
            const std::string want = t.data.empty() ? defaultTerrainData(o.name) : t.data;
            t.data = splash::uniqueTerrainData(m_scene, m_project, want, taken);
            if (!m_project.empty() && splash::terrainLoaded(t)) {
                try {
                    splash::saveTerrainData(t, m_project / t.data);
                } catch (const std::exception&) {
                }
            }
        }
        taken.push_back(t.data);
    }
    if (!o.prefab.empty()) return;
    for (splash::Object& c : o.children) giveTerrainsOwnFiles(c, taken);
}

std::optional<ObjectPath> Document::duplicateObject(const ObjectPath& path) {
    const splash::Object* o = object(path);
    if (!o || inInstance(path) || prefabRootLevel(ObjectPath(path.begin(), path.end() - 1))) return std::nullopt;
    const ObjectPath parentPath(path.begin(), path.end() - 1);
    splash::Object copy = *o;
    copy.name = numberedName(o->name, namesOf(parentPath.empty() ? m_scene.objects : object(parentPath)->children));
    std::vector<std::string> taken;
    giveTerrainsOwnFiles(copy, taken);
    ObjectPath at = path;
    ++at.back();
    insertObject(at, std::move(copy));
    return at;
}

std::string Document::copyObject(const ObjectPath& path) const {
    const splash::Object* o = object(path);
    if (!o) return {};
    return splash::writeObjectsText({*o});
}

std::string Document::cutObject(const ObjectPath& path) {
    if (inInstance(path) || prefabRootLevel(ObjectPath(path.begin(), path.end() - 1))) return {};
    std::string text = copyObject(path);
    if (!text.empty()) removeObject(path);
    return text;
}

void Document::batchRestoreSelection() {
    execute(std::make_unique<SelectionCommand>(m_selection, m_more, true));
}

void Document::batchSetSelection(std::optional<ObjectPath> primary, std::vector<ObjectPath> more) {
    execute(std::make_unique<SelectionCommand>(std::move(primary), std::move(more), false));
}

std::vector<ObjectPath> Document::topLevel(const std::vector<ObjectPath>& paths) const {
    std::vector<ObjectPath> out;
    for (const ObjectPath& p : paths) {
        if (!object(p) || std::find(out.begin(), out.end(), p) != out.end()) continue;
        bool inside = false;
        for (const ObjectPath& q : paths)
            if (q.size() < p.size() && std::equal(q.begin(), q.end(), p.begin()) && object(q)) inside = true;
        if (!inside) out.push_back(p);
    }
    std::sort(out.begin(), out.end());  // lexicographic = depth-first tree order
    return out;
}

int Document::removeObjects(const std::vector<ObjectPath>& paths) {
    std::vector<ObjectPath> go;
    for (const ObjectPath& p : topLevel(paths))
        if (!inInstance(p) && !prefabRootLevel(ObjectPath(p.begin(), p.end() - 1))) go.push_back(p);
    if (go.empty()) return 0;
    beginBatch();
    batchRestoreSelection();
    // Last first: removing one never moves another still to go.
    for (auto it = go.rbegin(); it != go.rend(); ++it) removeObject(*it);
    batchSetSelection(std::nullopt, {});
    endBatch();
    return static_cast<int>(go.size());
}

std::vector<ObjectPath> Document::duplicateObjects(const std::vector<ObjectPath>& paths) {
    std::vector<ObjectPath> src;
    for (const ObjectPath& p : topLevel(paths))
        if (!inInstance(p) && !prefabRootLevel(ObjectPath(p.begin(), p.end() - 1))) src.push_back(p);
    std::vector<ObjectPath> copies;
    if (src.empty()) return copies;
    const std::optional<ObjectPath> primary = m_selection;
    std::optional<size_t> primaryCopy;
    beginBatch();
    batchRestoreSelection();
    // Last first, so each original is still where it was; every copy made
    // before moves with the inserts after it.
    for (auto it = src.rbegin(); it != src.rend(); ++it) {
        std::optional<ObjectPath> at = duplicateObject(*it);
        if (!at) continue;
        for (ObjectPath& c : copies) c = *shifted(c, *at, +1);
        if (primary && *primary == *it) primaryCopy = copies.size();
        copies.push_back(*at);
    }
    if (!copies.empty()) {
        const size_t pi = primaryCopy ? *primaryCopy : 0;
        std::vector<ObjectPath> more;
        for (size_t i = copies.size(); i-- > 0;)  // tree order
            if (i != pi) more.push_back(copies[i]);
        batchSetSelection(copies[pi], more);
    }
    endBatch();
    std::reverse(copies.begin(), copies.end());
    return copies;
}

std::string Document::copyObjects(const std::vector<ObjectPath>& paths) const {
    std::vector<splash::Object> objs;
    for (const ObjectPath& p : topLevel(paths)) objs.push_back(*object(p));
    if (objs.empty()) return {};
    return splash::writeObjectsText(objs);
}

std::string Document::cutObjects(const std::vector<ObjectPath>& paths) {
    std::vector<ObjectPath> go;
    for (const ObjectPath& p : topLevel(paths))
        if (!inInstance(p) && !prefabRootLevel(ObjectPath(p.begin(), p.end() - 1))) go.push_back(p);
    std::string text = copyObjects(go);
    if (!text.empty()) removeObjects(go);
    return text;
}

std::optional<ObjectPath> Document::pasteObjects(const std::string& text, const std::optional<ObjectPath>& after, float step) {
    std::optional<std::vector<splash::Object>> objs = splash::readObjectsText(text);
    if (!objs) return std::nullopt;
    ObjectPath parentPath;
    size_t index = m_scene.objects.size();
    // Pasted onto an instance's content: after the instance instead.
    std::optional<ObjectPath> anchor = after;
    if (anchor && !anchor->empty() && object(*anchor) && inInstance(*anchor)) anchor = instanceRoot(*anchor);
    if (anchor && !anchor->empty() && object(*anchor)) {
        parentPath.assign(anchor->begin(), anchor->end() - 1);
        index = static_cast<size_t>(anchor->back()) + 1;
    }
    // Prefab mode: into the root, last.
    if (prefabRootLevel(parentPath) && !m_scene.objects.empty()) {
        parentPath = {0};
        index = m_scene.objects[0].children.size();
    }
    const std::vector<splash::Object>& siblings = *childrenMut(parentPath);
    // Moved off what it would cover: k steps along X, the least k at which no
    // pasted object sits on a sibling copied from the same object: the same
    // name but for its number, and the same in all but name and position.
    auto kind = [](const splash::Object& o) {
        splash::Object k = o;
        splitNumbered(o.name, &k.name);
        const std::string base = k.name;
        k.name.clear();
        k.transform.position = {};
        return base + '\n' + splash::writeObjectsText({k});
    };
    std::vector<std::string> sibKinds;
    for (const splash::Object& sib : siblings) sibKinds.push_back(kind(sib));
    auto covers = [&](const splash::Object& o, const std::string& k, float dx) {
        const splash::Vec3 p{o.transform.position.x + dx, o.transform.position.y, o.transform.position.z};
        auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-4f; };
        for (size_t i = 0; i < siblings.size(); ++i) {
            const splash::Vec3& q = siblings[i].transform.position;
            if (near(q.x, p.x) && near(q.y, p.y) && near(q.z, p.z) && sibKinds[i] == k) return true;
        }
        return false;
    };
    if (step > 0) {
        std::vector<std::string> kinds;
        for (const splash::Object& o : *objs) kinds.push_back(kind(o));
        float dx = 0;
        for (int k = 0; k <= static_cast<int>(siblings.size()); ++k, dx = step * static_cast<float>(k)) {
            bool hit = false;
            for (size_t i = 0; i < objs->size() && !hit; ++i) hit = covers((*objs)[i], kinds[i], dx);
            if (!hit) break;
        }
        for (splash::Object& o : *objs) o.transform.position.x += dx;
    }
    std::vector<std::string> names = namesOf(siblings);
    std::vector<std::string> taken;
    for (splash::Object& o : *objs) giveTerrainsOwnFiles(o, taken);
    std::vector<std::unique_ptr<Command>> parts;
    const size_t count = objs->size();
    // Several pasted: they become the selection, and undo restores the one before.
    if (count > 1) parts.push_back(std::make_unique<SelectionCommand>(m_selection, m_more, true));
    ObjectPath at = parentPath;
    at.push_back(0);
    std::vector<ObjectPath> pasted;
    for (splash::Object& o : *objs) {
        if (std::find(names.begin(), names.end(), o.name) != names.end()) o.name = numberedName(o.name, names);
        names.push_back(o.name);
        at.back() = static_cast<int>(index++);
        pasted.push_back(at);
        parts.push_back(std::make_unique<StructureCommand>(at, std::move(o), true));
    }
    if (count > 1) {
        pasted.pop_back();
        parts.push_back(std::make_unique<SelectionCommand>(at, pasted, false));
    }
    execute(std::make_unique<GroupCommand>(std::move(parts)));
    // Pasted instances are links: fill them in.
    if (text.find("\"prefab\"") != std::string::npos) resolvePrefabs();
    return at;
}

std::optional<ObjectPath> Document::moveObject(const ObjectPath& from, const ObjectPath& toParent, int index) {
    const splash::Object* o = object(from);
    if (!o || index < 0) return std::nullopt;
    if (!toParent.empty() && !object(toParent)) return std::nullopt;
    // An instance's content stays where its prefab has it, and nothing goes in.
    if (inInstance(from) || prefabRootLevel(ObjectPath(from.begin(), from.end() - 1)) || prefabRootLevel(toParent)) return std::nullopt;
    if (!toParent.empty() && (inInstance(toParent) || !object(toParent)->prefab.empty())) return std::nullopt;
    // Into itself or its own subtree.
    if (toParent.size() >= from.size() && std::equal(from.begin(), from.end(), toParent.begin())) return std::nullopt;
    const std::vector<splash::Object>& kids = toParent.empty() ? m_scene.objects : object(toParent)->children;
    index = std::min(index, static_cast<int>(kids.size()));
    const ObjectPath fromParent(from.begin(), from.end() - 1);
    // Where the object lands once it has been taken out.
    ObjectPath to = *shifted(toParent, from, -1);
    if (fromParent == toParent && from.back() < index) --index;
    to.push_back(index);
    if (to == from) return std::nullopt;
    const splash::Transform before = o->transform;
    splash::Transform after;
    const World w = worldOf(*this, from);
    if (toParent.empty()) {
        after.position = w.pos;
        after.rotation = w.rot;
        after.scale = w.scale;
    } else {
        after = localUnder(worldOf(*this, toParent), w);
    }
    execute(std::make_unique<MoveCommand>(from, to, before, after));
    return to;
}

bool Document::undo() {
    if (!canUndo()) return false;
    m_mergeOpen = false;
    m_history[--m_cursor]->revert(*this);
    refill();
    ++m_revision;
    return true;
}

bool Document::redo() {
    if (!canRedo()) return false;
    m_mergeOpen = false;
    m_history[m_cursor++]->apply(*this);
    refill();
    ++m_revision;
    return true;
}

std::optional<std::string> Document::save() {
    try {
        if (m_outer) {
            if (m_scene.objects.size() != 1) return std::string("a prefab holds exactly one top-level object");
            splash::savePrefab({m_scene.objects[0], m_prefabExtra}, scenePath());
        } else {
            splash::saveScene(m_scene, scenePath());
        }
        // Terrain grids live in their own files next to the scene's assets.
        std::vector<std::string> errs = splash::saveTerrains(m_scene, m_project);
        if (!errs.empty()) return errs.front();
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
    m_savedCursor = m_cursor;
    m_mergeOpen = false;  // an edit after saving starts a new step, so undo returns to the saved state
    return std::nullopt;
}

std::optional<std::string> Document::saveAs(const fs::path& scene) {
    if (m_outer) return std::string("a prefab is saved where it is: use Save, or go back to the scene first");
    const fs::path oldFile = m_sceneFile;
    const std::string oldStem = m_stem;
    m_sceneFile = scene;
    m_stem = scene.stem().string();
    if (m_stem.empty()) m_stem = "untitled";
    if (auto err = save()) {
        m_sceneFile = oldFile;
        m_stem = oldStem;
        return err;
    }
    return std::nullopt;
}

bool Document::selectByName(const std::string& name) {
    ObjectPath p;
    if (!findByName(m_scene.objects, name, p)) return false;
    select(p);
    return true;
}

std::vector<ObjectPath> Document::selectedPaths() const {
    std::vector<ObjectPath> v;
    if (!m_selection) return v;
    v = m_more;
    v.push_back(*m_selection);
    return v;
}

bool Document::isSelected(const ObjectPath& path) const {
    return (m_selection && *m_selection == path) || std::find(m_more.begin(), m_more.end(), path) != m_more.end();
}

void Document::selectMany(std::optional<ObjectPath> primary, const std::vector<ObjectPath>& more) {
    std::vector<ObjectPath> kept;
    if (primary && object(*primary))
        for (const ObjectPath& p : more)
            if (p != *primary && object(p) && std::find(kept.begin(), kept.end(), p) == kept.end()) kept.push_back(p);
    m_selection = std::move(primary);
    m_more = std::move(kept);
}

void Document::toggleSelected(const ObjectPath& path) {
    if (!object(path)) return;
    if (m_selection && *m_selection == path) {
        m_selection.reset();
        if (!m_more.empty()) {
            m_selection = m_more.back();
            m_more.pop_back();
        }
        return;
    }
    if (auto it = std::find(m_more.begin(), m_more.end(), path); it != m_more.end()) {
        m_more.erase(it);
        return;
    }
    if (m_selection) m_more.push_back(*m_selection);
    m_selection = path;
}

void Document::toggleExpanded(const ObjectPath& path) {
    if (!m_collapsed.erase(path)) m_collapsed.insert(path);
}

void Document::setAllExpanded(bool expanded) {
    const bool rootCollapsed = m_collapsed.count(ObjectPath{}) != 0;
    m_collapsed.clear();
    if (rootCollapsed) m_collapsed.insert(ObjectPath{});
    if (expanded) return;
    for (const ObjectPath& p : flatPaths())
        if (!object(p)->children.empty()) m_collapsed.insert(p);
}

const MeshInfo* Document::mesh(const std::string& projectPath) const {
    auto it = m_meshes.find(projectPath);
    return it == m_meshes.end() ? nullptr : &it->second;
}

const TextureInfo* Document::texture(const std::string& projectPath) const {
    auto it = m_textures.find(projectPath);
    return it == m_textures.end() ? nullptr : &it->second;
}

const AudioInfo* Document::audio(const std::string& projectPath) const {
    auto it = m_audio.find(projectPath);
    return it == m_audio.end() ? nullptr : &it->second;
}

bool Document::fileExists(const std::string& projectPath) const {
    auto it = m_files.find(projectPath);
    return it != m_files.end() && it->second;
}

bool Document::couldBe4bpp(const splash::MeshComponent& m) const {
    // Every textured material must fit 16 colours, and the mesh's textures
    // together must take less VRAM at 4 bpp than at the current depth.
    if (m.bitDepth == splash::BitDepth::Bpp4) return false;
    int textured = 0, now = 0, at4 = 0;
    for (const splash::Material& mat : m.materials) {
        if (mat.texture.empty()) continue;
        const TextureInfo* t = texture(mat.texture);
        if (!t || t->status != AssetStatus::Ok || t->colors15 > 16) return false;
        textured++;
        now += m.bitDepth == splash::BitDepth::Bpp8 ? t->vramBytes8 : t->vramBytes16;
        at4 += t->vramBytes4;
    }
    return textured > 0 && at4 < now;
}

bool Document::missingMeshColors(const splash::MeshComponent& m) const {
    if (m.vertexColors != splash::VertexColorMode::Mesh) return false;
    const MeshInfo* mi = mesh(m.mesh);
    return mi && mi->status == AssetStatus::Ok && !mi->hasColors;
}

bool Document::hasWarning(const splash::Object& o) const {
    if (!o.prefab.empty() && !o.prefabResolved) return true;
    if (o.mesh) {
        // A terrain builds its mesh from its grid instead of a .mesh file.
        if (o.terrain) {
            if (!splash::terrainLoaded(*o.terrain)) return true;
        } else if (const MeshInfo* mi = mesh(o.mesh->mesh); !mi || mi->status != AssetStatus::Ok) {
            return true;
        }
        for (const splash::Material& mat : o.mesh->materials)
            if (const TextureInfo* t = texture(mat.texture); t && t->status != AssetStatus::Ok) return true;
        if (couldBe4bpp(*o.mesh)) return true;
        if (missingMeshColors(*o.mesh)) return true;
    }
    if (o.script && !o.script->lua.empty() && !fileExists(o.script->lua)) return true;
    return false;
}

static std::string modelKey(const fs::path& project, const fs::path& model) {
    fs::path rel = model;
    if (model.is_absolute() && !project.empty()) {
        std::error_code ec;
        fs::path r = fs::relative(model, project, ec);
        if (!ec && !r.empty()) rel = r;
    }
    std::u8string u = rel.lexically_normal().generic_u8string();
    return std::string(u.begin(), u.end());
}

void Document::setImportWarnings(const fs::path& model, std::vector<std::string> warnings) {
    const std::string key = modelKey(m_project, model);
    if (warnings.empty()) m_importWarnings.erase(key);
    else m_importWarnings[key] = std::move(warnings);
}

const std::vector<std::string>& Document::importWarnings(const fs::path& model) const {
    static const std::vector<std::string> none;
    auto it = m_importWarnings.find(modelKey(m_project, model));
    return it == m_importWarnings.end() ? none : it->second;
}

// --- prefabs ---

bool Document::inInstance(const ObjectPath& path) const {
    for (size_t k = 1; k < path.size(); ++k) {
        const splash::Object* a = object(ObjectPath(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(k)));
        if (a && !a->prefab.empty()) return true;
    }
    return false;
}

std::optional<ObjectPath> Document::instanceRoot(const ObjectPath& path) const {
    for (size_t k = 1; k <= path.size(); ++k) {
        const ObjectPath p(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(k));
        const splash::Object* a = object(p);
        if (!a) return std::nullopt;
        if (!a->prefab.empty()) return p;
    }
    return std::nullopt;
}

namespace {

bool hasInstance(const std::vector<splash::Object>& objs) {
    for (const splash::Object& o : objs)
        if (!o.prefab.empty() || hasInstance(o.children)) return true;
    return false;
}

// Some instance in `o` (itself included) links to `key`.
bool usesPrefab(const splash::Object& o, const std::string& key) {
    if (!o.prefab.empty() && fs::path(o.prefab).lexically_normal() == fs::path(key).lexically_normal()) return true;
    for (const splash::Object& c : o.children)
        if (usesPrefab(c, key)) return true;
    return false;
}

// A file name made of `name`: characters a file name cannot hold become '_'.
std::string fileStem(const std::string& name) {
    std::string s;
    for (char c : name) s += std::string("/\\:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32 ? '_' : c;
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s.empty() ? std::string("Prefab") : s;
}

}  // namespace

void Document::refill() {
    if (hasInstance(m_scene.objects)) resolvePrefabs();
}

std::vector<std::string> Document::resolvePrefabs() {
    m_prefabErrors = splash::resolvePrefabs(m_scene, m_project);
    // Terrain grids the scene (or prefab content) brought in without them.
    m_terrainErrors = splash::loadTerrains(m_scene, m_project);
    // A selection inside content that changed shape moves to its instance.
    for (std::optional<ObjectPath> p : {m_selection, m_pinned}) {
        if (!p || object(*p)) continue;
        std::optional<ObjectPath> up = *p;
        while (up && !up->empty() && !object(*up)) up->pop_back();
        if (up && up->empty()) up.reset();
        if (p == m_selection) m_selection = up;
        if (p == m_pinned) m_pinned = up;
    }
    // The rest of the selection: what no longer resolves leaves it.
    if (!m_more.empty()) selectMany(m_selection, m_more);
    recount();
    for (const splash::Object& o : m_scene.objects) cacheAssets(o);
    ++m_revision;
    return m_prefabErrors;
}

std::string Document::createPrefab(const ObjectPath& path, std::string* error) {
    const splash::Object* o = object(path);
    auto failed = [&](const std::string& why) {
        if (error) *error = why;
        return std::string();
    };
    if (!o) return failed("there is no such object");
    if (inInstance(path)) return failed("it is part of a prefab: open that prefab to change it");
    if (!o->prefab.empty()) return failed("it is already an instance of " + o->prefab);
    if (m_project.empty()) return failed("the scene has no project folder");
    const std::string stem = fileStem(o->name);
    std::error_code ec;
    fs::create_directories(m_project / "prefabs", ec);
    std::string rel;
    for (int n = 1;; ++n) {
        rel = "prefabs/" + stem + (n == 1 ? std::string() : " (" + std::to_string(n) + ")") + ".prefab";
        if (!fs::exists(m_project / fs::path(rel), ec)) break;
    }
    // The root keeps where it is in the world: that is the frame its agent
    // waypoints (world positions) are in. Instances override it anyway.
    splash::Prefab p;
    p.root = *o;
    const World w = worldOf(*this, path);
    p.root.transform = {w.pos, w.rot, w.scale};
    try {
        splash::savePrefab(p, m_project / fs::path(rel));
    } catch (const std::exception& e) {
        return failed(e.what());
    }
    splash::Object inst = *o;
    inst.prefab = rel;
    inst.prefabResolved = true;
    replaceTrees({{path, std::move(inst)}});
    resolvePrefabs();
    return rel;
}

bool Document::insertPrefab(const ObjectPath& at, const std::string& prefab, splash::Vec3 position, std::string* error) {
    auto failed = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (at.empty()) return failed("there is nowhere to put it");
    splash::Prefab p;
    try {
        p = splash::loadPrefab(m_project / fs::path(prefab));
    } catch (const std::exception& e) {
        return failed(e.what());
    }
    // In prefab mode, a prefab that holds the one being edited (or one
    // further out) would hold itself once saved.
    for (const Document* d = this; d && d->m_outer; d = d->m_outer.get()) {
        splash::Scene probe;
        probe.objects.push_back(p.root);
        probe.objects.back().prefab = prefab;
        splash::resolvePrefabs(probe, m_project);
        if (usesPrefab(probe.objects[0], d->m_prefabFile)) return failed("a prefab cannot contain itself");
    }
    ObjectPath to = at;
    if (prefabRootLevel({}) && to.size() == 1 && !m_scene.objects.empty())
        to = {0, static_cast<int>(m_scene.objects[0].children.size())};
    const ObjectPath parentPath(to.begin(), to.end() - 1);
    const std::vector<splash::Object>* siblings = childrenMut(parentPath);
    if (!siblings) return failed("there is nowhere to put it");
    splash::Object inst;
    inst.name = uniqueName(fs::path(prefab).stem().string(), *siblings);
    inst.prefab = prefab;
    inst.transform.position = position;
    if (!insertObject(to, std::move(inst))) return failed("a prefab's content cannot hold other objects");
    resolvePrefabs();
    return true;
}

bool Document::unpackPrefab(const ObjectPath& path) {
    const splash::Object* o = object(path);
    if (!o || o->prefab.empty() || !o->prefabResolved || inInstance(path)) return false;
    splash::Object plain = *o;
    plain.prefab.clear();
    plain.prefabResolved = false;
    // Its terrains become the scene's own: a file each, apart from the prefab's.
    // (The instance itself is still in the scene, so every one counts as shared.)
    std::vector<std::string> taken;
    giveTerrainsOwnFiles(plain, taken);
    return replaceTrees({{path, std::move(plain)}}) == 1;
}

std::optional<std::string> Document::openPrefab(const std::string& prefab) {
    for (const Document* d = this; d && d->m_outer; d = d->m_outer.get())
        if (fs::path(d->m_prefabFile).lexically_normal() == fs::path(prefab).lexically_normal())
            return std::string(prefab + " is already open");
    splash::Prefab p;
    try {
        p = splash::loadPrefab(m_project / fs::path(prefab));
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
    auto outer = std::make_unique<Document>(std::move(*this));
    *this = Document();
    m_project = outer->m_project;
    m_importWarnings = outer->m_importWarnings;
    m_projectFiles = outer->m_projectFiles;
    m_sceneFile = fs::path(prefab);
    m_stem = m_sceneFile.stem().string();
    m_prefabFile = prefab;
    m_prefabExtra = p.extra;
    m_loadId = outer->m_loadId + 1;
    m_revision = outer->m_revision + 1;
    m_assetsId = outer->m_assetsId + 1;
    m_outer = std::move(outer);
    m_scene.objects.push_back(std::move(p.root));
    m_selection = ObjectPath{0};
    resolvePrefabs();
    return std::nullopt;
}

bool Document::closePrefab() {
    if (!m_outer) return false;
    const unsigned load = m_loadId, rev = m_revision, assets = m_assetsId;
    std::map<std::string, std::vector<std::string>> warnings = std::move(m_importWarnings);
    std::unique_ptr<Document> outer = std::move(m_outer);
    *this = std::move(*outer);
    m_importWarnings = std::move(warnings);
    m_loadId = std::max(load, m_loadId) + 1;
    m_revision = std::max(rev, m_revision) + 1;
    m_assetsId = std::max(assets, m_assetsId) + 1;
    // The cached asset facts may be stale (the prefab mode refreshed some).
    m_meshes.clear();
    m_textures.clear();
    m_audio.clear();
    m_files.clear();
    resolvePrefabs();
    return true;
}

}  // namespace editor
