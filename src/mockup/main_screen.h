#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "blend.hh"
#include "disc.hh"
#include "unitymath.hh"
#include "unity_import.hh"
#include "editor/asset_browser_view.hh"
#include "editor/hardware.hh"
#include "editor/live_export.hh"
#include "editor/log_view.hh"
#include "editor/model_import.hh"
#include "editor/texture_preview_view.hh"
#include "editor/lua_editor_view.hh"
#include "editor/tilemap_view.hh"
#include "editor/ui_canvas_view.hh"
#include "editor/play.hh"
#include "editor/project.hh"
#include "editor/timeline_view.hh"
#include "emulator.h"
#include "game_view.h"
#include "viewport/ps1view.h"

namespace viewport {
class Ps1View;
}
namespace editor {
class Document;
}

namespace mockup {

// Where an imported model goes (see editor::ModelTarget).
using ModelTarget = editor::ModelTarget;

struct State {
    int tool = 1;        // 0 hand, 1 move, 2 rotate, 3 scale
    int viewMode = 0;    // 0 PS1, 1 clean
    // Prefab mode, one entry a level deep: the camera and look to put back on leaving it.
    struct PrefabView {
        viewport::Ps1View::Pose pose;
        int viewMode = 0;
    };
    std::vector<PrefabView> prefabViews;
    bool framePrefab = false;  // frame the whole prefab once it is drawn
    bool maximized = false;  // set by the main loop each frame; picks the maximize or restore button
    bool quit = false;   // the close button was pressed
    std::string saveError;  // last Ctrl+S failure, shown in the status bar; empty after a good save
    // A file dragged over the window: where the pointer is while it hovers,
    // and files released but not imported yet, each with where it landed.
    struct Drop {
        bool hovering = false;
        ImVec2 pos{-1, -1};
        std::vector<std::pair<std::string, ImVec2>> files;
    } drop;
    // The inspector's Model field this frame (a drop on it replaces that
    // object's model), clipped to the panel; empty when not shown.
    ImRect modelField;
    std::vector<int> modelFieldPath;
    // Scene tree rows drawn this frame (a drop on one adds the model as that
    // object's child), and the viewport panel (a drop there lands where the
    // pointer meets the scene).
    std::vector<std::pair<ImRect, std::vector<int>>> treeRows;
    ImRect viewportRect;
    // Outcome of the last model import, in the status bar until noticeUntil (ImGui time).
    std::string notice;
    bool noticeBad = false;
    double noticeUntil = 0;
    // A file the notice offers to show in its folder (a written disc image); notify clears it.
    std::filesystem::path noticeReveal;
    // Add object (+ / Ctrl+A) and Add component pickers: open next frame, and the shared search text.
    bool openAddObject = false;
    bool openAddComponent = false;
    std::string pickQuery;
    // Gizmo snapping: move to 0.25 m, rotate to 15 degrees, scale to 0.1.
    // Ctrl snaps while it is off.
    bool snap = true;
    bool gizmos = true;  // viewport draws trigger boxes, interaction radii and nav regions
    // The viewport draws the exported BVH's boxes down to bvhDepth (-1: every
    // level). Its own toggle, independent of gizmos.
    bool bvhShow = false;
    int bvhDepth = -1;
    // Nav regions from the last export: outlines (navShow), "R<k>" labels at
    // the centroids that select a region when clicked (navLabels), portal edges
    // (navPortals), and the region the Scene inspector details (-1: none).
    bool navShow = true;
    bool navLabels = false;
    bool navPortals = false;
    int navRegion = -1;

    // Background dry-run export feeding the status bar's budget meters and problem count.
    editor::LiveExport live;

    // Play (F5): export, then pcsx-redux.
    struct Play {
        std::filesystem::path settingsFile;  // where tools are saved; empty = not saved (screenshot mode)
        editor::PlayTools tools;             // as set by the user; withDefaults() fills the rest
        std::filesystem::path bundleDir;     // where the editor lives; redux/ and engine/ ship there
        std::future<splash::ExportResult> build;
        Emulator emu;
        GameView game;            // what redux shows, once it has published a frame
        bool gameFocus = false;   // the Game view has the keyboard (a click in either view moves it)
        float split = 0.5f;       // share of the viewport the Scene view keeps while the game runs
        uint64_t startedAt = 0;   // SDL ticks when redux started, to notice it never publishing
        std::string message;  // why the last Play did not start, for the status bar
        // The last finished Play, Run on hardware or Export build's problems the
        // open scene's live export does not already list (the Problems popup's
        // second part), and, per build in flight, the open scene's tag in it
        // (splash::sceneTag; nullopt when it is not in the build list) and the
        // number of scenes it builds.
        editor::BuildReport built;
        struct Launch {
            std::optional<std::string> openTag;
            size_t scenes = 0;
        } playLaunch, hwLaunch, discLaunch;
        bool openSetup = false, openOutput = false;
        // Export (Ctrl+B): a disc image built in the background, where it goes,
        // and whether the setup popup was opened for it rather than for Play.
        std::future<splash::DiscResult> disc;
        std::filesystem::path discOut;
        std::filesystem::path discWritten;  // the last disc image Export wrote, for Show Disc Image in Folder
        int setupFor = 0;  // which action the setup popup is for: 0 Play, 1 Export, 2 Run on hardware
        // Run on hardware (Ctrl+F5): the export it uploads, then the serial session.
        std::future<splash::ExportResult> hwBuild;
        editor::HardwareRun hw;
        std::string hwPort;              // the port the session was started on
        std::vector<std::string> ports;  // serial ports found when the setup popup opened
        std::filesystem::path exportTo;  // screenshot mode: write here instead of asking
        // What the engine reported using in the last Play of scene `peakScene`
        // (Document::loadId), kept after it stops.
        std::optional<editor::RenderPeak> peak;
        unsigned peakScene = 0;
        uint64_t linesSeen = 0;
        // The output popups: filter, clear point and wrapping, for Play and Run on hardware.
        editor::LogView playLog, hwLog;
        // Live edits: the document revision the running game was last sent,
        // the revision seen last frame and since when, the export in flight,
        // the count written to reload.flag, and the open scene's place in the
        // build list Play exported (-1 when it is not in it: no live edits).
        std::future<splash::ExportResult> reload;
        unsigned sentRev = 0, seenRev = 0, reloadGeneration = 0;
        int reloadIndex = 0;
        double seenAt = 0;
    } play;

    // F2 rename in the tree: the object's path, and whether the editor still has to open.
    bool renaming = false;
    bool renameStart = false;
    std::vector<int> renamePath;

    // Viewport mouse: which button started the current press, and whether it
    // moved far enough to count as a drag rather than a click.
    int vpButton = -1;
    bool vpDragged = false;
    bool vpAlt = false;  // Alt was down when the press started (orbit / zoom drags)

    // Right-button flythrough: speed (wheel while held), how long a move key
    // has been held (acceleration), and when the speed readout fades.
    float flySpeed = 3.0f;
    float flyHeld = 0;
    double flySpeedShownUntil = 0;

    // Shift+F: the camera keeps the selected object framed as it moves.
    bool follow = false;
    const void* followObject = nullptr;
    float followAt[3] = {};
    // A hierarchy double-click asks the viewport to frame the selection.
    bool frameRequest = false;
    // Object > Align with View, done where the viewport has the camera.
    bool alignRequest = false;

    // Scene panel: the filter text and a request to put the caret in it
    // (Ctrl+F); a row being dragged onto a new parent; the context menu to
    // open next frame (1 = the object at treeMenuPath, 2 = the panel's own
    // menu) and where, with treeMenuRight anchoring its right edge there.
    std::string treeFilter;
    bool focusTreeFilter = false;
    bool treeDragging = false;
    std::vector<int> treeDragPath;
    int treeMenu = 0;
    int treeMenuShown = 0;
    std::vector<int> treeMenuPath;
    ImVec2 treeMenuPos;
    bool treeMenuRight = false;

    // The title-bar menu that is open (index into File..Help), or -1.
    int menuOpen = -1;
    // Recently opened scenes (File menu), and an action held behind the
    // unsaved-changes prompt until the user saves, discards or cancels.
    editor::RecentScenes recent;
    enum class Pending { None, New, Open, Recent, Quit, ImportUnity };
    Pending pending = Pending::None;
    std::filesystem::path pendingPath;  // the scene for Pending::Recent
    bool openUnsaved = false;
    bool unsavedShown = false;   // the prompt is up; document shortcuts wait
    bool quitRequested = false;  // window close: asks first when there are changes

    // New Project (Ctrl+N): the modal that picks a template, a name and a
    // folder. templatesDir is the editor's templates/ (main.cpp sets it).
    struct NewProjectDialog {
        std::filesystem::path templatesDir;
        std::vector<editor::TemplateInfo> templates;  // listed when the modal opens
        int selected = 0;
        std::string name, location;
        std::string error;  // why the last Create failed, shown in the modal
        bool open = false;   // open the modal next frame
        bool shown = false;  // the modal is up; document shortcuts wait
    } newProject;

    // File > Import Unity Project...: the scenes of a Unity SplashEdit
    // project made into a new project (splash::unity::importProject), run on
    // a worker thread; then the same modal shows what was not brought over
    // while the new project opens beneath it.
    struct UnityImportDialog {
        std::string source, dest;  // the Unity project folder and the new project's folder, as typed or picked
        struct Row {
            std::string name, path;  // "SCN_Menu", "Assets/Scenes/SCN_Menu.unity"
            bool on = true;          // imported
            bool exporter = true;    // holds a PSXSceneExporter
        };
        std::vector<Row> scenes;       // in the order they are imported (scenes.list)
        // CD music files as typed or picked: [0] is disc track 2. Unity keeps
        // this list in its editor preferences, not in the project.
        std::vector<std::string> cdTracks;
        std::string listedFor;         // the source `scenes` was read for
        std::future<splash::unity::ImportReport> running;
        std::filesystem::path runningDest;
        std::optional<splash::unity::ImportReport> result;  // set: the report page
        std::filesystem::path resultDest;
        std::string error;    // why the last import failed, shown in the modal
        bool open = false;    // open the modal next frame
        bool shown = false;   // the modal is up; document shortcuts wait
    } unityImport;

    // The inspector's Import settings: the model the shown object's mesh came
    // from and its sidecar, looked up again about once a second. `editing`
    // holds a scrub until it is released.
    struct ImportPanel {
        std::string mesh;             // the mesh the model was looked up for
        std::filesystem::path model;  // empty: not from an imported model
        splash::ImportSettings settings;
        std::string error;  // the sidecar could not be read
        double checked = -1;
        std::optional<splash::ImportSettings> editing;
        // The import warnings the inspector listed this frame, for scripted checks.
        std::vector<std::string> shown;
    } importPanel;
    // The inspector's nav Bake details disclosures (Player, Navigation).
    bool navBakeDetails[2] = {false, false};
    // Models to import again next frame (a reimport can rebuild the object the
    // inspector is drawing), and the watcher polled about once a second.
    struct Reimport {
        std::filesystem::path model;
        std::optional<bool> wasMerged;
    };
    std::vector<Reimport> reimports;
    editor::ModelWatcher watcher;
    double watchedAt = -1;

    // Models waiting to be imported (dropped, picked, or changed on disk),
    // oldest first, one running at a time: Blender for a .blend, then the
    // file is read and baked on a worker thread while the editor keeps
    // drawing; the status bar shows it with a Cancel button. Once it is
    // built the model is placed (or, for a reimport, the scene updated) here
    // on the UI thread, as one undo step.
    editor::ModelImports imports;
    ImRect importCancel;  // the status bar's Cancel button this frame; empty when not shown
    float importProgress = -1;  // the share of the running build done, as the status bar shows it this frame; below 0: not shown
    bool importBusy() const { return imports.busy(); }
    int importFrames() const { return imports.frames(); }

    // Gizmo drag in progress, for the tool in `tool`. Move: handle 0..2 =
    // X/Y/Z arrow, 3 = XZ plane. Rotate: 0..2 = X/Y/Z ring. Scale: 0..2 =
    // X/Y/Z handle, 3 = uniform centre. -1 = none.
    struct GizmoDrag {
        int handle = -1;
        int tool = -1;
        ImVec2 startMouse;
        splash::Vec3 startWorld, startLocal, startHit;
        ImVec2 axisDir;     // unit screen direction of the dragged arrow / scale handle
        float pxPerUnit = 1;  // move: screen pixels per world metre along it, at the object's depth
        // Rotate: the local rotation before the drag, the axis in parent
        // space, the last mouse angle around the centre, the summed angle,
        // and +-1 for which way a screen-angle increase turns the object.
        splash::Quat startRot;
        splash::Vec3 parentAxis;
        splash::Vec3 ringStart;  // world unit vector from the centre to where the ring was grabbed
        float lastAngle = 0, accum = 0, sign = 1;
        // Scale: the local scale before the drag and the handle's screen
        // length, the pixels that double the scale.
        splash::Vec3 startScale;
        float handlePx = 1;
        float shown = 0;  // the value the drag label shows (degrees or factor)
    } gizmo;
    // A waypoint of the selected agent being dragged in the viewport (-1 = none).
    struct AgentWpDrag {
        int index = -1;
        splash::Vec3 start, startHit;
    } agentWp;
    int agentWpHot = -1;  // the waypoint dot under the mouse or being dragged

    // Lua files open in the script editor, as tabs beside the Scene and UI.
    ui::ScriptTabs scripts;
    // The UI tab: the canvas editor.
    ui::CanvasView canvasView;
    // The Tilemap tab: the tile painter.
    ui::TilemapView tilemapView;
    std::filesystem::path scriptsProject;  // the project they belong to
    unsigned scriptsAssets = 0;            // doc.assetsId() the tabs last matched disk at
    // The Interactable section's Event row: whether the object's script
    // defines onInteract, from its open tab's text or the file on disk (read
    // again at most once a second, or when its time changes).
    struct InteractEvent {
        std::string file;                         // project path it was worked out for
        std::optional<uint64_t> tabRevision;      // set when it came from a tab
        std::filesystem::file_time_type mtime{};  // the file's, when it came from disk
        bool exists = false;
        double checkedAt = -1;
        bool defined = false;
    } interactEvent;

    // Cutscene and animation tracks, docked under the Scene.
    ui::TimelinePanel timeline;

    // Inspector texture preview: Source vs PS1, converted off the UI thread.
    ui::TexturePreviewView texPreview;

    // The bottom dock under the middle panel holds the Project panel and,
    // while it is open, the Timeline as tabs; one shows at a time.
    enum class BottomTab { Project, Timeline } bottomTab = BottomTab::Project;
    bool timelineWasOpen = false;  // to raise the Timeline tab when something opens it
    unsigned timelineShows = 0;
    // The Project panel under the middle panel, and the inspector's asset
    // fields drawn this frame, clipped to what shows: an asset dragged from
    // the panel and released on one of them is assigned to it.
    ui::AssetBrowser assets;
    struct AssetSlotTarget {
        ImRect rect;
        int slot;  // main_screen's AssetSlot, or -1 for the Model field
        std::vector<int> path;
        int index = -1;  // the material a Texture field sets, -1 when the slot has one
    };
    std::vector<AssetSlotTarget> assetSlots;
};

// Draws the main screen for `doc` filling `size`. Returns the title-bar
// rect so the platform layer can make it draggable.
ImRect drawMainScreen(State& state, editor::Document& doc, viewport::Ps1View& view, ImVec2 size);

}  // namespace mockup
