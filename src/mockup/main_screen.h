#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "disc.hh"
#include "unitymath.hh"
#include "editor/hardware.hh"
#include "editor/live_export.hh"
#include "editor/lua_editor_view.hh"
#include "editor/play.hh"
#include "editor/project.hh"
#include "editor/timeline_view.hh"
#include "emulator.h"
#include "game_view.h"

namespace viewport {
class Ps1View;
}
namespace editor {
class Document;
}

namespace mockup {

struct State {
    int tool = 1;        // 0 hand, 1 move, 2 rotate, 3 scale
    int viewMode = 0;    // 0 PS1, 1 clean
    bool maximized = false;
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
    // Add object (+ / Ctrl+A) and Add component pickers: open next frame, and the shared search text.
    bool openAddObject = false;
    bool openAddComponent = false;
    std::string pickQuery;
    // Gizmo snapping: move to 0.25 m, rotate to 15 degrees, scale to 0.1.
    // Ctrl snaps while it is off.
    bool snap = true;

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
        bool openSetup = false, openOutput = false;
        // Export (Ctrl+B): a disc image built in the background, where it goes,
        // and whether the setup popup was opened for it rather than for Play.
        std::future<splash::DiscResult> disc;
        std::filesystem::path discOut;
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
        // Live edits: the document revision the running game was last sent,
        // the revision seen last frame and since when, the export in flight,
        // and the count written to reload.flag.
        std::future<splash::ExportResult> reload;
        unsigned sentRev = 0, seenRev = 0, reloadGeneration = 0;
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
    enum class Pending { None, New, Open, Recent, Quit };
    Pending pending = Pending::None;
    std::filesystem::path pendingPath;  // the scene for Pending::Recent
    bool openUnsaved = false;
    bool unsavedShown = false;   // the prompt is up; document shortcuts wait
    bool quitRequested = false;  // window close: asks first when there are changes

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
    } importPanel;
    // Models to import again next frame (a reimport can rebuild the object the
    // inspector is drawing), and the watcher polled about once a second.
    struct Reimport {
        std::filesystem::path model;
        std::optional<bool> wasMerged;
    };
    std::vector<Reimport> reimports;
    editor::ModelWatcher watcher;
    double watchedAt = -1;

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

    // Lua files open in the script editor, as tabs beside the Scene.
    ui::ScriptTabs scripts;
    std::filesystem::path scriptsProject;  // the project they belong to

    // Cutscene and animation tracks, docked under the Scene.
    ui::TimelinePanel timeline;
};

// Draws the main screen for `doc` filling `size`. Returns the title-bar
// rect so the platform layer can make it draggable.
ImRect drawMainScreen(State& state, editor::Document& doc, viewport::Ps1View& view, ImVec2 size);

}  // namespace mockup
