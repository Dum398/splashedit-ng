// The editor's open document: one scene loaded through splashcore, plus the
// editor-only state around it (selection, tree expansion, asset caches) and
// the undo history every edit goes through.
#pragma once

#include <cstddef>
#include <cstdint>
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
    int submeshes = 0;  // submesh count (one material each), valid when Ok
    bool hasColors = false;  // vertex colours cover every vertex, as the exporter requires
    int vertices = 0;        // vertex count, valid when Ok
    std::string error;
};

// An audio clip's format and length, for the inspector's size estimate.
struct AudioInfo {
    AssetStatus status = AssetStatus::Missing;
    int rate = 0, channels = 0;
    uint64_t frames = 0;
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
    int vramBytesAt(splash::BitDepth d) const {
        return d == splash::BitDepth::Bpp4 ? vramBytes4 : d == splash::BitDepth::Bpp8 ? vramBytes8 : vramBytes16;
    }
    // The CLUT's part of vramBytes4 and vramBytes8 (none at 16 bpp).
    int clutBytes4 = 0, clutBytes8 = 0;
    int clutBytesAt(splash::BitDepth d) const {
        return d == splash::BitDepth::Bpp4 ? clutBytes4 : d == splash::BitDepth::Bpp8 ? clutBytes8 : 0;
    }
    // Some texel is under half alpha: a mesh exports it see-through (cutout).
    bool holes = false;
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

    // The selection: selection() is the primary object (the last clicked:
    // the inspector's, the one most commands act on); moreSelected() holds
    // the other selected objects, in the order they were added (#263).
    // select() selects one object (or none) and drops the rest.
    const std::optional<ObjectPath>& selection() const { return m_selection; }
    const splash::Object* selected() const { return m_selection ? object(*m_selection) : nullptr; }
    void select(std::optional<ObjectPath> path) {
        m_selection = std::move(path);
        m_more.clear();
    }
    const std::vector<ObjectPath>& moreSelected() const { return m_more; }
    // Every selected object, the primary last; empty with nothing selected.
    std::vector<ObjectPath> selectedPaths() const;
    // How many objects are selected.
    size_t selectionCount() const { return m_selection ? 1 + m_more.size() : 0; }
    bool isSelected(const ObjectPath& path) const;
    // Selects `primary` and `more` (duplicates, the primary and paths that
    // do not resolve left out). Without a primary, nothing is selected.
    void selectMany(std::optional<ObjectPath> primary, const std::vector<ObjectPath>& more);
    // Ctrl+click: a selected object leaves the selection (when it was the
    // primary, the one selected before it takes over); any other joins it
    // as the new primary.
    void toggleSelected(const ObjectPath& path);
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

    // The UI canvas editor's selection: the canvas being edited and the
    // element selected in it (-1 = none). Kept apart from the object
    // selection; UI edits (ui_canvas.hh) restore it on undo.
    // `element` is the primary one (the last clicked: the paste anchor and the
    // inspector's); `more` holds the other selected elements of the canvas.
    struct UiSelection {
        int canvas = -1, element = -1;
        std::vector<int> more;
        bool operator==(const UiSelection&) const = default;
    };
    const UiSelection& uiSelection() const { return m_uiSelection; }
    void selectUi(UiSelection s) { m_uiSelection = s; }

    // Editing. Every change goes through the history: it bumps revision() and
    // marks the document dirty.
    //
    // Runs `fn` on a copy of the object at `path` (children excluded) and
    // applies the result as one command. Consecutive edits with the same
    // non-empty `mergeKey` on the same object merge into one undo step until
    // endMerge() is called; a drag passes a key and calls endMerge() on release.
    bool edit(const ObjectPath& path, const std::function<void(splash::Object&)>& fn, const std::string& mergeKey = {});
    // Runs `fn` on each object in `paths` as one undo step. Returns how many it changed.
    // With a `mergeKey`, consecutive calls on the same paths with the same key
    // merge into one step until endMerge(), as edit() does.
    int editMany(const std::vector<ObjectPath>& paths, const std::function<void(splash::Object&)>& fn,
                 const std::string& mergeKey = {});
    // The same, with `fn` told which object it is editing (a multi-object
    // gizmo drag gives each its own position).
    int editEach(const std::vector<ObjectPath>& paths, const std::function<void(const ObjectPath&, splash::Object&)>& fn,
                 const std::string& mergeKey = {});
    // Renames the object at `path` as one undo step. Portals name their rooms
    // by object name, so when the object is a Room and no other Room still
    // carries the old name, every Portal's roomA/roomB that held the old name
    // follows it. Returns -1 if the path is invalid or the name is empty or
    // unchanged, else how many portals were retargeted.
    int renameObject(const ObjectPath& path, const std::string& name);
    // Puts each object, children included, in place of the one at its path,
    // as one undo step. The paths must not lie inside one another. A
    // selection or pin inside a replaced object that no longer resolves moves
    // to the object itself. Returns how many it replaced.
    int replaceTrees(std::vector<std::pair<ObjectPath, splash::Object>> trees);
    // Same for the scene's own settings (fog, render buffers, ...).
    void editSettings(const std::function<void(splash::SceneSettings&)>& fn, const std::string& mergeKey = {});
    // Runs `fn` on a copy of the scene's sprite sheets and applies the result
    // as one undo step (none when nothing changed); merges like editSettings.
    // Returns whether anything changed.
    bool editSpriteSheets(const std::function<void(std::vector<splash::SpriteSheet>&)>& fn, const std::string& mergeKey = {});
    // The same for the scene's tilemap (nullopt: the scene has none).
    bool editTilemap(const std::function<void(std::optional<splash::Tilemap>&)>& fn, const std::string& mergeKey = {});
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
    // The same over several objects (a multi-selection, #263), each as one
    // undo step. An object inside another of `paths` goes with it and is
    // left out itself; objects the single-object command refuses are
    // skipped. Removing restores the selection on undo; duplicating selects
    // the copies (the primary's copy primary). copyObjects writes them in
    // tree order; cutObjects copies the ones it can remove, then removes them.
    // Each returns what removeObject etc. return for the whole set: how many
    // were removed, the copies' paths, the text.
    // topLevel(): `paths` without the ones inside another, in tree order.
    std::vector<ObjectPath> topLevel(const std::vector<ObjectPath>& paths) const;
    int removeObjects(const std::vector<ObjectPath>& paths);
    std::vector<ObjectPath> duplicateObjects(const std::vector<ObjectPath>& paths);
    std::string copyObjects(const std::vector<ObjectPath>& paths) const;
    std::string cutObjects(const std::vector<ObjectPath>& paths);
    // Inserts the objects in clipboard `text` right after `after` as its
    // siblings, or at the end of the top level when `after` is unset or gone,
    // as one undo step. A name already taken among the new siblings is
    // numbered the way duplicateObject numbers it. Selects the last pasted
    // object (with several pasted, all of them, the last primary, and undo
    // restores the selection before) and returns its path; nullopt, with no change, for text that is
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
    // The same over several objects (a dragged multi-selection, #263), as
    // one undo step: topLevel(from) (an object inside another of them goes
    // with it) end up side by side, in tree order, as children `index`.. of
    // `toParent`, `index` counting its children as they are now. Each keeps
    // where it is in the world. All or none: refused (nullopt, no change)
    // when `toParent` is one of them or inside one, or when moveObject would
    // refuse any of them. The objects that were selected stay selected (the
    // primary primary) at their new paths, and undo restores the selection.
    // Returns the moved objects' new paths in tree order; when they are
    // already there, the paths as they are, with no change and no undo step.
    std::optional<std::vector<ObjectPath>> moveObjects(const std::vector<ObjectPath>& from, const ObjectPath& toParent,
                                                       int index);
    // Prefabs. An instance (Object::prefab set) shows its prefab's content,
    // which is read-only here: its children cannot be edited, renamed,
    // moved, deleted or given children, and on the instance itself only the
    // name, the active flags and the transform change. Edits that would touch
    // the content are refused (return false / nullopt / 0).
    //
    // The object at `path` is content of a prefab instance (some object
    // above it is an instance).
    bool inInstance(const ObjectPath& path) const;
    // The outermost instance at or above `path`; nullopt when there is none.
    // What a click in the viewport selects.
    std::optional<ObjectPath> instanceRoot(const ObjectPath& path) const;
    // Fills every instance in from its prefab file again (see
    // splash::resolvePrefabs), outside the history; load(), undo, redo and
    // leaving a prefab do it. Returns the instances it could not fill.
    std::vector<std::string> resolvePrefabs();
    // The messages of the last resolvePrefabs().
    const std::vector<std::string>& prefabErrors() const { return m_prefabErrors; }
    // Terrains whose data file the last resolvePrefabs() could not read
    // (it also fills terrain grids in, splash::loadTerrains).
    const std::vector<std::string>& terrainErrors() const { return m_terrainErrors; }
    // Writes the object at `path` with its children to prefabs/<name>.prefab
    // (a new file: "<name> (2).prefab" when that one exists) and turns it
    // into an instance of it, as one undo step. Returns the prefab's project
    // path, or empty with `error` set.
    std::string createPrefab(const ObjectPath& path, std::string* error);
    // Places an instance of `prefab` (project-relative) so that it ends up at
    // `at`, named after the file and unique among its siblings, at `position`,
    // and selects it. One undo step. False, with `error` set, when the file
    // does not load or the prefab would contain itself.
    bool insertPrefab(const ObjectPath& at, const std::string& prefab, splash::Vec3 position, std::string* error);
    // Turns the instance at `path` into plain objects holding what it shows,
    // as one undo step.
    bool unpackPrefab(const ObjectPath& path);
    // Prefab mode: the document edits the prefab file `prefab` (project
    // relative) as a scene whose only top-level object is the prefab's root;
    // save() writes the .prefab. The scene, with its selection and history,
    // waits until closePrefab(). Opening one from prefab mode goes one deeper.
    std::optional<std::string> openPrefab(const std::string& prefab);
    // Back to what was open before openPrefab (unsaved prefab edits are
    // dropped: save first), every instance filled in again from its file.
    // False when not in prefab mode.
    bool closePrefab();
    bool inPrefabMode() const { return m_outer != nullptr; }
    // How many prefabs deep the document is (0 = the scene).
    int prefabDepth() const { return m_outer ? 1 + m_outer->prefabDepth() : 0; }
    // The prefab being edited ('/' separators); empty outside prefab mode.
    const std::string& prefabFile() const { return m_prefabFile; }
    // The scene (or prefab) that closePrefab() goes back to.
    std::string outerName() const { return m_outer ? m_outer->m_stem : std::string(); }

    // Applies `cmd` and pushes it, dropping anything that could be redone.
    // With `mergeable`, it may fold into the previous command (see Command::merge).
    void execute(std::unique_ptr<Command> cmd, bool mergeable = false);
    // Every command executed between beginBatch() and endBatch() is applied
    // as it comes and undoes as one step (none when nothing ran). Not nested.
    void beginBatch();
    void endBatch();

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
    std::vector<splash::UICanvas>& canvasesMut() { return m_scene.canvases; }
    // Children of the object at `parentPath`, or the top level for the empty path.
    std::vector<splash::Object>* childrenMut(const ObjectPath& parentPath);
    // Keeps selection and tree expansion on the same objects when a sibling
    // is inserted at (delta = +1) or removed from (delta = -1) `path`.
    void shiftPaths(const ObjectPath& path, int delta);
    void recount();

    // Cached on load; nullptr when the path is empty.
    const MeshInfo* mesh(const std::string& projectPath) const;
    const TextureInfo* texture(const std::string& projectPath) const;
    const AudioInfo* audio(const std::string& projectPath) const;
    // Re-reads these files after something rewrote them on disk (a model
    // import), and bumps revision() and assetsId() so views reload them.
    void refreshAssets(const std::vector<std::string>& projectPaths);
    // Bumped by refreshAssets: cached file contents may be out of date.
    unsigned assetsId() const { return m_assetsId; }
    // True if the lua file exists on disk.
    bool fileExists(const std::string& projectPath) const;

    // Something about this object needs attention: a missing or unreadable
    // mesh, texture or script, a texture that would lose nothing at 4 bpp,
    // Mesh colours on a mesh without vertex colours, or Painted with a
    // painted layer that does not fit the mesh.
    bool hasWarning(const splash::Object& o) const;
    // The textures are stored deeper than their colours need: the mesh is set
    // to 8 or 16 bpp, every textured material's texture reads and has at most
    // 16 distinct 15-bit colours, and together they take less VRAM at 4 bpp.
    bool couldBe4bpp(const splash::MeshComponent& m) const;
    // VRAM the mesh's readable textures take at their depths (all at `depth`
    // when given), image + CLUT, placed the way the export packs them: each
    // texture once per depth, and one CLUT per palette group (group, depth,
    // cutout) rather than one per member.
    int meshTextureBytes(const splash::MeshComponent& m, std::optional<splash::BitDepth> depth = std::nullopt) const;
    // Set to Mesh colours, but the mesh has none, so it exports flat grey.
    bool missingMeshColors(const splash::MeshComponent& m) const;
    // Set to Painted, but the painted layer has not one colour per vertex of
    // the mesh (none, or the model changed since), so it exports baked lighting.
    bool paintMismatch(const splash::MeshComponent& m) const;

    std::filesystem::path resolve(const std::string& projectPath) const { return m_project / projectPath; }

    // Every warning of the last import of `model` (absolute or project
    // relative), for the inspector; empty when it has none or was not
    // imported this session. Kept across scene loads in the same project.
    void setImportWarnings(const std::filesystem::path& model, std::vector<std::string> warnings);
    const std::vector<std::string>& importWarnings(const std::filesystem::path& model) const;

private:
    void cacheAssets(const splash::Object& o);
    // A copy of objects about to go into the scene (duplicate, paste, unpack):
    // each terrain in it whose data file another terrain of the scene uses
    // gets a fresh unique file, written now with the grid it was copied from
    // (the scene's in-memory grid for that file, else the file). One whose
    // file no other terrain uses keeps it (cut then paste). Prefab instance
    // content is left alone: it is the prefab's and read-only.
    void giveTerrainsOwnFiles(splash::Object& o, std::vector<std::string>& taken) const;
    void clearHistory();
    // Prefab mode: the root is the only top-level object.
    bool prefabRootLevel(const ObjectPath& parentPath) const { return m_outer && parentPath.empty(); }
    // resolvePrefabs() when the scene has an instance (after undo and redo,
    // whose snapshots may hold older prefab content).
    void refill();

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
    std::vector<ObjectPath> m_more;  // the rest of the selection (#263)
    std::set<ObjectPath> m_collapsed;
    std::optional<ObjectPath> m_pinned;
    UiSelection m_uiSelection;
    std::map<std::string, MeshInfo> m_meshes;
    std::map<std::string, TextureInfo> m_textures;
    std::map<std::string, AudioInfo> m_audio;
    std::map<std::string, bool> m_files;
    std::map<std::string, std::vector<std::string>> m_importWarnings;  // by project-relative model path

    std::vector<std::unique_ptr<Command>> m_history;
    size_t m_cursor = 0;       // commands [0, m_cursor) are applied
    size_t m_savedCursor = 0;  // m_cursor at the last load or save; kNever once unreachable
    bool m_mergeOpen = false;
    std::optional<std::vector<std::unique_ptr<Command>>> m_batch;  // open batch (beginBatch)
    // Pushes `cmd`, already applied, onto the history.
    void record(std::unique_ptr<Command> cmd, bool mergeable);
    // At the front of a batch: undo puts the current selection back; at its
    // end: do and redo select `primary` and `more`.
    void batchRestoreSelection();
    void batchSetSelection(std::optional<ObjectPath> primary, std::vector<ObjectPath> more);
    static constexpr size_t kNever = static_cast<size_t>(-1);

    std::vector<std::string> m_prefabErrors;
    std::vector<std::string> m_terrainErrors;
    // Prefab mode: the document to go back to, the prefab file and its unknown keys.
    std::unique_ptr<Document> m_outer;
    std::string m_prefabFile;
    splash::ExtraKeys m_prefabExtra;
};

}  // namespace editor
