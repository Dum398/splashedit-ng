#include "editor/asset_browser.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <system_error>

#include "audio.hh"
#include "blocks.hh"
#include "editor/document.hh"
#include "import.hh"

namespace editor {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
bool startsWith(const std::string& s, const std::string& head) { return s.compare(0, head.size(), head) == 0; }

// A stored asset path as the index spells it: '/'-separated, no "./".
std::string normal(const std::string& p) {
    if (p.empty()) return p;
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    return utf8(fromUtf8(s).lexically_normal());
}

std::string parentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string join(const std::string& folder, const std::string& name) { return folder.empty() ? name : folder + "/" + name; }

bool inside(const std::string& path, const std::string& folder) { return folder.empty() || startsWith(path, folder + "/"); }

const char* kSidecar = ".import.json";

}  // namespace

const char* assetTypeName(AssetType t, bool plural) {
    switch (t) {
        case AssetType::Folder: return plural ? "Folders" : "Folder";
        case AssetType::Model: return plural ? "Models" : "Model";
        case AssetType::Mesh: return plural ? "Meshes" : "Mesh";
        case AssetType::Texture: return plural ? "Textures" : "Texture";
        case AssetType::Script: return plural ? "Scripts" : "Script";
        case AssetType::Audio: return plural ? "Audio" : "Audio";
        case AssetType::Animation: return plural ? "Animations" : "Animation";
        case AssetType::Scene: return plural ? "Scenes" : "Scene";
        case AssetType::Prefab: return plural ? "Prefabs" : "Prefab";
        case AssetType::Font: return plural ? "Fonts" : "Font";
        case AssetType::Other: return plural ? "Other files" : "File";
    }
    return "";
}

AssetType classifyAsset(const fs::path& file, bool directory) {
    if (directory) return AssetType::Folder;
    const std::string e = lower(utf8(file.extension()));
    if (splash::canImportModel(file)) return AssetType::Model;
    if (e == ".mesh") return AssetType::Mesh;
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || e == ".gif") return AssetType::Texture;
    // psxsplash.d.lua is lua-language-server definitions, no script to run.
    const std::string n = lower(utf8(file.filename()));
    if (e == ".lua" && n.size() > 6 && n.compare(n.size() - 6, 6, ".d.lua") == 0) return AssetType::Other;
    if (e == ".lua" || e == ".blocks") return AssetType::Script;
    if (splash::isAudioFile(file)) return AssetType::Audio;
    if (e == ".anim") return AssetType::Animation;
    if (e == ".scene") return AssetType::Scene;
    if (e == ".prefab") return AssetType::Prefab;
    if (e == ".ttf" || e == ".otf") return AssetType::Font;
    return AssetType::Other;
}

// --- references ---

namespace {

// Calls fn(field, objectNames, objectPath, value) on every asset path field of
// `scene` (const or not), in a fixed order. The one list both the reference
// scan and the reference update walk, so neither misses a field.
template <class SceneT, class Fn>
void visitObjectPaths(SceneT& o, const std::string& names, std::vector<int>& path, Fn& fn) {
    // An instance refers to its prefab; what it shows is the prefab file's
    // own references (the prefab file is scanned as one).
    if (!o.prefab.empty()) {
        fn("Prefab", names, path, o.prefab);
        return;
    }
    if (o.mesh) {
        fn("Mesh", names, path, o.mesh->mesh);
        for (auto& m : o.mesh->materials) fn("Texture", names, path, m.texture);
    }
    if (o.skin)
        for (auto& c : o.skin->clips) fn("Skin clip", names, path, c);
    if (o.script) fn("Script", names, path, o.script->lua);
    if (o.trigger) fn("Trigger script", names, path, o.trigger->lua);
    if (o.audio) fn("Audio clip", names, path, o.audio->clip);
    for (size_t i = 0; i < o.children.size(); ++i) {
        path.push_back(static_cast<int>(i));
        visitObjectPaths(o.children[i], names + "/" + o.children[i].name, path, fn);
        path.pop_back();
    }
}

template <class SceneT, class Fn>
void visitScenePaths(SceneT& scene, Fn fn) {
    std::vector<int> none;
    fn("Scene script", std::string(), none, scene.settings.script);
    for (auto& icon : scene.settings.memCard.icons) fn("Memory card icon", std::string(), none, icon);
    for (auto& f : scene.fonts) {
        fn("UI font", std::string(), none, f.source);
        fn("UI font", std::string(), none, f.bitmap);
    }
    for (auto& s : scene.spriteSheets) fn("Sprite sheet", s.name, none, s.texture);
    for (auto& c : scene.canvases)
        for (auto& e : c.elements) fn("UI image", c.name + "/" + e.name, none, e.texture);
    std::vector<int> path;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        path.assign(1, static_cast<int>(i));
        visitObjectPaths(scene.objects[i], scene.objects[i].name, path, fn);
    }
}

}  // namespace

namespace {

// A file whose references are scanned and rewritten: a scene, or a prefab
// read as a scene holding its root alone.
bool refersToAssets(AssetType t) { return t == AssetType::Scene || t == AssetType::Prefab; }

splash::Scene loadReferrer(const fs::path& file, AssetType t, splash::ExtraKeys* prefabExtra = nullptr) {
    if (t != AssetType::Prefab) return splash::loadScene(file);
    splash::Prefab p = splash::loadPrefab(file);
    if (prefabExtra) *prefabExtra = p.extra;
    splash::Scene s;
    s.objects.push_back(std::move(p.root));
    return s;
}

void saveReferrer(const splash::Scene& s, const fs::path& file, AssetType t, const splash::ExtraKeys& prefabExtra) {
    if (t != AssetType::Prefab) return splash::saveScene(s, file);
    splash::savePrefab({s.objects.at(0), prefabExtra}, file);
}

}  // namespace

std::vector<AssetRef> sceneReferences(const splash::Scene& scene, const std::string& sceneFile) {
    std::vector<AssetRef> out;
    visitScenePaths(scene, [&](const char* field, const std::string& names, const std::vector<int>& path, const std::string& asset) {
        if (!asset.empty()) out.push_back({sceneFile, names, field, normal(asset), path});
    });
    return out;
}

std::string movedPath(const std::string& p, const std::vector<std::pair<std::string, std::string>>& moves) {
    if (p.empty()) return p;
    const std::string n = normal(p);
    for (const auto& [from, to] : moves) {
        if (n == from) return to;
        if (startsWith(n, from + "/")) return to + n.substr(from.size());
    }
    return p;
}

int updateReferences(splash::Scene& scene, const std::vector<std::pair<std::string, std::string>>& moves,
                     std::vector<std::string>* changed) {
    int n = 0;
    visitScenePaths(scene, [&](const char*, const std::string&, const std::vector<int>&, std::string& asset) {
        const std::string to = movedPath(asset, moves);
        if (to == asset) return;
        if (changed) {
            changed->push_back(normal(asset));
            changed->push_back(to);
        }
        asset = to;
        ++n;
    });
    return n;
}

// --- index ---

bool AssetIndex::scan(const fs::path& project) {
    std::vector<AssetEntry> found;
    std::set<std::string> files;  // every file seen, sidecars included
    std::error_code ec;
    if (!project.empty() && fs::is_directory(project, ec)) {
        fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry& d = *it;
            const std::string name = utf8(d.path().filename());
            std::error_code sec;
            const bool dir = d.is_directory(sec);
            if (name.empty() || name[0] == '.') {
                if (dir) it.disable_recursion_pending();
                continue;
            }
            const std::string rel = utf8(d.path().lexically_relative(project));
            files.insert(rel);
            if (!dir && endsWith(lower(name), kSidecar)) continue;
            AssetEntry e;
            e.path = rel;
            e.name = name;
            e.folder = parentOf(rel);
            e.type = classifyAsset(d.path(), dir);
            if (!dir) e.size = d.file_size(sec);
            e.mtime = d.last_write_time(sec);
            found.push_back(std::move(e));
        }
    }
    // What each imported model (one in models/ with a sidecar) wrote.
    for (const AssetEntry& m : found) {
        if (m.type != AssetType::Model || m.folder != "models" || !files.count(m.path + kSidecar)) continue;
        const std::string stem = splash::importName(fromUtf8(m.name));
        const std::string base = "models/" + stem;
        for (AssetEntry& e : found) {
            if (&e == &m || !e.madeBy.empty()) continue;
            bool made = e.path == base + ".mesh" || e.path == base + ".png" || e.path == base || startsWith(e.path, base + "/");
            if (!made && e.folder == "models" && startsWith(e.path, base + "_") && endsWith(e.path, ".png")) {
                const std::string n = e.path.substr(base.size() + 1, e.path.size() - base.size() - 5);
                made = !n.empty() && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; });
            }
            if (made) e.madeBy = m.path;
        }
    }
    std::sort(found.begin(), found.end(), [](const AssetEntry& a, const AssetEntry& b) {
        if (a.folder != b.folder) return a.folder < b.folder;
        const bool fa = a.type == AssetType::Folder, fb = b.type == AssetType::Folder;
        if (fa != fb) return fa;
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    bool changed = project != m_project || found.size() != m_entries.size();
    for (size_t i = 0; !changed && i < found.size(); ++i) {
        const AssetEntry &a = found[i], &b = m_entries[i];
        // A folder's own time is not compared: on Windows the time a listing
        // reports for a folder can move by a few milliseconds between two
        // scans of an unchanged tree. What a folder holds is compared
        // through its own entries.
        const bool timeMoved = a.type != AssetType::Folder && a.mtime != b.mtime;
        changed = a.path != b.path || a.size != b.size || timeMoved || a.type != b.type || a.madeBy != b.madeBy;
    }
    if (!changed) return false;
    m_project = project;
    m_entries = std::move(found);
    m_byPath.clear();
    for (size_t i = 0; i < m_entries.size(); ++i) m_byPath[m_entries[i].path] = i;
    ++m_generation;
    return true;
}

const AssetEntry* AssetIndex::find(const std::string& path) const {
    auto it = m_byPath.find(path);
    return it == m_byPath.end() ? nullptr : &m_entries[it->second];
}

std::vector<std::string> AssetIndex::folders() const {
    std::vector<std::string> out{""};
    for (const AssetEntry& e : m_entries)
        if (e.type == AssetType::Folder) out.push_back(e.path);
    std::sort(out.begin() + 1, out.end());
    return out;
}

std::vector<const AssetEntry*> AssetIndex::children(const std::string& folder) const {
    std::vector<const AssetEntry*> out;
    for (const AssetEntry& e : m_entries)
        if (e.folder == folder) out.push_back(&e);
    return out;
}

std::vector<const AssetEntry*> filterAssets(const AssetIndex& index, const AssetFilter& filter) {
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : filter.query + " ") {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!w.empty()) words.push_back(lower(w));
                w.clear();
            } else {
                w += c;
            }
        }
    }
    if (words.empty() && filter.types == 0) return index.children(filter.folder);
    std::vector<const AssetEntry*> out;
    for (const AssetEntry& e : index.entries()) {
        if (e.type == AssetType::Folder || !inside(e.path, filter.folder)) continue;
        if (filter.types && !(filter.types & (1u << static_cast<unsigned>(e.type)))) continue;
        const std::string name = lower(e.name);
        bool all = true;
        for (const std::string& w : words) all = all && name.find(w) != std::string::npos;
        if (all) out.push_back(&e);
    }
    return out;
}

// --- references across the project ---

void AssetReferences::update(const AssetIndex& index, const splash::Scene& open, const std::string& openFile, unsigned openRevision) {
    bool changed = false;
    const std::string of = normal(openFile);
    if (openRevision != m_openRevision || of != m_openFile) {
        m_open = sceneReferences(open, of);
        m_openRevision = openRevision;
        m_openFile = of;
        changed = true;
    }
    if (index.generation() != m_indexGeneration || changed) {
        m_indexGeneration = index.generation();
        std::set<std::string> seen;
        for (const AssetEntry& e : index.entries()) {
            if (!refersToAssets(e.type) || e.path == m_openFile) continue;
            seen.insert(e.path);
            auto it = m_other.find(e.path);
            if (it != m_other.end() && it->second.mtime == e.mtime) continue;
            Cached c;
            c.mtime = e.mtime;
            try {
                c.refs = sceneReferences(loadReferrer(index.project() / fromUtf8(e.path), e.type), e.path);
                m_unreadable.erase(e.path);
            } catch (const std::exception& ex) {
                m_unreadable[e.path] = ex.what();
            }
            m_other[e.path] = std::move(c);
            changed = true;
        }
        for (auto it = m_other.begin(); it != m_other.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            m_unreadable.erase(it->first);
            it = m_other.erase(it);
            changed = true;
        }
    }
    if (!changed) return;
    m_byAsset.clear();
    for (const AssetRef& r : m_open) m_byAsset.emplace(r.asset, &r);
    for (const auto& [file, c] : m_other)
        for (const AssetRef& r : c.refs) m_byAsset.emplace(r.asset, &r);
}

std::vector<AssetRef> AssetReferences::to(const AssetIndex& index, const std::string& asset) const {
    std::vector<AssetRef> out;
    const AssetEntry* e = index.find(asset);
    auto exact = [&](const std::string& p) {
        auto [lo, hi] = m_byAsset.equal_range(p);
        for (auto it = lo; it != hi; ++it) out.push_back(*it->second);
    };
    if (e && e->type == AssetType::Folder) {
        for (auto it = m_byAsset.lower_bound(asset + "/"); it != m_byAsset.end() && startsWith(it->first, asset + "/"); ++it)
            out.push_back(*it->second);
        return out;
    }
    exact(asset);
    if (e && e->type == AssetType::Model)
        for (const std::string& o : modelOutputs(index, asset)) exact(o);
    return out;
}

std::vector<std::string> modelOutputs(const AssetIndex& index, const std::string& model) {
    std::vector<std::string> out;
    for (const AssetEntry& e : index.entries())
        if (e.madeBy == model) out.push_back(e.path);
    return out;
}

// --- operations ---

namespace {

// Why `asset` may not be renamed, moved or deleted, or empty. A delete is
// refused while anything uses the asset (nothing could stand in for it); a
// rename or move only when a reference to it could not be updated: it is
// (or holds) the open scene, whose file the document keeps, or a scene that
// could not be read names it.
std::string refusal(const AssetIndex& index, const AssetReferences& refs, const AssetEntry& e, const std::string& openScene,
                    bool deleting) {
    const std::string os = normal(openScene);
    if (!os.empty() && (e.path == os || (e.type == AssetType::Folder && inside(os, e.path))))
        return std::string(e.type == AssetType::Folder ? "it holds" : "it is") + " the open scene" +
               (deleting ? "" : "; use Save As to give the scene another name");
    // A scene that could not be read may name it: its reference could not be updated.
    for (const auto& [scene, why] : refs.unreadable()) {
        std::string text;
        if (FILE* f = std::fopen(utf8(index.project() / fromUtf8(scene)).c_str(), "rb")) {
            char buf[4096];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
            std::fclose(f);
        }
        std::vector<std::string> names{e.path};
        if (e.type == AssetType::Model)
            for (const std::string& o : modelOutputs(index, e.path)) names.push_back(o);
        for (const std::string& n : names)
            if (text.find("\"" + n + "\"") != std::string::npos || (e.type == AssetType::Folder && text.find("\"" + n + "/") != std::string::npos))
                return scene + " names it but could not be read (" + why + "), so its reference could not be updated. Fix or remove " +
                       scene + " first";
    }
    if (!deleting) return {};
    const std::vector<AssetRef> used = refs.to(index, e.path);
    if (used.empty()) return {};
    std::string where;
    std::set<std::string> listed;
    for (const AssetRef& r : used) {
        const std::string one = (r.object.empty() ? r.field : r.object) + " in " + r.scene;
        if (!listed.insert(one).second) continue;
        if (listed.size() > 3) continue;
        where += (where.empty() ? "" : ", ") + one;
    }
    if (listed.size() > 3) where += " and " + std::to_string(listed.size() - 3) + " more";
    return std::string("it is used by ") + where + "; deleting it would break " + (used.size() == 1 ? "that reference" : "those references") +
           ". Point them at another file first";
}

// Moves each (from, to) pair, making the destination's folder; undoes the
// ones done when one fails.
std::string moveAll(const fs::path& root, const std::vector<std::pair<std::string, std::string>>& moves) {
    std::error_code ec;
    for (size_t i = 0; i < moves.size(); ++i) {
        const fs::path to = root / fromUtf8(moves[i].second);
        fs::create_directories(to.parent_path(), ec);
        ec.clear();
        const fs::path from = root / fromUtf8(moves[i].first);
        if (fs::exists(to, ec) && !fs::equivalent(from, to, ec)) ec = std::make_error_code(std::errc::file_exists);
        else fs::rename(from, to, ec);
        if (ec) {
            const std::string why = "could not move " + moves[i].first + " to " + moves[i].second + ": " + ec.message();
            for (size_t j = i; j-- > 0;) {
                std::error_code back;
                fs::rename(root / fromUtf8(moves[j].second), root / fromUtf8(moves[j].first), back);
            }
            return why;
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> reversed(const std::vector<std::pair<std::string, std::string>>& moves) {
    std::vector<std::pair<std::string, std::string>> r;
    for (size_t i = moves.size(); i-- > 0;) r.push_back({moves[i].second, moves[i].first});
    return r;
}

// Removes `dir` and its parents up to (not including) `stop` while empty.
void pruneEmpty(const fs::path& stop, fs::path dir) {
    std::error_code ec;
    auto inside = [&](const fs::path& d) {
        fs::path rel = d.lexically_relative(stop);
        return !rel.empty() && *rel.begin() != "..";
    };
    while (!dir.empty() && dir != stop && inside(dir) && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
        fs::remove(dir, ec);
        if (ec) return;
        dir = dir.parent_path();
    }
}

bool readFile(const fs::path& file, std::string* out) {
    FILE* f = std::fopen(utf8(file).c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out->append(buf, n);
    std::fclose(f);
    return true;
}

// Writes `text` over `file` through a hidden temporary beside it.
bool writeFile(const fs::path& file, const std::string& text) {
    const fs::path tmp = file.parent_path() / fromUtf8("." + utf8(file.filename()) + ".writing");
    FILE* f = std::fopen(utf8(tmp).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) return false;
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

const char* kTrash = ".trash";
std::string g_historyError;

// The files a plan moved (and the saved scenes it rewrote), and the open
// scene before and after its references were updated: one undo step.
class AssetFilesCommand : public Command {
  public:
    struct SceneFile {
        std::string path;
        std::string before, after;
    };
    fs::path root;
    std::vector<std::pair<std::string, std::string>> moves;
    std::vector<SceneFile> scenes;
    bool sceneChanged = false;
    splash::Scene before, after;
    std::vector<std::string> recheck;  // asset paths the document should look up again
    // A conversion's file contents, at the moved path: `after` once moved,
    // `before` put back before moving back.
    struct Swap {
        std::string path;  // where the file is after the moves
        std::string before, after;
    };
    std::optional<Swap> swap;

    void apply(Document& doc) override {
        if (!m_first) {
            if (std::string why = moveAll(root, moves); !why.empty()) g_historyError = "Redo: " + why;
            if (swap && !writeFile(root / fromUtf8(swap->path), swap->after)) g_historyError = "Redo: could not write " + swap->path;
            for (const SceneFile& s : scenes)
                if (!writeFile(root / fromUtf8(s.path), s.after)) g_historyError = "Redo: could not write " + s.path;
        }
        m_first = false;
        if (sceneChanged) doc.sceneMut() = after;
        // Prefab files moved or rewritten: what the instances show, again.
        doc.resolvePrefabs();
        if (!recheck.empty()) doc.refreshAssets(recheck);
        log(moves);
    }
    void revert(Document& doc) override {
        for (const SceneFile& s : scenes)
            if (!writeFile(root / fromUtf8(s.path), s.before)) g_historyError = "Undo: could not write " + s.path;
        if (swap && !writeFile(root / fromUtf8(swap->path), swap->before)) g_historyError = "Undo: could not write " + swap->path;
        if (std::string why = moveAll(root, reversed(moves)); !why.empty()) g_historyError = "Undo: " + why;
        // A trashed file back home leaves its trash folder empty.
        for (const auto& [from, to] : moves) pruneEmpty(root / ".trash", (root / fromUtf8(to)).parent_path());
        if (sceneChanged) doc.sceneMut() = before;
        doc.resolvePrefabs();
        if (!recheck.empty()) doc.refreshAssets(recheck);
        log(reversed(moves));
    }
  private:
    void log(const std::vector<std::pair<std::string, std::string>>& m) {
        for (const auto& [from, to] : m) g_movedLog.push_back({root / fromUtf8(from), root / fromUtf8(to)});
    }
    bool m_first = true;

  public:
    static std::vector<std::pair<fs::path, fs::path>> g_movedLog;
};

std::vector<std::pair<fs::path, fs::path>> AssetFilesCommand::g_movedLog;

// Checks a name typed for `e`: trimmed, no separators, not hidden; a file
// keeps its extension (added when left out). Empty with `*error` set when bad.
std::string checkName(const AssetEntry& e, const std::string& rawName, std::string* error) {
    std::string name = rawName;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    if (name.empty()) {
        *error = "the name is empty";
        return {};
    }
    if (name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..") {
        *error = "a name cannot hold / \\ or :";
        return {};
    }
    if (name[0] == '.') {
        *error = "a name starting with '.' would hide the file";
        return {};
    }
    if (e.type != AssetType::Folder) {
        const std::string ext = utf8(fromUtf8(e.name).extension());
        const std::string got = utf8(fromUtf8(name).extension());
        if (got.empty()) name += ext;
        else if (lower(got) != lower(ext)) {
            *error = "keep the " + ext + " extension: the file would change type";
            return {};
        }
    }
    return name;
}

// The plan for `e` to become `to` (a path in the project): the moves and the checks they share.
AssetPlan planTo(const AssetIndex& index, const AssetReferences& refs, const AssetEntry& e, const std::string& to,
                 const std::string& openScene) {
    AssetPlan r;
    r.path = to;
    if (to == e.path) return r;
    const fs::path root = index.project();
    std::error_code ec;
    const bool sameFile = fs::exists(root / fromUtf8(to), ec) && fs::equivalent(root / fromUtf8(to), root / fromUtf8(e.path), ec);
    if (!sameFile && fs::exists(root / fromUtf8(to), ec)) {
        r.error = to + " already exists";
        return r;
    }
    if (!e.madeBy.empty()) {
        r.error = "the import of " + e.madeBy + " wrote it and writes it again on the next import; rename the model instead";
        return r;
    }
    if (e.type == AssetType::Folder && inside(to, e.path)) {
        r.error = "a folder cannot go inside itself";
        return r;
    }
    if (std::string why = refusal(index, refs, e, openScene, false); !why.empty()) {
        r.error = why;
        return r;
    }
    r.moves.push_back({e.path, to});
    if (e.type == AssetType::Model && fs::exists(root / fromUtf8(e.path + kSidecar), ec)) {
        const std::vector<std::string> outputs = modelOutputs(index, e.path);
        if (parentOf(to) != parentOf(e.path) && !outputs.empty()) {
            r.error = "its import wrote " + outputs.front() + (outputs.size() > 1 ? " and more" : "") +
                      " in models/, named after it, and imports only write there; rename it in place instead";
            r.moves.clear();
            return r;
        }
        r.moves.push_back({e.path + kSidecar, to + kSidecar});
        // The outputs follow the new import name.
        const std::string from = "models/" + splash::importName(fromUtf8(e.name));
        const std::string dest = "models/" + splash::importName(fromUtf8(to).filename());
        if (from != dest)
            for (const std::string& o : outputs) {
                if (index.find(parentOf(o)) && index.find(parentOf(o))->madeBy == e.path) continue;  // moves with its folder
                const std::string moved = dest + o.substr(from.size());
                if (fs::exists(root / fromUtf8(moved), ec)) {
                    r.error = moved + " already exists";
                    r.moves.clear();
                    return r;
                }
                r.moves.push_back({o, moved});
            }
    }
    return r;
}

}  // namespace

AssetPlan planRename(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& rawName,
                     const std::string& openScene) {
    AssetPlan r;
    const AssetEntry* e = index.find(asset);
    if (!e) {
        r.error = asset + " is not in the project any more";
        return r;
    }
    const std::string name = checkName(*e, rawName, &r.error);
    if (name.empty()) return r;
    return planTo(index, refs, *e, join(e->folder, name), openScene);
}

AssetPlan planMove(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& folder,
                   const std::string& openScene) {
    AssetPlan r;
    const AssetEntry* e = index.find(asset);
    if (!e) {
        r.error = asset + " is not in the project any more";
        return r;
    }
    const std::string f = normal(folder);
    if (!f.empty() && (!index.find(f) || index.find(f)->type != AssetType::Folder)) {
        r.error = f + " is not a folder of the project";
        return r;
    }
    if (f == e->folder) {
        r.path = e->path;  // already there
        return r;
    }
    return planTo(index, refs, *e, join(f, e->name), openScene);
}

std::vector<std::string> deletionSet(const AssetIndex& index, const std::string& asset) {
    std::vector<std::string> out;
    const AssetEntry* e = index.find(asset);
    if (!e) return out;
    out.push_back(asset);
    std::error_code ec;
    if (e->type == AssetType::Model) {
        if (fs::exists(index.project() / fromUtf8(asset + kSidecar), ec)) out.push_back(asset + kSidecar);
        for (const std::string& o : modelOutputs(index, asset)) out.push_back(o);
    }
    if (e->type == AssetType::Folder)
        for (const AssetEntry& c : index.entries())
            if (inside(c.path, asset)) out.push_back(c.path);
    return out;
}

AssetPlan planDelete(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& openScene) {
    AssetPlan r;
    const AssetEntry* e = index.find(asset);
    if (!e) {
        r.error = asset + " is not in the project any more";
        return r;
    }
    if (std::string why = refusal(index, refs, *e, openScene, true); !why.empty()) {
        r.error = why;
        return r;
    }
    // Into a fresh numbered folder of the project's .trash/, keeping the paths.
    const fs::path root = index.project();
    std::error_code ec;
    std::string bin;
    for (int n = 1;; ++n) {
        bin = std::string(kTrash) + "/" + std::to_string(n);
        if (!fs::exists(root / fromUtf8(bin), ec)) break;
    }
    r.trash = true;
    r.path = bin + "/" + asset;
    std::vector<std::string> set = deletionSet(index, asset);
    for (const std::string& p : set) {
        bool covered = false;  // inside a folder already going
        for (const auto& m : r.moves) covered = covered || inside(p, m.first);
        if (!covered && fs::exists(root / fromUtf8(p), ec)) r.moves.push_back({p, bin + "/" + p});
    }
    return r;
}

int emptyOldTrash(const fs::path& project, fs::file_time_type now, std::chrono::hours maxAge) {
    const fs::path trash = project / kTrash;
    std::error_code ec;
    if (project.empty() || !fs::is_directory(trash, ec)) return 0;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(trash, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        std::error_code tec;
        const fs::file_time_type t = fs::last_write_time(it->path(), tec);
        if (!tec && t + maxAge < now) old.push_back(it->path());
    }
    int removed = 0;
    for (const fs::path& p : old) {
        std::error_code rec;
        if (fs::remove_all(p, rec) != static_cast<std::uintmax_t>(-1) && !rec) ++removed;
    }
    if (fs::is_empty(trash, ec) && !ec) fs::remove(trash, ec);
    return removed;
}

namespace {

// applyAssetPlan, with the moved file's contents swapped as part of the step
// when `swap` is set.
AssetOp applyPlan(Document& doc, const AssetIndex& index, const std::string& openScene, const AssetPlan& plan,
                  std::optional<AssetFilesCommand::Swap> swap) {
    AssetOp r;
    r.path = plan.path;
    if (!plan.error.empty()) {
        r.error = plan.error;
        return r;
    }
    if (plan.moves.empty()) return r;
    const fs::path root = index.project();
    auto cmd = std::make_unique<AssetFilesCommand>();
    cmd->root = root;
    cmd->moves = plan.moves;
    cmd->swap = std::move(swap);
    // Every other scene file naming a moved file, rewritten. Worked out before
    // anything moves, so a scene that cannot be read stops it untouched.
    if (!plan.trash)
        for (const AssetEntry& e : index.entries()) {
            if (!refersToAssets(e.type) || e.path == normal(openScene)) continue;
            AssetFilesCommand::SceneFile sf;
            sf.path = movedPath(e.path, plan.moves);  // a scene inside a moved folder is rewritten where it lands
            const fs::path file = root / fromUtf8(e.path);
            if (!readFile(file, &sf.before)) {
                r.error = "could not read " + e.path;
                return r;
            }
            splash::Scene s;
            splash::ExtraKeys prefabExtra;
            try {
                s = loadReferrer(file, e.type, &prefabExtra);
            } catch (const std::exception&) {
                continue;  // the plan was refused when it names a moved file
            }
            const int n = updateReferences(s, plan.moves, nullptr);
            if (n == 0) continue;
            r.references += n;
            const fs::path tmp = file.parent_path() / fromUtf8("." + e.name + ".moving");
            try {
                saveReferrer(s, tmp, e.type, prefabExtra);
            } catch (const std::exception& ex) {
                r.error = "could not update " + e.path + ": " + ex.what();
                return r;
            }
            const bool read = readFile(tmp, &sf.after);
            std::error_code ec;
            fs::remove(tmp, ec);
            if (!read) {
                r.error = "could not update " + e.path;
                return r;
            }
            cmd->scenes.push_back(std::move(sf));
        }
    cmd->before = doc.scene();
    cmd->after = doc.scene();
    const int inOpen = updateReferences(cmd->after, plan.moves, &cmd->recheck);
    cmd->sceneChanged = inOpen > 0;
    r.references += inOpen;
    if (std::string why = moveAll(root, plan.moves); !why.empty()) {
        r.error = why;
        return r;
    }
    if (cmd->swap && !writeFile(root / fromUtf8(cmd->swap->path), cmd->swap->after)) {
        moveAll(root, reversed(plan.moves));
        r.error = "could not write " + cmd->swap->path;
        return r;
    }
    for (const AssetFilesCommand::SceneFile& s : cmd->scenes)
        if (!writeFile(root / fromUtf8(s.path), s.after)) {
            for (const AssetFilesCommand::SceneFile& w : cmd->scenes) {
                if (&w == &s) break;
                writeFile(root / fromUtf8(w.path), w.before);
            }
            if (cmd->swap) writeFile(root / fromUtf8(cmd->swap->path), cmd->swap->before);
            moveAll(root, reversed(plan.moves));
            r.error = "could not write " + s.path;
            return r;
        }
    for (const auto& [from, to] : plan.moves) {
        r.touched.push_back(from);
        r.touched.push_back(to);
    }
    for (const AssetFilesCommand::SceneFile& s : cmd->scenes) r.scenes.push_back(s.path);
    doc.execute(std::move(cmd));
    return r;
}

}  // namespace

AssetOp applyAssetPlan(Document& doc, const AssetIndex& index, const std::string& openScene, const AssetPlan& plan) {
    return applyPlan(doc, index, openScene, plan, std::nullopt);
}

AssetOp convertBlocksToLua(Document& doc, const AssetIndex& index, const std::string& openScene, const std::string& blocksPath) {
    AssetOp r;
    const std::string from = normal(blocksPath);
    const fs::path root = index.project();
    auto refuse = [&](const std::string& why) {
        r.error = "Can't convert: " + why;
        return r;
    };
    if (lower(fs::path(fromUtf8(from)).extension().string()) != ".blocks") return refuse(from + " is not a blocks script");
    const AssetEntry* e = index.find(from);
    if (!e) return refuse(from + " is not in the project any more");
    AssetFilesCommand::Swap swap;
    if (!readFile(root / fromUtf8(from), &swap.before)) return refuse("could not read " + from);
    try {
        splash::LuaGen gen = splash::blocksToLua(splash::parseBlocks(swap.before, from));
        if (!gen.errors.empty()) return refuse(gen.errors.front());
        swap.after = std::move(gen.lua);
    } catch (const std::exception& ex) {
        return refuse(ex.what());
    }
    // <stem>.lua, or <stem>_2.lua, ... when a script of that stem (in either
    // language, the source aside) is there.
    const std::string stem = utf8(fs::path(fromUtf8(e->name)).stem());
    std::error_code ec;
    auto taken = [&](const std::string& st) {
        return fs::exists(root / fromUtf8(join(e->folder, st + ".lua")), ec) ||
               (join(e->folder, st + ".blocks") != from && fs::exists(root / fromUtf8(join(e->folder, st + ".blocks")), ec));
    };
    std::string name = stem;
    for (int n = 2; taken(name); ++n) name = stem + "_" + std::to_string(n);
    AssetReferences refs;
    refs.update(index, doc.scene(), openScene, 0);
    const AssetPlan plan = planTo(index, refs, *e, join(e->folder, name + ".lua"), openScene);
    if (!plan.error.empty()) return refuse(plan.error);
    swap.path = plan.path;
    r = applyPlan(doc, index, openScene, plan, std::move(swap));
    if (!r.error.empty()) r.error = "Can't convert: " + r.error;
    return r;
}

std::string setAnimLoop(Document& doc, const std::string& animPath, bool loop) {
    const fs::path root = doc.projectRoot(), file = root / fromUtf8(animPath);
    AssetFilesCommand::Swap swap;
    swap.path = normal(animPath);
    if (!readFile(file, &swap.before)) return "could not read " + animPath;
    // Through loadAnim/saveAnim, so only the loop changes in what it says.
    const fs::path tmp = file.parent_path() / fromUtf8("." + utf8(file.filename()) + ".loop");
    try {
        splash::AnimClip c = splash::loadAnim(file);
        if (c.loop == loop) return {};
        c.loop = loop;
        splash::saveAnim(c, tmp);
    } catch (const std::exception& ex) {
        return std::string("could not change ") + animPath + ": " + ex.what();
    }
    const bool read = readFile(tmp, &swap.after);
    std::error_code ec;
    fs::remove(tmp, ec);
    if (!read) return "could not change " + animPath;
    if (!writeFile(file, swap.after)) return "could not write " + animPath;
    auto cmd = std::make_unique<AssetFilesCommand>();
    cmd->root = root;
    cmd->swap = std::move(swap);
    doc.execute(std::move(cmd));
    return {};
}

std::string takeAssetHistoryError() {
    std::string e;
    e.swap(g_historyError);
    return e;
}

std::vector<std::pair<fs::path, fs::path>> takeAssetMoves() {
    std::vector<std::pair<fs::path, fs::path>> m;
    m.swap(AssetFilesCommand::g_movedLog);
    return m;
}

std::string fileUrl(const fs::path& path) {
    std::string p = utf8(path);
    std::string out = "file://";
    if (!p.empty() && p[0] != '/') out += '/';  // C:/... on Windows
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string sizeLabel(uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof buf, "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
    else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

}  // namespace editor
