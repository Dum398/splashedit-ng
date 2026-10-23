#include "editor/skin_clips.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>

#include "editor/project.hh"

namespace fs = std::filesystem;

namespace editor {

namespace {

std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string genericUtf8(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// As the importer compares clip names: ASCII letters in any case.
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readBytes(const fs::path& file, std::string* out) {
    FILE* f = std::fopen(utf8(file).c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out->append(buf, n);
    std::fclose(f);
    return true;
}

// Writes `text` over `file` through a hidden temporary beside it.
bool writeBytes(const fs::path& file, const std::string& text) {
    const fs::path tmp = file.parent_path() / fromUtf8("." + utf8(file.filename()) + ".writing");
    FILE* f = std::fopen(utf8(tmp).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) return false;
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

// Files rewritten together, already written when it is executed: undo puts
// each one's bytes back (removing one that was not there), redo the new ones.
class FileSwapsCommand : public Command {
  public:
    struct File {
        fs::path path;
        std::optional<std::string> before, after;
    };
    std::vector<File> files;
    std::vector<std::string> recheck;  // project paths the document looks up again

    void apply(Document& doc) override {
        if (!m_first)
            for (const File& f : files) put(f.path, f.after);
        m_first = false;
        doc.refreshAssets(recheck);
    }
    void revert(Document& doc) override {
        for (auto it = files.rbegin(); it != files.rend(); ++it) put(it->path, it->before);
        doc.refreshAssets(recheck);
    }

  private:
    static void put(const fs::path& path, const std::optional<std::string>& bytes) {
        std::error_code ec;
        if (bytes) writeBytes(path, *bytes);
        else fs::remove(path, ec);
    }
    bool m_first = true;
};

// The clip's name as its .anim has it, empty when it does not read.
std::string clipName(const fs::path& file) {
    try {
        return splash::loadAnim(file).name;
    } catch (const std::exception&) {
        return {};
    }
}

// The model a skinned mesh came from, its settings and its skeleton.
struct Host {
    fs::path model;  // absolute
    splash::ImportSettings settings;
    splash::ClipTarget target;  // joints and model; folder and taken are per source
    std::string error;
};

Host hostOf(const fs::path& project, const std::string& mesh) {
    Host h;
    if (mesh.empty()) {
        h.error = "the object draws no mesh";
        return h;
    }
    h.model = modelOfMesh(project, mesh);
    if (h.model.empty()) {
        h.error = mesh + " was not imported from a model file, so there are no import settings to convert clips with";
        return h;
    }
    h.target.model = splash::importName(h.model);
    try {
        h.settings = splash::loadImportSettings(h.model);
        const splash::Mesh m = splash::loadMesh(project / fromUtf8(mesh));
        if (!m.skin) h.error = mesh + " has no skeleton";
        else
            for (const splash::Joint& j : m.skin->joints) h.target.joints.push_back(j.name);
    } catch (const std::exception& e) {
        h.error = e.what();
    }
    return h;
}

// What to read for the model file `copy` in the project: the file its
// sidecar says it was copied from while that is there (glTF buffers and
// textures beside it are found), else the copy.
fs::path readPath(const fs::path& copy) {
    std::error_code ec;
    try {
        const splash::ImportSettings s = splash::loadImportSettings(copy);
        if (!s.source.empty() && fs::is_regular_file(fromUtf8(s.source), ec)) return fromUtf8(s.source);
    } catch (const std::exception&) {
    }
    return copy;
}

// Imports the clips of `source` (project-relative) for `skin` on `h`'s
// skeleton and puts them in the skin's list where that source's clips were
// (at the end when it had none), the list cut at 16 (`left` counts what fell
// off). Throws std::runtime_error, leaving `skin` as it was.
splash::ImportedClips importSource(const fs::path& project, const Host& h, splash::SkinComponent& skin, const std::string& source,
                                   int& left) {
    splash::ClipTarget t = h.target;
    t.folder = splash::clipFolder(fromUtf8(source));
    const std::string prefix = "models/" + t.model + "/" + t.folder + "/";
    for (const std::string& c : skin.clips)
        if (c.rfind(prefix, 0) != 0)
            if (std::string n = clipName(project / fromUtf8(c)); !n.empty()) t.taken.insert(lower(n));
    splash::ImportedClips got = splash::importClips(readPath(project / fromUtf8(source)), project, h.settings, t);
    std::vector<std::string> clips;
    bool placed = false;
    for (const std::string& c : skin.clips) {
        if (c.rfind(prefix, 0) != 0) {
            clips.push_back(c);
        } else if (!placed) {
            clips.insert(clips.end(), got.clips.begin(), got.clips.end());
            placed = true;
        }
    }
    if (!placed) clips.insert(clips.end(), got.clips.begin(), got.clips.end());
    if (clips.size() > 16) {
        left += int(clips.size() - 16);
        clips.resize(16);
    }
    skin.clips = std::move(clips);
    // A new source is kept only when one of its clips fitted.
    const bool fitted = std::any_of(got.clips.begin(), got.clips.end(), [&](const std::string& c) {
        return std::find(skin.clips.begin(), skin.clips.end(), c) != skin.clips.end();
    });
    if (fitted && std::find(skin.clipSources.begin(), skin.clipSources.end(), source) == skin.clipSources.end())
        skin.clipSources.push_back(source);
    return got;
}

std::string withFile(const std::string& file, const std::string& message) {
    return message.rfind(file, 0) == 0 ? message : file + ": " + message;
}

// UTF-16 code units, as the export counts a clip name.
size_t utf16Units(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xc0) == 0x80) continue;  // a continuation byte
        n += c >= 0xf0 ? 2 : 1;
    }
    return n;
}

}  // namespace

AddedClips addClipSources(Document& doc, const ObjectPath& object, const std::vector<fs::path>& files) {
    AddedClips r;
    const splash::Object* o = doc.object(object);
    if (!o || !o->skin) {
        r.error = "the object has no Skinned animation component";
        return r;
    }
    const fs::path project = doc.projectRoot();
    const Host h = hostOf(project, o->mesh ? o->mesh->mesh : std::string());
    if (!h.error.empty()) {
        r.error = h.error;
        return r;
    }
    splash::SkinComponent skin = *o->skin;
    std::vector<std::string> added;
    for (const fs::path& file : files) {
        const std::string name = utf8(file.filename());
        if (!splash::canImportModel(file)) {
            r.refused.push_back(name + ": not a model file (.glb, .gltf, .fbx, .obj or .blend)");
            continue;
        }
        const AdoptedAsset a = adoptAsset(project, file, "models");
        if (!a.error.empty()) {
            r.refused.push_back(name + ": " + a.error);
            continue;
        }
        const fs::path copy = project / fromUtf8(a.path);
        std::error_code ec;
        if (fs::equivalent(copy, h.model, ec)) {
            r.refused.push_back(name + ": it is the skin's own model, whose clips the skin has already");
            continue;
        }
        // Where it came from, so a change to that file is noticed (ModelWatcher).
        const fs::path side = splash::importSettingsPath(copy);
        const bool newSidecar = a.copied && !fs::exists(side, ec);
        if (newSidecar) {
            splash::ImportSettings s;
            s.source = utf8(fs::absolute(file, ec));
            try {
                splash::saveImportSettings(copy, s);
            } catch (const std::exception&) {
            }
        }
        try {
            splash::SkinComponent next = skin;
            int left = 0;
            splash::ImportedClips got = importSource(project, h, next, a.path, left);
            skin = std::move(next);
            r.left += left;
            for (const std::string& w : got.warnings) r.warnings.push_back(name + ": " + w);
            for (const std::string& c : got.clips)
                if (std::find(skin.clips.begin(), skin.clips.end(), c) != skin.clips.end()) added.push_back(c);
        } catch (const std::exception& e) {
            r.refused.push_back(withFile(name, e.what()));
            // A refused file is not kept in the project.
            if (a.copied) fs::remove(copy, ec);
            if (newSidecar) fs::remove(side, ec);
        }
    }
    r.clips = added;
    if (skin.clips != o->skin->clips || skin.clipSources != o->skin->clipSources) {
        doc.edit(object, [&](splash::Object& ob) {
            if (!ob.skin) return;
            ob.skin->clips = skin.clips;
            ob.skin->clipSources = skin.clipSources;
        });
        doc.endMerge();
    }
    doc.refreshAssets(added);
    return r;
}

bool isClipSource(const Document& doc, const fs::path& model) {
    const fs::path abs = model.is_absolute() ? model : doc.projectRoot() / model;
    const std::string rel = genericUtf8(abs.lexically_relative(doc.projectRoot()));
    for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
        if (fo.object->skin) {
            const auto& s = fo.object->skin->clipSources;
            if (std::find(s.begin(), s.end(), rel) != s.end()) return true;
        }
    return false;
}

int refreshClipSources(Document& doc, const fs::path& model, std::vector<std::string>* problems) {
    const fs::path project = doc.projectRoot();
    const fs::path abs = model.is_absolute() ? model : project / model;
    const std::string rel = genericUtf8(abs.lexically_relative(project));
    std::vector<std::string> written;
    std::function<bool(splash::Object&)> walk = [&](splash::Object& o) {
        bool changed = false;
        if (o.skin && std::find(o.skin->clipSources.begin(), o.skin->clipSources.end(), rel) != o.skin->clipSources.end()) {
            const Host h = hostOf(project, o.mesh ? o.mesh->mesh : std::string());
            if (!h.error.empty()) {
                if (problems) problems->push_back(o.name + ": " + h.error);
            } else {
                splash::SkinComponent next = *o.skin;
                int left = 0;
                try {
                    splash::ImportedClips got = importSource(project, h, next, rel, left);
                    written.insert(written.end(), got.clips.begin(), got.clips.end());
                    if (next.clips != o.skin->clips) {
                        o.skin = std::move(next);
                        changed = true;
                    }
                } catch (const std::exception& e) {
                    if (problems) problems->push_back(withFile(utf8(abs.filename()), e.what()));
                }
            }
        }
        for (splash::Object& c : o.children) changed |= walk(c);
        return changed;
    };
    std::vector<std::pair<ObjectPath, splash::Object>> trees;
    for (size_t i = 0; i < doc.scene().objects.size(); ++i) {
        splash::Object copy = doc.scene().objects[i];
        if (walk(copy)) trees.push_back({ObjectPath{int(i)}, std::move(copy)});
    }
    const int n = trees.empty() ? 0 : doc.replaceTrees(std::move(trees));
    doc.refreshAssets(written);
    return n;
}

bool refreshSkinSources(const fs::path& project, const std::string& mesh, splash::SkinComponent& skin, std::vector<std::string>* problems) {
    if (skin.clipSources.empty()) return false;
    const Host h = hostOf(project, mesh);
    if (!h.error.empty()) {
        if (problems) problems->push_back(h.error);
        return false;
    }
    const std::vector<std::string> was = skin.clips;
    int left = 0;
    for (const std::string& source : std::vector<std::string>(skin.clipSources)) {
        try {
            importSource(project, h, skin, source, left);
        } catch (const std::exception& e) {
            if (problems) problems->push_back(withFile(utf8(fromUtf8(source).filename()), e.what()));
        }
    }
    return skin.clips != was;
}

bool pruneClipSources(const fs::path& project, const std::string& mesh, splash::SkinComponent& skin) {
    const fs::path model = mesh.empty() ? fs::path() : modelOfMesh(project, mesh);
    if (model.empty() || skin.clipSources.empty()) return false;
    const std::string base = "models/" + splash::importName(model) + "/";
    const size_t was = skin.clipSources.size();
    std::erase_if(skin.clipSources, [&](const std::string& source) {
        const std::string prefix = base + splash::clipFolder(fromUtf8(source)) + "/";
        return std::none_of(skin.clips.begin(), skin.clips.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; });
    });
    return skin.clipSources.size() != was;
}

std::string renameSkinClip(Document& doc, const ObjectPath& object, size_t index, const std::string& typed) {
    const splash::Object* o = doc.object(object);
    if (!o || !o->skin || index >= o->skin->clips.size()) return "there is no such clip";
    std::string name = typed;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    if (name.empty()) return "a clip needs a name, which SkinnedAnim.Play takes";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return "a clip name cannot hold control characters";
    if (utf16Units(name) > 24) return "'" + name + "' is longer than 24 characters, the most psxsplash keeps";
    const fs::path project = doc.projectRoot();
    const std::vector<std::string>& clips = o->skin->clips;
    for (size_t j = 0; j < clips.size(); ++j) {
        if (j == index) continue;
        const std::string other = clipName(project / fromUtf8(clips[j]));
        if (!other.empty() && lower(other) == lower(name)) return "another clip of this skin is named '" + other + "'";
    }
    const std::string path = clips[index];
    const fs::path file = project / fromUtf8(path);
    FileSwapsCommand::File anim{file, std::nullopt, std::nullopt};
    std::string bytes;
    if (!readBytes(file, &bytes)) return "could not read " + path;
    anim.before = bytes;
    splash::AnimClip clip;
    try {
        clip = splash::loadAnim(file);
    } catch (const std::exception& e) {
        return std::string("could not read ") + path + ": " + e.what();
    }
    if (clip.name == name) return {};
    clip.name = name;
    const fs::path tmp = file.parent_path() / fromUtf8("." + utf8(file.filename()) + ".rename");
    std::error_code ec;
    try {
        splash::saveAnim(clip, tmp);
    } catch (const std::exception& e) {
        return std::string("could not change ") + path + ": " + e.what();
    }
    const bool made = readBytes(tmp, &bytes);
    fs::remove(tmp, ec);
    if (!made) return "could not change " + path;
    anim.after = bytes;

    // A clip its model's import wrote, its own (models/<model>/<clip>.anim)
    // or from a clip source (models/<model>/clips/<file>/<clip>.anim): the
    // name goes into the model's sidecar, which the import names it from.
    std::optional<FileSwapsCommand::File> sidecar;
    const fs::path model = o->mesh ? modelOfMesh(project, o->mesh->mesh) : fs::path();
    const std::string prefix = model.empty() ? std::string() : "models/" + splash::importName(model) + "/";
    if (!prefix.empty() && path.rfind(prefix, 0) == 0 && path.size() > prefix.size() + 5 && path.ends_with(".anim")) {
        const std::string key = path.substr(prefix.size(), path.size() - prefix.size() - 5);
        const size_t slash = key.find('/');
        const bool fromSource = key.rfind("clips/", 0) == 0 && key.find('/', 6) != std::string::npos &&
                                key.find('/', key.find('/', 6) + 1) == std::string::npos;
        if (slash == std::string::npos || fromSource) {
            sidecar = FileSwapsCommand::File{splash::importSettingsPath(model), std::nullopt, std::nullopt};
            if (readBytes(sidecar->path, &bytes)) sidecar->before = bytes;
            try {
                splash::ImportSettings s = splash::loadImportSettings(model);
                s.clipNames[key] = name;
                splash::saveImportSettings(model, s);
            } catch (const std::exception& e) {
                if (sidecar->before) writeBytes(sidecar->path, *sidecar->before);
                return std::string("could not keep the name in ") + utf8(sidecar->path.filename()) + ": " + e.what();
            }
            if (!readBytes(sidecar->path, &bytes)) return "could not read " + utf8(sidecar->path.filename()) + " back";
            sidecar->after = bytes;
        }
    }
    if (!writeBytes(file, *anim.after)) {
        if (sidecar) {
            if (sidecar->before) writeBytes(sidecar->path, *sidecar->before);
            else fs::remove(sidecar->path, ec);
        }
        return "could not write " + path;
    }
    auto cmd = std::make_unique<FileSwapsCommand>();
    cmd->files.push_back(std::move(anim));
    if (sidecar) cmd->files.push_back(std::move(*sidecar));
    cmd->recheck = {path};
    doc.execute(std::move(cmd));
    return {};
}

}  // namespace editor
