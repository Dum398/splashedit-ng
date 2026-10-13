// Projects on disk: a folder holding one or more .scene files with the
// meshes/, textures/ and scripts/ they refer to by project-relative path.
// This half has no SDL; the dialogs live in the app.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "editor/document.hh"
#include "import.hh"

namespace editor {

// The first .scene directly in `project`, by name; empty when there is none.
std::filesystem::path firstScene(const std::filesystem::path& project);

// Makes `dir` a project: creates it and meshes/, textures/, scripts/, and
// writes an empty scene named after the folder. A folder that already holds a
// scene is refused, so New never overwrites one; open it instead. On success
// returns the new scene's file name, relative to `dir`.
struct NewProject {
    std::filesystem::path scene;
    std::string error;  // empty on success
};
NewProject createProject(const std::filesystem::path& dir);

// Where Save As may put a scene: inside `project`, since every asset path in
// it is relative to the project folder. Returns the path relative to the
// project (with a .scene extension added if missing), or nullopt outside it.
std::optional<std::filesystem::path> sceneInProject(const std::filesystem::path& project, const std::filesystem::path& file);

// A file picked for an asset field, as the project-relative path the scene
// stores. A file already inside `project` is used where it is; one outside is
// copied into `project`/`folder`, reusing an identical file of the same name
// and numbering the copy ("rock_2.png") when a different one is there.
struct AdoptedAsset {
    std::string path;   // project-relative, '/'-separated; empty on failure
    bool copied = false;
    std::string error;  // empty on success
};
AdoptedAsset adoptAsset(const std::filesystem::path& project, const std::filesystem::path& file, const std::string& folder);

// A new Lua file for an object's Script component or the scene's own script,
// for the inspector's New script button. The name is the object's or scene's,
// lowercased with every run of other characters one '_' ("Game Logic" ->
// game_logic, "script" when nothing is left), written as scripts/<name>.lua,
// numbered (<name>_2.lua) when that is taken: an existing file is never
// overwritten. The text names the psxsplash callbacks a script of its kind
// gets: onCreate for an object, onSceneCreationStart/End for a scene.
std::string scriptFileStem(const std::string& name);
std::string scriptTemplate(const std::string& name, bool scene);
struct NewScript {
    std::string path;   // project-relative, '/'-separated; empty on failure
    std::string error;  // empty on success
};
NewScript createScript(const std::filesystem::path& project, const std::string& name, bool scene);

// The objects an imported model becomes, the top one named `name`. A model of
// one node, or any model with `settings.mergeMeshes`, is one object drawing
// the baked mesh. Otherwise the top object is an empty holding an object per
// node, the file's tree kept: each with the node's local transform and, if it
// has one, its mesh and materials. Every mesh component gets the settings'
// bit depth and lighting. An object drawing a skinned mesh also gets a skin
// component playing the model's clips (the first 16), when it has any.
splash::Object modelObject(const splash::ImportedModel& model, const std::string& name,
                           const splash::ImportSettings& settings = {});

// A model file dropped on or picked in the editor. It is adopted into the
// project's models/ folder and imported, named after the copy, with the
// settings in the copy's sidecar; the sidecar is written (defaults and the
// file it came from) when there is none. With `replace` naming an object that
// has a mesh, that object's model and materials are swapped for the baked
// mesh; otherwise modelObject() is inserted at `at` (the parent's path plus
// the index), named after the file and unique among its siblings, at
// `position` in the parent's space. Either way it is one undo step.
struct PlacedModel {
    std::string error;  // empty on success
    splash::ImportedModel model;
    ObjectPath path;  // the object placed or changed
};
PlacedModel placeModel(Document& doc, const std::filesystem::path& file, const std::optional<ObjectPath>& replace,
                       const ObjectPath& at, splash::Vec3 position);

// The model in `project`'s models/ folder that wrote the project-relative
// `mesh` (models/<name>.mesh or models/<name>/<node>.mesh): the file with a
// sidecar whose importName is <name>. Empty when there is none.
std::filesystem::path modelOfMesh(const std::filesystem::path& project, const std::string& mesh);

// Imports `model` (a file in the project's models/ folder) again with the
// settings in its sidecar, read from the sidecar's source while that file is
// there (so files beside it are found) and from the copy otherwise. The same
// .mesh and .png paths are rewritten, so every reference keeps working; the
// views reload them. Objects drawing one of its meshes take its new
// materials, and the objects of each instance of it as modelObject made them
// (matched by name) take the new node transforms. When `wasMerged` is given
// and differs from the sidecar's mergeMeshes, the top object of each instance
// is rebuilt the other way: the node objects swapped for the baked mesh or
// back, the rest of the object kept. The scene changes are one undo step.
struct Reimported {
    std::string error;  // empty on success
    splash::ImportedModel model;
    int objects = 0;  // top-level objects whose tree changed
};
Reimported reimportModel(Document& doc, const std::filesystem::path& model, std::optional<bool> wasMerged = std::nullopt);
// placeModel and reimportModel keep the import's warnings, all of them, in the
// document under the model's project path (Document::importWarnings), the
// last import of a model replacing the ones before.

// One import warning as the inspector lists it: what happened and, where one
// is known, what to change so it does not. The importer's own "problem: fix"
// wording is split where it has one; the other known warnings get a fix here.
// An unknown warning is all problem, with no fix.
struct ImportWarning {
    std::string problem;
    std::string fix;  // empty when none is known
};
ImportWarning explainImportWarning(const std::string& warning);

// Notices model files that changed on disk. Each poll stats the models in the
// project's models/ folder that have a sidecar, and each one's source; a
// source newer than at the last poll is copied over the model first.
class ModelWatcher {
  public:
    // The models (absolute paths) whose file or source changed since the
    // last poll of this project. A model seen for the first time is only noted.
    std::vector<std::filesystem::path> poll(const std::filesystem::path& project);

  private:
    struct Seen {
        std::filesystem::file_time_type model, source, sidecar;
        std::string sourcePath;
    };
    std::filesystem::path m_project;
    std::map<std::filesystem::path, Seen> m_seen;
};

// Recently opened scenes, newest first, as absolute paths. Stored one per line.
class RecentScenes {
  public:
    static constexpr size_t kMax = 8;
    explicit RecentScenes(std::filesystem::path file = {}) : m_file(std::move(file)) {}
    void load();
    // Moves `scene` to the front, drops the oldest past kMax, and saves.
    void add(const std::filesystem::path& scene);
    void remove(const std::filesystem::path& scene);
    const std::vector<std::filesystem::path>& items() const { return m_items; }

  private:
    void save() const;
    std::filesystem::path m_file;  // empty = kept in memory only
    std::vector<std::filesystem::path> m_items;
};

}  // namespace editor
