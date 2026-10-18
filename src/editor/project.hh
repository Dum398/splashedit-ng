// Projects on disk: a folder holding one or more .scene files with the
// meshes/, textures/ and scripts/ they refer to by project-relative path.
// This half has no SDL; the dialogs live in the app.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "disc.hh"
#include "editor/document.hh"
#include "import.hh"

namespace editor {

// The first .scene directly in `project`, by name; empty when there is none.
std::filesystem::path firstScene(const std::filesystem::path& project);

// The build list: `project`/scenes.list, one project-relative .scene path per
// line ('/'-separated, UTF-8), in the order Play and Export number the scenes
// (scene 0 is the one the game starts in; Lua's Scene.Load(i) opens scene i).
// loadBuildList reads it, skipping blank lines; a missing file, or an empty
// `project`, reads as an empty list. saveBuildList writes it whole (an empty
// list as an empty file); false when it cannot be written.
std::vector<std::filesystem::path> loadBuildList(const std::filesystem::path& project);
bool saveBuildList(const std::filesystem::path& project, const std::vector<std::filesystem::path>& list);

// The project's VRAM layout: `project`/vram.json, holding the prohibited
// areas (rectangles of VRAM, in 16-bit words and lines, that the packer
// keeps textures and CLUTs out of) as
// {"prohibited": [{"x": 512, "y": 0, "w": 64, "h": 64}]}. Resolution and
// buffering are not stored: psxsplash always draws 320x240 into framebuffers
// at (0,0) and (0,256). loadVramSettings reads it; a missing or unreadable
// file, or an empty `project`, reads as the defaults (no areas); malformed
// entries are skipped and areas are clamped into the 1024x512 VRAM with w,
// h >= 1. saveVramSettings writes it whole; false when it cannot be written.
// Live export, Play and Export all pack with it.
splash::VramSettings loadVramSettings(const std::filesystem::path& project);
bool saveVramSettings(const std::filesystem::path& project, const splash::VramSettings& vs);

// The project's disc settings: `project`/disc.json (see splash::DiscSettings:
// the volume name and the licence). loadDiscSettings reads it; a missing or
// unreadable file, or an empty `project`, reads as the defaults (volume from
// the title, no licence). saveDiscSettings writes it whole; false when it
// cannot be written. Export passes it to the disc writer.
splash::DiscSettings loadDiscSettings(const std::filesystem::path& project);
bool saveDiscSettings(const std::filesystem::path& project, const splash::DiscSettings& s);

// The project-relative scenes a build writes, in order: the build list with
// the entries whose file is gone left out. When that leaves nothing (the list
// is empty or missing) it is `openScene` alone, the open scene's
// project-relative path.
std::vector<std::filesystem::path> buildScenes(const std::filesystem::path& project,
                                               const std::filesystem::path& openScene);

// Makes `dir` a project: creates it and meshes/, textures/, scripts/, and
// writes an empty scene named after the folder. A folder that already holds a
// scene is refused, so New never overwrites one; open it instead. On success
// returns the new scene's file name, relative to `dir`.
//
// New Project's templates: the editor's templates/ folder holds one folder
// per template, <id>/template.json ({"name", "description", "order"}) beside
// the project tree it starts from. "empty" is built in (no folder; an empty
// scene as above) and always listed first; the rest follow by order, then
// name. A folder without a readable template.json holding a non-empty
// string "name" (and, when present, a string "description" and an integer
// "order") is skipped, as is one named "empty".
struct TemplateInfo {
    std::string id, name, description;
    int order = 0;
    std::filesystem::path dir;  // empty for the built-in "empty"
};
std::vector<TemplateInfo> listTemplates(const std::filesystem::path& templatesDir);

// With a template that has a folder, createProject copies its tree into `dir`
// instead of writing an empty scene, all but template.json; when the
// template has exactly one .scene at its top it is named after the folder
// like the empty one. With several, the project opens on the first entry of
// the template's scenes.list that is one of them, or, when it has no
// scenes.list or none of its entries is, the alphabetically first. Nothing is copied when any file would land on an
// existing one, and the error names that file, as it names a file that
// cannot be copied. meshes/, textures/ and scripts/ are made either way.
struct NewProject {
    std::filesystem::path scene;
    std::string error;  // empty on success
};
NewProject createProject(const std::filesystem::path& dir, const TemplateInfo* tmpl = nullptr);

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

// A new script for an object's Script component or the scene's own script,
// for the inspector's New blocks and New Lua buttons, in either language: a
// Lua file (scripts/<name>.lua) or a block script (scripts/<name>.blocks).
// The name is the object's or scene's, lowercased with every run of other
// characters one '_' ("Game Logic" -> game_logic, "script" when nothing is
// left), numbered (<name>_2) when that stem is taken, by a file of either
// extension: an existing file is never overwritten and a .lua and a .blocks
// never share a stem. The starter names the psxsplash callbacks a script of
// its kind gets: onCreate for an object, onSceneCreationStart/End for a
// scene, onTriggerEnter/Exit for a Trigger component's script, whose file is
// <name>_trigger. The block starter is one hat stack per callback: "when
// created", "when the scene starts", or "when the player enters the trigger"
// and "when the player leaves the trigger". An Interactable object's script
// is the object starter plus onInteract(self) ("when interacted with").
enum class ScriptKind { Object, Scene, Trigger, Interactable };
enum class ScriptLanguage { Lua, Blocks };
std::string scriptFileStem(const std::string& name);
std::string scriptTemplate(const std::string& name, ScriptKind kind);
inline std::string scriptTemplate(const std::string& name, bool scene) {
    return scriptTemplate(name, scene ? ScriptKind::Scene : ScriptKind::Object);
}
std::string blocksTemplate(ScriptKind kind);
struct NewScript {
    std::string path;   // project-relative, '/'-separated; empty on failure
    std::string error;  // empty on success
};
NewScript createScript(const std::filesystem::path& project, const std::string& name, ScriptKind kind,
                       ScriptLanguage lang);
inline NewScript createScript(const std::filesystem::path& project, const std::string& name, ScriptKind kind) {
    return createScript(project, name, kind, ScriptLanguage::Lua);
}
inline NewScript createScript(const std::filesystem::path& project, const std::string& name, bool scene) {
    return createScript(project, name, scene ? ScriptKind::Scene : ScriptKind::Object);
}
// Writes `text` as <folder>/<stem>.lua or .blocks (project-relative folder,
// '/'-separated, empty for the project's top), numbered like createScript:
// <stem>_2 when a file of either extension has the stem. Creates the folder.
NewScript writeNewScript(const std::filesystem::path& project, const std::string& folder, const std::string& stem,
                         ScriptLanguage lang, const std::string& text);

// Whether a script's text defines the psxsplash callback `callback`. Lua: a
// `function <callback>` (or `function <callback>(`) at the start of a line,
// leading whitespace allowed; a line inside a -- comment does not count.
// Blocks (`blocks` true): a stack whose hat makes that function. Malformed
// block JSON defines nothing.
bool scriptDefinesCallback(const std::string& text, bool blocks, std::string_view callback);
// `text` with an empty onInteract(self) added: the commented Lua stub
// appended, or for blocks an "on_interact" hat stack placed below the
// existing stacks. Returns empty when block JSON does not parse.
std::string addInteractCallback(const std::string& text, bool blocks);

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
// `converted`, when given, is what to read instead of `file` (the GLB Blender
// wrote for a .blend, see splash::BlendExport); the outputs are still named
// after the copy of `file`.
struct PlacedModel {
    std::string error;  // empty on success
    splash::ImportedModel model;
    ObjectPath path;  // the object placed or changed
    // Set when the name the copy was to have was taken in models/ while it
    // imported: the copy's project path under the free name it got instead.
    std::string renamed;
};
PlacedModel placeModel(Document& doc, const std::filesystem::path& file, const std::optional<ObjectPath>& replace,
                       const ObjectPath& at, splash::Vec3 position, const std::filesystem::path& converted = {});

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
// `converted`, when given, is read instead of modelSource() (as for placeModel).
struct Reimported {
    std::string error;  // empty on success
    splash::ImportedModel model;
    int objects = 0;  // top-level objects whose tree changed
};
Reimported reimportModel(Document& doc, const std::filesystem::path& model, std::optional<bool> wasMerged = std::nullopt,
                         const std::filesystem::path& converted = {});
// placeModel and reimportModel keep the import's warnings, all of them, in the
// document under the model's project path (Document::importWarnings), the
// last import of a model replacing the ones before.

// placeModel and reimportModel in two halves, so the slow one can run on a
// worker thread while the editor keeps drawing. stagePlace / stageReimport
// (UI thread, quick) read the sidecar and work out where the model goes in
// the project without changing anything there. buildStaged is the slow half:
// it reads the file and bakes the meshes, textures and clips into a
// temporary folder standing in for the project (splashedit-import-* in the
// system temp folder), touching no Document; any thread may run it.
// applyPlace / applyReimport (UI thread) then adopt the file, move the
// outputs into the project and change the scene, as placeModel and
// reimportModel do, and remove the temporary folder. discardStaged drops a
// staged import instead (Cancel): the project and the scene are as before.
// A temporary folder removed with everything in it when its owner goes, so a
// staged import nobody applies leaves nothing behind. Move-only.
class StagingFolder {
  public:
    StagingFolder() = default;
    StagingFolder(StagingFolder&& o) noexcept;
    StagingFolder& operator=(StagingFolder&& o) noexcept;
    ~StagingFolder() { reset(); }
    void reset(std::filesystem::path p = {});  // removes the folder held, then holds `p`
    const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
};

struct StagedModel {
    std::filesystem::path project;  // the project it was staged for
    std::filesystem::path file;     // placed: the file dropped or picked; reimported: the project model
    std::filesystem::path read;     // what importModel reads (a .blend's GLB, the sidecar's source, or `file`)
    std::filesystem::path copy;     // the model in the project; for a placed model, where its copy goes
    bool reimport = false;
    bool adopt = false;             // `file` is copied to `copy` when applied
    bool inside = false;            // `file` is in the project already
    bool hadSidecar = false;
    bool outputsThere = false;      // models/<name>.mesh or models/<name>/ was there when it was staged
    splash::ImportSettings settings;
    StagingFolder staging;          // the temporary folder; empty when there is none
    std::string error;              // empty while it is going well
    splash::ImportedModel model;    // once built
};
StagedModel stagePlace(const Document& doc, const std::filesystem::path& file, const std::filesystem::path& converted = {});
StagedModel stageReimport(const Document& doc, const std::filesystem::path& model, const std::filesystem::path& converted = {});
// With `progress`, the build can be followed and cancelled from another
// thread (see splash::importModel); cancelled, it ends with an error.
void buildStaged(StagedModel& staged, splash::ImportProgress* progress = nullptr);
void discardStaged(StagedModel& staged);
PlacedModel applyPlace(Document& doc, StagedModel& staged, const std::optional<ObjectPath>& replace, const ObjectPath& at,
                       splash::Vec3 position);
Reimported applyReimport(Document& doc, StagedModel& staged, std::optional<bool> wasMerged = std::nullopt);

// Removes the temporary folders a build left in `dir` (the system temp folder
// when empty) when the editor was killed while importing: folders named
// splashedit-import-*, last written longer than `age` ago and held by no
// import in this process. Nothing else is touched. Returns how many went.
int sweepStaleImportFolders(std::chrono::seconds age = std::chrono::hours(1), const std::filesystem::path& dir = {});

// The file reimportModel reads for `model` (absolute, or relative to the
// project): its sidecar's source while that file is there, else the copy.
std::filesystem::path modelSource(const Document& doc, const std::filesystem::path& model);

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
