// Unity project import: the scenes of a Unity SplashEdit project turned into a
// new splashedit-ng project. Reads Unity's text YAML (scenes, prefabs,
// materials, assets) and the .meta files beside them; never writes into the
// Unity project. No ImGui, no SDL.
//
// What converts: objects (hierarchy in Unity's order, transforms, active
// flag), PSXSceneExporter settings, PSXObjectExporter (mesh, collider,
// script, start-active), built-in primitive and model meshes, material
// textures and colours, Lua files, Lights, PSXData's memory card and VRAM
// areas, prefab instances (unpacked, their overrides applied) and the scene
// order. Everything else (UI canvases, fonts, sprites, tilemaps, audio,
// loading screens, ...) is listed in the ImportReport, one item per thing.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "scene.hh"

namespace splash::unity {

// ---- YAML ----

// A node of Unity's YAML subset: block and flow mappings (keys in file
// order), sequences and scalars (kept as their text, quotes removed).
struct YNode {
    enum Kind : uint8_t { Null, Scalar, Map, Seq } kind = Null;
    std::string scalar;
    std::vector<std::pair<std::string, YNode>> map;
    std::vector<YNode> seq;

    bool isNull() const { return kind == Null; }
    // The value under `key` of a mapping, nullptr when absent (or not a mapping).
    const YNode* find(std::string_view key) const;
    YNode* find(std::string_view key);
    // The value under `key`, a Null node when absent.
    const YNode& operator[](std::string_view key) const;
    // Sets (or adds at the end) `key`.
    YNode& set(std::string_view key, YNode value);
    // Scalar text; "" for anything else.
    const std::string& str() const;
    double num(double fallback = 0) const;  // the scalar as a number, `fallback` when it is not one
    long long integer(long long fallback = 0) const;
    bool flag(bool fallback = false) const;  // 0/1 (and true/false)
    static YNode scalarOf(std::string s);
};

// One `--- !u!<classID> &<fileID>` document. `type` is its top key
// (GameObject, Transform, MonoBehaviour, ...) and `body` the mapping under it.
struct YDoc {
    int classId = 0;
    long long fileId = 0;
    bool stripped = false;  // a stand-in for an object of a prefab instance
    std::string type;
    YNode body;
};

// Parses a whole Unity YAML file (the %YAML / %TAG lines skipped). A file
// without document headers is read as one document of class 0. Lines it
// cannot read are skipped, never an error.
std::vector<YDoc> parseYaml(std::string_view text);
// A single flow value: `{fileID: 0, guid: ab, type: 3}`, `[1, 2]` or a scalar.
YNode parseFlow(std::string_view text);

// A reference: {fileID, guid, type}. guid empty = in the same file.
struct Ref {
    long long fileId = 0;
    std::string guid;
    int type = 0;
    bool null() const { return fileId == 0 && guid.empty(); }
};
Ref refOf(const YNode& n);

// Colours: Color32 packed as `rgba: <uint>` (r in the low byte), and
// {r:, g:, b:, a:} (0..1). colorOf reads either form ({rgba:} or {r,g,b,a}).
std::array<uint8_t, 4> unpackRgba(uint32_t rgba);
std::array<float, 4> colorOf(const YNode& n, std::array<float, 4> fallback = {1, 1, 1, 1});
// A hex string of little-endian int32s, 8 hex digits each (Unity's int
// arrays, e.g. tilemap cells). A trailing partial group is dropped.
std::vector<int32_t> hexInts(std::string_view hex);

// ---- Project index ----

// guid -> asset path relative to the Unity project ('/' separators,
// "Assets/..."), from every *.meta under Assets/ (and Packages/).
struct GuidIndex {
    std::filesystem::path root;  // the Unity project folder
    std::map<std::string, std::string> paths;
    // The asset's path, empty when the guid is unknown.
    std::string path(const std::string& guid) const;
    std::filesystem::path file(const std::string& guid) const;  // absolute, empty when unknown
};
GuidIndex indexGuids(const std::filesystem::path& unityProject);

// The class of a MonoBehaviour document: its m_EditorClassIdentifier
// ("asm::Namespace.Class"), else the known SplashEdit script guids, else
// the stem of the .cs file its m_Script guid names. Empty when none says.
std::string monoClass(const YNode& mono, const GuidIndex& index);
// The SplashEdit Runtime script guid -> class table (from its .cs.meta files).
const std::map<std::string, std::string>& knownScriptGuids();

// ---- Built-in meshes ----

// Unity's built-in meshes (Library/unity default resources, guid
// 0000000000000000e000000000000000) by fileID: 10202 Cube, 10209 Plane, 10210
// Quad (Unity's own vertices, triangles and UVs), 10207 Sphere, 10206
// Cylinder, 10208 Capsule (the same size and UV layout, fewer triangles).
// Unity's space, clockwise front faces. nullopt for any other fileID.
struct Primitive {
    std::string name;    // "cube", ...
    bool exact = false;  // Unity's own mesh, not an approximation
    Mesh mesh;
};
std::optional<Primitive> builtinMesh(long long fileId);
constexpr const char* kBuiltinGuid = "0000000000000000e000000000000000";

// ---- Scenes ----

struct SceneEntry {
    std::string name;            // the file stem, "SCN_Menu"
    std::string path;            // "Assets/Scenes/SCN_Menu.unity"
    bool hasExporter = false;    // holds a PSXSceneExporter
    bool inBuildSettings = false;
};
// Every .unity under Assets/: the ones in EditorBuildSettings first, in its
// order, then the rest by name.
std::vector<SceneEntry> listScenes(const std::filesystem::path& unityProject, const GuidIndex& index);
std::vector<SceneEntry> listScenes(const std::filesystem::path& unityProject);
// The default choice: those of listScenes that hold a PSXSceneExporter.
std::vector<SceneEntry> defaultScenes(const std::vector<SceneEntry>& all);

// ---- The import ----

// One line of the report. `kind`: Later = a SplashEdit feature this importer
// does not bring over yet (UI canvases, fonts, sprites, tilemaps, audio,
// loading screens, ...); LeftOut = no psxsplash equivalent (a Camera, a user
// script); Changed = brought over, but not exactly (an approximated sphere);
// Missing = a file the scene refers to is not in the project.
struct ReportItem {
    enum Kind : uint8_t { Later, LeftOut, Changed, Missing } kind = Later;
    std::string scene;   // scene name, empty for the project
    std::string object;  // object path ("Parent/Child"), empty for the scene
    std::string what;
    std::string line() const;  // "[later] SCN_Menu / Canvas: UI canvas with 3 elements"
};

struct ImportReport {
    std::vector<std::string> scenes;  // project-relative .scene files written, in build order
    int objects = 0, meshes = 0, scripts = 0, textures = 0;
    std::vector<ReportItem> items;
    std::string error;  // set when nothing could be imported
    bool ok() const { return error.empty(); }
    // Items that are not brought over (Later, LeftOut and Missing).
    int skipped() const;
    // "imported scenes=N objects=N meshes=N scripts=N textures=N skipped=N"
    std::string summary() const;
    // The report file's text: a heading, the summary, every item's line.
    std::string text() const;
};

struct ImportOptions {
    // Scene names (file stems) or asset paths, in the build order wanted.
    // Empty: defaultScenes, in listScenes order.
    std::vector<std::string> scenes;
};

// Imports into `dest`, a folder that does not exist yet or is empty (and is
// not inside the Unity project): <scene>.scene per scene, meshes/,
// textures/, scripts/, models/, scenes.list, vram.json when PSXData has
// prohibited areas, and import-report.txt. On an error before anything is
// written (no such project, a scene name not found, `dest` not empty) the
// report's `error` says why and `dest` is untouched.
ImportReport importProject(const std::filesystem::path& unityProject, const std::filesystem::path& dest,
                           const ImportOptions& options = {});

// The conversion of one scene, for tests: the objects and settings of
// `scenePath` (a .unity file) as importProject writes them, assets copied
// into `dest`. Report items go to `report`.
Scene convertScene(const std::filesystem::path& unityProject, const GuidIndex& index, const std::filesystem::path& scenePath,
                   const std::filesystem::path& dest, ImportReport& report);

}  // namespace splash::unity
