// Projects on disk: a folder holding one or more .scene files with the
// meshes/, textures/ and scripts/ they refer to by project-relative path.
// This half has no SDL; the dialogs live in the app.
#pragma once

#include <filesystem>
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

// The objects an imported model becomes, the top one named `name`. A model of
// one node is one object drawing the baked mesh. With more, the top object is
// an empty holding an object per node, the file's tree kept: each with the
// node's local transform and, if it has one, its mesh and materials.
splash::Object modelObject(const splash::ImportedModel& model, const std::string& name);

// A model file dropped on or picked in the editor. It is adopted into the
// project's models/ folder and imported. With `replace` naming an object that
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
