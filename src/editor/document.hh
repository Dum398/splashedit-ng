// The editor's open document: one scene loaded through splashcore, plus the
// editor-only state around it (selection, tree expansion, asset caches) and
// the undo history every edit goes through.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

// An object addressed by child indices from the scene's top level, so it
// stays valid when the object vectors reallocate.
using ObjectPath = std::vector<int>;

// What the editor knows about a referenced asset file.
enum class AssetStatus { Ok, Missing, Unreadable };

struct MeshInfo {
    AssetStatus status = AssetStatus::Missing;
    int triangles = 0;  // sum of submesh index counts / 3, valid when Ok
    std::string error;
};

struct TextureInfo {
    AssetStatus status = AssetStatus::Missing;
    int width = 0, height = 0;
    // Distinct colours after reduction to the PS1's 15-bit colour, counting
    // fully transparent texels once.
    int colors15 = 0;
    // VRAM taken by image + CLUT, in bytes, from splash::convertTexture.
    int vramBytes4 = 0, vramBytes8 = 0, vramBytes16 = 0;
    std::string error;
};

class Document;

// One undoable change. apply() does (and redoes) it, revert() undoes it.
class Command {
public:
    virtual ~Command() = default;
    virtual void apply(Document& doc) = 0;
    virtual void revert(Document& doc) = 0;
    // Absorbs `next`, which was just applied on top of this command, so the
    // pair undoes as one step. Returns false when they do not belong together.
    virtual bool merge(const Command& next) {
        (void)next;
        return false;
    }
};

class Document {
public:
    // Loads `scene` (relative to `project`). On failure the document holds an
    // empty scene, the error is returned and the paths are still recorded.
    std::optional<std::string> load(const std::filesystem::path& project, const std::filesystem::path& scene);
    // Replaces the scene without touching the disk (tests, new scenes). Clears history.
    void reset(splash::Scene scene, const std::filesystem::path& project = {}, const std::string& stem = "untitled");

    const splash::Scene& scene() const { return m_scene; }
    const std::filesystem::path& projectRoot() const { return m_project; }
    const std::string& sceneStem() const { return m_stem; }
    int objectCount() const { return m_objectCount; }
    int projectFileCount() const { return m_projectFiles; }
    // Bumped whenever the scene changes (load, edit, undo, redo), so the
    // viewport can tell when to rebuild its vertex buffer.
    unsigned revision() const { return m_revision; }
    // Bumped only by load() and reset(): a different scene, not an edit of this one.
    unsigned loadId() const { return m_loadId; }

    const splash::Object* object(const ObjectPath& path) const;
    // Parent of the object at `path`, or nullptr for a top-level object.
    const splash::Object* parent(const ObjectPath& path) const;

    const std::optional<ObjectPath>& selection() const { return m_selection; }
    const splash::Object* selected() const { return m_selection ? object(*m_selection) : nullptr; }
    void select(std::optional<ObjectPath> path) { m_selection = std::move(path); }
    // Selects the first object (depth-first) with this name. Returns false if none.
    bool selectByName(const std::string& name);
    // Path of every object in splash::flatten order (depth-first pre-order),
    // so a FlatObject index maps to the object's path.
    std::vector<ObjectPath> flatPaths() const;

    // Every node starts expanded; the empty path is the scene root row.
    bool expanded(const ObjectPath& path) const { return !m_collapsed.count(path); }
    void toggleExpanded(const ObjectPath& path);
    // Expands or collapses every object that has children (the root row stays as it is).
    void setAllExpanded(bool expanded);

    // The inspector's pinned object: shown instead of the selection while set.
    // Follows the object through inserts and removes; cleared when it goes.
    const std::optional<ObjectPath>& pinned() const { return m_pinned; }
    void pin(std::optional<ObjectPath> path) { m_pinned = std::move(path); }

    // Editing. Every change goes through the history: it bumps revision() and
    // marks the document dirty.
    //
    // Runs `fn` on a copy of the object at `path` (children excluded) and
    // applies the result as one command. Consecutive edits with the same
    // non-empty `mergeKey` on the same object merge into one undo step until
    // endMerge() is called; a drag passes a key and calls endMerge() on release.
    bool edit(const ObjectPath& path, const std::function<void(splash::Object&)>& fn, const std::string& mergeKey = {});
    // Runs `fn` on each object in `paths` as one undo step. Returns how many it changed.
    int editMany(const std::vector<ObjectPath>& paths, const std::function<void(splash::Object&)>& fn);
    // Puts each object, children included, in place of the one at its path,
    // as one undo step. The paths must not lie inside one another. A
    // selection or pin inside a replaced object that no longer resolves moves
    // to the object itself. Returns how many it replaced.
    int replaceTrees(std::vector<std::pair<ObjectPath, splash::Object>> trees);
    // Same for the scene's own settings (fog, render buffers, ...).
    void editSettings(const std::function<void(splash::SceneSettings&)>& fn, const std::string& mergeKey = {});
    // Closes the open merge, so the next edit starts a new undo step.
    void endMerge() { m_mergeOpen = false; }
    // Inserts `obj` so that it ends up at `path`, and selects it.
    bool insertObject(const ObjectPath& path, splash::Object obj);
    // Removes the object (and its children) at `path`.
    bool removeObject(const ObjectPath& path);
    // Inserts a copy right after the original, named the way Unity names
    // duplicates ("Crate (2)"), and selects it. Returns the copy's path.
    std::optional<ObjectPath> duplicateObject(const ObjectPath& path);
    // Clipboard text for the object at `path`, children included (see
    // splash::writeObjectsText); empty when there is no such object. Asset
    // paths stay project-relative, so a paste into another project points at
    // files that may be missing there.
    std::string copyObject(const ObjectPath& path) const;
    // copyObject, then removes the object as one undo step. Empty, with no
    // change, when there is no such object.
    std::string cutObject(const ObjectPath& path);
    // Inserts the objects in clipboard `text` right after `after` as its
    // siblings, or at the end of the top level when `after` is unset or gone,
    // as one undo step. A name already taken among the new siblings is
    // numbered the way duplicateObject numbers it. Selects the last pasted
    // object and returns its path; nullopt, with no change, for text that is
    // not ours. So a copy does not land on top of what it was copied from,
    // the pasted objects move together along X by `step`, as many steps as
    // it takes for none of them to sit where a new sibling copied from the
    // same object already sits (the same name but for its " (N)", the same in
    // all but name and position): pasting again lands one step further on.
    // Cut then paste finds the place free and stays where the object was; an
    // unrelated object at the same place does not count.
    std::optional<ObjectPath> pasteObjects(const std::string& text, const std::optional<ObjectPath>& after,
                                           float step = kPasteStep);
    // The snap grid's step (0.25 m), the one paste offsets copies by.
    static constexpr float kPasteStep = 0.25f;
    // Moves the object at `from`, with its children, to be child `index` of
    // `toParent` (empty = the scene root), keeping where it is in the world.
    // `index` counts the new parent's children as they are now, before the
    // move. Refused (nullopt) when `toParent` is the object itself or inside
    // it. Selects the object and returns its new path.
    std::optional<ObjectPath> moveObject(const ObjectPath& from, const ObjectPath& toParent, int index);
    // Applies `cmd` and pushes it, dropping anything that could be redone.
    // With `mergeable`, it may fold into the previous command (see Command::merge).
    void execute(std::unique_ptr<Command> cmd, bool mergeable = false);

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_history.size(); }
    bool undo();
    bool redo();
    size_t historySize() const { return m_history.size(); }
    // There are changes since the last load or save.
    bool dirty() const { return m_cursor != m_savedCursor; }
    // Writes the scene back to the file it was loaded from. Clears dirty on success.
    std::optional<std::string> save();
    // Saves under `scene` (relative to the project) and keeps editing that file.
    // On failure the document still points at the old file.
    std::optional<std::string> saveAs(const std::filesystem::path& scene);
    std::filesystem::path scenePath() const { return m_project / m_sceneFile; }

    // For commands: mutable access, and the bookkeeping around structural
    // changes. Everything else edits through the history.
    splash::Object* objectMut(const ObjectPath& path);
    splash::SceneSettings& settingsMut() { return m_scene.settings; }
    // The whole scene, for commands that rewrite paths across it (a project
    // file renamed or moved) and keep its structure.
    splash::Scene& sceneMut() { return m_scene; }
    std::vector<splash::Cutscene>& cutscenesMut() { return m_scene.cutscenes; }
    std::vector<splash::Animation>& animationsMut() { return m_scene.animations; }
    // Children of the object at `parentPath`, or the top level for the empty path.
    std::vector<splash::Object>* childrenMut(const ObjectPath& parentPath);
    // Keeps selection and tree expansion on the same objects when a sibling
    // is inserted at (delta = +1) or removed from (delta = -1) `path`.
    void shiftPaths(const ObjectPath& path, int delta);
    void recount();

    // Cached on load; nullptr when the path is empty.
    const MeshInfo* mesh(const std::string& projectPath) const;
    const TextureInfo* texture(const std::string& projectPath) const;
    // Re-reads these files after something rewrote them on disk (a model
    // import), and bumps revision() and assetsId() so views reload them.
    void refreshAssets(const std::vector<std::string>& projectPaths);
    // Bumped by refreshAssets: cached file contents may be out of date.
    unsigned assetsId() const { return m_assetsId; }
    // True if the lua file exists on disk.
    bool fileExists(const std::string& projectPath) const;

    // Something about this object needs attention: a missing or unreadable
    // mesh, texture or script, or a texture that would lose nothing at 4 bpp.
    bool hasWarning(const splash::Object& o) const;
    // The texture is stored deeper than its colours need: it has at most 16
    // distinct 15-bit colours but is set to 8 or 16 bpp.
    bool couldBe4bpp(const splash::MeshComponent& m) const;

    std::filesystem::path resolve(const std::string& projectPath) const { return m_project / projectPath; }

    // Every warning of the last import of `model` (absolute or project
    // relative), for the inspector; empty when it has none or was not
    // imported this session. Kept across scene loads in the same project.
    void setImportWarnings(const std::filesystem::path& model, std::vector<std::string> warnings);
    const std::vector<std::string>& importWarnings(const std::filesystem::path& model) const;

private:
    void cacheAssets(const splash::Object& o);
    void clearHistory();

    splash::Scene m_scene;
    std::filesystem::path m_project;
    std::filesystem::path m_sceneFile;
    std::string m_stem;
    int m_objectCount = 0;
    int m_projectFiles = 0;
    unsigned m_revision = 0;
    unsigned m_loadId = 0;
    unsigned m_assetsId = 0;
    std::optional<ObjectPath> m_selection;
    std::set<ObjectPath> m_collapsed;
    std::optional<ObjectPath> m_pinned;
    std::map<std::string, MeshInfo> m_meshes;
    std::map<std::string, TextureInfo> m_textures;
    std::map<std::string, bool> m_files;
    std::map<std::string, std::vector<std::string>> m_importWarnings;  // by project-relative model path

    std::vector<std::unique_ptr<Command>> m_history;
    size_t m_cursor = 0;       // commands [0, m_cursor) are applied
    size_t m_savedCursor = 0;  // m_cursor at the last load or save; kNever once unreachable
    bool m_mergeOpen = false;
    static constexpr size_t kNever = static_cast<size_t>(-1);
};

}  // namespace editor
