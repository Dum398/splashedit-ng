// The Project panel's model: the files of a project folder, each classified
// by what the editor does with it, with search and type filters, the scene
// references that point at each one, and the file operations (rename, delete)
// that carry those references along. No SDL, no ImGui; the panel lives in
// asset_browser_view.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "scene.hh"

namespace editor {

enum class AssetType { Folder, Model, Mesh, Texture, Script, Audio, Animation, Scene, Prefab, Font, Other };
constexpr int kAssetTypes = 11;

// "Model", "Texture", ...; plural for the type filter ("Models").
const char* assetTypeName(AssetType t, bool plural = false);
// By extension (case-insensitive). Directories are Folder.
AssetType classifyAsset(const std::filesystem::path& file, bool directory = false);

// A file or folder in the project, by its project-relative '/'-separated path.
struct AssetEntry {
    std::string path;    // "textures/brick.png"
    std::string name;    // "brick.png"
    std::string folder;  // "textures"; "" for the project root
    AssetType type = AssetType::Other;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    // For a mesh, texture or clip an imported model wrote (models/<stem>.mesh,
    // models/<stem>_<n>.png, models/<stem>/...): that model's path. Empty otherwise.
    std::string madeBy;
};

// Where an asset is used: one field of one object (or the scene's settings)
// in one scene file.
struct AssetRef {
    std::string scene;   // the scene file, project-relative ("courtyard.scene")
    std::string object;  // the object's path of names ("Crates/Crate (2)"); empty for the scene itself
    std::string field;   // "Texture", "Mesh", "Script", "Trigger script", "Audio clip", "Skin clip", "Scene script",
                         // "UI font", "UI image", "Sprite sheet", "Memory card icon", "Prefab"
    std::string asset;   // the path the field holds
    std::vector<int> path;  // the object's ObjectPath in that scene; empty for the scene itself
};

// Every asset path a scene refers to, with where.
std::vector<AssetRef> sceneReferences(const splash::Scene& scene, const std::string& sceneFile);

// The listing of one project folder. Hidden files and folders (a leading
// '.') and model import sidecars (*.import.json) are left out.
class AssetIndex {
  public:
    // Walks `project` again. Returns true when the listing differs from the
    // last scan (files or folders added, removed, resized or touched), and
    // bumps generation(). A different project always counts as a change.
    bool scan(const std::filesystem::path& project);
    const std::filesystem::path& project() const { return m_project; }
    unsigned generation() const { return m_generation; }
    // Folders first, then files, each by name (case-insensitive), grouped by folder.
    const std::vector<AssetEntry>& entries() const { return m_entries; }
    const AssetEntry* find(const std::string& path) const;
    // Every folder ("" first, then by path).
    std::vector<std::string> folders() const;
    // The entries directly inside `folder`.
    std::vector<const AssetEntry*> children(const std::string& folder) const;

  private:
    std::filesystem::path m_project;
    std::vector<AssetEntry> m_entries;
    std::map<std::string, size_t> m_byPath;
    unsigned m_generation = 0;
};

// What the panel shows: the entries in `folder` (or under it, at any depth,
// while `query` is set: a search covers the whole folder tree) whose name
// holds every space-separated word of `query` (case-insensitive) and whose
// type is in `types` (bit 1 << int(AssetType); 0 = every type). Folders are
// listed only when no search or filter is active.
struct AssetFilter {
    std::string folder;
    std::string query;
    uint32_t types = 0;
};
std::vector<const AssetEntry*> filterAssets(const AssetIndex& index, const AssetFilter& filter);

// The references to every asset of a project: those in the open document's
// scene (from memory, so unsaved edits count) and those in every other .scene
// file in the project (read from disk, cached by modification time).
class AssetReferences {
  public:
    // Rebuilds from `open` (saved as `openFile`, project-relative) and the
    // scene files the index lists. Cheap when nothing changed.
    void update(const AssetIndex& index, const splash::Scene& open, const std::string& openFile, unsigned openRevision);
    // The references to `asset`. A model counts the references to the files
    // its import wrote; a folder those to anything inside it.
    std::vector<AssetRef> to(const AssetIndex& index, const std::string& asset) const;
    // The scene files that could not be read, with the error.
    const std::map<std::string, std::string>& unreadable() const { return m_unreadable; }

  private:
    struct Cached {
        std::filesystem::file_time_type mtime;
        std::vector<AssetRef> refs;
    };
    std::map<std::string, Cached> m_other;
    std::map<std::string, std::string> m_unreadable;
    std::vector<AssetRef> m_open;
    std::string m_openFile;
    unsigned m_openRevision = ~0u;
    std::multimap<std::string, const AssetRef*> m_byAsset;
    unsigned m_indexGeneration = ~0u;
};

// The files an imported model in models/ wrote, project-relative: its
// models/<stem>.mesh, models/<stem>.png or models/<stem>_<n>.png, and
// models/<stem>/ with what is inside, as far as they exist.
std::vector<std::string> modelOutputs(const AssetIndex& index, const std::string& model);

// The asset path `p` ends up at after `moves` ((from, to) project-relative
// file or folder paths); `p` itself when no move covers it.
std::string movedPath(const std::string& p, const std::vector<std::pair<std::string, std::string>>& moves);
// Points every asset path field of `scene` (the ones sceneReferences lists)
// that `moves` covers at the new path. Returns how many changed; `changed`
// gets each old and new path.
int updateReferences(splash::Scene& scene, const std::vector<std::pair<std::string, std::string>>& moves,
                     std::vector<std::string>* changed = nullptr);

// A rename, move or delete of project files, worked out before anything is
// touched: the file moves it makes, or why it is refused.
struct AssetPlan {
    std::string error;  // empty when it can go ahead
    std::string path;   // the asset's new path (a delete: where it lies in the trash)
    std::vector<std::pair<std::string, std::string>> moves;  // (from, to), project-relative
    bool trash = false;                                       // a delete into the project's .trash/
};

// Renames `asset` to `newName` (a file name in the same folder). A file keeps
// its extension: a name typed without it gets it, one with another is refused.
// References follow (see applyAssetPlan). Refused when the name is taken or
// holds a path separator, when it is one of a model's outputs (the import
// writes it again: rename the model), when it is or holds `openScene` (the
// document keeps that file's name: Save As), or when a scene that cannot be
// read names it (its reference could not be updated). A model takes its
// sidecar and outputs along.
AssetPlan planRename(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& newName,
                     const std::string& openScene);
// Moves `asset` into `folder` (a project folder, "" for the root), keeping
// its name; the rules of planRename. A model whose import wrote outputs
// stays in models/, where imports write them.
AssetPlan planMove(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& folder,
                   const std::string& openScene);
// What deleting `asset` would remove: the asset, and for a model its sidecar
// and outputs, for a folder everything inside. Project-relative paths.
std::vector<std::string> deletionSet(const AssetIndex& index, const std::string& asset);
// Deletes `asset` and its deletionSet into .trash/<n>/ in the project (hidden
// from the panel), keeping their paths. Refused while anything references it
// (nothing could stand in for it), or when it is or holds `openScene`.
AssetPlan planDelete(const AssetIndex& index, const AssetReferences& refs, const std::string& asset, const std::string& openScene);
// How long a delete stays in .trash/ (see emptyOldTrash): 30 days.
constexpr std::chrono::hours kTrashKeep{24 * 30};

// Empties the old part of the project's .trash/: removes each numbered
// .trash/<n>/ last written before `now - maxAge`, and .trash/ itself when
// that leaves it empty. Run on opening a project, when no undo step can
// still bring those files back. Returns how many it removed.
int emptyOldTrash(const std::filesystem::path& project, std::filesystem::file_time_type now,
                  std::chrono::hours maxAge = kTrashKeep);

// What a carried-out plan did, or why it could not.
struct AssetOp {
    std::string error;                 // empty on success
    std::string path;                  // the asset's new path
    std::vector<std::string> touched;  // every project path moved, under both names
    std::vector<std::string> scenes;   // saved scene files rewritten
    int references = 0;                // reference fields pointed at a new path, open scene included
};

class Document;
// Carries out `plan` as one undo step of `doc`: moves the files, rewrites
// every other scene file of the project that names a moved file, and points
// the open scene's references (in memory, through the history: saved with
// the scene) at the new paths. Undo puts the files, the scene files and the
// references back; redo does it again. Nothing is touched when it fails.
AssetOp applyAssetPlan(Document& doc, const AssetIndex& index, const std::string& openScene, const AssetPlan& plan);
// Turns the blocks script `blocksPath` (project-relative) into a Lua script
// beside it, <stem>.lua (<stem>_2.lua, ... when taken), holding the Lua the
// exporter makes of it, as one undo step: the file is renamed and rewritten,
// and every reference follows (see applyAssetPlan). Undo brings the blocks
// back byte for byte. Refused, untouched, when the blocks have an error.
AssetOp convertBlocksToLua(Document& doc, const AssetIndex& index, const std::string& openScene, const std::string& blocksPath);
// Sets the loop flag of the clip file `animPath` (project-relative) as one
// undo step of `doc`: the file is read, its loop set and written back, and
// undo puts the old bytes back. Every edit bumps the revision, so the live
// export bakes the clip again. Empty on success (or when it already was so),
// else why not; the file is untouched then.
std::string setAnimLoop(Document& doc, const std::string& animPath, bool loop);
// A problem moving files back or forth on an undo or redo of an applied
// plan, cleared on read; empty when there was none.
std::string takeAssetHistoryError();
// Every file move (absolute from, to) done or undone since the last call, for
// views that hold paths (open script tabs).
std::vector<std::pair<std::filesystem::path, std::filesystem::path>> takeAssetMoves();

// A file:// URL for `path` (absolute), percent-encoded, for the system's
// file manager.
std::string fileUrl(const std::filesystem::path& path);

// Short "12 KB" style size.
std::string sizeLabel(uintmax_t bytes);

}  // namespace editor
