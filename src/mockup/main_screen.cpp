#include "mockup/main_screen.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "audio.hh"
#include "budget.hh"
#include "disc.hh"
#include "rooms.hh"
#include "splashpack.hh"
#include "import.hh"
#include "editor/catalog.hh"
#include "editor/document.hh"
#include "editor/gizmo.hh"
#include "editor/memcard.hh"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include "gl.h"
#include "ui/brand.h"
#include "ui/icons.h"
#include "ui/widgets.h"
#include "viewport/ps1view.h"

using namespace theme;
using namespace ui;

namespace mockup {

namespace kind {
constexpr ImU32 mesh = rgb(0x7aa7ff);
constexpr ImU32 light = rgb(0xf2c25c);
constexpr ImU32 camera = rgb(0xb49dff);
constexpr ImU32 audio = rgb(0x46c98a);
constexpr ImU32 script = rgb(0x5ccfe0);
constexpr ImU32 folder = rgb(0x8a91a1);
constexpr ImU32 player = rgb(0xff8fb1);
constexpr ImU32 collider = rgb(0x46c98a);
constexpr ImU32 nav = rgb(0x9fd36b);
constexpr ImU32 trigger = rgb(0xe08cff);
constexpr ImU32 interact = rgb(0xffb86b);
constexpr ImU32 room = rgb(0x40e0d0);
constexpr ImU32 portal = rgb(0xffe14d);
}  // namespace kind

static void panel(ImDrawList* dl, ImRect r) {
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window);
    dl->AddRect(r.Min, r.Max, rgb(0xffffff, 8), radius::window);
}

static float panelHeader(ImDrawList* dl, ImRect r, const char* title, const char* count) {
    Fonts& f = fonts();
    ImRect h(r.Min, ImVec2(r.Max.x, r.Min.y + size::panelHeader));
    float y = h.Min.y + (h.GetHeight() - measure(f.semibold, type::body, "Ag").y) * 0.5f;
    text(dl, ImVec2(h.Min.x + space::md, y), f.semibold, type::body, color::text, title);
    if (count) {
        float x = h.Min.x + space::md + measure(f.semibold, type::body, title).x + space::sm;
        text(dl, ImVec2(x, y + 1), f.regular, type::label, color::textFaint, count);
    }
    return h.Max.y;
}

static void windowControls(State& st, ImDrawList* dl, ImRect bar) {
    float w = 46;
    const char* ids[3] = {"##min", "##max", "##close"};
    for (int i = 0; i < 3; ++i) {
        ImRect r(ImVec2(bar.Max.x - w * (3 - i), bar.Min.y), ImVec2(bar.Max.x - w * (2 - i), bar.Max.y));
        Hit h = interact(ids[i], r);
        if (h.clicked) {
            SDL_Window* win = SDL_GL_GetCurrentWindow();
            if (i == 0) SDL_MinimizeWindow(win);
            if (i == 1) {
                if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)
                    SDL_RestoreWindow(win);
                else
                    SDL_MaximizeWindow(win);
            }
            if (i == 2) st.quitRequested = true;
        }
        if (h.hover > 0) dl->AddRectFilled(r.Min, r.Max, i == 2 ? rgb(0xe0475a, (int)(255 * h.hover)) : rgb(0x2a2f39, (int)(255 * h.hover)));
        ImVec2 c = r.GetCenter();
        ImU32 col = i == 2 && h.hover > 0.5f ? rgb(0xffffff) : color::textDim;
        if (i == 0) dl->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x + 5, c.y), col, 1.2f);
        if (i == 1) dl->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col, 1.5f, 0, 1.2f);
        if (i == 2) {
            dl->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col, 1.2f);
            dl->AddLine(ImVec2(c.x - 5, c.y + 5), ImVec2(c.x + 5, c.y - 5), col, 1.2f);
        }
    }
}

// ---- Play: export to a scratch directory, then boot it in pcsx-redux.

static std::filesystem::path playDir() {
    std::error_code ec;
    std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    return (ec ? std::filesystem::path(".") : tmp) / "splashedit-play";
}

static void notify(State& st, std::string msg, bool bad);
static void scriptLine(const char* tag, const std::string& line);
static std::string utf8(const std::filesystem::path& p);

// Renames from the hierarchy (F2) or the inspector title: portals naming a
// renamed Room follow it. Scripted runs print `rename: <old> -> <new> portals N`.
static void renameFromUi(editor::Document& doc, const editor::ObjectPath& path, const std::string& typed) {
    const splash::Object* o = doc.object(path);
    if (!o) return;
    const std::string before = o->name;
    const int n = doc.renameObject(path, typed);
    if (n >= 0) scriptLine(nullptr, "rename: " + before + " -> " + typed + " portals " + std::to_string(n));
}

// A build reads scripts from disk, so the script editor's unsaved tabs are
// saved first: what runs is what is on screen. False if one could not be.
static bool saveScriptsForBuild(State& st) {
    std::string err;
    if (st.scripts.saveAll(&err)) return true;
    notify(st, "Cannot save a script: " + err, true);
    return false;
}

// The open scene's path relative to the project, as the build list names it.
static std::filesystem::path openSceneFile(const editor::Document& doc) {
    if (doc.projectRoot().empty()) return doc.scenePath();
    return doc.scenePath().lexically_relative(doc.projectRoot());
}

// The build list's scenes for Play and Run on hardware, run on the worker
// thread: the open one is `scene` (unsaved edits included), the rest are read
// from disk; a file that does not load is an export error naming it.
static splash::ExportResult exportBuild(const std::vector<std::filesystem::path>& files, const std::filesystem::path& open,
                                        const splash::Scene& scene, const std::filesystem::path& root,
                                        const std::filesystem::path& dir) {
    editor::BuildScenes b = editor::loadBuildScenes(root, files, open, scene);
    if (!b.errors.empty()) {
        splash::ExportResult r;
        r.errors = std::move(b.errors);
        return r;
    }
    return editor::exportForPlay(b.pointers(), root, dir, b.names);
}

// Where the open scene is in `files`; -1 when it is not there.
static int indexIn(const std::vector<std::filesystem::path>& files, const std::filesystem::path& open) {
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].lexically_normal() == open.lexically_normal()) return int(i);
    return -1;
}

// What a build of `files` needs to know to report its problems later.
static State::Play::Launch launchOf(const std::vector<std::filesystem::path>& files, const std::filesystem::path& open) {
    State::Play::Launch l;
    l.scenes = files.size();
    const int i = indexIn(files, open);
    if (i >= 0) l.openTag = splash::sceneTag(size_t(i), files.size(), utf8(files[size_t(i)].stem()));
    return l;
}

// A build over the build list finished: keep what the Problems popup does not already show.
static void reportBuild(State& st, const char* what, const State::Play::Launch& l, const splash::ExportResult& r) {
    const std::optional<splash::ExportResult>& live = st.live.result();
    st.play.built = editor::buildReport(what, l.scenes, r, l.openTag, live ? &*live : nullptr);
    scriptLine(nullptr, std::string("build report: ") + what + " scenes " + std::to_string(l.scenes) + " errors " +
                            std::to_string(st.play.built.errors.size()) + " warnings " +
                            std::to_string(st.play.built.warnings.size()));
}

// F5 and the Play button: start, or stop what is running.
static void togglePlay(State& st, const editor::Document& doc) {
    State::Play& p = st.play;
    if (p.emu.running()) {
        p.emu.stop();
        return;
    }
    if (p.build.valid()) return;
    if (!saveScriptsForBuild(st)) return;
    if (p.reload.valid()) p.reload.wait();  // a live edit still writing the folder
    p.message.clear();
    p.peak.reset();
    p.peakScene = doc.loadId();
    if (!editor::missingTools(editor::withDefaults(p.tools, p.bundleDir)).empty()) {
        p.openSetup = true;
        p.setupFor = 0;
        return;
    }
    p.sentRev = p.seenRev = doc.revision();
    p.reloadGeneration = 0;
    std::vector<std::filesystem::path> files = editor::buildScenes(doc.projectRoot(), openSceneFile(doc));
    p.reloadIndex = indexIn(files, openSceneFile(doc));
    p.built = {};
    p.playLaunch = launchOf(files, openSceneFile(doc));
    p.build = std::async(std::launch::async, [files = std::move(files), open = openSceneFile(doc), scene = doc.scene(),
                                              root = doc.projectRoot()] {
        splash::ExportResult r = exportBuild(files, open, scene, root, playDir());
        // Its presence at boot is what makes the game watch for live edits.
        if (r.ok() && !editor::writeReloadFlag(playDir(), 0)) r.errors.push_back("Cannot write reload.flag");
        return r;
    });
}

// Run on hardware exports here, apart from Play's folder, and serves this one.
static std::filesystem::path hardwareDir() {
    std::error_code ec;
    std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    return (ec ? std::filesystem::path(".") : tmp) / "splashedit-hardware";
}

// Ctrl+F5 and the Run on hardware button: export, upload to the console and
// serve its files; or stop the session that is running.
static void toggleRun(State& st, const editor::Document& doc) {
    State::Play& p = st.play;
    if (p.hw.active()) {
        p.hw.stop();
        return;
    }
    if (p.hwBuild.valid()) return;
    if (!saveScriptsForBuild(st)) return;
    p.message.clear();
    if (!editor::missingHardwareTools(editor::withDefaults(p.tools, p.bundleDir)).empty()) {
        p.openSetup = true;
        p.setupFor = 2;
        return;
    }
    std::vector<std::filesystem::path> files = editor::buildScenes(doc.projectRoot(), openSceneFile(doc));
    p.built = {};
    p.hwLaunch = launchOf(files, openSceneFile(doc));
    p.hwBuild = std::async(std::launch::async, [files = std::move(files), open = openSceneFile(doc), scene = doc.scene(), root = doc.projectRoot()] {
        return exportBuild(files, open, scene, root, hardwareDir());
    });
}

// Ctrl+B and the Export button: ask where the disc image goes, then build it.
static void startExport(State& st, const editor::Document& doc);

// Edits made while the game runs: once the document has stayed unchanged for
// a moment, export it again and tell the running game to reload the scene.
static void sendEdits(State& st, const editor::Document& doc) {
    State::Play& p = st.play;
    const double now = ImGui::GetTime();
    if (p.reload.valid()) {
        if (p.reload.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        splash::ExportResult r = p.reload.get();
        p.message = r.ok() ? std::string() : "Live edit not sent: " + r.errors.front();
    }
    if (!p.emu.running() || p.build.valid() || doc.loadId() != p.peakScene || p.reloadIndex < 0) return;
    if (doc.revision() != p.seenRev) {
        p.seenRev = doc.revision();
        p.seenAt = now;
    }
    if (doc.revision() == p.sentRev || now - p.seenAt < 0.3) return;
    p.sentRev = doc.revision();
    const unsigned gen = ++p.reloadGeneration;
    p.reload = std::async(std::launch::async, [scene = doc.scene(), root = doc.projectRoot(), gen, index = size_t(p.reloadIndex)] {
        return editor::reloadForPlay(scene, root, playDir(), gen, index);
    });
}

static void updatePlay(State& st, const editor::Document& doc) {
    State::Play& p = st.play;
    sendEdits(st, doc);
    if (p.hwBuild.valid() && p.hwBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        splash::ExportResult r = p.hwBuild.get();
        const editor::PlayTools eff = editor::withDefaults(p.tools, p.bundleDir);
        reportBuild(st, "Run on hardware", p.hwLaunch, r);
        if (!r.ok()) p.message = editor::buildFailure("Run on hardware", r);
        else {
            p.hwPort = eff.port;
            p.hw.start(editor::HardwareRun::port(eff.port), editor::engineFor(eff), hardwareDir());
            editor::restartLog(p.hwLog);
        }
    }
    p.emu.poll();
    // Read lines printed since the last frame (some may already have scrolled out).
    const std::deque<std::string>& out = p.emu.output();
    if (p.emu.lineCount() < p.linesSeen) p.linesSeen = 0;  // restarted
    const uint64_t first = p.emu.lineCount() - out.size();
    for (uint64_t i = std::max(p.linesSeen, first); i < p.emu.lineCount(); ++i) {
        editor::RenderPeak rp;
        if (editor::parseRenderPeak(out[size_t(i - first)], &rp)) p.peak = rp;
    }
    p.linesSeen = p.emu.lineCount();
    if (p.emu.running()) {
        if (!p.game.attached() && p.game.attach(p.emu.pid())) p.gameFocus = true;
        p.game.update();
        if (!p.game.attached() && p.message.empty() && SDL_GetTicks() - p.startedAt > 10000)
            p.message = "pcsx-redux started but shows nothing here; it needs -shmdisplay support";
    } else if (p.game.attached()) {
        p.game.detach();
        p.gameFocus = false;
    }
    if (!p.build.valid() || p.build.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    splash::ExportResult r = p.build.get();
    reportBuild(st, "Play", p.playLaunch, r);
    if (!r.ok()) {
        p.message = editor::buildFailure("Play", r);
        return;
    }
    std::string err;
    if (!p.emu.start(editor::reduxCommand(editor::withDefaults(p.tools, p.bundleDir), playDir()), &err))
        p.message = "Could not start pcsx-redux: " + err;
    else {
        p.startedAt = SDL_GetTicks();
        editor::restartLog(p.playLog);
    }
}

// A file dialog answers on its own thread; the setup popup picks it up next frame.
static std::mutex g_pickMutex;
static int g_pickWhich = -1;
static std::string g_pickPath;

static int resizePort(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(d->UserData);
        str->resize(size_t(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

static void SDLCALL onToolPicked(void* which, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_pickMutex);
    g_pickWhich = static_cast<int>(reinterpret_cast<intptr_t>(which));
    g_pickPath = files[0];
}

static std::string utf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

static std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

static float buttonWidth(const char* ic, const char* label) {
    Fonts& f = fonts();
    const float iconW = ic ? measure(f.medium, type::icon, ic).x + space::sm - 2 : 0;
    return space::md * 2 + iconW + measure(f.medium, type::body, label).x;
}

static void pushPopupStyle() {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::lg, space::lg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(space::sm, space::sm));
}

// Shown when Play is missing a program: where each one is, and a way to point at it.
static void playSetup(State& st, const editor::Document& doc, ImVec2 size) {
    State::Play& p = st.play;
    {
        std::lock_guard<std::mutex> lock(g_pickMutex);
        if (g_pickWhich >= 0) {
            std::filesystem::path picked(std::u8string(g_pickPath.begin(), g_pickPath.end()));
            (g_pickWhich == 0   ? p.tools.redux
             : g_pickWhich == 1 ? p.tools.psxsplash
             : g_pickWhich == 2 ? p.tools.bios
                                : p.tools.disc) = picked;
            if (!p.settingsFile.empty()) editor::savePlayTools(p.settingsFile, p.tools);
            g_pickWhich = -1;
        }
    }
    const char* id = "##playsetup";
    if (p.openSetup) {
        p.openSetup = false;
        if (p.setupFor == 2) p.ports = editor::listSerialPorts();
        ImGui::OpenPopup(id);
    }
    const float w = 560;
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    pushPopupStyle();
    const bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (!open) return;
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float inner = w - space::lg * 2;
    float y = o.y;
    const int mode = std::clamp(p.setupFor, 0, 2);
    const char* titles[3] = {"Set up Play", "Set up Export", "Set up Run on hardware"};
    const char* blurbs[3] = {"Play exports the scene and boots it in pcsx-redux on your psxsplash build.",
                             "Export puts the scene on a disc image with the CD-ROM build of psxsplash.",
                             "Uploads psxsplash to a console at the Unirom shell, then serves it the scene over the cable."};
    text(dl, ImVec2(o.x, y), f.semibold, type::title, color::text, titles[mode]);
    y += 24;
    text(dl, ImVec2(o.x, y), f.regular, type::label, color::textDim, blurbs[mode]);
    y += 30;

    const editor::PlayTools eff = editor::withDefaults(p.tools, p.bundleDir);
    // With an overlay on, Play and Run boot the build beside it that has it: that file is the one shown.
    const std::filesystem::path engine = editor::engineFor(eff);
    const char* engineLabel = eff.overlayPerf && eff.overlayRooms ? "psxsplash build (perf + rooms overlays)"
                              : eff.overlayPerf                   ? "psxsplash build (performance overlay)"
                              : eff.overlayRooms                  ? "psxsplash build (room debug overlay)"
                                                                  : "psxsplash build";
    struct Row {
        const char* label;
        const std::filesystem::path* path;
        bool optional;
        const char* filter;
    } rows[] = {{"pcsx-redux", &eff.redux, false, nullptr},
                {engineLabel, &engine, false, "ps-exe"},
                {"BIOS", &eff.bios, true, "bin"},
                {"psxsplash disc build", &eff.disc, false, "ps-exe"}};
    static const SDL_DialogFileFilter exeFilter[] = {{"PlayStation executable", "ps-exe;exe"}};
    static const SDL_DialogFileFilter biosFilter[] = {{"BIOS image", "bin;rom"}};
    // Each popup lists what its own action runs.
    const int shown[3][3] = {{0, 1, 2}, {3, -1, -1}, {1, -1, -1}};
    for (int i : shown[mode]) {
        if (i < 0) continue;
        const Row& r = rows[i];
        ImGui::PushID(i);
        const float rowH = 44;
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + inner, y + rowH), color::field, radius::field);
        text(dl, ImVec2(o.x + space::md, y + 6), f.medium, type::body, color::text, r.label);
        std::error_code ec;
        std::string sub;
        ImU32 tone = color::textFaint;
        if (r.path->empty()) {
            sub = r.optional ? "Optional: pcsx-redux uses its own" : "Not set";
            if (!r.optional) tone = color::warn;
        } else if (!std::filesystem::is_regular_file(*r.path, ec)) {
            sub = "Not found: " + utf8(*r.path);
            tone = color::bad;
        } else {
            sub = utf8(*r.path);
        }
        ImGui::PushClipRect(ImVec2(o.x, y), ImVec2(o.x + inner - 100, y + rowH), true);
        text(dl, ImVec2(o.x + space::md, y + 24), f.regular, type::caption, tone, sub.c_str());
        ImGui::PopClipRect();
        const float bw = buttonWidth(icon::folderOpen, "Locate");
        if (button("locate", ImVec2(o.x + inner - bw - space::sm, y + (rowH - size::field - 6) * 0.5f), icon::folderOpen, "Locate",
                   ButtonKind::Secondary)) {
            const SDL_DialogFileFilter* flt = i == 1 || i == 3 ? exeFilter : i == 2 ? biosFilter : nullptr;
            SDL_ShowOpenFileDialog(onToolPicked, reinterpret_cast<void*>(static_cast<intptr_t>(i)), nullptr, flt, flt ? 1 : 0,
                                   nullptr, false);
        }
        ImGui::PopID();
        y += rowH + space::sm;
    }
    if (mode == 2) {
        // The port: typed, or one of those found on this machine.
        const float rowH = 44;
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + inner, y + rowH), color::field, radius::field);
        text(dl, ImVec2(o.x + space::md, y + 6), f.medium, type::body, color::text, "Serial port");
        text(dl, ImVec2(o.x + space::md, y + 24), f.regular, type::caption, p.tools.port.empty() ? color::warn : color::textFaint,
             p.tools.port.empty() ? "Not set" : "115200 baud, 8N2");
        const float fx = o.x + 150, fw = inner - 150 - space::sm;
        ImGui::SetCursorScreenPos(ImVec2(fx, y + (rowH - size::field) * 0.5f));
        ImGui::SetNextItemWidth(fw);
        ImGui::PushFont(f.regular, type::body);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, color::raised);
        ImGui::PushStyleColor(ImGuiCol_Text, color::text);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(space::sm, (size::field - type::body) * 0.5f - 1));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, radius::field);
        if (ImGui::InputTextWithHint("##port", "COM3, /dev/ttyUSB0, or tcp:localhost:6699", p.tools.port.data(),
                                     p.tools.port.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizePort, &p.tools.port) &&
            !p.settingsFile.empty())
            editor::savePlayTools(p.settingsFile, p.tools);
        interactiveRects().push_back(ImRect(ImVec2(fx, y), ImVec2(fx + fw, y + rowH)));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        y += rowH + space::sm;
        if (!p.ports.empty()) {
            text(dl, ImVec2(o.x, y + 6), f.regular, type::caption, color::textFaint, "Found:");
            const float left = o.x + 50;
            float px = left;
            for (size_t i = 0; i < p.ports.size() && i < 16; ++i) {
                const float bw = buttonWidth(nullptr, p.ports[i].c_str());
                if (px > left && px + bw > o.x + inner) {  // wrap
                    px = left;
                    y += size::field + 6 + space::xs;
                }
                ImGui::PushID(int(i) + 100);
                if (button("port", ImVec2(px, y), nullptr, p.ports[i].c_str(),
                           p.tools.port == p.ports[i] ? ButtonKind::Secondary : ButtonKind::Ghost)) {
                    p.tools.port = p.ports[i];
                    if (!p.settingsFile.empty()) editor::savePlayTools(p.settingsFile, p.tools);
                }
                px += bw + space::xs;
                ImGui::PopID();
            }
            y += size::field + 6 + space::sm;
        }
    }
    y += space::sm;
    const editor::PlayTools effNow = editor::withDefaults(p.tools, p.bundleDir);
    const bool ready = (mode == 1   ? editor::missingDiscTools(effNow)
                        : mode == 2 ? editor::missingHardwareTools(effNow)
                                    : editor::missingTools(effNow))
                           .empty();
    const char* goIcons[3] = {icon::play, icon::package, icon::cpu};
    const char* goLabels[3] = {"Play", "Export", "Run"};
    const char* goIcon = goIcons[mode];
    const char* goLabel = goLabels[mode];
    const float pw = buttonWidth(goIcon, goLabel), cw = buttonWidth(icon::x, "Cancel");
    float bx = o.x + inner - pw;
    if (ready) {
        if (button("go", ImVec2(bx, y), goIcon, goLabel, ButtonKind::Primary)) {
            ImGui::CloseCurrentPopup();
            if (mode == 1) startExport(st, doc);
            else if (mode == 2) toggleRun(st, doc);
            else togglePlay(st, doc);
        }
    }
    if (button("cancel", ImVec2(bx - cw - space::sm, y), icon::x, "Cancel", ButtonKind::Ghost)) ImGui::CloseCurrentPopup();
    y += size::field + 6;
    // The buttons above registered items at absolute positions; size the popup from its top.
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner, y - o.y));
    ImGui::EndPopup();
}

// Title-bar menu labels this frame, for placing the open menu under its label.
static ImRect g_menuRects[6];
// A press outside an open menu closes it; when that press lands on the same
// label, its release must not open the menu again.
static int g_menuIgnoreClick = -1;

// The bottom dock: the Timeline tab shows while it is open and is the one in front.
static bool timelineInFront(const State& st) { return st.timeline.open && st.bottomTab == State::BottomTab::Timeline; }

// Shows the Project tab in front, or folds it to its header when it already is and shows.
static void toggleProjectTab(State& st) {
    if (st.bottomTab == State::BottomTab::Project) {
        st.assets.open = !st.assets.open;
    } else {
        st.bottomTab = State::BottomTab::Project;
        st.assets.open = true;
    }
}

// The bottom dock's tabs: Project, then Timeline while it is open. Each tab
// is its label and icon with padding; `r` is the strip at the left of the
// panel in front's header.
static float dockTabWidth(const char* label) { return measure(fonts().semibold, type::body, label).x + 30 + space::md * 2; }
static float dockTabsWidth(const State& st) { return dockTabWidth("Project") + (st.timeline.open ? dockTabWidth("Timeline") : 0); }
static void dockTabs(State& st, ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    float x = r.Min.x;
    auto tab = [&](const char* id, const char* label, const char* ic, ImU32 iconColor, bool front, bool lit) {
        const ImRect tr(ImVec2(x, r.Min.y), ImVec2(x + dockTabWidth(label), r.Max.y));
        x = tr.Max.x;
        Hit h = interact(id, tr);
        if (h.hover > 0 && !front) dl->AddRectFilled(ImVec2(tr.Min.x + 2, tr.Min.y + 4), ImVec2(tr.Max.x - 2, tr.Max.y - 4), color::hover, radius::field);
        const float ih = measure(f.medium, type::icon, "Ag").y, lh = measure(f.semibold, type::body, "Ag").y;
        text(dl, ImVec2(tr.Min.x + space::md, tr.Min.y + (tr.GetHeight() - ih) * 0.5f), f.medium, type::icon - 1, iconColor, ic);
        text(dl, ImVec2(tr.Min.x + space::md + 22, tr.Min.y + (tr.GetHeight() - lh) * 0.5f), f.semibold, type::body,
             front ? color::text : color::textDim, label);
        if (front && lit) dl->AddRectFilled(ImVec2(tr.Min.x + space::sm, tr.Max.y - 2), ImVec2(tr.Max.x - space::sm, tr.Max.y), color::accent, 1);
        return h.clicked;
    };
    const bool projectFront = !timelineInFront(st);
    if (tab("tab_project", "Project", st.assets.open && projectFront ? icon::folderOpen : icon::folder, rgb(0xd8b46a), projectFront,
            st.assets.open))
        toggleProjectTab(st);
    if (st.timeline.open && tab("tab_timeline", "Timeline", icon::layers, color::accentHover, !projectFront, true))
        st.bottomTab = State::BottomTab::Timeline;
}

static ImRect titleBar(State& st, ImDrawList* dl, ImVec2 size, editor::Document& doc) {
    Fonts& f = fonts();
    ImRect bar(ImVec2(0, 0), ImVec2(size.x, size::titleBar));
    dl->AddRectFilled(bar.Min, bar.Max, color::chrome);

    // App mark: the psxsplash wordmark.
    const float logoH = bar.GetHeight() - 12;
    float x = space::sm + brand::drawLogo(dl, ImVec2(space::sm, bar.Min.y + 6), logoH) + space::xs;
    const char* menus[] = {"File", "Edit", "Object", "Build", "View", "Help"};
    for (int i = 0; i < 6; ++i) {
        const char* m = menus[i];
        ImVec2 s = measure(f.medium, type::body, m);
        ImRect r(ImVec2(x, bar.Min.y + 7), ImVec2(x + s.x + space::md * 2 - 4, bar.Max.y - 7));
        g_menuRects[i] = r;
        Hit h = interact(m, r);
        nameRect(std::string("menu.") + (i == 0 ? "file" : i == 1 ? "edit" : i == 2 ? "object" : i == 3 ? "build" : i == 4 ? "view" : "help"), r);
        const bool open = st.menuOpen == i;
        if (h.clicked) st.menuOpen = open || g_menuIgnoreClick == i ? -1 : i;
        else if (st.menuOpen >= 0 && h.hovered) st.menuOpen = i;
        if (open) dl->AddRectFilled(r.Min, r.Max, color::hover, radius::button);
        else if (h.hover > 0) dl->AddRectFilled(r.Min, r.Max, rgb(0x2a2f39, (int)(255 * h.hover)), radius::button);
        textCentered(dl, r, f.medium, type::body, open || h.hover > 0.5f ? color::text : color::textDim, m);
        x = r.Max.x;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_menuIgnoreClick = -1;

    // Project / scene breadcrumb.
    x += space::lg;
    dl->AddLine(ImVec2(x, bar.Min.y + 12), ImVec2(x, bar.Max.y - 12), color::borderStrong);
    x += space::lg;
    float ty = bar.Min.y + (bar.GetHeight() - measure(f.regular, type::body, "Ag").y) * 0.5f;
    const std::string project = doc.projectRoot().empty() ? std::string("No project") : utf8(doc.projectRoot().filename());
    text(dl, ImVec2(x, ty), f.regular, type::body, color::textDim, project.c_str());
    x += measure(f.regular, type::body, project.c_str()).x + space::sm;
    text(dl, ImVec2(x, ty), f.regular, type::body, color::textFaint, "/");
    x += 12;
    const char* stem = doc.sceneStem().c_str();
    text(dl, ImVec2(x, ty), f.medium, type::body, color::text, stem);
    x += measure(f.medium, type::body, stem).x + space::sm;
    if (doc.dirty()) dl->AddCircleFilled(ImVec2(x + 3, bar.GetCenter().y + 1), 3, color::textFaint, 12);

    // The three actions a user takes every minute, centred.
    float wPlay = 0, wRun = 0, wExport = 0;
    // Measure pass: draw off-screen is not possible, so compute widths from text.
    auto width = [&](const char* ic, const char* l) {
        return space::md * 2 + measure(f.medium, type::icon, ic).x + space::sm - 2 + measure(f.medium, type::body, l).x;
    };
    const bool playing = st.play.emu.running(), building = st.play.build.valid();
    const char* playIcon = playing ? icon::square : icon::play;
    const char* playLabel = playing ? "Stop" : building ? "Building" : "Play";
    wPlay = width(playIcon, playLabel);
    const bool onHw = st.play.hw.active(), hwBuilding = st.play.hwBuild.valid();
    const char* runIcon = onHw ? icon::square : icon::cpu;
    const char* runLabel = onHw ? "Stop hardware" : hwBuilding ? "Building" : "Run on hardware";
    wRun = width(runIcon, runLabel);
    const bool exporting = st.play.disc.valid();
    const char* exportLabel = exporting ? "Exporting" : "Export";
    wExport = width(icon::package, exportLabel);
    float total = wPlay + wRun + wExport + space::sm * 2;
    float ax = std::floor(size.x * 0.5f + 90 - total * 0.5f);
    float ay = bar.Min.y + (bar.GetHeight() - (size::field + 6)) * 0.5f;
    const editor::PlayTools& tools = st.play.tools;
    const std::string playTip = st.play.emu.running() ? std::string("Stop pcsx-redux (F5)")
                                : tools.overlayPerf && tools.overlayRooms
                                    ? std::string("Build and run in pcsx-redux with the performance and room debug overlays (F5)")
                                : tools.overlayPerf  ? std::string("Build and run in pcsx-redux with the performance overlay (F5)")
                                : tools.overlayRooms ? std::string("Build and run in pcsx-redux with the room debug overlay (F5)")
                                                     : std::string("Build and run in pcsx-redux (F5)");
    if (button("play", ImVec2(ax, ay), playIcon, playLabel, ButtonKind::Primary, nullptr, playTip.c_str()))
        togglePlay(st, doc);
    if (namedRectsOn()) {
        // Scripted tests: the overlays and the build they boot, at the start and after every change.
        static std::string reported;
        const std::string ov = tools.overlayPerf && tools.overlayRooms ? "perf,rooms"
                               : tools.overlayPerf                     ? "perf"
                               : tools.overlayRooms                    ? "rooms"
                                                                       : "none";
        const std::string now = "play: overlays " + ov + " engine " +
                                utf8(editor::engineFor(editor::withDefaults(tools, st.play.bundleDir)).filename());
        if (now != reported) {
            reported = now;
            std::printf("%s\n", now.c_str());
            std::fflush(stdout);
        }
    }
    if (button("run", ImVec2(ax + wPlay + space::sm, ay), runIcon, runLabel, ButtonKind::Secondary, nullptr,
               onHw ? "Stop serving the console (Ctrl+F5)" : "Upload to a console over serial (Ctrl+F5)"))
        toggleRun(st, doc);
    if (button("export", ImVec2(ax + wPlay + wRun + space::sm * 2, ay), icon::package, exportLabel, ButtonKind::Ghost, nullptr,
               "Build a disc image (Ctrl+B)"))
        startExport(st, doc);

    // Undo / redo just left of the window controls.
    float rx = size.x - 46 * 3 - space::sm;
    if (iconButton("redo", ImRect(ImVec2(rx - 30, bar.Min.y + 6), ImVec2(rx, bar.Max.y - 6)), icon::redo, false, "Redo (Ctrl+Y)",
                   color::textDim, doc.canRedo()))
        doc.redo();
    if (iconButton("undo", ImRect(ImVec2(rx - 62, bar.Min.y + 6), ImVec2(rx - 32, bar.Max.y - 6)), icon::undo, false, "Undo (Ctrl+Z)",
                   color::textDim, doc.canUndo()))
        doc.undo();
    windowControls(st, dl, bar);
    (void)st;
    return bar;
}

// Icon and colour for an object, by what it carries.
struct ObjectLook {
    const char* icon;
    ImU32 color;
};
static ObjectLook lookOf(editor::ComponentKind k) {
    using K = editor::ComponentKind;
    switch (k) {
        case K::Mesh: return {icon::box, kind::mesh};
        case K::Collider: return {icon::square, kind::collider};
        case K::Script: return {icon::script, kind::script};
        case K::Light: return {icon::lightbulb, kind::light};
        case K::Player: return {icon::user, kind::player};
        case K::Navigation: return {icon::grid, kind::nav};
        case K::Trigger: return {icon::maximize, kind::trigger};
        case K::Interactable: return {icon::pointer, kind::interact};
        case K::Audio: return {icon::volume, kind::audio};
        case K::Skin: return {icon::play, kind::camera};
        case K::Room: return {icon::layoutTemplate, kind::room};
        case K::Portal: return {icon::link, kind::portal};
    }
    return {icon::box, color::textDim};
}
static ObjectLook lookOf(const splash::Object& o, bool expanded) {
    using K = editor::ComponentKind;
    if (o.light) return lookOf(K::Light);
    if (o.mesh) return lookOf(K::Mesh);
    if (o.script && !o.collider) return {icon::script, kind::script};
    if (o.player) return lookOf(K::Player);
    if (o.trigger) return lookOf(K::Trigger);
    if (o.interactable) return lookOf(K::Interactable);
    if (o.audio) return lookOf(K::Audio);
    if (o.portal) return lookOf(K::Portal);
    if (o.room) return lookOf(K::Room);
    if (!o.children.empty() && !o.collider) return {expanded ? icon::folderOpen : icon::folder, kind::folder};
    return {icon::box, color::textDim};
}

static std::string fileName(const std::string& projectPath) {
    return std::filesystem::path(projectPath).filename().string();
}

struct TreeCtx {
    State& st;
    editor::Document& doc;
    ImRect panel;
    float y;
    bool renameShown = false;
    std::string filter{};  // lower case; empty = every object
    int shown = 0;       // object rows drawn
    // Where a dragged row would land: as child `dropIndex` of `dropParent`,
    // shown as a box around a row (into it) or a line between rows.
    bool hasDrop = false;
    editor::ObjectPath dropParent{};
    int dropIndex = 0;
    bool dropInto = false;
    ImRect dropMark{};
};

static std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The object or something under it has `filter` (lower case) in its name.
static bool treeMatches(const splash::Object& o, const std::string& filter) {
    if (lower(o.name).find(filter) != std::string::npos) return true;
    for (const splash::Object& c : o.children)
        if (treeMatches(c, filter)) return true;
    return false;
}

static bool isUnder(const editor::ObjectPath& p, const editor::ObjectPath& ancestor) {
    return p.size() >= ancestor.size() && std::equal(ancestor.begin(), ancestor.end(), p.begin());
}

// While a row is dragged: the drop the mouse points at over row `rr`, which
// shows the object at `path` (its top quarter drops before it, the bottom
// quarter after it or, when it is open, as its first child, and the middle into it).
static void treeDropTarget(TreeCtx& c, ImRect rr, const editor::ObjectPath& path, const splash::Object& o, bool open) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (!c.st.treeDragging || m.y < rr.Min.y || m.y >= rr.Max.y || m.x < c.panel.Min.x || m.x >= c.panel.Max.x) return;
    if (isUnder(path, c.st.treeDragPath)) return;  // onto itself or inside it
    const float t = (m.y - rr.Min.y) / rr.GetHeight();
    const float indent = rr.Min.x + space::xs + static_cast<float>(path.size()) * 16.0f + 16;
    const editor::ObjectPath parent(path.begin(), path.end() - 1);
    c.hasDrop = true;
    c.dropInto = false;
    if (t < 0.25f) {
        c.dropParent = parent;
        c.dropIndex = path.back();
        c.dropMark = ImRect(ImVec2(indent, rr.Min.y - 1), ImVec2(rr.Max.x, rr.Min.y + 1));
    } else if (t >= 0.75f && open && !o.children.empty()) {
        c.dropParent = path;
        c.dropIndex = 0;
        c.dropMark = ImRect(ImVec2(indent + 16, rr.Max.y - 1), ImVec2(rr.Max.x, rr.Max.y + 1));
    } else if (t >= 0.75f) {
        c.dropParent = parent;
        c.dropIndex = path.back() + 1;
        c.dropMark = ImRect(ImVec2(indent, rr.Max.y - 1), ImVec2(rr.Max.x, rr.Max.y + 1));
    } else {
        c.dropParent = path;
        c.dropIndex = static_cast<int>(o.children.size());
        c.dropInto = true;
        c.dropMark = rr;
    }
}

// Clicks left of the chevron's right edge toggle; anywhere else selects.
static bool chevronClicked(ImRect rr, int depth) {
    return ImGui::GetIO().MousePos.x < rr.Min.x + space::xs + depth * 16.0f + 16;
}

// `ancestorOff`: an object above these is switched off, so they are off in
// the hierarchy whatever their own flag says.
static void treeObjects(TreeCtx& c, const std::vector<splash::Object>& objs, editor::ObjectPath& path,
                        bool ancestorOff = false) {
    for (size_t i = 0; i < objs.size(); ++i) {
        const splash::Object& o = objs[i];
        if (!c.filter.empty() && !treeMatches(o, c.filter)) continue;
        const bool childrenOff = ancestorOff || !o.active;  // read before any edit below
        path.push_back(static_cast<int>(i));
        // A filter shows every match with the rows above it, opened.
        bool open = !c.filter.empty() || c.doc.expanded(path);
        ObjectLook look = lookOf(o, open);
        std::string meta;
        if (o.mesh)
            if (const editor::MeshInfo* mi = c.doc.mesh(o.mesh->mesh); mi && mi->status == editor::AssetStatus::Ok)
                meta = std::to_string(mi->triangles) + " tris";
        TreeRow row;
        row.depth = static_cast<int>(path.size());
        row.icon = look.icon;
        row.iconColor = look.color;
        row.label = o.name.c_str();
        row.meta = meta.empty() ? nullptr : meta.c_str();
        row.hasChildren = !o.children.empty();
        row.expanded = open;
        row.selected = c.doc.selection() && *c.doc.selection() == path;
        row.warning = c.doc.hasWarning(o);
        row.hidden = !o.active;
        row.inheritedOff = ancestorOff;
        ImRect rr(ImVec2(c.panel.Min.x + space::xs + 2, c.y), ImVec2(c.panel.Max.x - space::xs - 2, c.y + size::row));
        ImGui::PushID(static_cast<int>(i));
        bool flipActive = false;
        Hit h = treeRow("row", rr, row, &flipActive);
        nameRect("tree." + o.name, rr);
        ++c.shown;
        if (ImGui::IsRectVisible(rr.Min, rr.Max)) {
            ImRect shown = rr;
            shown.ClipWith(c.panel);
            c.st.treeRows.push_back({shown, path});
        }
        if (c.st.treeDragging) {
            treeDropTarget(c, rr, path, o, open);
        } else if (flipActive) {
            c.doc.edit(path, [](splash::Object& ob) { ob.active = !ob.active; });
        } else if (h.clicked) {
            if (row.hasChildren && chevronClicked(rr, row.depth)) {
                if (c.filter.empty()) c.doc.toggleExpanded(path);
            } else {
                c.doc.select(path);
            }
        }
        if (!c.st.treeDragging && h.hovered && ImGui::IsMouseDoubleClicked(0) && !(row.hasChildren && chevronClicked(rr, row.depth))) {
            c.doc.select(path);
            c.st.frameRequest = true;
        }
        // Right-click: select it and open its menu.
        if (!c.st.treeDragging && h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            c.doc.select(path);
            c.st.treeMenu = 1;
            c.st.treeMenuPath = path;
            c.st.treeMenuPos = ImGui::GetIO().MousePos;
            c.st.treeMenuRight = false;
        }
        // Pressing a row and moving starts dragging it.
        if (h.held && !c.st.treeDragging && !c.st.renaming && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6)) {
            c.st.treeDragging = true;
            c.st.treeDragPath = path;
        }
        // F2: the label becomes a text field.
        if (c.st.renaming && c.st.renamePath == path) {
            c.renameShown = true;
            if (c.st.renameStart) {
                beginTextEdit("rename", o.name);
                c.st.renameStart = false;
            }
            float lx = rr.Min.x + space::xs + row.depth * 16.0f + 16 + 22;
            ImRect field(ImVec2(lx - space::xs - 2, rr.Min.y + 2), ImVec2(rr.Max.x - 2, rr.Max.y - 2));
            std::string typed;
            TextEdit res = textEdit("rename", field, fonts().medium, type::body, lx, field.Max.x - space::xs, &typed);
            if (res == TextEdit::Commit && !typed.empty() && typed != o.name)
                renameFromUi(c.doc, path, typed);
            if (res != TextEdit::Editing) c.st.renaming = false;
        }
        c.y += size::row;
        if (row.hasChildren && open) treeObjects(c, o.children, path, childrenOff);
        ImGui::PopID();
        path.pop_back();
    }
}

static splash::Vec3 spawnPoint(const viewport::Ps1View& view, const editor::Document& doc,
                               const editor::ObjectPath& parentPath);

// The Add object picker. A new object goes right after the selection, as its
// sibling, or at the end of the scene when nothing is selected. It lands on
// the viewport's pivot, the point the camera orbits and frames on.
static void addObjectPicker(State& st, editor::Document& doc, const viewport::Ps1View& view, ImVec2 pos) {
    const char* id = "##addobject";
    if (st.openAddObject) {
        st.openAddObject = false;
        st.pickQuery.clear();
        ImGui::OpenPopup(id);
    }
    if (!ImGui::IsPopupOpen(id)) return;
    const std::vector<editor::ObjectPreset>& ps = editor::presets();
    std::vector<std::pair<const char*, const char*>> keys;
    for (const editor::ObjectPreset& p : ps) keys.push_back({p.label, p.keywords});
    std::vector<int> order = editor::rank(st.pickQuery, keys);
    std::vector<PickerItem> items;
    for (int i : order) {
        ObjectLook look = ps[i].parts.empty() ? ObjectLook{icon::box, color::textDim} : lookOf(ps[i].parts.front());
        items.push_back({look.icon, look.color, ps[i].label, ps[i].blurb, nullptr});
    }
    int pick = picker(id, pos, 320, "Add object", &st.pickQuery, items);
    if (pick < 0) return;
    editor::ObjectPath at;
    if (const std::optional<editor::ObjectPath>& sel = doc.selection(); sel && doc.object(*sel)) {
        at = *sel;
        at.back() += 1;
    } else {
        at = {static_cast<int>(doc.scene().objects.size())};
    }
    const editor::ObjectPath parentPath(at.begin(), at.end() - 1);
    const splash::Object* parent = doc.object(parentPath);
    const std::vector<splash::Object>& siblings = parent ? parent->children : doc.scene().objects;
    splash::Object obj = editor::makeObject(ps[order[pick]], siblings);
    obj.transform.position = spawnPoint(view, doc, parentPath);
    if (!doc.expanded(parentPath)) doc.toggleExpanded(parentPath);
    doc.insertObject(at, std::move(obj));
}

// The Model field's file dialog, answered on its own thread and picked up next frame.
static std::string g_modelPicked;
static editor::ObjectPath g_modelTarget;

static void SDLCALL onModelPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_pickMutex);
    g_modelPicked = files[0];
}

static void notify(State& st, std::string msg, bool bad) {
    st.notice = std::move(msg);
    st.noticeBad = bad;
    st.noticeUntil = ImGui::GetTime() + 8;
    st.noticeReveal.clear();
}

// Shows a project Lua file in the script editor.
static void openScript(State& st, const editor::Document& doc, const std::string& projectPath) {
    std::string err;
    if (projectPath.empty() || !st.scripts.open(doc.resolve(projectPath), &err)) notify(st, "Cannot open " + projectPath + ": " + err, true);
}

// Copy, Cut and Paste of objects through the system clipboard, as the text
// Document::copyObject writes. Menu, row menu and shortcuts share these.
static void copySelection(State& st, editor::Document& doc, const editor::ObjectPath& path) {
    const std::string text = doc.copyObject(path);
    if (!text.empty() && !SDL_SetClipboardText(text.c_str())) notify(st, std::string("Cannot copy: ") + SDL_GetError(), true);
}

static void cutSelection(State& st, editor::Document& doc, const editor::ObjectPath& path) {
    const std::string text = doc.copyObject(path);
    if (text.empty()) return;
    // Only removed once the clipboard holds it, so a failed copy loses nothing.
    if (!SDL_SetClipboardText(text.c_str())) {
        notify(st, std::string("Cannot cut: ") + SDL_GetError(), true);
        return;
    }
    doc.cutObject(path);
}

// Pastes right after `after`, or at the end of the top level when unset.
static void pasteClipboard(State& st, editor::Document& doc, const std::optional<editor::ObjectPath>& after) {
    char* raw = SDL_GetClipboardText();
    const std::string text = raw ? raw : "";
    SDL_free(raw);
    if (!doc.pasteObjects(text, after)) notify(st, "The clipboard does not hold SplashEdit objects", true);
}

// The other asset fields' file dialogs (texture, scripts, audio, skin clips),
// answered on their own thread and applied next frame to the object they were
// opened for. A file outside the project is copied into it first.
// SceneLua and MemCardIcon: no target object. MemCardIcon replaces icon frame
// `index`, or with index -1 adds the picked files as new frames.
enum class AssetSlot { Texture, Lua, TriggerLua, Clip, SkinClips, SceneLua, MemCardIcon };
static AssetSlot g_assetSlot = AssetSlot::Texture;
static editor::ObjectPath g_assetTarget;
static int g_assetIndex = 0;
static std::vector<std::string> g_assetPicked;

static void SDLCALL onAssetPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_pickMutex);
    g_assetPicked.clear();
    for (const char* const* f = files; *f; ++f) g_assetPicked.emplace_back(*f);
}

static void pickAsset(AssetSlot slot, const editor::ObjectPath& target, int index = 0) {
    {
        std::lock_guard<std::mutex> lock(g_pickMutex);
        g_assetSlot = slot;
        g_assetTarget = target;
        g_assetIndex = index;
    }
    static const SDL_DialogFileFilter images[] = {{"Images", "png;jpg;jpeg;bmp;tga;gif"}};
    static const SDL_DialogFileFilter lua[] = {{"Lua scripts", "lua"}};
    static const SDL_DialogFileFilter wav[] = {{"WAV audio", "wav"}};
    static const SDL_DialogFileFilter anim[] = {{"Animation clips", "anim"}};
    const SDL_DialogFileFilter* filter = slot == AssetSlot::Texture || slot == AssetSlot::MemCardIcon ? images
                                         : slot == AssetSlot::Clip    ? wav
                                         : slot == AssetSlot::SkinClips ? anim
                                                                        : lua;
    SDL_ShowOpenFileDialog(onAssetPicked, nullptr, nullptr, filter, 1, nullptr,
                           slot == AssetSlot::SkinClips || (slot == AssetSlot::MemCardIcon && index < 0));
}

// Sets `files` on the field `slot` of the object at `target` (or the scene's
// settings for the scene script and memory card icon) as one undo step; a
// skin's clips are replaced, or with `append` added to (up to 16). `index` is
// the memory card icon frame to replace, or -1 to append frames; for a
// Texture, the material to set (-1 is the first), the list grown to reach it.
static void applyAssetFiles(State& st, editor::Document& doc, AssetSlot slot, const editor::ObjectPath& target,
                            const std::vector<std::string>& files, bool append = false, int index = -1) {
    const bool sceneWide = slot == AssetSlot::SceneLua || slot == AssetSlot::MemCardIcon;
    if (files.empty() || (!sceneWide && !doc.object(target))) return;
    const char* folder = slot == AssetSlot::Texture || slot == AssetSlot::MemCardIcon ? "textures"
                         : slot == AssetSlot::Clip      ? "audio"
                         : slot == AssetSlot::SkinClips ? "anims"
                                                        : "scripts";
    std::vector<std::string> paths;
    for (const std::string& file : files) {
        editor::AdoptedAsset a = editor::adoptAsset(doc.projectRoot(), fromUtf8(file), folder);
        if (!a.error.empty()) {
            notify(st, "Could not use " + utf8(fromUtf8(file).filename()) + ": " + a.error, true);
            return;
        }
        paths.push_back(a.path);
    }
    std::string note;
    if (slot == AssetSlot::SkinClips && paths.size() > 16) {
        note = "Kept the first 16 of " + std::to_string(paths.size()) + " clips; a skin plays at most 16.";
        paths.resize(16);
    }
    const std::string& first = paths.front();
    if (slot == AssetSlot::SceneLua) {
        doc.editSettings([&](splash::SceneSettings& s) { s.script = first; });
        doc.refreshAssets(paths);
        return;
    }
    if (slot == AssetSlot::MemCardIcon) {
        const size_t have = doc.scene().settings.memCard.icons.size();
        if (index < 0 && have + paths.size() > splash::kMemCardIconsMax) {
            const size_t room = have < splash::kMemCardIconsMax ? splash::kMemCardIconsMax - have : 0;
            notify(st, "Kept " + std::to_string(room) + " of " + std::to_string(paths.size()) + " images; an icon has at most 3 frames.",
                   false);
            paths.resize(room);
            if (paths.empty()) return;
        }
        doc.editSettings([&](splash::SceneSettings& s) {
            std::vector<std::string>& icons = s.memCard.icons;
            if (index < 0) icons.insert(icons.end(), paths.begin(), paths.end());
            else if (size_t(index) < icons.size()) icons[size_t(index)] = paths.front();
            else icons.push_back(paths.front());
        });
        doc.refreshAssets(paths);
        return;
    }
    doc.edit(target, [&](splash::Object& o) {
        switch (slot) {
            case AssetSlot::Texture:
                if (!o.mesh) return;
                if (o.mesh->materials.size() <= size_t(std::max(index, 0))) o.mesh->materials.resize(size_t(std::max(index, 0)) + 1);
                o.mesh->materials[size_t(std::max(index, 0))].texture = first;
                break;
            case AssetSlot::Lua:
                if (o.script) o.script->lua = first;
                break;
            case AssetSlot::TriggerLua:
                if (o.trigger) o.trigger->lua = first;
                break;
            case AssetSlot::Clip:
                if (!o.audio) return;
                o.audio->clip = first;
                if (o.audio->clipName.empty()) o.audio->clipName = utf8(fromUtf8(first).stem());
                break;
            case AssetSlot::SkinClips:
                if (!o.skin) return;
                if (!append) o.skin->clips = paths;
                for (const std::string& p : append ? paths : std::vector<std::string>{})
                    if (o.skin->clips.size() < 16 && std::find(o.skin->clips.begin(), o.skin->clips.end(), p) == o.skin->clips.end())
                        o.skin->clips.push_back(p);
                break;
            case AssetSlot::SceneLua:
            case AssetSlot::MemCardIcon:
                break;
        }
    });
    doc.endMerge();
    doc.refreshAssets(paths);
    if (!note.empty()) notify(st, note, false);
}

static void applyPickedAssets(State& st, editor::Document& doc) {
    std::vector<std::string> files;
    AssetSlot slot;
    editor::ObjectPath target;
    int index;
    {
        std::lock_guard<std::mutex> lock(g_pickMutex);
        files.swap(g_assetPicked);
        slot = g_assetSlot;
        target = g_assetTarget;
        index = g_assetIndex;
    }
    applyAssetFiles(st, doc, slot, target, files, false, index);
}

// Whether the asset field `slot` (an AssetSlot, or -1 for the Model field)
// takes a file of type `t` from the Project panel.
static bool slotTakes(int slot, editor::AssetType t) {
    using T = editor::AssetType;
    if (slot < 0) return t == T::Model || t == T::Mesh;
    switch (static_cast<AssetSlot>(slot)) {
        case AssetSlot::Texture: return t == T::Texture;
        case AssetSlot::Lua:
        case AssetSlot::TriggerLua:
        case AssetSlot::SceneLua: return t == T::Script;
        case AssetSlot::Clip: return t == T::Audio;
        case AssetSlot::SkinClips: return t == T::Animation;
        case AssetSlot::MemCardIcon: return t == T::Texture;
    }
    return false;
}

// An inspector asset field that also takes a file dragged from the Project
// panel: a drop target where it shows (not in a folded section), outlined
// while a file it takes is dragged, strongly under the pointer.
static bool slotField(State& st, int slot, const editor::ObjectPath& path, const char* id, ImRect r, const char* ic, ImU32 iconColor,
                      const char* name, const char* meta, int index = -1) {
    const bool clicked = assetField(id, r, ic, iconColor, name, meta);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect shown = r;
    shown.ClipWith(ImRect(dl->GetClipRectMin(), dl->GetClipRectMax()));
    if (shown.GetWidth() <= 0 || shown.GetHeight() <= 0) return clicked;
    st.assetSlots.push_back({shown, slot, path, index});
    if (const editor::AssetEntry* e = st.assets.dragging(); e && slotTakes(slot, e->type)) {
        const bool over = shown.Contains(ImGui::GetIO().MousePos);
        dl->AddRect(r.Min - ImVec2(1, 1), r.Max + ImVec2(1, 1), over ? color::accent : color::accentSoft, radius::field + 1, 0,
                    over ? 2.0f : 1.5f);
    }
    return clicked;
}

// The inspector's New blocks and New Lua: writes scripts/<name>.blocks or
// .lua from a starter, sets it on the object's Script component (its Trigger
// component with `trigger`, the scene's script when `object` is null) as one
// undo step and opens it in the block or script editor. An object with an
// Interactable gets the starter with onInteract(self). With `addScript` the
// object's Script component is added in the same undo step.
static void newScript(State& st, editor::Document& doc, const editor::ObjectPath* object, editor::ScriptLanguage lang,
                      bool trigger = false, bool addScript = false) {
    const splash::Object* o = object ? doc.object(*object) : nullptr;
    if (object && (!o || !(trigger ? o->trigger.has_value() : o->script.has_value() || addScript))) return;
    const editor::ScriptKind kind = !o        ? editor::ScriptKind::Scene
                                    : trigger ? editor::ScriptKind::Trigger
                                    : o->interactable ? editor::ScriptKind::Interactable
                                                      : editor::ScriptKind::Object;
    editor::NewScript ns = editor::createScript(doc.projectRoot(), o ? o->name : doc.sceneStem(), kind, lang);
    if (!ns.error.empty()) {
        notify(st, "Cannot create a script: " + ns.error, true);
        return;
    }
    if (o) {
        doc.edit(*object, [&](splash::Object& ob) {
            if (trigger) {
                ob.trigger->lua = ns.path;
            } else {
                if (!ob.script) ob.script.emplace();
                ob.script->lua = ns.path;
            }
        });
        doc.endMerge();
    } else {
        doc.editSettings([&](splash::SceneSettings& s) { s.script = ns.path; });
    }
    doc.refreshAssets({ns.path});
    openScript(st, doc, ns.path);
}

// New blocks and New Lua, side by side from `at`, moved left as far as it
// takes to end by `right`. The language of the one clicked, if one was.
static std::optional<editor::ScriptLanguage> newScriptButtons(const char* blocksId, const char* luaId, ImVec2 at, float right,
                                                               const char* blocksTip, const char* luaTip) {
    const float pair = buttonWidth(icon::plus, "New blocks") + space::xs + buttonWidth(icon::plus, "New Lua");
    at.x = std::min(at.x, std::floor(right - pair));
    std::optional<editor::ScriptLanguage> clicked;
    float bw = 0;
    if (button(blocksId, at, icon::plus, "New blocks", ButtonKind::Secondary, &bw, blocksTip)) clicked = editor::ScriptLanguage::Blocks;
    if (button(luaId, ImVec2(at.x + bw + space::xs, at.y), icon::plus, "New Lua", ButtonKind::Secondary, nullptr, luaTip))
        clicked = editor::ScriptLanguage::Lua;
    return clicked;
}

static bool isBlocksPath(const std::string& p) {
    return p.size() >= 7 && p.compare(p.size() - 7, 7, ".blocks") == 0;
}

static std::optional<std::string> readTextFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Whether the script at `lua` (a project path) defines onInteract: its tab's
// text when it is open in one, else the file. Worked out again when the tab's
// text changes, or for a file at most once a second and when its time does.
static bool interactDefined(State& st, const editor::Document& doc, const std::string& lua, bool* exists) {
    State::InteractEvent& c = st.interactEvent;
    const std::filesystem::path abs = doc.resolve(lua);
    const bool blocks = isBlocksPath(lua);
    if (std::optional<uint64_t> rev = st.scripts.tabRevision(abs)) {
        if (c.file != lua || c.tabRevision != rev) {
            c.file = lua;
            c.tabRevision = rev;
            c.exists = true;
            c.checkedAt = -1;
            c.defined = editor::scriptDefinesCallback(st.scripts.tabText(abs).value_or(std::string()), blocks, "onInteract");
        }
        *exists = c.exists;
        return c.defined;
    }
    const double now = ImGui::GetTime();
    if (c.file == lua && !c.tabRevision && c.checkedAt >= 0 && now - c.checkedAt < 1.0) {
        *exists = c.exists;
        return c.defined;
    }
    std::error_code ec;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(abs, ec);
    const bool there = !ec;
    if (c.file != lua || c.tabRevision || c.checkedAt < 0 || there != c.exists || (there && mtime != c.mtime)) {
        std::optional<std::string> text = there ? readTextFile(abs) : std::nullopt;
        c.file = lua;
        c.tabRevision.reset();
        c.mtime = there ? mtime : std::filesystem::file_time_type{};
        c.exists = there && text.has_value();
        c.defined = text && editor::scriptDefinesCallback(*text, blocks, "onInteract");
    }
    c.checkedAt = now;
    *exists = c.exists;
    return c.defined;
}

// The Interactable section's Add onInteract: an empty onInteract(self) added
// to the object's script file (a block script gets an "when interacted with"
// stack under its others), and to its tab's unsaved text when it has some;
// then the script opens.
static void addInteractEvent(State& st, editor::Document& doc, const std::string& lua) {
    const std::filesystem::path abs = doc.resolve(lua);
    const bool blocks = isBlocksPath(lua);
    std::optional<std::string> text = readTextFile(abs);
    if (!text) {
        notify(st, "Cannot read " + lua, true);
        return;
    }
    if (!editor::scriptDefinesCallback(*text, blocks, "onInteract")) {
        const std::string out = editor::addInteractCallback(*text, blocks);
        if (out.empty()) {
            notify(st, "Cannot add onInteract: " + fileName(lua) + " is not a block script that loads", true);
            return;
        }
        std::ofstream o(abs, std::ios::binary | std::ios::trunc);
        o << out;
        if (!o.flush()) {
            notify(st, "Cannot write " + lua, true);
            return;
        }
    }
    auto add = [blocks](const std::string& t) {
        if (editor::scriptDefinesCallback(t, blocks, "onInteract")) return t;
        const std::string out = editor::addInteractCallback(t, blocks);
        return out.empty() ? t : out;
    };
    if (blocks) st.scripts.rewriteUnsavedBlocks({abs}, add);
    else st.scripts.rewriteUnsaved({abs}, add);
    st.scripts.reloadFromDisk();
    doc.refreshAssets({lua});
    st.interactEvent.checkedAt = -1;
    st.interactEvent.file.clear();
    openScript(st, doc, lua);
}

// ---- File menu: projects, scenes, and the unsaved-changes prompt ----

static float centerY(ImFont* font, float size, float y0, float y1) {
    return std::floor(y0 + (y1 - y0 - measure(font, size, "Ag").y) * 0.5f);
}

// File dialogs answer on their own thread; the result is picked up next frame.
enum class FileAsk { None, NewFolder, Open, SaveAs, ExportDisc };
static std::mutex g_fileMutex;
static FileAsk g_fileAsk = FileAsk::None;
static std::string g_filePath;

static void SDLCALL onFilePicked(void* kind, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_fileMutex);
    g_fileAsk = static_cast<FileAsk>(reinterpret_cast<intptr_t>(kind));
    g_filePath = files[0];
}

static const SDL_DialogFileFilter kSceneFilter[] = {{"psxsplash scene", "scene"}};
static const SDL_DialogFileFilter kDiscFilter[] = {{"Disc image", "bin"}};

// The licence file the Disc section's Choose file... picked, until it is saved.
static std::mutex g_licenceMutex;
static std::optional<std::string> g_licencePicked;

static void SDLCALL onLicencePicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_licenceMutex);
    g_licencePicked = files[0];
}

static const SDL_DialogFileFilter kLicenceFilter[] = {{"Disc licence", "dat"}, {"All files", "*"}};

// A typed volume name character: upper-cased, dropped unless the volume name
// keeps it (splash::volumeName's rule).
static int volumeChar(ImGuiInputTextCallbackData* d) {
    if (d->EventChar >= 0x80) return 1;
    const std::string kept = splash::volumeName(std::string(1, char(d->EventChar)));
    if (kept.empty()) return 1;
    d->EventChar = ImWchar(static_cast<unsigned char>(kept[0]));
    return 0;
}

static void buildDisc(State& st, const editor::Document& doc, std::filesystem::path bin) {
    State::Play& p = st.play;
    if (bin.extension() != ".bin") bin += ".bin";
    p.discOut = bin;
    std::vector<std::filesystem::path> files = editor::buildScenes(doc.projectRoot(), openSceneFile(doc));
    p.built = {};
    p.discLaunch = launchOf(files, openSceneFile(doc));
    p.disc = std::async(std::launch::async, [files = std::move(files), open = openSceneFile(doc), scene = doc.scene(), root = doc.projectRoot(),
                                             engine = editor::withDefaults(p.tools, p.bundleDir).disc, bin,
                                             title = doc.sceneStem()] {
        editor::BuildScenes b = editor::loadBuildScenes(root, files, open, scene);
        if (!b.errors.empty()) {
            splash::DiscResult r;
            r.exported.errors = std::move(b.errors);
            return r;
        }
        return splash::exportDisc(b.pointers(), root, engine, bin, title, b.names,
                                  editor::loadVramSettings(root), editor::loadDiscSettings(root));
    });
}

static void startExport(State& st, const editor::Document& doc) {
    State::Play& p = st.play;
    if (p.disc.valid() || doc.projectRoot().empty()) return;
    if (!editor::missingDiscTools(editor::withDefaults(p.tools, p.bundleDir)).empty()) {
        p.openSetup = true;
        p.setupFor = 1;
        return;
    }
    if (!p.exportTo.empty()) {
        buildDisc(st, doc, p.exportTo);
        return;
    }
    const std::string loc = utf8(doc.projectRoot() / (doc.sceneStem() + ".bin"));
    SDL_ShowSaveFileDialog(onFilePicked, reinterpret_cast<void*>(static_cast<intptr_t>(FileAsk::ExportDisc)), nullptr, kDiscFilter,
                           1, loc.c_str());
}

// Opens the folder holding `file` in the system file manager.
static void revealInFolder(const std::filesystem::path& file) {
    scriptLine(nullptr, "reveal: " + utf8(file));
    SDL_OpenURL(editor::fileUrl(file.parent_path()).c_str());
}

// Reports a finished Export in the status bar, with a link to its folder.
static void updateExport(State& st) {
    State::Play& p = st.play;
    if (!p.disc.valid() || p.disc.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    splash::DiscResult r = p.disc.get();
    reportBuild(st, "Export", p.discLaunch, r.exported);
    if (!r.exported.ok()) {
        notify(st, editor::buildFailure("Export failed", r.exported), true);
        return;
    }
    char size[32];
    std::snprintf(size, sizeof size, "%.1f MB", r.disc.sectors * double(splash::kRawSectorSize) / (1024.0 * 1024.0));
    std::filesystem::path cue = p.discOut;
    cue.replace_extension(".cue");
    notify(st, "Disc image written: " + utf8(p.discOut.filename()) + " + " + utf8(cue.filename()) + " (" + size + ")", false);
    st.noticeReveal = p.discOut;
    st.noticeUntil = ImGui::GetTime() + 20;
    p.discWritten = p.discOut;
}

// Opens `file` as the document. The scene is parsed first, so a file that does
// not load leaves the open document as it was.
static bool openScene(State& st, editor::Document& doc, const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(file, ec);
    if (ec) abs = file;
    const std::string name = utf8(abs.filename());
    try {
        (void)splash::loadScene(abs);
    } catch (const std::exception& e) {
        notify(st, "Could not open " + name + ": " + e.what(), true);
        st.recent.remove(abs);
        return false;
    }
    st.play.emu.stop();
    st.play.gameFocus = false;
    if (auto err = doc.load(abs.parent_path(), abs.filename())) {
        notify(st, "Could not open " + name + ": " + *err, true);
        return false;
    }
    st.recent.add(abs);
    st.saveError.clear();
    notify(st, "Opened " + utf8(abs.parent_path().filename()) + " / " + name, false);
    return true;
}

static bool saveDoc(State& st, editor::Document& doc) {
    auto err = doc.save();
    st.saveError = err ? "Save failed: " + *err : std::string();
    if (err) {
        std::fprintf(stderr, "save failed: %s\n", err->c_str());
        return false;
    }
    st.recent.add(doc.scenePath());
    return true;
}

static void saveDocAs(editor::Document& doc) {
    const std::string loc = utf8(doc.projectRoot() / doc.scenePath().filename());
    SDL_ShowSaveFileDialog(onFilePicked, reinterpret_cast<void*>(static_cast<intptr_t>(FileAsk::SaveAs)), nullptr, kSceneFilter, 1,
                           doc.projectRoot().empty() ? nullptr : loc.c_str());
}

static void perform(State& st, editor::Document& doc, State::Pending what, const std::filesystem::path& path) {
    switch (what) {
        case State::Pending::New:
            SDL_ShowOpenFolderDialog(onFilePicked, reinterpret_cast<void*>(static_cast<intptr_t>(FileAsk::NewFolder)), nullptr, nullptr,
                                     false);
            break;
        case State::Pending::Open: {
            const std::string loc = utf8(doc.projectRoot());
            SDL_ShowOpenFileDialog(onFilePicked, reinterpret_cast<void*>(static_cast<intptr_t>(FileAsk::Open)), nullptr, kSceneFilter, 1,
                                   loc.empty() ? nullptr : loc.c_str(), false);
            break;
        }
        case State::Pending::Recent:
            openScene(st, doc, path);
            break;
        case State::Pending::Quit:
            st.quit = true;
            break;
        case State::Pending::None:
            break;
    }
}

// Runs `what` now, or after the unsaved-changes prompt when there are changes.
static void request(State& st, editor::Document& doc, State::Pending what, const std::filesystem::path& path = {}) {
    if (!doc.dirty() && !st.scripts.dirtyCount()) {
        perform(st, doc, what, path);
        return;
    }
    st.pending = what;
    st.pendingPath = path;
    st.openUnsaved = true;
}

// Answers from the file dialogs, applied on the UI thread.
static void filesPending(State& st, editor::Document& doc) {
    FileAsk ask;
    std::string picked;
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        ask = g_fileAsk;
        picked = std::move(g_filePath);
        g_fileAsk = FileAsk::None;
    }
    if (ask == FileAsk::None) return;
    const std::filesystem::path path = fromUtf8(picked);
    if (ask == FileAsk::Open) {
        openScene(st, doc, path);
    } else if (ask == FileAsk::NewFolder) {
        if (std::filesystem::path existing = editor::firstScene(path); !existing.empty()) {
            if (openScene(st, doc, path / existing))
                notify(st, utf8(path.filename()) + " already holds a scene, so it was opened rather than replaced", false);
            return;
        }
        editor::NewProject made = editor::createProject(path);
        if (!made.error.empty()) {
            notify(st, "Could not create the project: " + made.error, true);
            return;
        }
        if (openScene(st, doc, path / made.scene)) notify(st, "Created project " + utf8(path.filename()), false);
    } else if (ask == FileAsk::ExportDisc) {
        buildDisc(st, doc, path);
    } else if (ask == FileAsk::SaveAs) {
        std::optional<std::filesystem::path> rel = editor::sceneInProject(doc.projectRoot(), path);
        if (!rel) {
            notify(st, "Save the scene inside the project folder " + utf8(doc.projectRoot().filename()) +
                           ": its models, textures and scripts are found relative to it",
                   true);
            return;
        }
        if (auto err = doc.saveAs(*rel)) {
            st.saveError = "Save failed: " + *err;
            return;
        }
        st.saveError.clear();
        st.recent.add(doc.scenePath());
        notify(st, "Saved as " + utf8(*rel), false);
    }
}

// "Save changes to X?" before New, Open, a recent scene or quitting.
static void unsavedPrompt(State& st, editor::Document& doc, ImVec2 size) {
    const char* id = "##unsaved";
    if (st.openUnsaved) {
        st.openUnsaved = false;
        ImGui::OpenPopup(id);
    }
    const float w = 440;
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    pushPopupStyle();
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    st.unsavedShown = open;
    if (!open) return;
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float inner = w - space::lg * 2;
    float y = o.y;
    // Changed scripts are asked about with the scene.
    const int scripts = st.scripts.dirtyCount();
    const std::string what = scripts == 0  ? doc.sceneStem()
                             : !doc.dirty() ? (scripts == 1 ? std::string("1 script") : std::to_string(scripts) + " scripts")
                                            : doc.sceneStem() + (scripts == 1 ? " and 1 script" : " and " + std::to_string(scripts) + " scripts");
    const std::string title = "Save changes to " + what + "?";
    text(dl, ImVec2(o.x, y), f.semibold, type::title, color::text, title.c_str());
    y += 26;
    text(dl, ImVec2(o.x, y), f.regular, type::label, color::textDim, "Your changes are lost if you don't save them.");
    y += 34;
    const float sw = buttonWidth(nullptr, "Save"), dw = buttonWidth(nullptr, "Don't save"), cw = buttonWidth(nullptr, "Cancel");
    float bx = o.x + inner - sw;
    State::Pending go = State::Pending::None;
    bool close = false;
    if (button("save", ImVec2(bx, y), nullptr, "Save", ButtonKind::Primary) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        std::string err;
        bool saved = (!doc.dirty() || saveDoc(st, doc)) && st.scripts.saveAll(&err);
        if (!err.empty()) notify(st, "Save failed: " + err, true);
        if (saved) go = st.pending;
        close = true;
    }
    bx -= space::sm + dw;
    if (button("discard", ImVec2(bx, y), nullptr, "Don't save", ButtonKind::Secondary)) {
        go = st.pending;
        close = true;
    }
    if (button("cancel", ImVec2(o.x, y), nullptr, "Cancel", ButtonKind::Ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    (void)cw;
    y += size::field + 6;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner, y - o.y));
    if (close) {
        ImGui::CloseCurrentPopup();
        st.unsavedShown = false;
        const std::filesystem::path path = st.pendingPath;
        st.pending = State::Pending::None;
        st.pendingPath.clear();
        if (go != State::Pending::None) perform(st, doc, go, path);
    }
    ImGui::EndPopup();
}

// ---- Title-bar menus ----

struct MenuEntry {
    enum Kind { Item, Separator, Heading } kind = Item;
    std::string label, shortcut;
    bool enabled = true, checked = false;
    std::function<void()> run;
};

static MenuEntry item(std::string label, std::string shortcut, bool enabled, std::function<void()> run, bool checked = false) {
    MenuEntry e;
    e.label = std::move(label);
    e.shortcut = std::move(shortcut);
    e.enabled = enabled;
    e.checked = checked;
    e.run = std::move(run);
    return e;
}
static MenuEntry separator() {
    MenuEntry e;
    e.kind = MenuEntry::Separator;
    return e;
}
static MenuEntry heading(std::string label) {
    MenuEntry e;
    e.kind = MenuEntry::Heading;
    e.label = std::move(label);
    return e;
}

// Build > Performance / Room Debug Overlay: which psxsplash build Play and
// Run on hardware boot. Saved with the tools.
static void setOverlays(State& st, bool perf, bool rooms) {
    State::Play& p = st.play;
    p.tools.overlayPerf = perf;
    p.tools.overlayRooms = rooms;
    if (!p.settingsFile.empty()) editor::savePlayTools(p.settingsFile, p.tools);
}

static std::vector<MenuEntry> menuEntries(int which, State& st, editor::Document& doc, viewport::Ps1View& view) {
    const std::optional<editor::ObjectPath> sel = doc.selection();
    const bool hasSel = sel.has_value();
    std::vector<MenuEntry> m;
    switch (which) {
        case 0:  // File
            m.push_back(item("New Project...", "Ctrl+N", true, [&] { request(st, doc, State::Pending::New); }));
            m.push_back(item("Open...", "Ctrl+O", true, [&] { request(st, doc, State::Pending::Open); }));
            m.push_back(separator());
            m.push_back(heading("Recent"));
            if (st.recent.items().empty()) m.push_back(item("No recent scenes", "", false, nullptr));
            for (const std::filesystem::path& p : st.recent.items()) {
                std::filesystem::path scene = p;
                m.push_back(item(utf8(p.parent_path().filename()) + " / " + utf8(p.filename()), "", true,
                                 [&st, &doc, scene] { request(st, doc, State::Pending::Recent, scene); }));
            }
            m.push_back(separator());
            m.push_back(item("Save", "Ctrl+S", !doc.projectRoot().empty(), [&] { saveDoc(st, doc); }));
            m.push_back(item("Save As...", "Ctrl+Shift+S", !doc.projectRoot().empty(), [&] { saveDocAs(doc); }));
            m.push_back(separator());
            m.push_back(item("Quit", "Ctrl+Q", true, [&] { request(st, doc, State::Pending::Quit); }));
            break;
        case 1:  // Edit
            m.push_back(item("Undo", "Ctrl+Z", doc.canUndo(), [&] { doc.undo(); }));
            m.push_back(item("Redo", "Ctrl+Y", doc.canRedo(), [&] { doc.redo(); }));
            m.push_back(separator());
            if (st.scripts.uiShown()) {
                // The UI tab renames its element, not the scene object that happens to be selected.
                m.push_back(item("Rename", "F2", st.canvasView.canRename(doc), [&st, &doc] { st.canvasView.rename(doc); }));
            } else {
                m.push_back(item("Rename", "F2", hasSel, [&st, sel] {
                    st.renaming = true;
                    st.renameStart = true;
                    st.renamePath = *sel;
                }));
            }
            if (st.scripts.uiShown()) {
                // The UI tab: these act on the selected UI element.
                const bool hasEl = st.canvasView.hasElement(doc);
                m.push_back(item("Duplicate", "Ctrl+D", hasEl, [&st, &doc] { st.canvasView.duplicate(doc); }));
                m.push_back(item("Cut", "Ctrl+X", hasEl, [&st, &doc] { st.canvasView.cut(doc); }));
                m.push_back(item("Copy", "Ctrl+C", hasEl, [&st, &doc] { st.canvasView.copy(doc); }));
                m.push_back(item("Paste", "Ctrl+V", true, [&st, &doc] { st.canvasView.paste(doc, false); }));
                m.push_back(item("Delete", "Del", hasEl, [&st, &doc] { st.canvasView.remove(doc); }));
            } else {
                m.push_back(item("Duplicate", "Ctrl+D", hasSel, [&doc, sel] { doc.duplicateObject(*sel); }));
                m.push_back(item("Cut", "Ctrl+X", hasSel, [&st, &doc, sel] { cutSelection(st, doc, *sel); }));
                m.push_back(item("Copy", "Ctrl+C", hasSel, [&st, &doc, sel] { copySelection(st, doc, *sel); }));
                m.push_back(item("Paste", "Ctrl+V", true, [&st, &doc, sel] { pasteClipboard(st, doc, sel); }));
                m.push_back(item("Delete", "Del", hasSel, [&doc, sel] { doc.removeObject(*sel); }));
            }
            m.push_back(separator());
            m.push_back(item("Select None", "", hasSel, [&] { doc.select(std::nullopt); }));
            break;
        case 2:  // Object
            m.push_back(item("Add Object...", "Ctrl+A", true, [&] { st.openAddObject = true; }));
            m.push_back(item("Add Component...", "", hasSel, [&] { st.openAddComponent = true; }));
            m.push_back(separator());
            m.push_back(item("Frame Selection", "F", hasSel, [&] { st.frameRequest = true; }));
            m.push_back(item("Align with View", "Ctrl+Shift+F", hasSel, [&] { st.alignRequest = true; }));
            break;
        case 3:  // Build
            m.push_back(item(st.play.emu.running() ? "Stop" : "Play", "F5", !st.play.build.valid(), [&] { togglePlay(st, doc); }));
            m.push_back(item(st.play.hw.active() ? "Stop Hardware Run" : "Run on Hardware", "Ctrl+F5", !st.play.hwBuild.valid(),
                             [&] { toggleRun(st, doc); }));
            m.push_back(item("Export Disc Image...", "Ctrl+B", !st.play.disc.valid(), [&] { startExport(st, doc); }));
            {
                std::error_code ec;
                const bool written = !st.play.discWritten.empty() && std::filesystem::is_regular_file(st.play.discWritten, ec);
                m.push_back(item("Show Disc Image in Folder", "", written, [&] { revealInFolder(st.play.discWritten); }));
            }
            m.push_back(separator());
            m.push_back(item("Performance Overlay", "", true,
                             [&] { setOverlays(st, !st.play.tools.overlayPerf, st.play.tools.overlayRooms); },
                             st.play.tools.overlayPerf));
            m.push_back(item("Room Debug Overlay", "", true,
                             [&] { setOverlays(st, st.play.tools.overlayPerf, !st.play.tools.overlayRooms); },
                             st.play.tools.overlayRooms));
            break;
        case 4:  // View
            m.push_back(item("Hand", "Q", true, [&] { st.tool = 0; }, st.tool == 0));
            m.push_back(item("Move", "W", true, [&] { st.tool = 1; }, st.tool == 1));
            m.push_back(item("Rotate", "E", true, [&] { st.tool = 2; }, st.tool == 2));
            m.push_back(item("Scale", "R", true, [&] { st.tool = 3; }, st.tool == 3));
            m.push_back(separator());
            m.push_back(item("PS1 Look", "", true, [&] { st.viewMode = 0; }, st.viewMode == 0));
            m.push_back(item("Clean Look", "", true, [&] { st.viewMode = 1; }, st.viewMode == 1));
            m.push_back(separator());
            m.push_back(item("Snap", "", true, [&] { st.snap = !st.snap; }, st.snap));
            m.push_back(item("Orthographic", "", true, [&] { view.setOrtho(!view.ortho()); }, view.ortho()));
            {
                // Every Lua file the scene uses, to open in the script editor.
                std::vector<std::string> luas;
                auto addLua = [&](const std::string& p) {
                    if (!p.empty() && std::find(luas.begin(), luas.end(), p) == luas.end()) luas.push_back(p);
                };
                addLua(doc.scene().settings.script);
                for (const splash::FlatObject& fo : splash::flatten(doc.scene())) {
                    if (fo.object->script) addLua(fo.object->script->lua);
                    if (fo.object->trigger) addLua(fo.object->trigger->lua);
                }
                m.push_back(separator());
                m.push_back(heading("Scripts"));
                if (luas.empty()) m.push_back(item("No scripts in this scene", "", false, nullptr));
                for (const std::string& p : luas) m.push_back(item(p, "", true, [&st, &doc, p] { openScript(st, doc, p); }));
            }
            m.push_back(separator());
            m.push_back(heading("Panels"));
            m.push_back(item("Project", "", true, [&] { toggleProjectTab(st); }, st.assets.open && !timelineInFront(st)));
            m.push_back(separator());
            m.push_back(heading("Timeline"));
            m.push_back(item("Timeline", "", true, [&] { st.timeline.open = !st.timeline.open; }, st.timeline.open));
            for (const editor::SeqRef ref : editor::allSequences(doc)) {
                const char* kindName = ref.kind == editor::SeqKind::Cutscene ? "Cutscene: " : "Animation: ";
                m.push_back(item(kindName + *editor::seqName(doc, ref), "", true, [&st, &doc, ref] { st.timeline.show(doc, ref); }));
            }
            break;
        case 5:  // Help
            m.push_back(item("Documentation", "", true, [] { SDL_OpenURL("https://github.com/psxsplash/splashedit-ng#readme"); }));
            m.push_back(item("Report a Problem", "", true, [] { SDL_OpenURL("https://github.com/psxsplash/splashedit-ng/issues/new"); }));
            break;
    }
    return m;
}

// The open title-bar menu, under its label.
// Size of a menu popup holding `entries`.
static void menuSize(const std::vector<MenuEntry>& entries, float* outW, float* outH) {
    Fonts& f = fonts();
    const float pad = space::xs, rowH = size::field + 4, sepH = 9, headH = 24;
    float w = 220, h = pad * 2;
    for (const MenuEntry& e : entries) {
        if (e.kind == MenuEntry::Separator) h += sepH;
        else if (e.kind == MenuEntry::Heading) h += headH;
        else {
            h += rowH;
            w = std::max(w, 36 + measure(f.regular, type::body, e.label.c_str()).x + space::xl +
                                measure(f.regular, type::label, e.shortcut.c_str()).x + space::md);
        }
    }
    *outW = w;
    *outH = h;
}

// Draws `entries` in the popup that is open (sized by menuSize) and returns
// the action of the item clicked this frame, if any. With a `menu` name each
// item is named "<menu>/<label>" for scripted clicks (menu.build/Room Debug Overlay).
static std::function<void()> menuRows(const std::vector<MenuEntry>& entries, float w, float h, const std::string& menu = {}) {
    Fonts& f = fonts();
    const float pad = space::xs, rowH = size::field + 4, sepH = 9, headH = 24;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImVec2(w, h));
    dl->AddRectFilled(box.Min + ImVec2(0, 2), box.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
    dl->AddRectFilled(box.Min, box.Max, color::raised, radius::card);
    dl->AddRect(box.Min, box.Max, color::borderStrong, radius::card);
    float y = box.Min.y + pad;
    std::function<void()> run;
    int i = 0;
    for (const MenuEntry& e : entries) {
        ImGui::PushID(i++);
        if (e.kind == MenuEntry::Separator) {
            dl->AddLine(ImVec2(box.Min.x + space::sm, y + sepH * 0.5f), ImVec2(box.Max.x - space::sm, y + sepH * 0.5f), color::border);
            y += sepH;
        } else if (e.kind == MenuEntry::Heading) {
            text(dl, ImVec2(box.Min.x + space::md, centerY(f.medium, type::label, y, y + headH)), f.medium, type::label, color::textFaint,
                 e.label.c_str());
            y += headH;
        } else {
            ImRect ir(ImVec2(box.Min.x + pad, y), ImVec2(box.Max.x - pad, y + rowH));
            Hit hit = interact("item", ir);
            if (!menu.empty()) nameRect(menu + "/" + e.label, ir);
            const bool on = e.enabled && hit.hover > 0;
            if (on) dl->AddRectFilled(ir.Min, ir.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, hit.hover), radius::field);
            if (e.checked) dl->AddCircleFilled(ImVec2(ir.Min.x + space::md, ir.GetCenter().y), 3.5f, color::accent, 12);
            const ImU32 tc = !e.enabled ? color::textFaint : on && hit.hover > 0.5f ? color::text : color::textDim;
            text(dl, ImVec2(ir.Min.x + 28, centerY(f.regular, type::body, ir.Min.y, ir.Max.y)), f.regular, type::body, tc, e.label.c_str());
            if (!e.shortcut.empty()) {
                ImVec2 ss = measure(f.regular, type::label, e.shortcut.c_str());
                text(dl, ImVec2(ir.Max.x - space::sm - ss.x, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular, type::label,
                     color::textFaint, e.shortcut.c_str());
            }
            if (hit.clicked && e.enabled && e.run) run = e.run;
            y += rowH;
        }
        ImGui::PopID();
    }
    return run;
}

static void menus(State& st, editor::Document& doc, viewport::Ps1View& view) {
    static const char* ids[6] = {"##m_file", "##m_edit", "##m_object", "##m_build", "##m_view", "##m_help"};
    static int shown = -1;  // the menu whose popup was open last frame
    if (shown >= 0 && !ImGui::IsPopupOpen(ids[shown])) {
        // Closed by a press elsewhere: that press must not reopen it from its own label.
        if (st.menuOpen == shown && ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_menuIgnoreClick = shown;
        if (st.menuOpen == shown) st.menuOpen = -1;
        shown = -1;
    }
    // The open popup takes the hover from the title bar, so moving to another
    // label is checked against the label rects directly.
    if (shown >= 0 && st.menuOpen == shown)
        for (int j = 0; j < 6; ++j)
            if (j != shown && g_menuRects[j].Contains(ImGui::GetIO().MousePos)) st.menuOpen = j;
    if (st.menuOpen != shown) {
        // Opening a popup at the same level replaces the one that was open.
        if (st.menuOpen >= 0) ImGui::OpenPopup(ids[st.menuOpen]);
        shown = st.menuOpen;
    }
    if (shown < 0) return;

    std::vector<MenuEntry> entries = menuEntries(shown, st, doc, view);
    float w, h;
    menuSize(entries, &w, &h);
    const ImRect anchor = g_menuRects[shown];
    ImGui::SetNextWindowPos(ImVec2(anchor.Min.x, anchor.Max.y + space::xs));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    const bool open = ImGui::BeginPopup(ids[shown], ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    static const char* names[6] = {"menu.file", "menu.edit", "menu.object", "menu.build", "menu.view", "menu.help"};
    std::function<void()> run = menuRows(entries, w, h, names[shown]);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.menuOpen = -1;
    if (run) st.menuOpen = -1;
    if (st.menuOpen < 0) {
        ImGui::CloseCurrentPopup();
        shown = -1;
    }
    ImGui::EndPopup();
    if (run) run();
}

// Where a model dropped on the viewport at `pos` lands, Unity world space: on
// the first drawn surface under the pointer, else on the level plane through
// the pivot, else the pivot itself.
static splash::Vec3 dropPoint(const viewport::Ps1View& view, ImRect r, ImVec2 pos) {
    if (std::optional<viewport::Vec3> hit = view.pickPoint(pos, r.Min, r.GetSize())) return {hit->x, hit->y, -hit->z};
    viewport::Vec3 o, d, p = view.pivot();
    view.ray(pos, r.Min, r.GetSize(), &o, &d);
    if (std::fabs(d.y) > 1e-4f) {
        float t = (p.y - o.y) / d.y;
        if (t > 0) return {o.x + d.x * t, o.y + d.y * t, -(o.z + d.z * t)};
    }
    return {p.x, p.y, -p.z};
}

// The scene tree row drawn this frame under `pos`, if any.
static std::optional<editor::ObjectPath> treeRowAt(const State& st, ImVec2 pos) {
    for (const auto& [rect, path] : st.treeRows)
        if (rect.Contains(pos)) return path;
    return std::nullopt;
}

// The status bar's word on an import's warnings: one is shown whole; more are
// counted, the inspector's Import settings listing them all with their fixes.
static std::string importNote(const std::vector<std::string>& warnings) {
    if (warnings.empty()) return {};
    if (warnings.size() == 1) return ". Note: " + warnings.front();
    return ". " + std::to_string(warnings.size()) + " import warnings, see the inspector";
}

// Where a model placed now goes, worked out from `to` against the scene as it
// is now: a .blend is placed when Blender is done, after any edits made meanwhile.
static void modelPlace(editor::Document& doc, const viewport::Ps1View& view, const ModelTarget& to, editor::ObjectPath& at,
                       splash::Vec3& pos) {
    pos = {};
    if (to.under && doc.object(*to.under)) {
        at = *to.under;
        at.push_back(static_cast<int>(doc.object(*to.under)->children.size()));
    } else if (to.at) {
        at = {static_cast<int>(doc.scene().objects.size())};
        pos = *to.at;
    } else {
        if (const std::optional<editor::ObjectPath>& sel = doc.selection(); sel && doc.object(*sel)) {
            at = *sel;
            at.back() += 1;
        } else {
            at = {static_cast<int>(doc.scene().objects.size())};
        }
        pos = spawnPoint(view, doc, editor::ObjectPath(at.begin(), at.end() - 1));
    }
}

// Places a model built on the worker thread (see editor::applyPlace), as one undo step.
static void placeModelFile(State& st, editor::Document& doc, const viewport::Ps1View& view, const std::filesystem::path& src,
                           const ModelTarget& to, editor::StagedModel& staged) {
    editor::ObjectPath at;
    splash::Vec3 pos{};
    modelPlace(doc, view, to, at, pos);
    editor::PlacedModel placed = editor::applyPlace(doc, staged, to.replace, at, pos);
    if (!placed.error.empty()) {
        notify(st, "Import failed: " + placed.error, true);
        return;
    }
    // Show what was added: its parent row open.
    const editor::ObjectPath parentPath(placed.path.begin(), placed.path.end() - 1);
    if (!doc.expanded(parentPath)) doc.toggleExpanded(parentPath);
    const splash::ImportedModel& m = placed.model;
    std::string msg = "Imported " + utf8(src.filename()) + ", " + std::to_string(m.triangles) +
                      (m.triangles == 1 ? " triangle" : " triangles");
    if (m.nodes.size() > 1 && !to.replace) msg += " in " + std::to_string(m.nodes.size()) + " parts";
    if (!placed.renamed.empty()) msg += ", as " + placed.renamed + " (its name was taken in models/ while it imported)";
    msg += importNote(m.warnings);
    notify(st, msg, false);
}

// A model file dropped or picked: queued, to be imported in the background
// (see updateImports).
static void importModelFile(State& st, editor::Document& doc, const viewport::Ps1View&, const std::string& file,
                            const ModelTarget& to) {
    editor::ModelImports::Request r;
    r.file = fromUtf8(file);
    r.to = to;
    r.loadId = doc.loadId();
    st.imports.add(std::move(r));
}

// Files released over the window and files picked in the Model field's
// dialog, once this frame's inspector has placed the field.
static void importPending(State& st, editor::Document& doc, const viewport::Ps1View& view) {
    std::string picked;
    editor::ObjectPath pickedFor;
    {
        std::lock_guard<std::mutex> lock(g_pickMutex);
        picked.swap(g_modelPicked);
        pickedFor = g_modelTarget;
    }
    if (!picked.empty()) importModelFile(st, doc, view, picked, ModelTarget{pickedFor, std::nullopt, std::nullopt});
    applyPickedAssets(st, doc);
    std::vector<std::pair<std::string, ImVec2>> files;
    files.swap(st.drop.files);
    for (const auto& [file, pos] : files) {
        ModelTarget to;
        if (st.modelField.GetWidth() > 0 && st.modelField.Contains(pos))
            to.replace = st.modelFieldPath;
        else if (std::optional<editor::ObjectPath> row = treeRowAt(st, pos))
            to.under = row;
        else if (st.viewportRect.GetWidth() > 0 && st.viewportRect.Contains(pos))
            to.at = dropPoint(view, st.viewportRect, pos);
        importModelFile(st, doc, view, file, to);
    }
}

// A .mesh from the Project panel: a new object drawing it, placed the way an
// imported model is, with the mesh settings and materials of an object that
// already draws it when there is one. On the Model field, that object draws it
// instead. One undo step either way.
static void placeMesh(State& st, editor::Document& doc, const viewport::Ps1View& view, const std::string& mesh, const ModelTarget& to) {
    std::optional<splash::MeshComponent> like;
    for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
        if (fo.object->mesh && fo.object->mesh->mesh == mesh) {
            like = *fo.object->mesh;
            break;
        }
    std::vector<std::string> assets{mesh};
    if (like)
        for (const splash::Material& m : like->materials)
            if (!m.texture.empty()) assets.push_back(m.texture);
    if (to.replace && doc.object(*to.replace) && doc.object(*to.replace)->mesh) {
        doc.edit(*to.replace, [&](splash::Object& o) {
            o.mesh->mesh = mesh;
            if (like) o.mesh->materials = like->materials;
        });
        doc.endMerge();
        doc.refreshAssets(assets);
        return;
    }
    editor::ObjectPath at;
    splash::Vec3 pos{};
    modelPlace(doc, view, to, at, pos);
    const editor::ObjectPath parentPath(at.begin(), at.end() - 1);
    const splash::Object* parent = doc.object(parentPath);
    splash::Object obj;
    obj.name = editor::uniqueName(utf8(std::filesystem::path(std::u8string(mesh.begin(), mesh.end())).stem()),
                                  parent ? parent->children : doc.scene().objects);
    obj.mesh = like ? *like : splash::MeshComponent{};
    obj.mesh->mesh = mesh;
    obj.transform.position = pos;
    const std::string name = obj.name;
    if (!doc.insertObject(at, std::move(obj))) {
        notify(st, "There is no object to put " + mesh + " in", true);
        return;
    }
    doc.refreshAssets(assets);
    if (!doc.expanded(parentPath)) doc.toggleExpanded(parentPath);
    notify(st, "Added " + name, false);
}

// Where a model or mesh dragged from the Project panel would land: the Model
// field, a scene tree row (as its child) or the viewport (at the surface
// under the pointer). Nothing elsewhere.
static std::optional<ModelTarget> modelTargetAt(State& st, const viewport::Ps1View& view, ImVec2 pos) {
    ModelTarget to;
    for (const State::AssetSlotTarget& t : st.assetSlots)
        if (t.slot < 0 && t.rect.Contains(pos)) {
            to.replace = t.path;
            return to;
        }
    if (std::optional<editor::ObjectPath> row = treeRowAt(st, pos)) {
        to.under = row;
        return to;
    }
    if (st.viewportRect.GetWidth() > 0 && st.viewportRect.Contains(pos)) {
        to.at = dropPoint(view, st.viewportRect, pos);
        return to;
    }
    return std::nullopt;
}

// While a model or mesh is dragged out of the Project panel: the tree row or
// the viewport it would land on, outlined, and a hint saying so.
static void assetDragHint(State& st, ImDrawList* dl, const editor::Document& doc) {
    const editor::AssetEntry* e = st.assets.dragging();
    if (!e || (e->type != editor::AssetType::Model && e->type != editor::AssetType::Mesh)) return;
    const ImVec2 pos = ImGui::GetIO().MousePos;
    std::string hint;
    ImRect zone;
    for (const State::AssetSlotTarget& t : st.assetSlots)
        if (t.slot < 0 && t.rect.Contains(pos)) return;  // the Model field outlines itself
    if (std::optional<editor::ObjectPath> rowPath = treeRowAt(st, pos)) {
        for (const auto& [rect, path] : st.treeRows)
            if (path == *rowPath && doc.object(path)) {
                zone = rect;
                hint = "Drop to add " + e->name + " under " + doc.object(path)->name;
            }
    } else if (st.viewportRect.GetWidth() > 0 && st.viewportRect.Contains(pos)) {
        zone = st.viewportRect;
        hint = "Drop to add " + e->name + " to the scene";
    }
    if (hint.empty()) return;
    dl->AddRect(zone.Min, zone.Max, color::accent, radius::card, 0, 2.0f);
    if (zone.GetHeight() < 100) return;
    Fonts& f = fonts();
    ImVec2 hs = measure(f.medium, type::body, hint.c_str());
    ImRect pill(ImVec2((zone.Min.x + zone.Max.x - hs.x) * 0.5f - space::md, zone.Max.y - 56),
                ImVec2((zone.Min.x + zone.Max.x + hs.x) * 0.5f + space::md, zone.Max.y - 24));
    dl->AddRectFilled(pill.Min, pill.Max, color::raised, radius::pill);
    dl->AddRect(pill.Min, pill.Max, color::accent, radius::pill);
    textCentered(dl, pill, f.medium, type::body, color::text, hint.c_str());
}

// What the Project panel asked for this frame: files dropped on the scene or
// an asset field, opened, reimported, or an object to show.
static void assetRequests(State& st, editor::Document& doc, const viewport::Ps1View& view) {
    using T = editor::AssetType;
    for (const ui::AssetBrowser::Request& r : st.assets.takeRequests()) {
        const std::filesystem::path abs = doc.resolve(r.path);
        switch (r.kind) {
            case ui::AssetBrowser::Request::Drop: {
                const State::AssetSlotTarget* slot = nullptr;
                for (const State::AssetSlotTarget& t : st.assetSlots)
                    if (t.rect.Contains(r.pos)) slot = &t;
                if (slot && slot->slot >= 0) {
                    if (slotTakes(slot->slot, r.type))
                        applyAssetFiles(st, doc, static_cast<AssetSlot>(slot->slot), slot->path, {utf8(abs)}, true, slot->index);
                    else {
                        std::string what = editor::assetTypeName(r.type, true);
                        for (char& c : what) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        notify(st, "That field does not take " + what, true);
                    }
                    break;
                }
                if (r.type != T::Model && r.type != T::Mesh) break;
                std::optional<ModelTarget> to = modelTargetAt(st, view, r.pos);
                if (!to) break;
                if (r.type == T::Model) importModelFile(st, doc, view, utf8(abs), *to);
                else placeMesh(st, doc, view, r.path, *to);
                break;
            }
            case ui::AssetBrowser::Request::Open:
                if (r.type == T::Script) openScript(st, doc, r.path);
                else if (r.type == T::Scene) request(st, doc, State::Pending::Recent, abs);
                else if (r.type == T::Model) importModelFile(st, doc, view, utf8(abs), ModelTarget{});
                else if (r.type == T::Mesh) placeMesh(st, doc, view, r.path, ModelTarget{});
                else SDL_OpenURL(editor::fileUrl(abs).c_str());
                break;
            case ui::AssetBrowser::Request::Reimport:
                st.reimports.push_back({abs, std::nullopt});
                break;
            case ui::AssetBrowser::Request::Select:
                if (doc.object(r.object)) {
                    for (size_t n = 1; n < r.object.size(); ++n) {
                        const editor::ObjectPath parent(r.object.begin(), r.object.begin() + static_cast<std::ptrdiff_t>(n));
                        if (!doc.expanded(parent)) doc.toggleExpanded(parent);
                    }
                    doc.select(r.object);
                    st.frameRequest = true;
                }
                break;
            case ui::AssetBrowser::Request::Notice:
                notify(st, r.text, r.bad);
                break;
        }
    }
}

// The menu a right-click on a row (kind 1) or the panel's ellipsis (kind 2) opens.
static std::vector<MenuEntry> treeMenuEntries(int kind, State& st, editor::Document& doc) {
    std::vector<MenuEntry> m;
    if (kind == 1) {
        const editor::ObjectPath p = st.treeMenuPath;
        if (!doc.object(p)) return m;
        m.push_back(item("Rename", "F2", true, [&st, p] {
            st.renaming = true;
            st.renameStart = true;
            st.renamePath = p;
        }));
        m.push_back(item("Duplicate", "Ctrl+D", true, [&doc, p] { doc.duplicateObject(p); }));
        m.push_back(item("Cut", "Ctrl+X", true, [&st, &doc, p] { cutSelection(st, doc, p); }));
        m.push_back(item("Copy", "Ctrl+C", true, [&st, &doc, p] { copySelection(st, doc, p); }));
        m.push_back(item("Paste", "Ctrl+V", true, [&st, &doc, p] { pasteClipboard(st, doc, p); }));
        m.push_back(item("Delete", "Del", true, [&doc, p] { doc.removeObject(p); }));
        m.push_back(separator());
        // Out of its group, to the top level right after the group it was in.
        m.push_back(item("Move to Top Level", "", p.size() > 1, [&doc, p] { doc.moveObject(p, {}, p.front() + 1); }));
        m.push_back(item("Frame in View", "F", true, [&st] { st.frameRequest = true; }));
        const bool pinned = doc.pinned() && *doc.pinned() == p;
        m.push_back(item("Pin in Inspector", "", true, [&doc, p, pinned] { doc.pin(pinned ? std::nullopt : std::optional(p)); }, pinned));
    } else {
        m.push_back(item("Add Object...", "Ctrl+A", true, [&st] { st.openAddObject = true; }));
        m.push_back(separator());
        m.push_back(item("Expand All", "", true, [&doc] { doc.setAllExpanded(true); }));
        m.push_back(item("Collapse All", "", true, [&doc] { doc.setAllExpanded(false); }));
        m.push_back(separator());
        m.push_back(item("Filter...", "Ctrl+F", true, [&st] { st.focusTreeFilter = true; }));
        m.push_back(item("Clear Filter", "", !st.treeFilter.empty(), [&st] { st.treeFilter.clear(); }));
    }
    return m;
}

static void treeMenu(State& st, editor::Document& doc) {
    static const char* id = "##treemenu";
    if (st.treeMenu) {
        st.treeMenuShown = st.treeMenu;
        st.treeMenu = 0;
        ImGui::OpenPopup(id);
    }
    if (!ImGui::IsPopupOpen(id)) return;
    std::vector<MenuEntry> entries = treeMenuEntries(st.treeMenuShown, st, doc);
    float w, h;
    menuSize(entries, &w, &h);
    // Kept on screen, flipped up or left when it would run off.
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    ImVec2 pos = st.treeMenuPos;
    if (st.treeMenuRight) pos.x -= w;
    if (pos.x + w > d.x - space::xs) pos.x = st.treeMenuPos.x - w;
    if (pos.y + h > d.y - space::xs) pos.y -= h;
    pos.x = std::max(pos.x, space::xs);
    pos.y = std::max(pos.y, space::xs);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    const bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    std::function<void()> run = menuRows(entries, w, h);
    if (run || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || entries.empty()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (run) run();
}

static void sceneTree(State& st, ImDrawList* dl, ImRect r, editor::Document& doc, const viewport::Ps1View& view) {
    st.treeRows.clear();
    panel(dl, r);
    std::string count = std::to_string(doc.objectCount()) + (doc.objectCount() == 1 ? " object" : " objects");
    float y = panelHeader(dl, r, "Scene", count.c_str());
    ImRect addBtn(ImVec2(r.Max.x - 62, r.Min.y + 5), ImVec2(r.Max.x - 36, r.Min.y + 29));
    if (iconButton("addobj", addBtn, icon::plus, false, "Add object (Ctrl+A)")) st.openAddObject = true;
    addObjectPicker(st, doc, view, ImVec2(addBtn.Min.x, addBtn.Max.y + space::xs));
    ImRect menuBtn(ImVec2(r.Max.x - 32, r.Min.y + 5), ImVec2(r.Max.x - 6, r.Min.y + 29));
    if (iconButton("treemenu", menuBtn, icon::ellipsis, false, "More")) {
        st.treeMenu = 2;
        st.treeMenuPos = ImVec2(menuBtn.Max.x, menuBtn.Max.y + space::xs);
        st.treeMenuRight = true;
    }
    searchField("treesearch", ImRect(ImVec2(r.Min.x + space::sm, y), ImVec2(r.Max.x - space::sm, y + 28)), "Filter objects", "Ctrl F",
                &st.treeFilter, st.focusTreeFilter);
    st.focusTreeFilter = false;
    y += 28 + space::sm;

    ImRect foot(ImVec2(r.Min.x, r.Max.y - 40), r.Max);
    ImGui::PushClipRect(ImVec2(r.Min.x, y), ImVec2(r.Max.x, foot.Min.y), true);
    const editor::ObjectPath root;
    TreeCtx ctx{st, doc, r, y + size::row};
    ctx.filter = lower(st.treeFilter);
    bool rootOpen = !ctx.filter.empty() || doc.expanded(root);
    TreeRow top{0, icon::layers, color::accentHover, doc.sceneStem().c_str(), nullptr, !doc.scene().objects.empty(), rootOpen};
    ImRect rr(ImVec2(r.Min.x + space::xs + 2, y), ImVec2(r.Max.x - space::xs - 2, y + size::row));
    Hit h = treeRow("root", rr, top);
    if (h.clicked && !st.treeDragging) {
        if (top.hasChildren && chevronClicked(rr, 0)) {
            if (ctx.filter.empty()) doc.toggleExpanded(root);
        } else {
            doc.select(std::nullopt);
        }
    }
    if (!st.treeDragging && h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        st.treeMenu = 2;
        st.treeMenuPos = ImGui::GetIO().MousePos;
        st.treeMenuRight = false;
    }
    editor::ObjectPath path;
    if (rootOpen) treeObjects(ctx, doc.scene().objects, path);
    if (st.renaming && !ctx.renameShown) st.renaming = false;  // the row is collapsed away or gone
    if (!ctx.filter.empty() && ctx.shown == 0) {
        const char* none = "No objects match";
        text(dl, ImVec2(r.Min.x + space::md + 16, centerY(fonts().regular, type::body, ctx.y, ctx.y + size::row)), fonts().regular,
             type::body, color::textFaint, none);
    }

    // Dragging a row: the scene row or the empty space under the rows drops it at the top level, last.
    if (st.treeDragging) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const bool inTree = m.x >= r.Min.x && m.x < r.Max.x && m.y >= y && m.y < foot.Min.y;
        if (!ctx.hasDrop && inTree && (m.y < y + size::row || m.y >= ctx.y)) {
            ctx.hasDrop = true;
            ctx.dropParent.clear();
            ctx.dropIndex = static_cast<int>(doc.scene().objects.size());
            ctx.dropInto = m.y < y + size::row;
            ctx.dropMark = ctx.dropInto ? rr : ImRect(ImVec2(rr.Min.x + space::xs + 16, ctx.y - 1), ImVec2(rr.Max.x, ctx.y + 1));
        }
        // A drop that leaves the object where it is shows nothing.
        if (ctx.hasDrop) {
            const editor::ObjectPath& from = st.treeDragPath;
            const editor::ObjectPath fromParent(from.begin(), from.end() - 1);
            if (ctx.dropParent == fromParent && (ctx.dropIndex == from.back() || ctx.dropIndex == from.back() + 1)) ctx.hasDrop = false;
        }
        if (ctx.hasDrop) {
            if (ctx.dropInto) dl->AddRect(ctx.dropMark.Min, ctx.dropMark.Max, color::accent, radius::field, 0, 1.5f);
            else dl->AddRectFilled(ctx.dropMark.Min, ctx.dropMark.Max, color::accent, 1);
        }
    }
    ImGui::PopClipRect();

    if (st.treeDragging) {
        const splash::Object* dragged = doc.object(st.treeDragPath);
        if (dragged) {
            // The dragged object's name follows the pointer.
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 m = ImGui::GetIO().MousePos + ImVec2(14, 4);
            ImVec2 ts = measure(fonts().regular, type::body, dragged->name.c_str());
            ImRect tag(m, m + ts + ImVec2(space::md * 2, space::xs * 2));
            fg->AddRectFilled(tag.Min, tag.Max, color::raised, radius::field);
            fg->AddRect(tag.Min, tag.Max, ctx.hasDrop ? color::accent : color::borderStrong, radius::field);
            text(fg, tag.Min + ImVec2(space::md, space::xs), fonts().regular, type::body, color::text, dragged->name.c_str());
        }
        if (!dragged || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            st.treeDragging = false;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            st.treeDragging = false;
            if (ctx.hasDrop)
                if (std::optional<editor::ObjectPath> to = doc.moveObject(st.treeDragPath, ctx.dropParent, ctx.dropIndex))
                    // Open the rows above it so it stays in sight.
                    for (size_t n = 0; n < to->size(); ++n) {
                        const editor::ObjectPath up(to->begin(), to->begin() + static_cast<std::ptrdiff_t>(n));
                        if (!doc.expanded(up)) doc.toggleExpanded(up);
                    }
        }
    }
    treeMenu(st, doc);

    // Footer: the project's folder; a click shows or hides the Project panel.
    dl->AddLine(ImVec2(foot.Min.x + 1, foot.Min.y), ImVec2(foot.Max.x - 1, foot.Min.y), color::border);
    std::string files = std::to_string(doc.projectFileCount()) + (doc.projectFileCount() == 1 ? " file" : " files");
    TreeRow assets{0, st.assets.open ? icon::folderOpen : icon::folder, kind::folder, "Assets", files.c_str(), true, st.assets.open};
    if (treeRow("assets", ImRect(ImVec2(foot.Min.x + 6, foot.Min.y + 7), ImVec2(foot.Max.x - 6, foot.Max.y - 7)), assets).clicked)
        toggleProjectTab(st);
}

static void selectionOutline(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, viewport::Vec3 a, viewport::Vec3 b,
                             ImU32 col = rgb(0xb3a8ff, 230), float thick = 1.5f) {
    viewport::Vec3 p[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z},
                           {a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
    ImVec2 s[8];
    for (int i = 0; i < 8; ++i)
        if (!view.project(p[i], mn, sz, &s[i])) return;
    const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& ed : e) dl->AddLine(s[ed[0]], s[ed[1]], col, thick);
}

// A clickable marker for an object without geometry. Returns true when clicked.
static bool sceneIcon(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, viewport::Vec3 p, const char* ic, ImU32 col) {
    ImVec2 c;
    if (!view.project(p, mn, sz, &c)) return false;
    if (!ImRect(mn, mn + sz).Contains(c)) return false;
    Hit h = interact("marker", ImRect(c - ImVec2(13, 13), c + ImVec2(13, 13)));
    dl->AddCircleFilled(c, 13, rgb(0x0e1014, 200), 24);
    // The ring brightens on hover.
    ImU32 ringA = (ImU32)(0x90 + (0xff - 0x90) * h.hover);
    dl->AddCircle(c, 13, (col & 0x00ffffffu) | (ringA << IM_COL32_A_SHIFT), 24, 1.2f + 0.6f * h.hover);
    textCentered(dl, ImRect(c - ImVec2(13, 13), c + ImVec2(13, 13)), fonts().medium, type::icon - 1, col, ic);
    return h.clicked;
}

// Unity's scene gizmo. Clicking an axis tip (or the faint opposite end)
// looks down that axis from its side, orthographic; the centre toggles
// perspective and orthographic. The caption names the view.
static void axisWidget(ImDrawList* dl, ImVec2 c, viewport::Ps1View& view) {
    Fonts& f = fonts();
    Hit h = interact("##axiswidget", ImRect(c - ImVec2(34, 34), c + ImVec2(34, 34)));
    ImVec2 m = ImGui::GetIO().MousePos;
    dl->AddCircleFilled(c, 34, rgb(0x0e1014, 140), 40);
    // Unity's axes (Z negated into GL) as the camera sees them.
    viewport::Vec3 s, u, fw;
    view.basis(&s, &u, &fw);
    struct A {
        ImVec2 d;
        ImU32 col;
        const char* l;
        float depth;
        int axis;
    } axes[3];
    const viewport::Vec3 w[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    const ImU32 cols[3] = {color::axisX, color::axisY, color::axisZ};
    const char* labels[3] = {"X", "Y", "Z"};
    auto d3 = [](viewport::Vec3 a, viewport::Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    for (int i = 0; i < 3; ++i) axes[i] = {ImVec2(d3(w[i], s), -d3(w[i], u)) * 0.82f, cols[i], labels[i], d3(w[i], fw), i};
    // What the mouse is over: an axis tip (sign +-1), the centre (axis 3), or nothing.
    int hotAxis = -1, hotSign = 0;
    // The centre wins over a tip pointing straight at the camera, which would only re-pick the current view.
    if ((h.hovered || h.held) && std::hypot(m.x - c.x, m.y - c.y) < 7) hotAxis = 3;
    else if (h.hovered || h.held) {
        float best = 1e9f;
        for (const A& a : axes)
            for (int sign : {1, -1}) {
                ImVec2 p = c + a.d * (24.0f * sign);
                float dist = std::hypot(m.x - p.x, m.y - p.y);
                if (dist < (sign > 0 ? 9.0f : 7.0f) && dist < best) best = dist, hotAxis = a.axis, hotSign = sign;
            }
    }
    if (h.clicked && hotAxis == 3) view.setOrtho(!view.ortho());
    if (h.clicked && hotAxis >= 0 && hotAxis < 3) {
        viewport::Vec3 a = w[hotAxis];
        view.lookAlong({-a.x * hotSign, -a.y * hotSign, -a.z * hotSign});
        view.setOrtho(true);
    }
    // Far axes first, so the nearer ones overlap them.
    std::sort(std::begin(axes), std::end(axes), [](const A& a, const A& b) { return a.depth > b.depth; });
    for (auto& a : axes) {
        ImVec2 tip = c + a.d * 24;
        bool hot = hotAxis == a.axis;
        dl->AddLine(c, tip, a.col, 2);
        dl->AddCircleFilled(tip, hot && hotSign > 0 ? 9.5f : 8, a.col, 20);
        textCentered(dl, ImRect(tip - ImVec2(8, 8), tip + ImVec2(8, 8)), f.semibold, type::caption - 1, rgb(0x111318), a.l);
        dl->AddCircleFilled(c - a.d * 24, hot && hotSign < 0 ? 6.5f : 5, hot && hotSign < 0 ? a.col : a.col & 0x70ffffff, 16);
    }
    dl->AddCircleFilled(c, 5, hotAxis == 3 ? rgb(0xffffff) : rgb(0xd8dbe2, 200), 16);
    // Caption: the named side when looking straight down an axis, else Persp / Iso.
    const char* name = view.ortho() ? "Iso" : "Persp";
    static const char* sides[3][2] = {{"Right", "Left"}, {"Top", "Bottom"}, {"Back", "Front"}};
    for (int i = 0; i < 3; ++i) {
        float k = d3(w[i], fw);
        if (view.ortho() && std::fabs(k) > 0.9999f) name = sides[i][k < 0 ? 0 : 1];
    }
    // A backing pill keeps the caption legible over a light fog colour.
    const float tw = f.medium->CalcTextSizeA(type::caption, FLT_MAX, 0, name).x;
    dl->AddRectFilled(c + ImVec2(-tw / 2 - 6, 38), c + ImVec2(tw / 2 + 6, 54), rgb(0x111318, 170), 8);
    textCentered(dl, ImRect(c + ImVec2(-34, 38), c + ImVec2(34, 54)), f.medium, type::caption, color::textDim, name);
}

// Scene data is Unity-space (Y-up, left-handed, +Z into the back wall); the
// renderer is right-handed GL, so world points cross over by negating Z.
static viewport::Vec3 toGl(splash::Vec3 p) { return {p.x, p.y, -p.z}; }

// The BVH level the viewport shows down to: the State's depth clamped to the
// tree, -1 meaning the deepest.
static int bvhShownDepth(const State& st, const splash::ExportStats& s) {
    return st.bvhDepth < 0 ? s.bvhDepth : std::min(st.bvhDepth, s.bvhDepth);
}

// The BVH's boxes down to `depth`, coloured by level; leaves are brighter.
// Returns how many boxes it drew (offscreen ones included).
static int bvhOverlay(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, const splash::ExportStats& s, int depth) {
    static const ImU32 palette[] = {0x5ad1ff, 0xffb347, 0x7bed9f, 0xff6b9d, 0xc89bff, 0xf9f871};
    int drawn = 0;
    for (const splash::ExportStats::BvhBox& b : s.bvhBoxes) {
        if (b.depth > depth) continue;
        const ImU32 col = rgb(palette[size_t(b.depth) % (sizeof palette / sizeof palette[0])], b.leaf ? 230 : 120);
        selectionOutline(dl, view, mn, sz, toGl(b.lo), toGl(b.hi), col, b.leaf ? 1.4f : 1.0f);
        drawn++;
    }
    return drawn;
}

static float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    ImVec2 ab = b - a, ap = p - a;
    float len2 = ab.x * ab.x + ab.y * ab.y;
    float t = len2 > 0 ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
    ImVec2 d = ap - ab * t;
    return std::sqrt(d.x * d.x + d.y * d.y);
}

// Inside a convex quad, either winding.
static bool inQuad(ImVec2 p, const ImVec2 q[4]) {
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; ++i) {
        ImVec2 a = q[i], b = q[(i + 1) % 4];
        float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        (c >= 0 ? pos : neg)++;
    }
    return pos == 0 || neg == 0;
}

// Maps a world-space offset into the local space of an object whose parent
// has `parentToWorld` (the inverse of its 3x3 part). Identity when singular.
static splash::Vec3 worldToParentDelta(const splash::Mat34* parentToWorld, splash::Vec3 d) {
    if (!parentToWorld) return d;
    const float(*m)[4] = parentToWorld->m;
    float a = m[0][0], b = m[0][1], c = m[0][2], e = m[1][0], f = m[1][1], g = m[1][2], h = m[2][0], i = m[2][1], k = m[2][2];
    float det = a * (f * k - g * i) - b * (e * k - g * h) + c * (e * i - f * h);
    if (std::fabs(det) < 1e-12f) return d;
    float inv = 1.0f / det;
    return {((f * k - g * i) * d.x + (c * i - b * k) * d.y + (b * g - c * f) * d.z) * inv,
            ((g * h - e * k) * d.x + (a * k - c * h) * d.y + (c * e - a * g) * d.z) * inv,
            ((e * i - f * h) * d.x + (b * h - a * i) * d.y + (a * f - b * e) * d.z) * inv};
}

// The viewport pivot in the local space of the object at `parentPath` (the
// scene root when the path is empty).
static splash::Vec3 spawnPoint(const viewport::Ps1View& view, const editor::Document& doc,
                               const editor::ObjectPath& parentPath) {
    viewport::Vec3 p = view.pivot();
    splash::Vec3 w{p.x, p.y, -p.z};
    const splash::Object* parent = doc.object(parentPath);
    if (!parent) return w;
    for (const splash::FlatObject& pf : splash::flatten(doc.scene()))
        if (pf.object == parent) return worldToParentDelta(&pf.localToWorld, w - pf.localToWorld.position());
    return w;
}

static float& component(splash::Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// Where the mouse ray meets the horizontal plane at height `y` (Unity space).
static bool mouseOnPlaneY(const viewport::Ps1View& view, ImRect r, float y, splash::Vec3* hit) {
    viewport::Vec3 o, d;
    view.ray(ImGui::GetIO().MousePos, r.Min, r.GetSize(), &o, &d);
    if (std::fabs(d.y) < 1e-4f) return false;
    float t = (y - o.y) / d.y;
    if (t <= 0) return false;
    *hit = {o.x + d.x * t, o.y + d.y * t, -(o.z + d.z * t)};
    return true;
}

// Where a gizmo sits: the selected object's projected origin and the world
// length that spans ~76 px on screen at its depth, so every gizmo keeps one
// screen size however far away the object is.
struct GizmoFrame {
    splash::Vec3 wpos;   // object origin, Unity world space
    ImVec2 c;            // its projection
    float len = 0;       // world length of a 76 px arm
    bool visible = false;
};

static GizmoFrame gizmoFrame(const viewport::Ps1View& view, ImRect r, const splash::FlatObject& fo) {
    GizmoFrame gf;
    gf.wpos = fo.localToWorld.position();
    const viewport::Vec3 o = toGl(gf.wpos);
    viewport::Vec3 right, up, fwd, eye = view.eye();
    view.basis(&right, &up, &fwd);
    float depth = (o.x - eye.x) * fwd.x + (o.y - eye.y) * fwd.y + (o.z - eye.z) * fwd.z;
    gf.visible = depth > 0.05f && view.project(o, r.Min, r.GetSize(), &gf.c);
    gf.len = 76.0f * view.worldPerPixel(depth, r.GetHeight());
    return gf;
}

// A gizmo may take hover only when nothing else holds or hovers the mouse.
static bool gizmoMayHover(bool active, ImRect r) {
    ImGuiContext& g = *GImGui;
    // Alt+LMB orbits the camera, so gizmos let go of the mouse while Alt is down.
    return !active && g.ActiveId == 0 && g.HoveredId == 0 && !ImGui::GetIO().KeyAlt && ImGui::IsWindowHovered() &&
           r.Contains(ImGui::GetIO().MousePos);
}

static float length(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// The readout next to the cursor while a gizmo drags: an axis tag in its
// colour, then the value. It sits on the cursor's side away from the gizmo's
// centre `c`, so it does not cover the handles, and inside the viewport.
static void gizmoLabel(ImDrawList* dl, ImRect r, ImVec2 c, ImVec2 at, const char* tag, ImU32 tagCol, const char* value) {
    Fonts& f = fonts();
    ImVec2 ts = measure(f.semibold, type::label, tag), vs = measure(f.medium, type::label, value);
    const float padX = 8, gap = 6, h = 22;
    float w = padX + ts.x + gap + vs.x + padX;
    const bool left = at.x < c.x, above = at.y < c.y;
    ImVec2 p(left ? at.x - 18 - w : at.x + 18, above ? at.y - 14 - h : at.y + 14);
    if (p.x + w > r.Max.x - 4) p.x = at.x - 18 - w;
    if (p.x < r.Min.x + 4) p.x = at.x + 18;
    if (p.y + h > r.Max.y - 4) p.y = at.y - 14 - h;
    if (p.y < r.Min.y + 4) p.y = at.y + 14;
    ImRect b(p, p + ImVec2(w, h));
    dl->AddRectFilled(b.Min + ImVec2(0, 1), b.Max + ImVec2(0, 1), rgb(0x000000, 70), radius::button);
    dl->AddRectFilled(b.Min, b.Max, rgb(0x0e1014, 225), radius::button);
    dl->AddRect(b.Min, b.Max, rgb(0xffffff, 18), radius::button);
    text(dl, ImVec2(b.Min.x + padX, b.Min.y + (h - ts.y) * 0.5f), f.semibold, type::label, tagCol, tag);
    text(dl, ImVec2(b.Min.x + padX + ts.x + gap, b.Min.y + (h - vs.y) * 0.5f), f.medium, type::label, color::text, value);
}

// The move gizmo: three world-axis arrows and an XZ plane handle, a fixed
// size on screen. Dragging an arrow moves along it by the mouse motion
// projected onto the arrow; dragging the plane follows the mouse across the
// horizontal plane through the object. One drag is one undo step.
static void moveGizmo(State& st, ImDrawList* dl, viewport::Ps1View& view, ImRect r, editor::Document& doc, const splash::FlatObject& fo,
                      const splash::Mat34* parentToWorld) {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID gid = ImGui::GetID("##gizmo");
    ImVec2 mn = r.Min, sz = r.GetSize();
    State::GizmoDrag& drag = st.gizmo;
    const bool active = g.ActiveId == gid && drag.handle >= 0;
    if (active) ImGui::KeepAliveID(gid);

    // Handle geometry, sized from the object's depth so it stays ~76 px long.
    const GizmoFrame gf = gizmoFrame(view, r, fo);
    const splash::Vec3 wpos = gf.wpos;
    const ImVec2 c = gf.c;
    const bool visible = gf.visible;
    const float len = gf.len;
    const splash::Vec3 axisDirs[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const ImU32 axisCols[3] = {color::axisX, color::axisY, color::axisZ};
    ImVec2 tips[3];
    bool tipOk[3] = {};
    ImVec2 quad[4];
    bool quadOk = false;
    if (visible) {
        for (int i = 0; i < 3; ++i) {
            viewport::Vec3 t = toGl(wpos + axisDirs[i] * len);
            tipOk[i] = view.project(t, mn, sz, &tips[i]);
            if (tipOk[i]) {
                ImVec2 d = tips[i] - c;
                tipOk[i] = d.x * d.x + d.y * d.y >= 1;
            }
        }
        const float pl = len * 0.32f;
        quadOk = view.project(toGl(wpos + splash::Vec3{pl, 0, 0}), mn, sz, &quad[1]) &&
                 view.project(toGl(wpos + splash::Vec3{pl, 0, pl}), mn, sz, &quad[2]) &&
                 view.project(toGl(wpos + splash::Vec3{0, 0, pl}), mn, sz, &quad[3]);
        quad[0] = c;
    }

    // Hover: arrows over the plane, nearest arrow wins.
    int hover = -1;
    if (visible && gizmoMayHover(active, r)) {
        float best = 7.0f;
        for (int i = 0; i < 3; ++i) {
            if (!tipOk[i]) continue;
            ImVec2 d = tips[i] - c;
            d = d * (1.0f / std::sqrt(d.x * d.x + d.y * d.y));
            float dist = distToSegment(io.MousePos, c + d * 8, tips[i] + d * 6);
            if (dist < best) best = dist, hover = i;
        }
        if (hover < 0 && quadOk && inQuad(io.MousePos, quad)) hover = 3;
        if (hover < 0 && distToSegment(io.MousePos, c, c) < 7) hover = 3;  // the centre dot also moves in XZ
    }
    if (hover >= 0) {
        ImGui::SetHoveredID(gid);
        if (ImGui::IsMouseClicked(0)) {
            drag.handle = hover;
            drag.tool = 1;
            drag.startMouse = io.MousePos;
            drag.startWorld = wpos;
            drag.startLocal = fo.object->transform.position;
            bool ok = true;
            if (hover < 3) {
                ImVec2 d = tips[hover] - c;
                float l = std::sqrt(d.x * d.x + d.y * d.y);
                drag.axisDir = d * (1.0f / l);
                drag.pxPerUnit = l / len;
            } else {
                ok = mouseOnPlaneY(view, r, wpos.y, &drag.startHit);
            }
            if (ok) {
                ImGui::SetActiveID(gid, g.CurrentWindow);
                doc.endMerge();  // a new drag never merges into an earlier edit
            } else {
                drag.handle = -1;
            }
        }
    }

    // Drag.
    if (g.ActiveId == gid && drag.handle >= 0) {
        if (ImGui::IsMouseDown(0)) {
            splash::Vec3 nw = drag.startWorld;
            bool moved = true;
            if (drag.handle < 3) {
                ImVec2 m = io.MousePos - drag.startMouse;
                component(nw, drag.handle) += (m.x * drag.axisDir.x + m.y * drag.axisDir.y) / drag.pxPerUnit;
            } else {
                splash::Vec3 hit;
                moved = mouseOnPlaneY(view, r, drag.startWorld.y, &hit);
                nw.x += hit.x - drag.startHit.x;
                nw.z += hit.z - drag.startHit.z;
            }
            // Snap the moved world coordinates to the grid.
            if (moved && (st.snap || io.KeyCtrl)) {
                const float step = 0.25f;
                for (int i = 0; i < 3; ++i)
                    if (drag.handle == i || (drag.handle == 3 && i != 1)) component(nw, i) = std::round(component(nw, i) / step) * step;
            }
            splash::Vec3 local = drag.startLocal + worldToParentDelta(parentToWorld, nw - drag.startWorld);
            if (moved && doc.selection() && !(local == fo.object->transform.position))
                doc.edit(*doc.selection(), [&](splash::Object& ob) { ob.transform.position = local; }, "gizmo.move");
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            doc.endMerge();
            ImGui::ClearActiveID();
            drag.handle = -1;
        }
    }

    if (!visible) return;
    // Draw: plane first so the arrows sit on top. Hover and drag brighten and thicken.
    const int lit = drag.handle >= 0 && g.ActiveId == gid ? drag.handle : hover;
    float t[4];
    for (int i = 0; i < 4; ++i) t[i] = anim(gid + 1 + (ImGuiID)i, lit == i);
    if (quadOk) {
        ImU32 fill = lerpColor(rgb(0x7fcb55, 60), rgb(0x9be070, 110), t[3]);
        dl->AddConvexPolyFilled(quad, 4, fill);
        dl->AddPolyline(quad, 4, lerpColor(rgb(0x7fcb55, 180), rgb(0xd4f5bf, 255), t[3]), ImDrawFlags_Closed, 1.2f + 1.0f * t[3]);
    }
    for (int i = 0; i < 3; ++i) {
        if (!tipOk[i]) continue;
        ImVec2 dir = tips[i] - c;
        dir = dir * (1.0f / std::sqrt(dir.x * dir.x + dir.y * dir.y));
        ImVec2 n(-dir.y, dir.x);
        ImU32 col = lerpColor(axisCols[i], rgb(0xffffff), 0.35f * t[i]);
        float wdt = 2.5f + 1.5f * t[i], head = 6 + 2 * t[i];
        dl->AddLine(c + dir * 10, tips[i] - dir * 10, rgb(0x000000, 90), wdt + 2);
        dl->AddLine(c + dir * 10, tips[i] - dir * 10, col, wdt);
        dl->AddTriangleFilled(tips[i] + dir * head, tips[i] - dir * 10 + n * head, tips[i] - dir * 10 - n * head, col);
    }
    dl->AddCircleFilled(c, 6, rgb(0xf4f5f8), 20);
    dl->AddCircle(c, 6, rgb(0x000000, 90), 20, 1.5f);
}

// The world axes, and for ring i two unit vectors spanning its plane with
// U x V = axis i, so a positive turn about the axis carries U toward V.
static const splash::Vec3 kAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const splash::Vec3 kRingU[3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
static const splash::Vec3 kRingV[3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};

// The camera position in Unity space.
static splash::Vec3 eyeUnity(const viewport::Ps1View& view) {
    viewport::Vec3 e = view.eye();
    return {e.x, e.y, -e.z};
}

// The rotate gizmo: three world-axis rings (the axes move uses) at the move
// gizmo's screen size. Dragging a ring turns the object about that axis by
// the mouse's angle around the gizmo's centre on screen; snapping rounds the
// turn to 15 degrees. One drag is one undo step.
static void rotateGizmo(State& st, ImDrawList* dl, viewport::Ps1View& view, ImRect r, editor::Document& doc, const splash::FlatObject& fo,
                        const splash::Mat34* parentToWorld) {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID gid = ImGui::GetID("##gizmo");
    ImVec2 mn = r.Min, sz = r.GetSize();
    State::GizmoDrag& drag = st.gizmo;
    const bool active = g.ActiveId == gid && drag.handle >= 0;
    if (active) ImGui::KeepAliveID(gid);

    const GizmoFrame gf = gizmoFrame(view, r, fo);
    const float radius = gf.len * 0.92f;
    const ImU32 axisCols[3] = {color::axisX, color::axisY, color::axisZ};
    const splash::Vec3 toEye = eyeUnity(view) - gf.wpos;
    constexpr int kSeg = 72;
    ImVec2 ring[3][kSeg];
    bool facing[3][kSeg];  // segment k (point k to k + 1) is on the camera's half
    bool ringOk[3] = {};
    if (gf.visible) {
        for (int i = 0; i < 3; ++i) {
            ringOk[i] = true;
            for (int k = 0; k < kSeg && ringOk[i]; ++k) {
                float a = 2 * editor::kPi * k / kSeg, m = a + editor::kPi / kSeg;
                splash::Vec3 d = kRingU[i] * std::cos(a) + kRingV[i] * std::sin(a);
                ringOk[i] = view.project(toGl(gf.wpos + d * radius), mn, sz, &ring[i][k]);
                facing[i][k] = splash::dot(kRingU[i] * std::cos(m) + kRingV[i] * std::sin(m), toEye) >= 0;
            }
        }
    }

    // Hover: the nearest ring, front halves preferred where rings cross.
    int hover = -1, hoverSeg = 0;
    if (gf.visible && gizmoMayHover(active, r)) {
        float best = 7.0f;
        for (int i = 0; i < 3; ++i) {
            if (!ringOk[i]) continue;
            for (int k = 0; k < kSeg; ++k) {
                float dist = distToSegment(io.MousePos, ring[i][k], ring[i][(k + 1) % kSeg]) + (facing[i][k] ? 0.0f : 3.0f);
                if (dist < best) best = dist, hover = i, hoverSeg = k;
            }
        }
    }
    if (hover >= 0) {
        ImGui::SetHoveredID(gid);
        if (ImGui::IsMouseClicked(0)) {
            drag.handle = hover;
            drag.tool = 2;
            drag.startMouse = io.MousePos;
            drag.startRot = fo.object->transform.rotation;
            // A world axis seen from the parent's space, so the turn is about the world axis.
            splash::Vec3 pa = worldToParentDelta(parentToWorld, kAxis[hover]);
            drag.parentAxis = splash::sqrMagnitude(pa) > 1e-12f ? splash::normalized(pa) : kAxis[hover];
            float a = 2 * editor::kPi * (hoverSeg + 0.5f) / kSeg;
            drag.ringStart = kRingU[hover] * std::cos(a) + kRingV[hover] * std::sin(a);
            drag.lastAngle = editor::screenAngle(gf.c.x, gf.c.y, io.MousePos.x, io.MousePos.y);
            drag.accum = 0;
            drag.shown = 0;
            // Which way a growing screen angle turns the object: V is U turned
            // +90 degrees, so compare their winding on screen (y down).
            ImVec2 su, sv;
            float cr = 0;
            if (view.project(toGl(gf.wpos + kRingU[hover] * radius), mn, sz, &su) &&
                view.project(toGl(gf.wpos + kRingV[hover] * radius), mn, sz, &sv)) {
                su = su - gf.c, sv = sv - gf.c;
                cr = su.x * sv.y - su.y * sv.x;
            }
            drag.sign = cr < 0 ? -1.0f : 1.0f;
            ImGui::SetActiveID(gid, g.CurrentWindow);
            doc.endMerge();  // a new drag never merges into an earlier edit
        }
    }

    // Drag.
    if (g.ActiveId == gid && drag.handle >= 0) {
        if (ImGui::IsMouseDown(0)) {
            // Too close to the centre the angle is noise: hold it until the mouse leaves.
            ImVec2 m = io.MousePos - gf.c;
            if (m.x * m.x + m.y * m.y > 16) {
                float a = editor::screenAngle(gf.c.x, gf.c.y, io.MousePos.x, io.MousePos.y);
                drag.accum += editor::angleStep(drag.lastAngle, a);
                drag.lastAngle = a;
            }
            float deg = editor::rotateDragDegrees(drag.accum * drag.sign, st.snap || io.KeyCtrl);
            drag.shown = deg;
            splash::Quat q = drag.startRot;
            if (deg != 0)
                q = editor::quatNormalize(editor::quatMul(editor::quatAxisAngle(drag.parentAxis, deg * (editor::kPi / 180.0f)), drag.startRot));
            const splash::Quat& cur = fo.object->transform.rotation;
            bool same = q.x == cur.x && q.y == cur.y && q.z == cur.z && q.w == cur.w;
            if (doc.selection() && !same)
                doc.edit(*doc.selection(), [&](splash::Object& ob) { ob.transform.rotation = q; }, "gizmo.rotate");
        } else {
            doc.endMerge();
            ImGui::ClearActiveID();
            drag.handle = -1;
        }
    }

    if (!gf.visible) return;
    // Draw: back halves thin and faint, then front halves, the lit ring last.
    const bool dragging = g.ActiveId == gid && drag.handle >= 0;
    const int lit = dragging ? drag.handle : hover;
    float t[3];
    for (int i = 0; i < 3; ++i) t[i] = anim(gid + 1 + (ImGuiID)i, lit == i);
    auto stroke = [&](int i, bool front, ImU32 col, float w) {
        int k0 = -1;
        for (int k = 0; k < kSeg; ++k)
            if (facing[i][k] != facing[i][(k + kSeg - 1) % kSeg]) {
                k0 = k;
                break;
            }
        if (k0 < 0) {  // all on one side
            if (facing[i][0] == front) dl->AddPolyline(ring[i], kSeg, col, ImDrawFlags_Closed, w);
            return;
        }
        ImVec2 run[kSeg + 1];
        int n = 0;
        for (int j = 0; j < kSeg; ++j) {
            int k = (k0 + j) % kSeg;
            if (facing[i][k] == front) {
                if (n == 0) run[n++] = ring[i][k];
                run[n++] = ring[i][(k + 1) % kSeg];
            } else if (n) {
                dl->AddPolyline(run, n, col, 0, w);
                n = 0;
            }
        }
        if (n) dl->AddPolyline(run, n, col, 0, w);
    };
    int order[3] = {0, 1, 2};
    if (lit >= 0) std::swap(order[lit], order[2]);
    for (int i : order) {
        if (!ringOk[i] || (dragging && i != drag.handle)) continue;
        stroke(i, false, lerpColor(axisCols[i] & 0x00ffffff, axisCols[i], 0.32f + 0.3f * t[i]), 1.5f);
    }
    // While dragging, the wedge swept so far, from the grab point.
    if (dragging) {
        const int h = drag.handle;
        float rad = drag.shown * (editor::kPi / 180.0f);
        float sweep = std::clamp(rad, -2 * editor::kPi, 2 * editor::kPi);
        int n = std::clamp((int)(std::fabs(sweep) / (2 * editor::kPi) * kSeg) + 1, 1, kSeg);
        ImVec2 arc[kSeg + 1];
        bool ok = true;
        for (int j = 0; j <= n && ok; ++j) {
            splash::Vec3 d = splash::rotate(editor::quatAxisAngle(kAxis[h], sweep * j / n), drag.ringStart);
            ok = view.project(toGl(gf.wpos + d * radius), mn, sz, &arc[j]);
        }
        if (ok) {
            // Unsmoothed fan, so the triangles meet without seams.
            ImDrawListFlags fl = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            if (sweep != 0)
                for (int j = 0; j < n; ++j) dl->AddTriangleFilled(gf.c, arc[j], arc[j + 1], (axisCols[h] & 0x00ffffff) | 0x38000000);
            dl->Flags = fl;
            dl->AddLine(gf.c, arc[0], rgb(0xffffff, 110), 1.2f);
            dl->AddLine(gf.c, arc[n], rgb(0xffffff, 220), 1.5f);
        }
    }
    for (int i : order) {
        if (!ringOk[i]) continue;
        if (dragging && i != drag.handle) {
            stroke(i, true, (axisCols[i] & 0x00ffffff) | 0x40000000, 1.5f);
            continue;
        }
        float w = 2.5f + 1.5f * t[i];
        stroke(i, true, rgb(0x000000, 90), w + 2);
        stroke(i, true, lerpColor(axisCols[i], rgb(0xffffff), 0.35f * t[i]), w);
    }
    dl->AddCircleFilled(gf.c, 3.5f, rgb(0xf4f5f8), 16);
    dl->AddCircle(gf.c, 3.5f, rgb(0x000000, 90), 16, 1.2f);
    if (dragging) {
        static const char* tags[3] = {"X", "Y", "Z"};
        char v[32];
        std::snprintf(v, sizeof v, std::fmod(drag.shown, 1.0f) == 0 ? "%+.0f\xc2\xb0" : "%+.1f\xc2\xb0", (double)drag.shown);
        if (drag.shown == 0) std::snprintf(v, sizeof v, "0\xc2\xb0");
        gizmoLabel(dl, r, gf.c, io.MousePos, tags[drag.handle], axisCols[drag.handle], v);
    }
}

// A solid cube centred on `centre` with edges along `ax` (unit, world),
// half-size `half`, faces lit from above. Back faces are culled, so the
// faces never need sorting.
static void gizmoCube(ImDrawList* dl, const viewport::Ps1View& view, ImRect r, splash::Vec3 centre, const splash::Vec3 ax[3], float half,
                      ImU32 col, splash::Vec3 eye) {
    ImVec2 p[8];
    for (int c = 0; c < 8; ++c) {
        splash::Vec3 w = centre + ax[0] * (c & 1 ? half : -half) + ax[1] * (c & 2 ? half : -half) + ax[2] * (c & 4 ? half : -half);
        if (!view.project(toGl(w), r.Min, r.GetSize(), &p[c])) return;
    }
    for (int k = 0; k < 3; ++k)
        for (int s = 0; s < 2; ++s) {
            splash::Vec3 n = ax[k] * (s ? 1.0f : -1.0f);
            if (splash::dot(n, eye - (centre + n * half)) <= 0) continue;
            int a = 1 << ((k + 1) % 3), b = 1 << ((k + 2) % 3), base = s ? 1 << k : 0;
            ImVec2 q[4] = {p[base], p[base | a], p[base | a | b], p[base | b]};
            float light = 0.55f + 0.45f * std::clamp(n.y * 0.7f + 0.5f, 0.0f, 1.0f);
            ImU32 face = lerpColor(rgb(0x000000, (int)(col >> 24)), col, light);
            dl->AddConvexPolyFilled(q, 4, face);
            dl->AddPolyline(q, 4, rgb(0x000000, 70), ImDrawFlags_Closed, 1.0f);
        }
}

// The scale gizmo: three handles along the object's own axes (scale is
// local) ending in cubes, and a centre cube for uniform scale. Dragging a
// handle by its own screen length doubles that axis; the centre scales by
// the drag up and to the right. Snapping rounds to 0.1, and no component
// ever reaches zero or changes sign. One drag is one undo step.
static void scaleGizmo(State& st, ImDrawList* dl, viewport::Ps1View& view, ImRect r, editor::Document& doc, const splash::FlatObject& fo) {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID gid = ImGui::GetID("##gizmo");
    ImVec2 mn = r.Min, sz = r.GetSize();
    State::GizmoDrag& drag = st.gizmo;
    const bool active = g.ActiveId == gid && drag.handle >= 0;
    if (active) ImGui::KeepAliveID(gid);

    const GizmoFrame gf = gizmoFrame(view, r, fo);
    const ImU32 axisCols[3] = {color::axisX, color::axisY, color::axisZ};
    const splash::Vec3 eye = eyeUnity(view);
    splash::Vec3 dirs[3];
    for (int i = 0; i < 3; ++i) dirs[i] = splash::normalized(splash::rotate(fo.worldRotation, kAxis[i]));
    const splash::Vec3 cur = fo.object->transform.scale;
    // While dragging, the handles stretch with the scale so the change reads.
    float stretch[4] = {1, 1, 1, 1};
    if (active)
        for (int i = 0; i < 3; ++i) {
            float s0 = drag.startScale[i];
            if (std::fabs(s0) > 1e-6f) stretch[i] = std::clamp(cur[i] / s0, 0.2f, 3.0f);
        }
    ImVec2 tips[3], dirS[3];
    float tipPx[3] = {};
    bool tipOk[3] = {};
    if (gf.visible)
        for (int i = 0; i < 3; ++i) {
            ImVec2 p;
            if (!view.project(toGl(gf.wpos + dirs[i] * gf.len), mn, sz, &p)) continue;
            float l = length(p - gf.c);
            if (l < 10) continue;  // pointing at the camera: too short to grab
            tipOk[i] = true;
            dirS[i] = (p - gf.c) * (1.0f / l);
            tipPx[i] = l;
            tips[i] = gf.c + dirS[i] * (l * stretch[i]);
        }

    // Hover: the centre cube first, then the nearest handle.
    int hover = -1;
    if (gf.visible && gizmoMayHover(active, r)) {
        if (length(io.MousePos - gf.c) < 10) {
            hover = 3;
        } else {
            float best = 7.0f;
            for (int i = 0; i < 3; ++i) {
                if (!tipOk[i]) continue;
                float dist = distToSegment(io.MousePos, gf.c + dirS[i] * 10, tips[i] + dirS[i] * 7);
                if (dist < best) best = dist, hover = i;
            }
        }
    }
    if (hover >= 0) {
        ImGui::SetHoveredID(gid);
        if (ImGui::IsMouseClicked(0)) {
            drag.handle = hover;
            drag.tool = 3;
            drag.startMouse = io.MousePos;
            drag.startScale = cur;
            drag.axisDir = hover < 3 ? dirS[hover] : ImVec2(0.70710678f, -0.70710678f);
            drag.handlePx = hover < 3 ? tipPx[hover] : 76.0f;
            drag.shown = hover < 3 ? cur[hover] : 1.0f;
            ImGui::SetActiveID(gid, g.CurrentWindow);
            doc.endMerge();  // a new drag never merges into an earlier edit
        }
    }

    // Drag.
    if (g.ActiveId == gid && drag.handle >= 0) {
        if (ImGui::IsMouseDown(0)) {
            ImVec2 m = io.MousePos - drag.startMouse;
            float factor = 1 + (m.x * drag.axisDir.x + m.y * drag.axisDir.y) / drag.handlePx;
            const bool snap = st.snap || io.KeyCtrl;
            splash::Vec3 ns = drag.startScale;
            if (drag.handle < 3) {
                component(ns, drag.handle) = editor::scaleAxis(drag.startScale[drag.handle], factor, snap);
                drag.shown = ns[drag.handle];
            } else {
                ns = editor::scaleUniform(drag.startScale, factor, snap);
                drag.shown = 1;
                for (int i = 0; i < 3; ++i)
                    if (std::fabs(drag.startScale[i]) > 1e-6f) {
                        drag.shown = ns[i] / drag.startScale[i];
                        break;
                    }
            }
            if (doc.selection() && !(ns == cur))
                doc.edit(*doc.selection(), [&](splash::Object& ob) { ob.transform.scale = ns; }, "gizmo.scale");
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            doc.endMerge();
            ImGui::ClearActiveID();
            drag.handle = -1;
        }
    }

    if (!gf.visible) return;
    // Draw far handles first so near ones overlap them; the centre cube last.
    const bool dragging = g.ActiveId == gid && drag.handle >= 0;
    const int lit = dragging ? drag.handle : hover;
    float t[4];
    for (int i = 0; i < 4; ++i) t[i] = anim(gid + 1 + (ImGuiID)i, lit == i);
    int order[3] = {0, 1, 2};
    float dist[3];
    for (int i = 0; i < 3; ++i) dist[i] = splash::sqrMagnitude(gf.wpos + dirs[i] * gf.len - eye);
    std::sort(order, order + 3, [&](int a, int b) { return dist[a] > dist[b]; });
    const float cube = gf.len * 0.075f;
    for (int i : order) {
        if (!tipOk[i]) continue;
        ImU32 col = lerpColor(axisCols[i], rgb(0xffffff), 0.35f * t[i]);
        if (dragging && drag.handle < 3 && i != drag.handle) col = (col & 0x00ffffff) | 0x60000000;
        float wdt = 2.5f + 1.5f * t[i];
        ImVec2 a = gf.c + dirS[i] * 10, b = tips[i] - dirS[i] * 5;
        if (length(b - gf.c) > 10) {
            dl->AddLine(a, b, rgb(0x000000, 90), wdt + 2);
            dl->AddLine(a, b, col, wdt);
        }
        splash::Vec3 at = gf.wpos + dirs[i] * (gf.len * stretch[i]);
        gizmoCube(dl, view, r, at, dirs, cube * (1 + 0.25f * t[i]), col, eye);
    }
    gizmoCube(dl, view, r, gf.wpos, dirs, cube * (1.15f + 0.25f * t[3]), lerpColor(rgb(0xd8dbe2), rgb(0xffffff), t[3]), eye);
    if (dragging) {
        static const char* tags[3] = {"X", "Y", "Z"};
        char v[32];
        if (drag.handle < 3) {
            std::snprintf(v, sizeof v, "%.2f", (double)drag.shown);
            gizmoLabel(dl, r, gf.c, io.MousePos, tags[drag.handle], axisCols[drag.handle], v);
        } else {
            std::snprintf(v, sizeof v, "\xc3\x97%.2f", (double)drag.shown);
            gizmoLabel(dl, r, gf.c, io.MousePos, "XYZ", color::text, v);
        }
    }
}

// Local-space bounds of a mesh, loaded and cached by project path. Empty when
// the mesh is missing or unreadable.
static const std::optional<splash::Bounds>& localBounds(const editor::Document& doc, const std::string& path) {
    static std::map<std::string, std::optional<splash::Bounds>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::optional<splash::Bounds> b;
    try {
        b = splash::loadMesh(doc.resolve(path)).bounds();
    } catch (...) {
    }
    return cache.emplace(path, b).first->second;
}

// World AABB of the selected object's mesh in GL space, or a small box at its
// origin when it has no usable mesh.
static void selectedBoxGl(const editor::Document& doc, const splash::FlatObject& fo, viewport::Vec3* lo,
                          viewport::Vec3* hi) {
    if (fo.object->mesh && !fo.object->mesh->mesh.empty()) {
        if (const std::optional<splash::Bounds>& lb = localBounds(doc, fo.object->mesh->mesh)) {
            splash::Vec3 mn = lb->min(), mx = lb->max();
            splash::Vec3 wmin, wmax;
            for (int c = 0; c < 8; ++c) {
                splash::Vec3 corner{c & 1 ? mx.x : mn.x, c & 2 ? mx.y : mn.y, c & 4 ? mx.z : mn.z};
                splash::Vec3 w = fo.localToWorld.point(corner);
                wmin = c == 0 ? w : splash::vmin(wmin, w);
                wmax = c == 0 ? w : splash::vmax(wmax, w);
            }
            *lo = toGl(wmin);
            *hi = toGl(wmax);
            return;
        }
    }
    splash::Vec3 p = fo.localToWorld.position();
    *lo = toGl({p.x - 0.25f, p.y - 0.25f, p.z - 0.25f});
    *hi = toGl({p.x + 0.25f, p.y + 0.25f, p.z + 0.25f});
}

// GL-space bounds of the selection, or of the whole scene with nothing selected.
static bool frameBounds(const editor::Document& doc, const viewport::Ps1View& view, const std::vector<splash::FlatObject>& flats,
                        viewport::Vec3* lo, viewport::Vec3* hi) {
    if (const splash::Object* sel = doc.selected())
        for (const splash::FlatObject& fo : flats)
            if (fo.object == sel) {
                selectedBoxGl(doc, fo, lo, hi);
                return true;
            }
    return view.sceneBounds(lo, hi);
}

// Puts the camera's pivot on the selection (or the scene) and backs off to fit it.
static void frameSelection(const editor::Document& doc, viewport::Ps1View& view, const std::vector<splash::FlatObject>& flats) {
    viewport::Vec3 lo, hi;
    if (!frameBounds(doc, view, flats, &lo, &hi)) return;
    viewport::Vec3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    viewport::Vec3 e{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
    view.frame(c, std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z));
}

// The selected object's world origin (GL space) and identity, for Shift+F.
static bool selectedOrigin(const editor::Document& doc, const std::vector<splash::FlatObject>& flats, const void** id,
                           viewport::Vec3* at) {
    const splash::Object* sel = doc.selected();
    if (!sel) return false;
    for (const splash::FlatObject& fo : flats)
        if (fo.object == sel) {
            *id = sel;
            *at = toGl(fo.localToWorld.position());
            return true;
        }
    return false;
}

// Unity's Quaternion.LookRotation(forward, up), both unit and orthogonal.
static splash::Quat lookRotation(splash::Vec3 f, splash::Vec3 u) {
    splash::Vec3 r{u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
    float m00 = r.x, m01 = u.x, m02 = f.x, m10 = r.y, m11 = u.y, m12 = f.y, m20 = r.z, m21 = u.z, m22 = f.z;
    splash::Quat q;
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float k = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / k, (m02 - m20) / k, (m10 - m01) / k, k / 4};
    } else if (m00 > m11 && m00 > m22) {
        float k = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {k / 4, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k};
    } else if (m11 > m22) {
        float k = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / k, k / 4, (m12 + m21) / k, (m02 - m20) / k};
    } else {
        float k = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / k, (m12 + m21) / k, k / 4, (m10 - m01) / k};
    }
    return editor::quatNormalize(q);
}

// Ctrl+Shift+F: moves and turns the selection to where the camera is and
// how it looks, like Unity's Align With View.
static void alignWithView(editor::Document& doc, const viewport::Ps1View& view, const std::vector<splash::FlatObject>& flats) {
    const std::optional<editor::ObjectPath> path = doc.selection();
    if (!path) return;
    viewport::Vec3 s, u, f, e = view.ortho() ? view.pivot() : view.eye();
    view.basis(&s, &u, &f);
    splash::Vec3 worldPos{e.x, e.y, -e.z};
    splash::Quat worldRot = lookRotation({f.x, f.y, -f.z}, {u.x, u.y, -u.z});
    const splash::Object* par = doc.parent(*path);
    const splash::FlatObject* pf = nullptr;
    for (const splash::FlatObject& fo : flats)
        if (par && fo.object == par) pf = &fo;
    splash::Vec3 pos = pf ? worldToParentDelta(&pf->localToWorld, worldPos - pf->localToWorld.position()) : worldPos;
    splash::Quat rot = worldRot;
    if (pf) {
        splash::Quat inv{-pf->worldRotation.x, -pf->worldRotation.y, -pf->worldRotation.z, pf->worldRotation.w};
        rot = editor::quatNormalize(editor::quatMul(inv, worldRot));
    }
    doc.edit(*path, [&](splash::Object& ob) {
        ob.transform.position = pos;
        ob.transform.rotation = rot;
    });
}

// Mouse and keys over the viewport, registered after the overlays so the
// toolbars, icons and gizmo take the mouse first. Unity's Scene view
// controls: RMB looks around, and with it held WASD/QE fly (Shift faster,
// the wheel sets the speed); Alt+LMB orbits the pivot, Alt+RMB zooms,
// MMB (or LMB with the hand tool) pans, the wheel zooms to the pivot. F
// frames the selection, Shift+F also follows it, Ctrl+Shift+F aligns it
// with the view. Effects show from the next frame's render.
static void viewportInput(State& st, ImRect r, editor::Document& doc, viewport::Ps1View& view,
                          const std::vector<splash::FlatObject>& flats) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *GImGui;
    ImGuiID id = ImGui::GetID("##viewport");
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::ItemSize(r.GetSize());
    ImGui::ItemAdd(r, id);
    bool hovered = ImGui::ItemHoverable(r, id, 0);
    if (hovered && g.ActiveId == 0)
        for (int b = 0; b < 3; ++b)
            if (ImGui::IsMouseClicked(b)) {
                ImGui::SetActiveID(id, g.CurrentWindow);
                ImGui::FocusWindow(g.CurrentWindow);
                st.vpButton = b;
                st.vpDragged = false;
                st.vpAlt = io.KeyAlt;
                st.flyHeld = 0;
                break;
            }
    enum { None, Select, Look, Orbit, Zoom, Pan } mode = None;
    if (g.ActiveId == id && st.vpButton >= 0) {
        if (st.vpButton == 0) mode = st.vpAlt ? Orbit : st.tool == 0 ? Pan : Select;
        if (st.vpButton == 1) mode = st.vpAlt ? Zoom : Look;
        if (st.vpButton == 2) mode = Pan;
    }
    bool moved = false;
    if (mode != None) {
        if (ImGui::IsMouseDown(st.vpButton)) {
            if (ImGui::IsMouseDragPastThreshold(st.vpButton)) st.vpDragged = true;
            ImVec2 d = io.MouseDelta;
            if (st.vpDragged && (d.x != 0 || d.y != 0)) {
                if (mode == Look) view.lookAround(d.x, d.y);
                if (mode == Orbit) view.orbit(d.x, d.y);
                if (mode == Pan) view.pan(d.x, d.y, r.GetHeight());
                if (mode == Zoom) view.dolly((std::fabs(d.x) > std::fabs(d.y) ? -d.x : d.y) * 0.03f);
                moved = mode == Pan || mode == Look;  // orbit and zoom keep a Shift+F follow
            }
            if (mode == Look) {
                // Flythrough: camera-space WASD + Q down / E up, accelerating while held.
                viewport::Vec3 dir;
                if (ImGui::IsKeyDown(ImGuiKey_D)) dir.x += 1;
                if (ImGui::IsKeyDown(ImGuiKey_A)) dir.x -= 1;
                if (ImGui::IsKeyDown(ImGuiKey_E)) dir.y += 1;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) dir.y -= 1;
                if (ImGui::IsKeyDown(ImGuiKey_W)) dir.z += 1;
                if (ImGui::IsKeyDown(ImGuiKey_S)) dir.z -= 1;
                if (dir.x != 0 || dir.y != 0 || dir.z != 0) {
                    st.flyHeld += io.DeltaTime;
                    float speed = st.flySpeed * (1 + std::min(st.flyHeld, 2.0f) * 1.5f) * (io.KeyShift ? 4.0f : 1.0f);
                    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                    float k = speed * io.DeltaTime / len;
                    view.fly({dir.x * k, dir.y * k, dir.z * k});
                    moved = true;
                } else {
                    st.flyHeld = 0;
                }
                if (hovered && io.MouseWheel != 0) {
                    st.flySpeed = std::clamp(st.flySpeed * std::pow(1.2f, io.MouseWheel), 0.05f, 200.0f);
                    st.flySpeedShownUntil = ImGui::GetTime() + 1.0;
                }
            }
            if (st.vpDragged && mode != Select)
                ImGui::SetMouseCursor(mode == Pan ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
        } else {
            // A left click that did not drag selects what is under the mouse, or clears the selection.
            if (mode == Select && !st.vpDragged && r.Contains(io.MousePos)) {
                std::optional<int> hit = view.pick(io.MousePos, r.Min, r.GetSize());
                std::vector<editor::ObjectPath> paths = doc.flatPaths();
                if (hit && *hit >= 0 && (size_t)*hit < paths.size()) doc.select(paths[(size_t)*hit]);
                else doc.select(std::nullopt);
            }
            ImGui::ClearActiveID();
            st.vpButton = -1;
            st.flyHeld = 0;
        }
    }
    if (hovered && io.MouseWheel != 0 && mode != Look) view.dolly(io.MouseWheel);

    // Shift+F follow: stays on until the camera is panned or flown, or the selection changes.
    const void* selId = nullptr;
    viewport::Vec3 selAt;
    bool haveSel = selectedOrigin(doc, flats, &selId, &selAt);
    if (moved || !haveSel || selId != st.followObject) st.follow = false;
    if (st.follow) {
        view.translate({selAt.x - st.followAt[0], selAt.y - st.followAt[1], selAt.z - st.followAt[2]});
        st.followAt[0] = selAt.x, st.followAt[1] = selAt.y, st.followAt[2] = selAt.z;
    }

    const bool keys = !io.WantTextInput && !(st.play.gameFocus && st.play.game.attached());
    if ((keys && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false)) || st.alignRequest) alignWithView(doc, view, flats);
    st.alignRequest = false;
    if ((keys && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) || st.frameRequest) {
        frameSelection(doc, view, flats);
        st.follow = keys && io.KeyShift && !st.frameRequest && haveSel;
        st.followObject = selId;
        st.followAt[0] = selAt.x, st.followAt[1] = selAt.y, st.followAt[2] = selAt.z;
    }
    st.frameRequest = false;

    // The fly speed, shown for a moment after the wheel changes it.
    if (ImGui::GetTime() < st.flySpeedShownUntil) {
        Fonts& f = fonts();
        char b[32];
        std::snprintf(b, sizeof b, "Speed %.2f", st.flySpeed);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 ts = measure(f.medium, type::label, b);
        ImVec2 c = r.GetCenter();
        ImRect chip(c - ImVec2(ts.x / 2 + 12, 14), c + ImVec2(ts.x / 2 + 12, 14));
        dl->AddRectFilled(chip.Min, chip.Max, rgb(0x0e1014, 210), radius::pill);
        textCentered(dl, chip, f.medium, type::label, color::text, b);
    }
}

// The controller on port 1, from the keyboard and the first connected gamepad,
// the keyboard only while the Game view has focus.
static uint16_t hostPad(bool keyboard) {
    struct Map {
        ImGuiKey key;
        uint16_t bit;
    };
    static const Map map[] = {
        {ImGuiKey_UpArrow, padbit::up},      {ImGuiKey_DownArrow, padbit::down}, {ImGuiKey_LeftArrow, padbit::left},
        {ImGuiKey_RightArrow, padbit::right}, {ImGuiKey_Enter, padbit::start},    {ImGuiKey_Backspace, padbit::select},
        {ImGuiKey_Z, padbit::cross},          {ImGuiKey_X, padbit::circle},       {ImGuiKey_A, padbit::square},
        {ImGuiKey_S, padbit::triangle},       {ImGuiKey_Q, padbit::l1},           {ImGuiKey_W, padbit::r1},
        {ImGuiKey_1, padbit::l2},             {ImGuiKey_2, padbit::r2},
    };
    static const Map gamepad[] = {
        {ImGuiKey_GamepadDpadUp, padbit::up},       {ImGuiKey_GamepadDpadDown, padbit::down},
        {ImGuiKey_GamepadDpadLeft, padbit::left},   {ImGuiKey_GamepadDpadRight, padbit::right},
        {ImGuiKey_GamepadLStickUp, padbit::up},     {ImGuiKey_GamepadLStickDown, padbit::down},
        {ImGuiKey_GamepadLStickLeft, padbit::left}, {ImGuiKey_GamepadLStickRight, padbit::right},
        {ImGuiKey_GamepadStart, padbit::start},     {ImGuiKey_GamepadBack, padbit::select},
        {ImGuiKey_GamepadFaceDown, padbit::cross},  {ImGuiKey_GamepadFaceRight, padbit::circle},
        {ImGuiKey_GamepadFaceLeft, padbit::square}, {ImGuiKey_GamepadFaceUp, padbit::triangle},
        {ImGuiKey_GamepadL1, padbit::l1},           {ImGuiKey_GamepadR1, padbit::r1},
        {ImGuiKey_GamepadL2, padbit::l2},           {ImGuiKey_GamepadR2, padbit::r2},
    };
    uint16_t pad = 0xffff;
    for (const Map& m : gamepad)
        if (ImGui::IsKeyDown(m.key)) pad &= (uint16_t)~m.bit;
    if (!keyboard || ImGui::GetIO().WantTextInput) return pad;
    for (const Map& m : map)
        if (ImGui::IsKeyDown(m.key)) pad &= (uint16_t)~m.bit;
    return pad;
}

// The game as pcsx-redux shows it, 4:3 and letterboxed.
static void gamePanel(State& st, ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    State::Play& p = st.play;
    dl->AddRectFilled(r.Min, r.Max, IM_COL32_BLACK, radius::window);
    float w = r.GetWidth(), h = r.GetHeight();
    float gw = std::min(w, h * 4 / 3), gh = gw * 3 / 4;
    ImVec2 c = r.GetCenter();
    ImRect g(c - ImVec2(gw / 2, gh / 2), c + ImVec2(gw / 2, gh / 2));
    if (unsigned tex = p.game.texture())
        dl->AddImage((ImTextureID)(intptr_t)tex, g.Min, g.Max);
    else
        textCentered(dl, r, f.regular, type::body, color::textDim, "Waiting for pcsx-redux...");
    // The keyboard is the controller only while the Game view has focus; a gamepad always is.
    p.game.setPads(hostPad(p.gameFocus), 0xffff);
    const char* label = "Game";
    ImVec2 ls = measure(f.medium, type::label, label);
    ImRect tag(r.Min + ImVec2(space::md, space::md), r.Min + ImVec2(space::md + ls.x + 20, space::md + 24));
    dl->AddRectFilled(tag.Min, tag.Max, rgb(0x0e1014, 190), radius::pill);
    textCentered(dl, tag, f.medium, type::label, p.gameFocus ? color::text : color::textDim, label);
    const char* info = p.gameFocus ? "Arrows  ·  Z X A S  ·  Q W 1 2  ·  Enter Start  ·  Backspace Select"
                                   : "Click to control the game with the keyboard";
    ImVec2 is = measure(f.regular, type::caption, info);
    ImRect chip(ImVec2(r.Min.x + space::md, r.Max.y - space::md - 24), ImVec2(r.Min.x + space::md + is.x + 20, r.Max.y - space::md));
    dl->AddRectFilled(chip.Min, chip.Max, rgb(0x0e1014, 190), radius::pill);
    textCentered(dl, chip, f.regular, type::caption, color::textDim, info);
    if (p.gameFocus) dl->AddRect(r.Min, r.Max, color::accent, radius::window, 0, 2.0f);
}

// While a game runs the viewport splits in two, Scene and Game, both live: the
// scene stays editable while the game plays. Side by side when there is room,
// stacked otherwise; the bar between them drags. A click in either one gives it
// the keyboard. Returns the Scene part.
static ImRect splitViewport(State& st, ImDrawList* dl, ImRect r) {
    State::Play& p = st.play;
    // Whichever way gives the 4:3 game the bigger picture at an even split.
    const float g = size::gutter, w = r.GetWidth(), h = r.GetHeight();
    const bool across = std::min((w - g) / 2, h * 4 / 3) > std::min(w, (h - g) / 2 * 4 / 3);
    const float len = across ? w : h;
    const float lo = 160, hi = std::max(lo, len - g - 160);
    float at = std::clamp(len * p.split, lo, hi);
    ImRect bar = across ? ImRect(ImVec2(r.Min.x + at, r.Min.y), ImVec2(r.Min.x + at + g, r.Max.y))
                        : ImRect(ImVec2(r.Min.x, r.Min.y + at), ImVec2(r.Max.x, r.Min.y + at + g));
    ImGui::SetCursorScreenPos(bar.Min);
    ImGui::InvisibleButton("##vpsplit", ImMax(bar.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(across ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
        float d = across ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        at = std::clamp(at + d, lo, hi);
        p.split = at / len;
    }
    ImRect scene = across ? ImRect(r.Min, ImVec2(r.Min.x + at, r.Max.y)) : ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + at));
    ImRect game = across ? ImRect(ImVec2(bar.Max.x, r.Min.y), r.Max) : ImRect(ImVec2(r.Min.x, bar.Max.y), r.Max);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        if (game.Contains(ImGui::GetMousePos())) p.gameFocus = true;
        if (scene.Contains(ImGui::GetMousePos())) p.gameFocus = false;
    }
    gamePanel(st, dl, game);
    return scene;
}

// The same pose as far as the console can tell: the preview samples in its
// fixed point, so a key on the scene's own pose comes back rounded. Within
// one position step (gte / 4096 world units) and half an angle step on each
// axis (180 / 2048 degrees) it is the scene's own.
static bool samePose(const splash::FlatObject& a, const splash::FlatObject& b, float gte) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::fabs(a.localToWorld.m[i][j] - b.localToWorld.m[i][j]) > (j == 3 ? gte / 4096.0f + 1e-4f : 5e-3f)) return false;
    return (a.object->mesh.has_value() == b.object->mesh.has_value());
}

// The camera a cutscene's camera tracks put at the scrub head: a frustum
// (Unity camera axes, 4:3) with a camera icon at the eye.
static void previewCamera(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, const editor::Preview& p) {
    if (!p.cameraPosition && !p.cameraRotation && !p.cameraH) return;
    const splash::Vec3 eye = p.cameraPosition.value_or(splash::Vec3{});
    const splash::Quat q = editor::quatEuler(p.cameraRotation.value_or(splash::Vec3{}));
    const splash::Vec3 f = splash::rotate(q, {0, 0, 1}), u = splash::rotate(q, {0, 1, 0}), rt = splash::rotate(q, {1, 0, 0});
    // A Camera H track sets the projection distance: half the 240 lines over H
    // is the half height at distance 1 (camera.cpp projects with H).
    const float d = 1.2f, hh = d * (p.cameraH ? 120.0f / std::max(1.0f, *p.cameraH) : 0.45f), hw = hh * 4.0f / 3.0f;
    const ImU32 col = rgb(0x5ccfe0);
    ImVec2 e, c[4];
    if (!view.project(toGl(eye), mn, sz, &e)) return;
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; ++i)
        if (!view.project(toGl(eye + f * d + rt * (hw * sx[i]) + u * (hh * sy[i])), mn, sz, &c[i])) return;
    for (int i = 0; i < 4; ++i) {
        dl->AddLine(e, c[i], col, 1.5f);
        dl->AddLine(c[i], c[(i + 1) % 4], col, 1.5f);
    }
    // The up side, so a rolled camera reads as one.
    ImVec2 top;
    if (view.project(toGl(eye + f * d + u * (hh * 1.4f)), mn, sz, &top)) dl->AddTriangleFilled(c[0], c[1], top, (col & 0x00ffffffu) | (150u << IM_COL32_A_SHIFT));
    ImGui::PushID("previewCamera");
    sceneIcon(dl, view, mn, sz, toGl(eye), icon::camera, col);
    ImGui::PopID();
}

constexpr ImU32 kNavPortal = rgb(0xff6650);  // nav portal edges, apart from Room portals' yellow
static ImU32 overlayCol(ImU32 c, unsigned a) { return (c & 0x00ffffffu) | (a << IM_COL32_A_SHIFT); }

// What the game reads off the scene but never draws: every Room box and
// Portal opening (with its A to B arrow), every Trigger box (the
// world box the splashpack stores), every Interactable's reach as a ring on
// the ground plane through its origin, and the last export's nav regions.
// The selected object's shapes are drawn brighter.
static std::string fmtFixed(float v);

// Where a nav region sits: its Room's name, or with no Room objects in the
// scene its connectivity island. Scripted: "<name>", "island-k" or "none";
// else "Room <name>", "Island k" or "No room".
static std::string navRoomName(const splash::ExportStats& s, const splash::ExportStats::NavRegionInfo& info, bool scripted) {
    if (info.room < 0) return scripted ? "none" : "No room";
    if (s.navIslands) return (scripted ? "island-" : "Island ") + std::to_string(info.room);
    const std::string name = size_t(info.room) < s.rooms.size() ? s.rooms[size_t(info.room)].name : "#" + std::to_string(info.room);
    return scripted ? name : "Room " + name;
}

// The last export's nav regions (Scene inspector, Navigation section): the
// outlines, the selected one brighter, the portal edges at floor height, and
// "R<k>" labels at the centroids. A label is an item like sceneIcon's
// markers, so clicking it selects the region and does not reach picking.
static void navOverlay(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, State& st,
                       const std::optional<splash::ExportResult>& res) {
    if (!st.navShow || !res || !res->ok()) return;
    const splash::ExportStats& s = res->stats;
    for (size_t i = 0; i < s.navOutlines.size(); i++) {
        const std::vector<splash::Vec3>& poly = s.navOutlines[i];
        if (poly.size() < 3) continue;
        std::vector<ImVec2> p(poly.size());
        bool ok = true;
        for (size_t k = 0; k < poly.size() && ok; k++) ok = view.project(toGl(poly[k]), mn, sz, &p[k]);
        if (!ok) continue;
        const bool hot = int(i) == st.navRegion;
        dl->AddConvexPolyFilled(p.data(), (int)p.size(), overlayCol(kind::nav, hot ? 110 : 40));
        dl->AddPolyline(p.data(), (int)p.size(), overlayCol(hot ? rgb(0xffffff) : kind::nav, hot ? 255 : 170), ImDrawFlags_Closed,
                        hot ? 2.2f : 1.2f);
    }
    if (st.navPortals)
        for (const splash::ExportStats::NavRegionInfo& info : s.navInfo)
            for (const splash::ExportStats::NavPortalInfo& pt : info.portals) {
                ImVec2 a, b;
                if (!view.project(toGl(pt.a), mn, sz, &a) || !view.project(toGl(pt.b), mn, sz, &b)) continue;
                dl->AddLine(a, b, overlayCol(kNavPortal, 230), 2.0f);
            }
    if (!st.navLabels) return;
    const ImRect clip(mn, mn + sz);
    for (size_t i = 0; i < s.navInfo.size(); i++) {
        ImVec2 c;
        if (!view.project(toGl(s.navInfo[i].centroid), mn, sz, &c) || !clip.Contains(c)) continue;
        const std::string label = "R" + std::to_string(i);
        const ImVec2 ts = measure(fonts().medium, type::caption, label.c_str());
        const ImRect lr(c - ImVec2(ts.x * 0.5f + 5, 9), c + ImVec2(ts.x * 0.5f + 5, 9));
        ImGui::PushID((int)i);
        Hit h = interact("navlabel", lr);
        ImGui::PopID();
        nameRect("nav.label/" + std::to_string(i), lr);
        const bool hot = int(i) == st.navRegion;
        dl->AddRectFilled(lr.Min, lr.Max, hot ? overlayCol(kind::nav, 235) : rgb(0x0e1014, 200), 4.f);
        dl->AddRect(lr.Min, lr.Max, overlayCol(kind::nav, (unsigned)(150 + 105 * h.hover)), 4.f, 0, 1.f + 0.6f * h.hover);
        textCentered(dl, lr, fonts().medium, type::caption, hot ? rgb(0x0e1014) : kind::nav, label.c_str());
        if (h.clicked) st.navRegion = int(i);
    }
}

static void sceneOverlays(ImDrawList* dl, viewport::Ps1View& view, ImVec2 mn, ImVec2 sz, const std::vector<splash::FlatObject>& flats,
                          const std::vector<splash::FlatObject>& shown, const splash::Object* sel,
                          const std::optional<splash::ExportResult>& res, State& st) {
    navOverlay(dl, view, mn, sz, st, res);
    // The Rooms a portal can join: the exported ones, as the exporter picks them.
    std::vector<const splash::FlatObject*> rooms;
    for (const splash::FlatObject& fo : shown)
        if (fo.activeInHierarchy && fo.object->active && fo.object->room) rooms.push_back(&fo);
    for (size_t i = 0; i < flats.size() && i < shown.size(); ++i) {
        const splash::FlatObject& fo = shown[i];
        const bool hot = flats[i].object == sel;
        if (fo.object->room) {
            splash::Vec3 lo, hi;
            splash::roomWorldBox(fo, &lo, &hi);
            selectionOutline(dl, view, mn, sz, toGl(lo), toGl(hi), overlayCol(kind::room, hot ? 255 : 190), hot ? 2.0f : 1.5f);
        }
        if (fo.object->portal) {
            // The opening, and once both rooms resolve an arrow along the
            // normal the splashpack stores: from Room A into Room B.
            const splash::PortalComponent& pc = *fo.object->portal;
            const int a = splash::portalRoomIndex(rooms, pc.roomA), b = splash::portalRoomIndex(rooms, pc.roomB);
            const bool linked = a >= 0 && b >= 0 && a != b;
            const splash::PortalFrame pf =
                linked ? splash::portalFrameAtoB(fo, *rooms[size_t(a)], *rooms[size_t(b)]) : splash::portalWorldFrame(fo);
            const ImU32 col = overlayCol(kind::portal, hot ? 255 : 210);
            const float w = hot ? 2.0f : 1.5f;
            ImVec2 c[4];
            bool ok = true;
            const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4 && ok; k++)
                ok = view.project(toGl(pf.center + pf.right * (pf.halfW * sx[k]) + pf.up * (pf.halfH * sy[k])), mn, sz, &c[k]);
            if (ok) {
                dl->AddConvexPolyFilled(c, 4, overlayCol(kind::portal, hot ? 60 : 35));
                dl->AddPolyline(c, 4, col, ImDrawFlags_Closed, w);
            }
            if (linked) {
                const float len = std::clamp(std::max(pf.halfW, pf.halfH), 0.5f, 2.0f), head = std::min(0.3f, len * 0.3f);
                const splash::Vec3 tip = pf.center + pf.normal * len;
                ImVec2 p0, p1, h0, h1;
                if (view.project(toGl(pf.center), mn, sz, &p0) && view.project(toGl(tip), mn, sz, &p1)) {
                    dl->AddLine(p0, p1, col, w + 0.5f);
                    if (view.project(toGl(tip - pf.normal * head + pf.up * head), mn, sz, &h0)) dl->AddLine(p1, h0, col, w + 0.5f);
                    if (view.project(toGl(tip - pf.normal * head - pf.up * head), mn, sz, &h1)) dl->AddLine(p1, h1, col, w + 0.5f);
                    if (view.project(toGl(tip - pf.normal * head + pf.right * head), mn, sz, &h0)) dl->AddLine(p1, h0, col, w + 0.5f);
                    if (view.project(toGl(tip - pf.normal * head - pf.right * head), mn, sz, &h1)) dl->AddLine(p1, h1, col, w + 0.5f);
                }
                // Which side is which: A behind the opening, B past the arrow's tip.
                Fonts& f = fonts();
                ImVec2 la, lb;
                if (view.project(toGl(pf.center - pf.normal * 0.35f + pf.up * 0.6f), mn, sz, &la))
                    dl->AddText(f.semibold, type::body, la - measure(f.semibold, type::body, "A") * 0.5f, col, "A");
                if (view.project(toGl(tip + pf.normal * 0.2f + pf.up * 0.3f), mn, sz, &lb))
                    dl->AddText(f.semibold, type::body, lb - measure(f.semibold, type::body, "B") * 0.5f, col, "B");
            }
        }
        if (fo.object->trigger) {
            splash::Vec3 lo, hi;
            splash::triggerWorldBox(fo, &lo, &hi);
            selectionOutline(dl, view, mn, sz, toGl(lo), toGl(hi), overlayCol(kind::trigger, hot ? 255 : 200), hot ? 2.0f : 1.5f);
        }
        if (fo.object->interactable && fo.object->interactable->radius > 0) {
            const splash::Vec3 c = fo.localToWorld.position();
            const float rad = fo.object->interactable->radius;
            constexpr int kSeg = 48;
            ImVec2 p[kSeg];
            bool ok[kSeg];
            for (int k = 0; k < kSeg; k++) {
                const float a = 6.2831853f * k / kSeg;
                ok[k] = view.project(toGl(c + splash::Vec3{rad * std::cos(a), 0, rad * std::sin(a)}), mn, sz, &p[k]);
            }
            const ImU32 col = overlayCol(kind::interact, hot ? 255 : 210);
            for (int k = 0; k < kSeg; k++)
                if (ok[k] && ok[(k + 1) % kSeg]) dl->AddLine(p[k], p[(k + 1) % kSeg], col, hot ? 2.0f : 1.5f);
        }
    }
}

static void viewportPanel(State& st, ImDrawList* dl, ImRect r, editor::Document& doc, viewport::Ps1View& view) {
    Fonts& f = fonts();
    if (st.play.game.attached()) r = splitViewport(st, dl, r);
    view.clean = st.viewMode == 1;
    unsigned tex = view.render((int)r.GetWidth(), (int)r.GetHeight(), 240);
    dl->AddImageRounded((ImTextureID)(intptr_t)tex, r.Min, r.Max, ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, radius::window);

    ImVec2 mn = r.Min, sz = r.GetSize();
    nameRect("vp.view", r);
    // Overlays follow the camera, so keep them inside the panel.
    ImGui::PushClipRect(r.Min, r.Max, true);
    std::vector<splash::FlatObject> flats = splash::flatten(doc.scene());
    const splash::Object* sel = doc.selected();
    // While the timeline previews a pose, outlines and markers follow the
    // scene as drawn (same flatten order); gizmos, picking and every edit
    // keep reading the document's own transforms.
    const splash::Scene* drawn = view.drawnScene();
    const bool previewing = drawn && drawn != &doc.scene();
    const std::vector<splash::FlatObject> shown = previewing ? splash::flatten(*drawn) : flats;

    // The mouse goes to the first item that claims it, so items are submitted
    // toolbars first, then the gizmo, then scene markers, then the viewport
    // itself; channels keep the drawing in the opposite order.
    enum { kScene, kGizmo, kChrome };
    dl->ChannelsSplit(3);

    // Floating toolbars.
    dl->ChannelsSetCurrent(kChrome);
    ImVec2 p = r.Min + ImVec2(space::md, space::md);
    float w = 0;
    ImRect tools(p, p + ImVec2(4 * 30 + 4, 32));
    dl->AddRectFilled(tools.Min, tools.Max, rgb(0x0e1014, 210), radius::button + 2);
    dl->AddRect(tools.Min, tools.Max, rgb(0xffffff, 14), radius::button + 2);
    const char* tIcons[4] = {icon::hand, icon::move, icon::rotate, icon::scale};
    const char* tTips[4] = {"Hand (Q)", "Move (W)", "Rotate (E)", "Scale (R)"};
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        if (iconButton("tool", ImRect(ImVec2(p.x + 2 + i * 30, p.y + 2), ImVec2(p.x + 2 + i * 30 + 30, p.y + 30)), tIcons[i],
                       st.tool == i, tTips[i]))
            st.tool = i;
        ImGui::PopID();
    }
    ImRect snap(ImVec2(tools.Max.x + space::sm, p.y), ImVec2(tools.Max.x + space::sm + 34, p.y + 32));
    dl->AddRectFilled(snap.Min, snap.Max, rgb(0x0e1014, 210), radius::button + 2);
    dl->AddRect(snap.Min, snap.Max, rgb(0xffffff, 14), radius::button + 2);
    if (iconButton("snap", ImRect(snap.Min + ImVec2(2, 2), snap.Max - ImVec2(2, 2)), icon::magnet, st.snap,
                   st.snap ? "Snap to grid: 0.25 m" : "Snap to grid: off (hold Ctrl to snap to 0.25 m)"))
        st.snap = !st.snap;
    ImRect giz(ImVec2(snap.Max.x + space::sm, p.y), ImVec2(snap.Max.x + space::sm + 34, p.y + 32));
    dl->AddRectFilled(giz.Min, giz.Max, rgb(0x0e1014, 210), radius::button + 2);
    dl->AddRect(giz.Min, giz.Max, rgb(0xffffff, 14), radius::button + 2);
    nameRect("vp.gizmos", giz);
    if (iconButton("gizmos", ImRect(giz.Min + ImVec2(2, 2), giz.Max - ImVec2(2, 2)), icon::box, st.gizmos,
                   st.gizmos ? "Trigger boxes, interaction radii and nav regions: shown" : "Trigger boxes, interaction radii and nav regions: hidden"))
        st.gizmos = !st.gizmos;

    // Right side: view mode and camera.
    float segW = 0;
    {
        // Measure by drawing at a known position after computing width.
        float wa = measure(f.medium, type::label, "PS1").x + space::md * 2;
        float wb = measure(f.medium, type::label, "Clean").x + space::md * 2;
        segW = wa + wb + 4;
    }
    float camW = 128;
    ImVec2 rp(r.Max.x - space::md - segW - space::sm - camW, p.y + 2);
    int cam = dropdownMenu("camera", ImRect(rp, rp + ImVec2(camW, 28)), icon::camera, view.ortho() ? "Orthographic" : "Perspective",
                           {"Perspective", "Orthographic"}, view.ortho() ? 1 : 0);
    if (cam >= 0) view.setOrtho(cam == 1);
    st.viewMode = segmented("viewmode", ImVec2(rp.x + camW + space::sm, p.y + 2), {"PS1", "Clean"}, st.viewMode, &w);
    axisWidget(dl, ImVec2(r.Max.x - 52, r.Min.y + 96), view);

    // Bottom-left chip describing what the edit view is showing.
    const char* info = st.viewMode == 0 ? "320 x 240  ·  15-bit dither  ·  affine" : "Clean view";
    ImVec2 is = measure(f.regular, type::caption, info);
    ImRect chip(ImVec2(r.Min.x + space::md, r.Max.y - space::md - 24), ImVec2(r.Min.x + space::md + is.x + 20, r.Max.y - space::md));
    dl->AddRectFilled(chip.Min, chip.Max, rgb(0x0e1014, 190), radius::pill);
    textCentered(dl, chip, f.regular, type::caption, color::textDim, info);

    // A tool switch (W/E/R) mid-drag ends the drag; what it did so far stays as one undo step.
    if (st.gizmo.handle >= 0 && st.gizmo.tool != st.tool) {
        doc.endMerge();
        if (GImGui->ActiveId == ImGui::GetID("##gizmo")) ImGui::ClearActiveID();
        st.gizmo.handle = -1;
    }

    // Selection outline and the tool's gizmo follow the selected object.
    if (sel) {
        for (size_t si = 0; si < flats.size(); ++si)
            if (const splash::FlatObject& fo = flats[si]; fo.object == sel) {
                const splash::FlatObject& drawnFo = si < shown.size() ? shown[si] : fo;
                dl->ChannelsSetCurrent(kScene);
                viewport::Vec3 lo, hi;
                selectedBoxGl(doc, drawnFo, &lo, &hi);
                selectionOutline(dl, view, mn, sz, lo, hi);
                dl->ChannelsSetCurrent(kGizmo);
                // An object the preview moves (itself or through a parent)
                // shows no gizmo: a drag from the drawn pose would write the
                // previewed value into the document.
                const bool posed = previewing && !samePose(fo, drawnFo, doc.scene().settings.gteScaling);
                if (posed) {
                    const char* note = "Posed by the timeline: its keys move it";
                    ImVec2 ns = measure(f.regular, type::caption, note);
                    ImRect nc(ImVec2(r.Min.x + space::md, r.Max.y - space::md - 24 - space::sm - 24),
                              ImVec2(r.Min.x + space::md + ns.x + 20, r.Max.y - space::md - 24 - space::sm));
                    dl->AddRectFilled(nc.Min, nc.Max, rgb(0x0e1014, 190), radius::pill);
                    textCentered(dl, nc, f.regular, type::caption, color::textDim, note);
                }
                if (posed && st.gizmo.handle >= 0) {
                    doc.endMerge();
                    if (GImGui->ActiveId == ImGui::GetID("##gizmo")) ImGui::ClearActiveID();
                    st.gizmo.handle = -1;
                }
                if (!posed && st.tool >= 1 && st.tool <= 3) {
                    const splash::Object* par = doc.parent(*doc.selection());
                    const splash::Mat34* parentToWorld = nullptr;
                    for (const splash::FlatObject& pf : flats)
                        if (par && pf.object == par) parentToWorld = &pf.localToWorld;
                    if (st.tool == 1) moveGizmo(st, dl, view, r, doc, fo, parentToWorld);
                    if (st.tool == 2) rotateGizmo(st, dl, view, r, doc, fo, parentToWorld);
                    if (st.tool == 3) scaleGizmo(st, dl, view, r, doc, fo);
                }
                break;
            }
    }

    // World streaming regions, outlined on the ground while the scene's own
    // settings are in the inspector (nothing selected).
    if (!sel && doc.scene().settings.streamWorld) {
        const std::optional<splash::ExportResult>& res = st.live.result();
        if (res && res->ok()) {
            const splash::ExportStats& s = res->stats;
            const float gte = doc.scene().settings.gteScaling > 0 ? doc.scene().settings.gteScaling : 100.f;
            dl->ChannelsSetCurrent(kScene);
            for (size_t i = 0; i < s.streamRegionBounds.size(); i++) {
                const auto& b = s.streamRegionBounds[i];
                const float y = i < s.streamRegionFloor.size() ? s.streamRegionFloor[i] : 0.f;
                const float x0 = b[0] * gte / 4096.f, z0 = b[1] * gte / 4096.f, x1 = b[2] * gte / 4096.f, z1 = b[3] * gte / 4096.f;
                const splash::Vec3 c[4] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
                ImVec2 p[4];
                bool ok = true;
                for (int k = 0; k < 4 && ok; k++) ok = view.project(toGl(c[k]), mn, sz, &p[k]);
                if (!ok) continue;
                dl->AddQuadFilled(p[0], p[1], p[2], p[3], (color::accentHover & 0x00ffffffu) | (18u << IM_COL32_A_SHIFT));
                dl->AddQuad(p[0], p[1], p[2], p[3], (color::accentHover & 0x00ffffffu) | (150u << IM_COL32_A_SHIFT), 1.2f);
            }
        }
    }

    if (st.gizmos) {
        dl->ChannelsSetCurrent(kScene);
        sceneOverlays(dl, view, mn, sz, flats, shown, sel, st.live.result(), st);
    }
    // Scripted tests: the nav bake's counts and the selected region.
    {
        const std::optional<splash::ExportResult>& lr = st.live.result();
        const splash::ExportStats* ns = lr && lr->ok() ? &lr->stats : nullptr;
        if (ns && st.navRegion >= ns->navRegions) st.navRegion = -1;
        if (namedRectsOn() && ImGui::GetFrameCount() > 1 && ns) {
            static std::string reportedCounts, reportedRegion;
            auto report = [](std::string& last, const std::string& now2) {
                if (last == now2) return;
                last = now2;
                std::printf("%s\n", now2.c_str());
                std::fflush(stdout);
            };
            report(reportedCounts, "nav: regions " + std::to_string(ns->navRegions) + " portals " + std::to_string(ns->navPortals));
            std::string line = "nav: region none";
            if (st.navRegion >= 0 && size_t(st.navRegion) < ns->navInfo.size()) {
                const splash::ExportStats::NavRegionInfo& info = ns->navInfo[size_t(st.navRegion)];
                std::string js;
                for (const auto& pt : info.portals) js += (js.empty() ? "" : ",") + std::to_string(pt.neighbor);
                line = "nav: region " + std::to_string(st.navRegion) + " floor " + fmtFixed(info.floorY) + " room " +
                       navRoomName(*ns, info, true) + " portals " + (js.empty() ? "-" : js);
            }
            report(reportedRegion, line);
        }
    }

    // The BVH, on its own toggle (Scene inspector, BVH section).
    {
        const std::optional<splash::ExportResult>& lr = st.live.result();
        const splash::ExportStats* bs = lr && lr->ok() ? &lr->stats : nullptr;
        int drawn = -1, depth = 0;
        if (st.bvhShow && bs) {
            dl->ChannelsSetCurrent(kScene);
            depth = bvhShownDepth(st, *bs);
            drawn = bvhOverlay(dl, view, mn, sz, *bs, depth);
        }
        if (namedRectsOn() && ImGui::GetFrameCount() > 1) {
            // Scripted tests: the tree once it exports, and what the viewport draws.
            static std::string reportedTree, reportedShow;
            auto report = [](std::string& last, const std::string& now2) {
                if (last == now2) return;
                last = now2;
                if (now2.empty()) return;
                std::printf("%s\n", now2.c_str());
                std::fflush(stdout);
            };
            if (bs) {
                int leaves = 0;
                for (const auto& b : bs->bvhBoxes) leaves += b.leaf ? 1 : 0;
                report(reportedTree, "bvh: nodes " + std::to_string(bs->bvhBoxes.size()) + " leaves " + std::to_string(leaves) +
                                         " depth " + std::to_string(bs->bvhDepth));
                report(reportedShow, drawn >= 0 ? "bvh: showing depth " + std::to_string(depth) + " boxes " + std::to_string(drawn)
                                                : std::string("bvh: hidden"));
            }
        }
    }

    // Markers: a lightbulb for lights, a generic marker for objects with
    // neither mesh nor light (cameras, spawns, audio). Grouping nodes and
    // script-only objects get none. Clicking one selects its object.
    dl->ChannelsSetCurrent(kScene);
    std::vector<editor::ObjectPath> paths = doc.flatPaths();
    for (size_t i = 0; i < shown.size() && i < flats.size(); ++i) {
        const splash::FlatObject& fo = flats[i];
        viewport::Vec3 pos = toGl(shown[i].localToWorld.position());
        const char* ic = nullptr;
        ImU32 col = 0;
        if (fo.object->light) ic = icon::lightbulb, col = kind::light;
        else if (!fo.object->mesh && fo.object->children.empty() && !fo.object->script) ic = icon::square, col = kind::folder;
        if (!ic) continue;
        ImGui::PushID((int)i);
        if (sceneIcon(dl, view, mn, sz, pos, ic, col) && i < paths.size()) doc.select(paths[i]);
        ImGui::PopID();
    }
    if (previewing) previewCamera(dl, view, mn, sz, view.preview());
    dl->ChannelsMerge();

    viewportInput(st, r, doc, view, flats);
    ImGui::PopClipRect();
}

// Fixed two decimals, as position and scale read in a column ("0.50").
static std::string fmtFixed(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", static_cast<double>(v));
    std::string s = b;
    if (s == "-0.00") s = "0.00";
    return s;
}

// Up to two decimals with trailing zeros dropped ("15", "12.5", "0.75").
static std::string fmtShort(float v, int decimals = 2) {
    std::string s = fmtFixed(v);
    if (decimals != 2) {
        char b[32];
        std::snprintf(b, sizeof b, "%.*f", decimals, static_cast<double>(v));
        s = b;
    }
    if (s.find('.') != std::string::npos) {  // "20" with no decimals keeps its zero
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

static std::string fmtKB(int bytes) { return fmtShort(static_cast<float>(bytes) / 1024.0f) + " KB"; }

// Unity's Euler order (Z, then X, then Y), in degrees within -180..180.
static splash::Vec3 eulerDegrees(splash::Quat q) {
    const double kDeg = 57.29577951308232;
    double x = q.x, y = q.y, z = q.z, w = q.w;
    double sx = 2 * (w * x - y * z);
    double ex, ey, ez;
    if (std::fabs(sx) > 0.99999) {
        ex = std::copysign(90.0, sx);
        ey = std::atan2(-2 * (x * z - w * y), 1 - 2 * (y * y + z * z)) * kDeg;
        ez = 0;
    } else {
        ex = std::asin(sx) * kDeg;
        ey = std::atan2(2 * (x * z + w * y), 1 - 2 * (x * x + y * y)) * kDeg;
        ez = std::atan2(2 * (x * y + w * z), 1 - 2 * (x * x + z * z)) * kDeg;
    }
    return {static_cast<float>(ex), static_cast<float>(ey), static_cast<float>(ez)};
}

// Quaternion.Euler: rotates around Z, then X, then Y (degrees).
static splash::Quat quatFromEuler(splash::Vec3 deg) {
    const double h = 3.14159265358979323846 / 360.0;  // half angle, in radians per degree
    double cx = std::cos(deg.x * h), sx = std::sin(deg.x * h);
    double cy = std::cos(deg.y * h), sy = std::sin(deg.y * h);
    double cz = std::cos(deg.z * h), sz = std::sin(deg.z * h);
    // qY * qX * qZ
    return {static_cast<float>(cy * sx * cz + sy * cx * sz), static_cast<float>(sy * cx * cz - cy * sx * sz),
            static_cast<float>(cy * cx * sz - sy * sx * cz), static_cast<float>(cy * cx * cz + sy * sx * sz)};
}

static ImU32 toColor(const std::array<float, 3>& c) {
    auto u = [](float v) { return static_cast<int>(splash::clampv(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(u(c[0]), u(c[1]), u(c[2]), 255);
}

static std::string toHex(const std::array<float, 3>& c) {
    auto u = [](float v) { return static_cast<unsigned>(splash::clampv(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    char b[16];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", u(c[0]), u(c[1]), u(c[2]));
    return b;
}

static const char* bppLabel(splash::BitDepth d) {
    switch (d) {
        case splash::BitDepth::Bpp4: return "4 bpp";
        case splash::BitDepth::Bpp8: return "8 bpp";
        case splash::BitDepth::Bpp16: return "16 bpp";
    }
    return "";
}

static const char* lightingLabel(splash::VertexColorMode m) {
    switch (m) {
        case splash::VertexColorMode::Baked: return "Baked vertex";
        case splash::VertexColorMode::Flat: return "Flat";
        case splash::VertexColorMode::Mesh: return "Mesh colours";
    }
    return "";
}

static const char* colliderLabel(splash::ColliderKind k) {
    switch (k) {
        case splash::ColliderKind::Static: return "Static";
        case splash::ColliderKind::Dynamic: return "Dynamic";
        case splash::ColliderKind::None: return "None";
    }
    return "";
}

static const char* lightKindLabel(splash::LightKind k) {
    switch (k) {
        case splash::LightKind::Point: return "Point";
        case splash::LightKind::Spot: return "Spot";
        case splash::LightKind::Directional: return "Directional";
    }
    return "";
}

static std::string kb(size_t bytes);

// With nothing selected (or the scene row picked) the inspector shows the
// scene's own settings. For now: psxsplash's render buffers, sized by the
// exporter unless overridden.
static void sceneInspector(State& st, ImDrawList* dl, ImRect r, float y, editor::Document& doc) {
    Fonts& f = fonts();
    ImGui::PushClipRect(ImVec2(r.Min.x, y), ImVec2(r.Max.x, r.Max.y - 1), true);
    const float x0 = r.Min.x + space::md, x1 = r.Max.x - space::md;
    const splash::SceneSettings& set = doc.scene().settings;
    // Taller than the panel, it scrolls with the wheel.
    static float scroll = 0;
    const float top0 = y;
    y -= scroll;

    ImRect ic(ImVec2(x0, y), ImVec2(x0 + 40, y + 40));
    dl->AddRectFilled(ic.Min, ic.Max, (color::accentHover & 0x00ffffffu) | (34u << IM_COL32_A_SHIFT), radius::card);
    textCentered(dl, ic, f.medium, type::icon + 3, color::accentHover, icon::layers);
    text(dl, ImVec2(ic.Max.x + space::md, y + 2), f.semibold, type::title, color::text, doc.sceneStem().c_str());
    text(dl, ImVec2(ic.Max.x + space::md, y + 22), f.regular, type::caption, color::textFaint, "Scene settings");
    y += 40 + space::lg;

    const std::optional<splash::ExportResult>& res = st.live.result();
    const splash::ExportStats* stats = res && res->ok() ? &res->stats : nullptr;
    const float lw = 112, rowH = 32;
    auto row = [&](const char* id, const char* label, const char* tip) {
        ImRect rr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + rowH));
        y += rowH;
        return property(id, rr, lw, label, tip);
    };
    auto caption = [&](const std::string& s, ImU32 col) {
        float tx = x0 + space::sm + lw;
        dl->AddText(f.regular, type::caption, ImVec2(tx, y - 2), col, s.c_str(), nullptr, x1 - space::sm - tx);
        y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, x1 - space::sm - tx, s.c_str()).y + space::xs;
    };

    // The scene's own script: onSceneCreationStart/End and anything scene-wide.
    {
        const float stop = y;
        const bool open = section("s_scenescript", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::script, kind::script, "Script",
                                  true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        const std::string& lua = set.script;
        const bool found = !lua.empty() && doc.fileExists(lua);
        if (slotField(st, static_cast<int>(AssetSlot::SceneLua), {}, "scenelua", row("lscenelua", "File", "The script, blocks or Lua, that runs for the whole scene. Click to pick one."),
                       icon::fileCode, lua.empty() || found ? kind::script : color::warn, lua.empty() ? "None" : fileName(lua).c_str(),
                       nullptr))
            pickAsset(AssetSlot::SceneLua, {});
        const ImVec2 at(x0 + space::sm + lw, y + space::xxs);
        if (lua.empty()) {
            if (auto lang = newScriptButtons("newsceneblocks", "newscenelua", at, x1 - space::sm,
                                             "Writes scripts/<scene name>.blocks with a when-the-scene-starts stack, and opens it.",
                                             "Writes scripts/<scene name>.lua with onSceneCreationStart and onSceneCreationEnd, and opens it."))
                newScript(st, doc, nullptr, *lang);
        } else if (button("editscenelua", at, icon::fileCode, found ? "Edit script" : "Create script", ButtonKind::Secondary, nullptr,
                          "Opens the file in the script editor, beside the Scene.")) {
            openScript(st, doc, lua);
        }
        y += size::field + space::sm;
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // Cutscenes and animations, each with Edit in timeline.
    {
        const float stop = y;
        const bool open = section("s_sequences", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::camera, rgb(0x5ccfe0),
                                  "Cutscenes and animations", true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y = st.timeline.sequenceList(dl, x0 + space::sm, x1 - space::sm, y + 34 + space::xs, doc);
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // Fog, GTE scaling and scene type: what the old exporter inspector had.
    {
        const float stop = y;
        const bool open = section("s_rendering", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::eye, color::textDim, "Rendering",
                                  true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        const float gteNow = set.gteScaling > 0 ? set.gteScaling : 100.f;
        // psxsplash clears the screen to this colour every frame (renderer.cpp
        // SetFog), fog or not; the pack keeps it in the fog block.
        const ImRect colRow = row("lfogcol", "Background",
                                  "The screen clears to this colour every frame, fog on or off. With fog on, distant "
                                  "triangles also fade to it. Click to pick one.");
        nameRect("scene.fogcol", colRow);
        if (colorField("fogcol", colRow, toColor(set.fog.color), toHex(set.fog.color).c_str())) ImGui::OpenPopup("##fogcolpick");
        ImGui::SetNextWindowPos(ImVec2(colRow.Max.x, colRow.Max.y + space::xs), ImGuiCond_Always, ImVec2(1, 0));
        pushPopupStyle();
        if (ImGui::BeginPopup("##fogcolpick")) {
            float c[3] = {set.fog.color[0], set.fog.color[1], set.fog.color[2]};
            ImGui::SetNextItemWidth(220);
            if (ImGui::ColorPicker3("##fogcolwheel", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex |
                                                            ImGuiColorEditFlags_NoAlpha))
                doc.editSettings([&](splash::SceneSettings& s) { s.fog.color = {c[0], c[1], c[2]}; }, "fogcol");
            if (ImGui::IsItemDeactivated()) doc.endMerge();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        ImRect v = row("lfog", "Fog", "Fades distant triangles to the background colour, and culls anything wholly past the fog wall.");
        const ImRect fogSw(ImVec2(v.Min.x, v.GetCenter().y - 8), ImVec2(v.Min.x + 30, v.GetCenter().y + 8));
        nameRect("scene.fog", fogSw);
        if (toggle("fogon", fogSw, set.fog.enabled))
            doc.editSettings([](splash::SceneSettings& s) { s.fog.enabled = !s.fog.enabled; });
        const int density = std::clamp(set.fog.density, 1, 10), wall = 20000 / density;
        char tip[256];
        std::snprintf(tip, sizeof tip,
                      "1 to 10: higher is thicker. The fog wall sits at 20000 / density depth units, %d now (%.1f m at this "
                      "GTE scaling); nothing wholly past it is drawn.",
                      wall, double(wall) * gteNow / 4096.0);
        const ImRect denRow = row("lfogden", "Density", tip);
        nameRect("scene.fogdensity", denRow);
        float dv = float(density);
        FieldEdit fe = numberField("fogden", denRow, std::to_string(density).c_str(), nullptr, 0, nullptr, &dv);
        if (fe.changed) {
            const int n = std::clamp(int(std::lround(dv)), 1, 10);
            doc.editSettings([n](splash::SceneSettings& s) { s.fog.density = n; }, "fogdensity");
        }
        if (fe.done) doc.endMerge();
        if (!set.fog.enabled) caption("Off: density is kept for when fog is on.", color::textFaint);
        const ImRect gteRow = row("lgte", "GTE scaling",
                                  "Editor units per PS1 unit. Positions export as value / scaling in 16-bit 4.12 fixed point, so "
                                  "everything must sit within 8 x scaling of the origin. Smaller is finer, larger reaches farther.");
        nameRect("scene.gte", gteRow);
        float gv = gteNow;
        fe = numberField("gte", gteRow, fmtShort(gteNow).c_str(), nullptr, 0, nullptr, &gv);
        if (fe.changed) {
            const float g = std::clamp(gv, 0.1f, 10000.f);
            doc.editSettings([g](splash::SceneSettings& s) { s.gteScaling = g; }, "gte");
        }
        if (fe.done) doc.endMerge();
        const ImRect typeRow = row("lscenetype", "Scene type",
                                   "Written to the pack, but psxsplash does not read it: culling follows the Room objects. With "
                                   "any Room the scene draws room by room through portals; with none it frustum-culls the "
                                   "BVH. Both types draw the same way.");
        nameRect("scene.scenetype", typeRow);
        const bool interior = set.sceneType == splash::SceneType::Interior;
        int pick = dropdownMenu("scenetype", typeRow, nullptr, interior ? "Interior" : "Exterior", {"Exterior", "Interior"}, interior);
        if (pick >= 0) {
            const splash::SceneType t = pick ? splash::SceneType::Interior : splash::SceneType::Exterior;
            if (t != set.sceneType) doc.editSettings([t](splash::SceneSettings& s) { s.sceneType = t; });
        }
        // The culling psxsplash will use, from the Room objects the exporter
        // writes (rooms.cpp collectRoomObjects: on in the hierarchy).
        size_t roomCount = 0;
        {
            const std::vector<splash::FlatObject> flat = splash::flatten(doc.scene());
            std::vector<std::string> roomErrors;
            roomCount = splash::collectRoomObjects(flat, roomErrors).rooms.size();
        }
        const float cullTop = y;
        caption(roomCount ? "Culling: rooms and portals, " + std::to_string(roomCount) + (roomCount == 1 ? " room." : " rooms.")
                          : std::string("Culling: frustum (BVH). Add a Room to cull by rooms and portals."),
                color::textFaint);
        nameRect("scene.culling", ImRect(ImVec2(x0, cullTop - 2), ImVec2(x1, y)));
        if (namedRectsOn()) {
            // Scripted tests: the section's values when they change.
            char now[128];
            std::snprintf(now, sizeof now, "scene: fog %d density %d colour %s gte %g type %s culling %s", set.fog.enabled ? 1 : 0,
                          set.fog.density, toHex(set.fog.color).c_str(), double(set.gteScaling), interior ? "interior" : "exterior",
                          roomCount ? ("rooms " + std::to_string(roomCount)).c_str() : "frustum");
            static std::string reported;
            if (reported != now) {
                reported = now;
                std::printf("%s\n", now);
                std::fflush(stdout);
            }
        }
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // The loading screen: one of the scene's UI canvases, shown by psxsplash
    // while the scene loads and left out of the scene itself.
    {
        const float stop = y;
        const bool open = section("s_loading", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::layoutTemplate, color::textDim,
                                  "Loading screen", true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        const std::string& pickName = set.loadingCanvas;
        const splash::UICanvas* picked = nullptr;
        std::vector<const char*> items{"None"};
        int current = pickName.empty() ? 0 : -1;
        for (const splash::UICanvas& cv : doc.scene().canvases) {
            if (!pickName.empty() && !picked && cv.name == pickName) {
                picked = &cv;
                current = int(items.size());
            }
            items.push_back(cv.name.c_str());
        }
        const ImRect canvasRow = row("lloading", "Canvas",
                                     "A UI canvas of this scene that psxsplash draws while the scene loads. Its progress bar "
                                     "named \"loading\" fills as the scene comes in.");
        nameRect("scene.loading", canvasRow);
        const int pick = dropdownMenu("scene.loading", canvasRow, icon::layoutTemplate, pickName.empty() ? "None" : pickName.c_str(),
                                      items, current);
        std::string chosen = pick > 0 ? std::string(items[size_t(pick)]) : std::string();
        const bool isPick = pick >= 0;
        caption(pickName.empty() ? "Shown while the scene loads. Pick a UI canvas."
                                 : "Drawn while the scene loads; the canvas is left out of the scene.",
                color::textFaint);
        // What the engine will do with the pick: worked out from the scene, not an export.
        bool noBar = false, lines = false;
        const bool missing = !pickName.empty() && !picked;
        if (picked) {
            noBar = std::none_of(picked->elements.begin(), picked->elements.end(), [](const splash::UIElement& e) {
                return e.type == splash::UIElementType::Progress && e.name == "loading";
            });
            lines = std::any_of(picked->elements.begin(), picked->elements.end(),
                                [](const splash::UIElement& e) { return e.type == splash::UIElementType::Line; });
        }
        if (missing) {
            const float top = y;
            caption("Canvas \"" + pickName + "\" no longer exists: export stops until another canvas or None is picked.",
                    color::warn);
            nameRect("scene.loadingmissing", ImRect(ImVec2(x0 + space::sm + lw, top), ImVec2(x1 - space::sm, y)));
        }
        if (noBar) caption("No progress bar named \"loading\": the screen shows without one.", color::warn);
        if (lines) caption("Lines are not drawn on the loading screen.", color::warn);
        if (namedRectsOn()) {
            // Scripted tests: the pick and its warnings when they change (not the scene's first frame).
            std::string warns;
            if (missing) warns += "missing";
            if (noBar) warns += std::string(warns.empty() ? "" : ",") + "bar";
            if (lines) warns += std::string(warns.empty() ? "" : ",") + "line";
            const std::string now = "scene: loading " + (pickName.empty() ? std::string("none") : pickName);
            const std::string nowWarn = "scene: loading warnings " + (warns.empty() ? std::string("none") : warns);
            static unsigned seenLoad = ~0u;
            static std::string reported, reportedWarn;
            if (seenLoad != doc.loadId()) {
                seenLoad = doc.loadId();
                reported = now;
                reportedWarn = nowWarn;
            }
            if (reported != now) {
                reported = now;
                std::printf("%s\n", now.c_str());
            }
            if (reportedWarn != nowWarn) {
                reportedWarn = nowWarn;
                std::printf("%s\n", nowWarn.c_str());
            }
            std::fflush(stdout);
        }
        if (isPick && chosen != pickName) doc.editSettings([chosen](splash::SceneSettings& s) { s.loadingCanvas = chosen; });
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // The build list (scenes.list): the scenes Play and Export write, in order.
    // A project setting, saved as soon as it changes and not part of undo.
    {
        namespace fs = std::filesystem;
        const fs::path root = doc.projectRoot();
        static fs::path cachedRoot;
        static std::vector<fs::path> list, scenes;
        static double readAt = -1;
        const double now = ImGui::GetTime();
        // Read again now and then, so files added or a list edited outside show up.
        if (root != cachedRoot || now - readAt > 1.0) {
            cachedRoot = root;
            readAt = now;
            list = editor::loadBuildList(root);
            scenes.clear();
            std::error_code ec;
            if (!root.empty())
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                     !ec && it != end; it.increment(ec))
                    if (it->path().extension() == ".scene" && it->is_regular_file(ec))
                        scenes.push_back(it->path().lexically_relative(root));
            std::sort(scenes.begin(), scenes.end());
        }
        auto save = [&](std::vector<fs::path> next) {
            if (editor::saveBuildList(root, next)) list = std::move(next);
            else notify(st, "Cannot write " + utf8(root / "scenes.list"), true);
            readAt = now;
        };
        const fs::path open = openSceneFile(doc);
        auto note = [&](const char* t) {
            const float tx = x0 + space::sm, w = x1 - space::sm - tx;
            dl->AddText(f.regular, type::caption, ImVec2(tx, y), color::textFaint, t, nullptr, w);
            y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t).y + space::xs;
        };
        auto listed = [&](const fs::path& p) {
            for (const fs::path& e : list)
                if (e.lexically_normal() == p.lexically_normal()) return true;
            return false;
        };

        const float stop = y;
        const bool sOpen = section("s_build", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::layers, color::textDim, "Build scenes",
                                   true, true, false);
        if (!sOpen) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        int move = -1, moveBy = 0, remove = -1, shown = 0;
        std::error_code ec;
        for (size_t i = 0; i < list.size(); ++i) {
            const ImRect rr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + 28));
            const bool there = fs::is_regular_file(root / list[i], ec);
            const bool isOpen = list[i].lexically_normal() == open.lexically_normal();
            // Missing files are left out of the build, so they get no number.
            const std::string num = there ? std::to_string(shown++) : "-";
            const float ty = centerY(f.regular, type::body, rr.Min.y, rr.Max.y);
            text(dl, ImVec2(rr.Min.x + space::xs, ty), f.medium, type::body, color::textFaint, num.c_str());
            const std::string label = utf8(list[i].generic_u8string()) + (there ? (isOpen ? "  (open)" : "") : "  (missing)");
            dl->AddText(f.regular, type::body, ImVec2(rr.Min.x + 28, ty), there ? (isOpen ? color::text : color::textDim) : color::warn,
                        label.c_str(), nullptr);
            ImGui::PushID(int(i));
            const float bw = 22;
            const ImRect up(ImVec2(rr.Max.x - 3 * bw - 4, rr.Min.y + 3), ImVec2(rr.Max.x - 2 * bw - 4, rr.Max.y - 3));
            const ImRect down(ImVec2(rr.Max.x - 2 * bw - 2, rr.Min.y + 3), ImVec2(rr.Max.x - bw - 2, rr.Max.y - 3));
            const ImRect del(ImVec2(rr.Max.x - bw, rr.Min.y + 3), ImVec2(rr.Max.x, rr.Max.y - 3));
            nameRect("build.up." + std::to_string(i), up);
            nameRect("build.down." + std::to_string(i), down);
            nameRect("build.remove." + std::to_string(i), del);
            if (iconButton("bup", up, icon::arrowUp, false, "Move up", color::textDim, i > 0)) {
                move = int(i);
                moveBy = -1;
            }
            if (iconButton("bdown", down, icon::arrowDown, false, "Move down", color::textDim, i + 1 < list.size())) {
                move = int(i);
                moveBy = 1;
            }
            if (iconButton("bremove", del, icon::x, false, "Take out of the build")) remove = int(i);
            ImGui::PopID();
            y += 28;
        }
        if (move >= 0) {
            std::vector<fs::path> next = list;
            std::swap(next[size_t(move)], next[size_t(move + moveBy)]);
            save(std::move(next));
        } else if (remove >= 0) {
            std::vector<fs::path> next = list;
            next.erase(next.begin() + remove);
            save(std::move(next));
        }
        if (list.empty())
            note("Empty: Play and Export write the open scene alone.");
        std::vector<std::string> names;
        for (const fs::path& p : scenes)
            if (!listed(p)) names.push_back(utf8(p.generic_u8string()));
        std::vector<const char*> items;
        for (const std::string& n : names) items.push_back(n.c_str());
        const ImRect addRow(ImVec2(x0 + space::sm, y + space::xs), ImVec2(x0 + space::sm + 220, y + space::xs + size::field));
        nameRect("build.add", addRow);
        const int pick = dropdownMenu("build.add", addRow, icon::plus, names.empty() ? "Every scene is listed" : "Add scene", items, -1);
        if (pick >= 0) {
            std::vector<fs::path> next = list;
            const fs::path picked = fromUtf8(names[size_t(pick)]);
            // The first scene added to an empty list goes after the open one,
            // which stays scene 0 as it was before there was a list.
            if (next.empty() && !open.empty() && picked.lexically_normal() != open.lexically_normal() &&
                fs::is_regular_file(root / open, ec))
                next.push_back(open.generic_u8string());
            next.push_back(picked);
            save(std::move(next));
        }
        y += size::field + space::xs + space::sm;
        note("Play and Export start at scene 0. Scene.Load(i) opens scene i.");
        if (namedRectsOn()) {
            // Scripted tests: the list after every change.
            std::string now = "build:";
            for (size_t i = 0; i < list.size(); ++i)
                now += (i ? " | " : " ") + std::to_string(i) + " " + utf8(list[i].generic_u8string());
            if (list.empty()) now += " (empty)";
            static std::string reported;
            if (reported != now) {
                reported = now;
                std::printf("%s\n", now.c_str());
                std::fflush(stdout);
            }
        }
        if (!sOpen) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // The project's VRAM layout (vram.json): a map of where export packs the
    // framebuffers, atlases, CLUTs and fonts, and the prohibited areas the
    // packer keeps textures and CLUTs out of. A project setting, saved as soon
    // as it changes and not part of undo.
    {
        namespace fs = std::filesystem;
        using K = splash::ExportStats::VramKind;
        const fs::path root = doc.projectRoot();
        static fs::path cachedRoot;
        static splash::VramSettings vs;
        static double readAt = -1;
        const double now = ImGui::GetTime();
        auto same = [](const splash::VramSettings& a, const splash::VramSettings& b) {
            if (a.prohibited.size() != b.prohibited.size()) return false;
            for (size_t i = 0; i < a.prohibited.size(); ++i) {
                const auto &p = a.prohibited[i], &q = b.prohibited[i];
                if (p.x != q.x || p.y != q.y || p.w != q.w || p.h != q.h) return false;
            }
            return true;
        };
        // Read again now and then, so an edit made outside shows up (and re-exports).
        if (root != cachedRoot || now - readAt > 1.0) {
            const bool moved = root != cachedRoot;
            cachedRoot = root;
            readAt = now;
            splash::VramSettings disk = editor::loadVramSettings(root);
            if (!same(disk, vs)) {
                vs = std::move(disk);
                if (!moved) st.live.settingsChanged();
            }
        }
        auto save = [&](splash::VramSettings next) {
            for (auto& a : next.prohibited) a = splash::clampVramArea(a);
            if (editor::saveVramSettings(root, next)) {
                vs = std::move(next);
                st.live.settingsChanged();
            } else {
                notify(st, "Cannot write " + utf8(root / "vram.json"), true);
            }
            readAt = now;
        };
        auto note = [&](const std::string& t, ImU32 col) {
            const float tx = x0 + space::sm, w = x1 - space::sm - tx;
            dl->AddText(f.regular, type::caption, ImVec2(tx, y), col, t.c_str(), nullptr, w);
            y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t.c_str()).y + space::xs;
        };

        const float stop = y;
        const bool sOpen = section("s_vram", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::grid, color::textDim, "VRAM layout",
                                   true, true, false);
        if (!sOpen) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;

        // What occupies VRAM: the live export's map; before there is one, the
        // fixed reservations and the areas as they are set.
        std::vector<splash::ExportStats::VramRect> map;
        if (res && !res->stats.vramMap.empty()) {
            map = res->stats.vramMap;
        } else {
            map.push_back({K::Framebuffer, 0, 0, 320, 240, "Framebuffer 0 (320x240)"});
            map.push_back({K::Framebuffer, 0, 256, 320, 240, "Framebuffer 1 (320x240)"});
            map.push_back({K::Reserved, 960, 0, 64, 512, "Font column (x 960, reserved)"});
            for (size_t i = 0; i < vs.prohibited.size(); ++i) {
                const auto& a = vs.prohibited[i];
                map.push_back({K::Prohibited, a.x, a.y, a.w, a.h, "Prohibited area " + std::to_string(i + 1)});
            }
        }
        auto kindColor = [](K k) -> ImU32 {
            switch (k) {
                case K::Framebuffer: return rgb(0x4f9bef);
                case K::Reserved: return rgb(0x7d8496);
                case K::Prohibited: return color::bad;
                case K::Atlas4: return rgb(0x5ccfe0);
                case K::Atlas8: return color::good;
                case K::Atlas16: return color::accent;
                case K::Clut: return color::warn;
                case K::Font: return rgb(0xe8d36a);
            }
            return color::textDim;
        };
        auto withAlpha = [](ImU32 c, unsigned a) { return (c & 0x00ffffffu) | (a << IM_COL32_A_SHIFT); };

        const float mx0 = x0 + space::sm, mx1 = x1 - space::sm;
        const float scale = (mx1 - mx0) / float(splash::kVramWidth);
        const ImRect mapR(ImVec2(mx0, y), ImVec2(mx1, y + std::floor(scale * splash::kVramHeight)));
        nameRect("vram.map", mapR);
        auto toScreen = [&](int x, int yy, int w, int h) {
            ImVec2 a(mapR.Min.x + std::floor(x * scale), mapR.Min.y + std::floor(yy * scale));
            ImVec2 b(mapR.Min.x + std::floor((x + w) * scale), mapR.Min.y + std::floor((yy + h) * scale));
            // A CLUT is one line: keep it visible.
            if (b.y - a.y < 2) b.y = a.y + 2;
            if (b.x - a.x < 2) b.x = a.x + 2;
            return ImRect(a, b);
        };
        auto snap8 = [&](float px, float py) {
            int vx = int(std::lround((px - mapR.Min.x) / scale / 8.f)) * 8;
            int vy = int(std::lround((py - mapR.Min.y) / scale / 8.f)) * 8;
            return std::array<int, 2>{std::clamp(vx, 0, splash::kVramWidth), std::clamp(vy, 0, splash::kVramHeight)};
        };

        dl->AddRectFilled(mapR.Min, mapR.Max, color::field, radius::field);
        // Texture pages are 64 words by 256 lines.
        for (int gx = 64; gx < splash::kVramWidth; gx += 64)
            dl->AddLine(ImVec2(mapR.Min.x + std::floor(gx * scale), mapR.Min.y),
                        ImVec2(mapR.Min.x + std::floor(gx * scale), mapR.Max.y), rgb(0xffffff, 10));
        dl->AddLine(ImVec2(mapR.Min.x, mapR.Min.y + std::floor(256 * scale)), ImVec2(mapR.Max.x, mapR.Min.y + std::floor(256 * scale)),
                    rgb(0xffffff, 22));

        const Hit mh = interact("vram.map", mapR);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hovered = -1;
        // Drawn back to front: prohibited areas last, over what they cover.
        const K order[] = {K::Framebuffer, K::Reserved, K::Atlas16, K::Atlas8, K::Atlas4, K::Font, K::Clut, K::Prohibited};
        for (K k : order)
            for (size_t i = 0; i < map.size(); ++i) {
                const auto& m = map[i];
                if (m.kind != k) continue;
                const ImRect rr = toScreen(m.x, m.y, m.w, m.h);
                const ImU32 c = kindColor(k);
                const bool prohibited = k == K::Prohibited;
                dl->AddRectFilled(rr.Min, rr.Max, withAlpha(c, prohibited ? 120u : k == K::Clut ? 230u : 80u));
                if (k != K::Clut) dl->AddRect(rr.Min, rr.Max, withAlpha(c, prohibited ? 255u : 200u));
                if (k == K::Framebuffer && rr.GetWidth() > 90) {
                    const std::string t = "Framebuffer " + std::to_string(m.y >= 256 ? 1 : 0);
                    textCentered(dl, rr, f.regular, type::caption, withAlpha(c, 220u), t.c_str());
                }
                if (mh.hovered && rr.Contains(mouse)) hovered = int(i);  // last drawn wins
            }
        // A drag on the map draws a new prohibited area, snapped to 8 words and lines.
        static bool dragging = false;
        static std::array<int, 2> dragFrom{}, dragTo{};
        if (mh.held && ImGui::IsMouseDragPastThreshold(0, 3.f)) {
            dragging = true;
            dragFrom = snap8(ImGui::GetIO().MouseClickedPos[0].x, ImGui::GetIO().MouseClickedPos[0].y);
            dragTo = snap8(mouse.x, mouse.y);
        }
        if (dragging) {
            const int ax = std::min(dragFrom[0], dragTo[0]), ay = std::min(dragFrom[1], dragTo[1]);
            const int aw = std::abs(dragTo[0] - dragFrom[0]), ah = std::abs(dragTo[1] - dragFrom[1]);
            if (mh.held) {
                const ImRect rr = toScreen(ax, ay, std::max(aw, 1), std::max(ah, 1));
                dl->AddRectFilled(rr.Min, rr.Max, withAlpha(color::bad, 90u));
                dl->AddRect(rr.Min, rr.Max, color::text);
                const std::string t = std::to_string(ax) + "," + std::to_string(ay) + "  " + std::to_string(aw) + "x" + std::to_string(ah);
                text(dl, ImVec2(rr.Max.x + 4, rr.Max.y + 2), f.regular, type::caption, color::text, t.c_str());
            } else {
                dragging = false;
                if (aw >= 8 && ah >= 8) {
                    splash::VramSettings next = vs;
                    next.prohibited.push_back({ax, ay, aw, ah});
                    save(std::move(next));
                }
            }
        }
        if (hovered >= 0 && !dragging) {
            const auto& m = map[size_t(hovered)];
            const ImRect rr = toScreen(m.x, m.y, m.w, m.h);
            dl->AddRect(rr.Min - ImVec2(1, 1), rr.Max + ImVec2(1, 1), color::text);
            const std::string t = m.label + "\n" + std::to_string(m.x) + ", " + std::to_string(m.y) + "   " + std::to_string(m.w) + " x " +
                                  std::to_string(m.h);
            tooltip(t.c_str());
        } else if (mh.hovered && !dragging) {
            tooltip("Drag to mark an area textures and CLUTs must stay out of.");
        }
        dl->AddRect(mapR.Min, mapR.Max, color::borderStrong, radius::field);
        y = mapR.Max.y + space::sm;

        // Legend: the kinds on the map, wrapping.
        {
            const std::pair<K, const char*> kinds[] = {{K::Framebuffer, "Framebuffer"}, {K::Reserved, "Reserved"}, {K::Atlas16, "16 bpp"},
                                                       {K::Atlas8, "8 bpp"},           {K::Atlas4, "4 bpp"},     {K::Clut, "CLUT"},
                                                       {K::Font, "Font"},              {K::Prohibited, "Prohibited"}};
            float lx = mx0;
            const float lh = 16;
            for (const auto& [k, name] : kinds) {
                const float w = 10 + space::xs + measure(f.regular, type::caption, name).x;
                if (lx + w > mx1 && lx > mx0) {
                    lx = mx0;
                    y += lh;
                }
                const float cy = y + lh * 0.5f;
                dl->AddRectFilled(ImVec2(lx, cy - 5), ImVec2(lx + 10, cy + 5), withAlpha(kindColor(k), k == K::Prohibited ? 160u : 140u), 2);
                text(dl, ImVec2(lx + 10 + space::xs, centerY(f.regular, type::caption, y, y + lh)), f.regular, type::caption, color::textDim,
                     name);
                lx += w + space::md;
            }
            y += lh + space::sm;
        }

        // The line is there before the first export lands too, so the rows
        // and Add area below it do not move when it does.
        {
            const std::string used =
                stats ? "VRAM used: " + kb(stats->vramBytes()) + " KB of 1024 KB"
                      : std::string(res ? "VRAM used: unknown, the export failed" : "VRAM used: measuring...");
            text(dl, ImVec2(mx0, centerY(f.regular, type::body, y, y + 20)), f.regular, type::body,
                 stats ? color::text : color::textFaint, used.c_str());
            y += 20 + space::xs;
        }
        if (res)
            for (const std::string& e : res->errors)
                if (e.find("does not fit") != std::string::npos) note(e, color::bad);

        // One row per area: x, y, w, h and a remove button.
        text(dl, ImVec2(mx0, centerY(f.medium, type::label, y, y + 22)), f.medium, type::label, color::textDim, "Prohibited areas");
        y += 22;
        int remove = -1;
        for (size_t i = 0; i < vs.prohibited.size(); ++i) {
            const auto& a = vs.prohibited[i];
            const ImRect rr(ImVec2(mx0, y), ImVec2(mx1, y + 28));
            ImGui::PushID(int(i) + 7000);
            const float numW = 22, bw = 22, gap = space::xs;
            const float fx0 = rr.Min.x + numW, fx1 = rr.Max.x - bw - gap;
            const float fw = (fx1 - fx0 - 3 * gap) / 4;
            const std::string num = std::to_string(i + 1);
            text(dl, ImVec2(rr.Min.x + 2, centerY(f.medium, type::body, rr.Min.y, rr.Max.y)), f.medium, type::body, color::textFaint,
                 num.c_str());
            const char* ids[4] = {"x", "y", "w", "h"};
            const char* labels[4] = {"X", "Y", "W", "H"};
            const ImU32 axes[4] = {color::axisX, color::axisY, color::axisZ, color::accent};
            const int vals[4] = {a.x, a.y, a.w, a.h};
            for (int k = 0; k < 4; ++k) {
                const ImRect fr(ImVec2(fx0 + k * (fw + gap), rr.Min.y + 2), ImVec2(fx0 + k * (fw + gap) + fw, rr.Max.y - 2));
                nameRect("vram." + std::string(ids[k]) + "." + std::to_string(i), fr);
                float v = float(vals[k]);
                const std::string shown = std::to_string(vals[k]);
                FieldEdit fe = numberField(ids[k], fr, shown.c_str(), nullptr, axes[k], labels[k], &v);
                if (fe.changed) {
                    splash::VramSettings next = vs;
                    auto& n = next.prohibited[i];
                    int* fld[4] = {&n.x, &n.y, &n.w, &n.h};
                    *fld[k] = int(std::lround(v));
                    save(std::move(next));
                }
            }
            const ImRect del(ImVec2(rr.Max.x - bw, rr.Min.y + 3), ImVec2(rr.Max.x, rr.Max.y - 3));
            nameRect("vram.remove." + std::to_string(i), del);
            if (iconButton("vremove", del, icon::x, false, "Remove this area")) remove = int(i);
            ImGui::PopID();
            y += 28 + space::xxs;
        }
        if (remove >= 0) {
            splash::VramSettings next = vs;
            next.prohibited.erase(next.prohibited.begin() + remove);
            save(std::move(next));
        }
        if (vs.prohibited.empty()) note("None: the packer may use all of VRAM but the framebuffers and x 960.", color::textFaint);

        // Add area: 64 x 64 at the first spot clear of the framebuffers, the
        // font column and the other areas.
        float addW = 0;
        const ImVec2 addAt(mx0, y + space::xs);
        if (button("vram.add", addAt, icon::plus, "Add area", ButtonKind::Secondary, &addW,
                   "A 64 x 64 area at the first free spot; drag on the map for any size")) {
            auto clear = [&](int ax, int ay) {
                auto hit = [&](int bx, int by, int bw2, int bh) { return ax < bx + bw2 && bx < ax + 64 && ay < by + bh && by < ay + 64; };
                if (hit(0, 0, 320, 240) || hit(0, 256, 320, 240) || hit(960, 0, 64, 512)) return false;
                for (const auto& p : vs.prohibited)
                    if (hit(p.x, p.y, p.w, p.h)) return false;
                return true;
            };
            splash::VramSettings next = vs;
            bool found = false;
            for (int ay = 0; ay + 64 <= splash::kVramHeight && !found; ay += 8)
                for (int ax = 0; ax + 64 <= splash::kVramWidth && !found; ax += 8)
                    if (clear(ax, ay)) {
                        next.prohibited.push_back({ax, ay, 64, 64});
                        found = true;
                    }
            if (found) save(std::move(next));
            else notify(st, "No free 64 x 64 spot left in VRAM", true);
        }
        nameRect("vram.add", ImRect(addAt, addAt + ImVec2(addW, size::field + 6)));
        y += size::field + 6 + space::xs + space::sm;
        note("Areas are in 16-bit words and lines (VRAM is 1024 x 512). Textures and CLUTs stay out of them; saved in vram.json "
             "for every scene.",
             color::textFaint);
        if (namedRectsOn()) {
            // Scripted tests: the areas after every change.
            std::string now2 = "vram:";
            for (size_t i = 0; i < vs.prohibited.size(); ++i) {
                const auto& a = vs.prohibited[i];
                now2 += std::string(i ? " | " : " ") + std::to_string(a.x) + "," + std::to_string(a.y) + "," + std::to_string(a.w) + "," +
                        std::to_string(a.h);
            }
            if (vs.prohibited.empty()) now2 += " none";
            static std::string reported;
            if (reported != now2) {
                reported = now2;
                std::printf("%s\n", now2.c_str());
                std::fflush(stdout);
            }
        }
        if (!sOpen) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // The project's disc settings (disc.json): the volume name and the
    // licence data Export writes into sectors 0-15. A project setting like
    // vram.json, saved as soon as it changes and not part of undo.
    {
        namespace fs = std::filesystem;
        const fs::path root = doc.projectRoot();
        static fs::path cachedRoot;
        static splash::DiscSettings ds;
        static std::string licenceError;
        static double readAt = -1;
        const double now = ImGui::GetTime();
        auto resolve = [&] {
            licenceError.clear();
            if (!root.empty()) splash::resolveLicence(ds.licence, root, licenceError);
        };
        // Read again now and then, so an edit made outside (or a licence
        // file that appears or changes) shows up.
        if (root != cachedRoot || now - readAt > 1.0) {
            cachedRoot = root;
            readAt = now;
            ds = editor::loadDiscSettings(root);
            resolve();
        }
        auto save = [&](splash::DiscSettings next) {
            if (editor::saveDiscSettings(root, next)) {
                ds = std::move(next);
                resolve();
            } else {
                notify(st, "Cannot write " + utf8(root / "disc.json"), true);
            }
            readAt = now;
        };
        {
            std::optional<std::string> picked;
            {
                std::lock_guard<std::mutex> lock(g_licenceMutex);
                picked.swap(g_licencePicked);
            }
            if (picked && !root.empty()) {
                // Inside the project it is kept relative, so the project can move.
                std::error_code ec;
                const fs::path abs = fs::weakly_canonical(fs::path(std::u8string(picked->begin(), picked->end())), ec);
                const fs::path base = fs::weakly_canonical(root, ec);
                const fs::path rel = abs.lexically_relative(base);
                const std::u8string u = (!rel.empty() && *rel.begin() != "..") ? rel.generic_u8string() : abs.generic_u8string();
                splash::DiscSettings next = ds;
                next.licence = std::string(u.begin(), u.end());
                save(std::move(next));
            }
        }
        const std::string licenceFile = [&] {
            const fs::path p(std::u8string(ds.licence.begin(), ds.licence.end()));
            return utf8(p.filename());
        }();

        const float stop = y;
        const bool sOpen = section("s_disc", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::hardDrive, color::textDim, "Disc", true,
                                   true, false);
        if (!sOpen) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;

        // Volume label: empty = what Export takes from the scene name.
        const std::string fallback = splash::discVolume("", doc.sceneStem());
        const ImRect vr = row("ldiscvolume", "Volume label",
                              "The disc's volume name: A-Z, 0-9 and _, at most 31. Empty, Export uses the scene name.");
        nameRect("disc.volume", vr);
        std::string typed;
        if (textField("discvolume", vr, ds.volume, &typed, fallback.c_str(), 31, volumeChar)) {
            splash::DiscSettings next = ds;
            next.volume = typed;  // volumeChar kept it to the volume name's characters
            save(std::move(next));
        }

        // Licence: none, the built-in psxsplash one, or a file.
        const int cur = ds.licence.empty() ? 0 : ds.licence == "psxsplash" ? 1 : 2;
        const std::string shown = cur == 0 ? "None" : cur == 1 ? "psxsplash" : licenceFile;
        const ImRect lr = row("ldisclicence", "Licence",
                              "The licence data in the disc's first 16 sectors. A retail console's BIOS checks it before booting; "
                              "emulators and modded consoles do not need it.");
        nameRect("disc.licence", lr);
        const int pick = dropdownMenu("disc.licence", lr, nullptr, shown.c_str(), {"None", "psxsplash", "Choose file..."}, cur);
        if (pick == 0 || pick == 1) {
            if (pick != cur) {
                splash::DiscSettings next = ds;
                next.licence = pick == 0 ? "" : "psxsplash";
                save(std::move(next));
            }
        } else if (pick == 2) {
            const std::string loc = utf8(root);
            SDL_ShowOpenFileDialog(onLicencePicked, nullptr, nullptr, kLicenceFilter, 2, root.empty() ? nullptr : loc.c_str(), false);
        }
        // One dim line, then room for the error, there from the first frame
        // so nothing below moves when one shows.
        const std::string about = cur == 0   ? "No licence data: boots in emulators and on modded consoles"
                                  : cur == 1 ? "psxsplash licence: the boot logo reads Created with psxsplash"
                                             : licenceFile;
        {
            // Fixed heights (two lines, then three), whatever the text wraps to.
            const float tx = x0 + space::sm, tw = x1 - space::sm - tx;
            const float lineH = f.regular->CalcTextSizeA(type::caption, FLT_MAX, 0, "A").y;
            const ImRect ar(ImVec2(tx, y - 2), ImVec2(tx + tw, y - 2 + 2 * lineH));
            const ImVec4 aclip(ar.Min.x, ar.Min.y, ar.Max.x, ar.Max.y);
            dl->AddText(f.regular, type::caption, ar.Min, color::textFaint, about.c_str(), nullptr, tw, &aclip);
            y += 2 * lineH + space::xs;
            const float errH = 3 * lineH;
            const ImRect er(ImVec2(tx, y - 2), ImVec2(tx + tw, y - 2 + errH));
            if (!licenceError.empty()) {
                // The full path is long and the line above names the file.
                std::string msg = licenceError;
                const fs::path p(std::u8string(ds.licence.begin(), ds.licence.end()));
                const std::string full = utf8(p.is_relative() ? root / p : p);
                if (!full.empty() && msg.rfind(full + ": ", 0) == 0) msg.erase(0, full.size() + 2);
                for (size_t at; !full.empty() && (at = msg.find(full)) != std::string::npos;) msg.replace(at, full.size(), licenceFile);
                const ImVec4 clip(er.Min.x, er.Min.y, er.Max.x, er.Max.y);
                dl->AddText(f.regular, type::caption, er.Min, color::warn, msg.c_str(), nullptr, tw, &clip);
                nameRect("disc.error", er);
                interact("discerror", er);
                tooltip(licenceError.c_str());
            }
            y += errH + space::xs;
        }
        if (namedRectsOn() && ImGui::GetFrameCount() > 1 && !root.empty()) {
            // Scripted tests: the settings after every change.
            static std::string reportedVolume, reportedLicence, reportedError;
            auto report = [](std::string& last, const std::string& now2) {
                if (last == now2) return;
                last = now2;
                if (now2.empty()) return;
                std::printf("%s\n", now2.c_str());
                std::fflush(stdout);
            };
            report(reportedVolume, "disc: volume " + (ds.volume.empty() ? std::string("default") : ds.volume));
            report(reportedLicence, "disc: licence " + (cur == 0 ? std::string("none") : cur == 1 ? std::string("psxsplash") : licenceFile));
            report(reportedError, licenceError.empty() ? std::string() : "disc: licence error " + licenceError);
        }
        if (!sOpen) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    float top = y;
    // Closed, the body runs under an empty clip rect (see the object inspector's fold).
    const bool renderOpen = section("s_render", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::cpu, color::textDim, "Render buffers",
                                    true, true, false);
    if (!renderOpen) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
    y += 34 + space::xs;

    // One buffer: an integer field showing the size the export uses, "auto"
    // while the exporter picks it, and a reset button once overridden.
    struct Buf {
        const char* id;
        const char* label;
        const char* tip;
        const char* unit;  // shown once overridden
        int setting;
        uint32_t need;  // 0 = no export yet
        uint32_t used;  // what the export writes
        int splash::SceneSettings::*field;
    };
    auto buffer = [&](const Buf& b) {
        ImRect vr = row(b.id, b.label, b.tip);
        const bool over = b.setting > 0;
        ImRect field = over ? ImRect(vr.Min, ImVec2(vr.Max.x - 28, vr.Max.y)) : vr;
        std::string shown = b.used ? std::to_string(b.used) : over ? std::to_string(b.setting) : std::string("...");
        float v = float(b.used ? b.used : std::max(b.setting, 0));
        FieldEdit fe = numberField((std::string(b.id) + "f").c_str(), field, shown.c_str(), over ? b.unit : "auto", 0, nullptr, &v);
        if (fe.changed) {
            int n = std::max(1, int(std::lround(v)));
            doc.editSettings([&](splash::SceneSettings& s) { s.*b.field = n; }, b.id);
        }
        if (fe.done) doc.endMerge();
        if (over) {
            if (iconButton((std::string(b.id) + "r").c_str(), ImRect(ImVec2(vr.Max.x - 24, vr.Min.y + 4), ImVec2(vr.Max.x, vr.Max.y - 4)),
                           icon::undo, false, "Size automatically again"))
                doc.editSettings([&](splash::SceneSettings& s) { s.*b.field = 0; });
        }
    };

    const float gte = set.gteScaling > 0 ? set.gteScaling : 100.f;
    auto metres = [&](uint32_t depth) {
        char m[32];
        std::snprintf(m, sizeof m, "%.1f m", double(depth) * gte / 4096.0);
        return std::string(m);
    };

    const uint32_t otNeed = stats ? stats->orderingTableNeed : 0, otUsed = stats ? stats->orderingTableSize : 0;
    buffer({"ot", "Ordering table",
            "Depth slots the renderer sorts triangles into, back to front. One slot per depth step, so this is also how far "
            "the camera sees: anything farther away is not drawn. Larger costs 8 bytes of RAM per slot.",
            "slots", set.orderingTableSize, otNeed, otUsed, &splash::SceneSettings::orderingTableSize});
    const editor::RenderPeak* peak = st.play.peak && st.play.peakScene == doc.loadId() ? &*st.play.peak : nullptr;
    if (otUsed) caption("Draws up to " + metres(otUsed) + " away", color::textFaint);
    if (peak) {
        // The peak is the farthest corner of any triangle the fog let through.
        // psxsplash drops a triangle only when all three corners are past the
        // table and clamps the rest into its last slot, and fog already culls
        // anything wholly past the fog wall (20000 / density), so with that
        // wall inside the table nothing past it is lost.
        const bool past = otUsed && uint32_t(peak->depth) >= otUsed;
        const bool fogInside = set.fog.enabled && 20000 / std::clamp(set.fog.density, 1, 10) <= int(otUsed);
        std::string s = "In Play: farthest drawn at " + std::to_string(peak->depth) + " (" + metres(uint32_t(peak->depth)) + ")";
        if (past) s += fogInside ? ", past the table. The fog hides everything that far, so nothing is lost."
                                 : ", past the table: triangles wholly beyond it are not drawn.";
        caption(s, past && !fogInside ? color::warn : color::accentHover);
    }
    const uint32_t buNeed = stats ? stats->bumpAllocatorNeed : 0, buUsed = stats ? stats->bumpAllocatorSize : 0;
    buffer({"bump", "Primitive buffer",
            "Bytes of GPU commands one frame can build: every triangle drawn, plus sprites and text. If a frame needs more, "
            "the game stops with \"Out of memory\". Larger costs twice its size in RAM, one per frame being drawn. "
            "psxsplash calls it the bump allocator.",
            "bytes", set.bumpAllocatorSize, buNeed, buUsed, &splash::SceneSettings::bumpAllocatorSize});
    if (buUsed) caption("Up to " + std::to_string(buUsed / 28) + " plain triangles per frame", color::textFaint);
    if (peak)
        caption("In Play: busiest frame used " + std::to_string(peak->bump) + " bytes" +
                    (buUsed ? " (" + std::to_string(int(100.0 * peak->bump / buUsed + 0.5)) + "%)" : std::string()),
                buUsed && peak->bump > buUsed ? color::warn : color::accentHover);
    if (stats) caption(kb(stats->rendererBytes()) + " KB of RAM for both, double-buffered", color::textFaint);
    else caption("Sizes appear once the scene exports.", color::textFaint);
    if (!renderOpen) {
        ImGui::PopClipRect();
        y = top + 34 + space::md;
    } else {
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
        y += space::sm + space::md;
    }

    // The exported BVH, which psxsplash culls static geometry with. Its
    // boxes can be drawn in the viewport, down to a chosen depth.
    {
        const float stop = y;
        const bool open = section("s_bvh", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::layers, color::textDim, "BVH",
                                  false, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        if (stats) {
            int leaves = 0;
            for (const auto& b : stats->bvhBoxes) leaves += b.leaf ? 1 : 0;
            caption(std::to_string(stats->bvhBoxes.size()) + " nodes, " + std::to_string(leaves) + " leaves, depth " +
                        std::to_string(stats->bvhDepth) + ", " + std::to_string(stats->bvhTriangleRefs) + " triangle refs",
                    color::textFaint);
        } else {
            caption("Appears once the scene exports.", color::textFaint);
        }
        ImRect v = row("lbvhshow", "Show in viewport",
                       "Draws the BVH's boxes over the scene, one colour per level; leaves are brighter. psxsplash draws "
                       "only the triangles in boxes the camera can see.");
        const ImRect sw(ImVec2(v.Min.x, v.GetCenter().y - 8), ImVec2(v.Min.x + 30, v.GetCenter().y + 8));
        nameRect("bvh.show", sw);
        if (toggle("bvhshow", sw, st.bvhShow)) st.bvhShow = !st.bvhShow;
        const int maxDepth = stats ? stats->bvhDepth : 0;
        const int shownDepth = stats ? bvhShownDepth(st, *stats) : std::max(st.bvhDepth, 0);
        const ImRect dr = row("lbvhdepth", "Depth", "The deepest level drawn: 0 is the root box around the whole scene.");
        nameRect("bvh.depth", dr);
        float dv = float(shownDepth);
        FieldEdit fe = numberField("bvhdepth", dr, std::to_string(shownDepth).c_str(), nullptr, 0, nullptr, &dv);
        if (fe.changed) st.bvhDepth = std::clamp(int(std::lround(dv)), 0, maxDepth);
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // The nav bake: its regions and portals, drawn in the viewport, with the
    // selected region's floor, room, surface and neighbours. Every line has
    // its space from the first frame, so nothing moves when the export lands.
    {
        const float stop = y;
        const bool open = section("s_nav", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::user, kind::nav, "Navigation",
                                  false, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        const float lineH = type::caption + 4 + space::xs;
        auto line = [&](const std::string& t, ImU32 col) {
            const float tx = x0 + space::sm + lw;
            const ImVec4 clip(tx, y - 4, x1 - space::sm, y + lineH);
            if (!t.empty()) dl->AddText(f.regular, type::caption, ImVec2(tx, y - 2), col, t.c_str(), nullptr, 0, &clip);
            y += lineH;
        };
        const bool baked = stats && stats->navWanted;
        constexpr int kNavPortalLines = 3;
        line(!stats ? std::string("measuring...")
             : !stats->navWanted
                 ? std::string("no nav bake")
                 : std::to_string(stats->navRegions) + " regions, " + std::to_string(stats->navPortals) + " portals",
             color::textFaint);
        auto sw = [&](const char* id, const char* name, const char* label, const char* tip, bool& v) {
            const ImRect rr = row(id, label, tip);
            const ImRect t(ImVec2(rr.Min.x, rr.GetCenter().y - 8), ImVec2(rr.Min.x + 30, rr.GetCenter().y + 8));
            nameRect(name, t);
            if (toggle(name, t, v)) v = !v;
        };
        sw("lnavshow", "nav.show", "Show in viewport", "Draws each nav region's outline over the scene.", st.navShow);
        sw("lnavlabels", "nav.labels", "Labels", "Names each region R<k> at its centre; click a label to select the region.",
           st.navLabels);
        sw("lnavportals", "nav.portals", "Portals", "Draws the edges the player can cross from one region into the next.",
           st.navPortals);
        const int count = baked ? stats->navRegions : 0;
        const ImRect rr = row("lnavregion", "Region", "The region detailed below; -1 for none.");
        nameRect("nav.region", rr);
        float rv = float(st.navRegion);
        FieldEdit fe = numberField("navregion", rr, st.navRegion < 0 ? "none" : std::to_string(st.navRegion).c_str(), nullptr, 0,
                                   nullptr, &rv);
        if (fe.changed) st.navRegion = std::clamp(int(std::lround(rv)), -1, count - 1);
        const float detailTop = y;
        const splash::ExportStats::NavRegionInfo* info =
            baked && st.navRegion >= 0 && size_t(st.navRegion) < stats->navInfo.size() ? &stats->navInfo[size_t(st.navRegion)]
                                                                                       : nullptr;
        if (info) {
            static const char* const surfaces[] = {"flat", "ramp", "stairs"};
            line("Floor Y " + fmtFixed(info->floorY) + " m", color::textDim);
            line(navRoomName(*stats, *info, false), color::textDim);
            line(std::string("Surface ") + (info->surface >= 0 && info->surface < 3 ? surfaces[info->surface] : "?"), color::textDim);
            line(info->platform ? "Platform" : "", color::textDim);
            // The neighbours, wrapped over kNavPortalLines lines; what does not fit is counted.
            const float wrapW = x1 - space::sm - (x0 + space::sm + lw);
            std::vector<std::string> rows(1, info->portals.empty() ? "No portals" : "Portals ->");
            for (size_t k = 0; k < info->portals.size(); k++) {
                char b[48];
                std::snprintf(b, sizeof b, " R%d (step %+.2f)%s", info->portals[k].neighbor, double(info->portals[k].heightDelta),
                              k + 1 < info->portals.size() ? "," : "");
                if (measure(f.regular, type::caption, (rows.back() + b).c_str()).x > wrapW) {
                    if (rows.size() == size_t(kNavPortalLines)) {
                        rows.back() += " +" + std::to_string(info->portals.size() - k) + " more";
                        break;
                    }
                    rows.emplace_back();
                }
                rows.back() += rows.back().empty() ? b + 1 : b;
            }
            for (int k = 0; k < kNavPortalLines; k++) line(size_t(k) < rows.size() ? rows[size_t(k)] : "", color::textDim);
        } else {
            line(baked ? "Click a label in the viewport." : "", color::textFaint);
            for (int k = 0; k < 3 + kNavPortalLines; k++) line("", 0);
        }
        nameRect("nav.details", ImRect(ImVec2(x0 + space::sm + lw, detailTop), ImVec2(x1 - space::sm, y)));
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    // An override below the estimate is allowed (the estimate has margin)
    // but deserves a word.
    auto warn = [&](const char* id, const char* title, const std::string& body, int splash::SceneSettings::*field) {
        bool fix = false;
        float h = problemCard(id, ImVec2(x0, y), x1 - x0, title, body.c_str(), "Size automatically", &fix);
        y += h + space::sm;
        if (fix) doc.editSettings([&](splash::SceneSettings& s) { s.*field = 0; });
    };
    if (set.orderingTableSize > 0 && otNeed && otUsed < otNeed)
        warn("otwarn", "Ordering table may cut off the scene",
             "The scene reaches " + metres(otNeed) + " from end to end, but only what is within " + metres(otUsed) +
                 " of the camera is drawn. Fine if the camera never sees that far.",
             &splash::SceneSettings::orderingTableSize);
    if (set.bumpAllocatorSize > 0 && buNeed && buUsed < buNeed)
        warn("bumpwarn", "Primitive buffer may run out",
             "The estimate for this scene is " + std::to_string(buNeed) + " bytes: every triangle, plus a margin. With " +
                 std::to_string(buUsed) + ", a frame that draws more stops the game. Play the scene to see what a frame really uses.",
             &splash::SceneSettings::bumpAllocatorSize);

    // Every object costs heap when the scene loads, however small: past some
    // count that, not the geometry, is what keeps a scene from fitting.
    if (stats && splash::objectsBindRam(*stats)) {
        bool fix = false;
        const std::string body = splash::objectRamWarning(*stats);
        float h = problemCard("objramwarn", ImVec2(x0, y), x1 - x0, "Too many objects for RAM", body.c_str(),
                              "Select the biggest group", &fix);
        y += h + space::sm;
        if (fix) {
            // The object with the most children: usually a scatter of props to merge.
            editor::ObjectPath best, path;
            size_t most = 0;
            std::function<void(const std::vector<splash::Object>&)> walk = [&](const std::vector<splash::Object>& objs) {
                for (size_t i = 0; i < objs.size(); i++) {
                    path.push_back(int(i));
                    if (objs[i].children.size() > most) most = objs[i].children.size(), best = path;
                    walk(objs[i].children);
                    path.pop_back();
                }
            };
            walk(doc.scene().objects);
            if (most) {
                doc.select(best);
                st.frameRequest = true;
            }
        }
    }

    // World streaming: one switch, everything else is worked out by the export.
    {
        const float stop = y;
        const bool open = section("s_stream", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::globe, color::textDim,
                                  "World streaming", true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        ImRect v = row("lstream", "Stream world",
                       "For worlds bigger than RAM. Scenery far from the camera stays on the disc and loads while the player "
                       "moves; anything a script, animation or interaction can reach stays loaded. The world is split into "
                       "regions automatically, and big meshes are cut at region edges.");
        if (toggle("streamworld", ImRect(ImVec2(v.Min.x, v.GetCenter().y - 8), ImVec2(v.Min.x + 30, v.GetCenter().y + 8)),
                   set.streamWorld))
            doc.editSettings([](splash::SceneSettings& s) { s.streamWorld = !s.streamWorld; });
        y += space::xs;
        if (!set.streamWorld) {
            caption("Off: the whole scene loads into RAM at once.", color::textFaint);
        } else if (!stats) {
            caption("Regions appear once the scene exports.", color::textFaint);
        } else if (stats->streamRegions) {
            caption(std::to_string(stats->streamRegions) + " regions of about " + metres(uint32_t(stats->streamRegionSize)) +
                        ", " + std::to_string(stats->streamedObjects) + " objects stream" +
                        (stats->streamSplitObjects ? " (" + std::to_string(stats->streamSplitObjects) + " cut pieces)" : std::string()),
                    color::textFaint);
            caption("Loads scenery within " + metres(uint32_t(stats->streamLoadRadius)) + " of the camera", color::textFaint);
            caption(std::to_string(stats->streamSlots) + " slots of " + kb(stats->streamSlotBytes) + " KB = " +
                        kb(stats->streamPoolBytes()) + " KB of RAM while playing",
                    color::textFaint);
            const long long saved = stats->streamRamSaved();
            if (saved > 0) caption("Saves " + kb(size_t(saved)) + " KB of RAM", color::accentHover);
        }
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
        if (set.streamWorld && stats)
            for (size_t i = 0; i < stats->streamWarnings.size(); i++) {
                const splash::StreamWarning& sw = stats->streamWarnings[i];
                const bool shorter = sw.kind == splash::StreamProblem::LargeRegion || sw.kind == splash::StreamProblem::TooManyNear;
                if (sw.kind == splash::StreamProblem::UnevenRegions) {
                    caption(sw.title + ": " + sw.body, color::warn);
                    continue;
                }
                bool fix = false;
                std::string id = "streamwarn" + std::to_string(i);
                float h = problemCard(id.c_str(), ImVec2(x0, y), x1 - x0, sw.title.c_str(), sw.body.c_str(),
                                      shorter ? "Shorten draw distance" : "Turn streaming off", &fix);
                y += h + space::sm;
                if (fix && shorter) {
                    const int now = int(otUsed ? otUsed : splash::kStreamDrawDistance);
                    doc.editSettings([&](splash::SceneSettings& s) { s.orderingTableSize = std::max(int(splash::kOtMin), now * 3 / 4); });
                } else if (fix) {
                    doc.editSettings([](splash::SceneSettings& s) { s.streamWorld = false; });
                }
            }
    }

    // Memory card: what MemCard.Save files are called and how the console's
    // memory card screen shows them. Off, the fields stay editable, dimmed.
    {
        const splash::MemCardSettings& mc = set.memCard;
        auto edit = [&](const std::function<void(splash::MemCardSettings&)>& fn) {
            doc.editSettings([&](splash::SceneSettings& s) { fn(s.memCard); });
        };
        // Each field's tip; while saves are off it says so first.
        auto tip = [&](const char* t) { return mc.enabled ? std::string(t) : "Not used: turn on Enable saves first. " + std::string(t); };
        const float stop = y;
        const bool open = section("s_memcard", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::hardDrive, color::textDim, "Memory card",
                                  true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        ImRect ev = row("lmcenable", "Enable saves",
                        "Pack memory card settings into this scene. Off, the console keeps the last scene's settings. Use the same "
                        "values in every scene.");
        const ImRect mcSw(ImVec2(ev.Min.x, ev.GetCenter().y - 8), ImVec2(ev.Min.x + 30, ev.GetCenter().y + 8));
        nameRect("scene.mcenable", mcSw);
        if (toggle("mcenable", mcSw, mc.enabled))
            edit([](splash::MemCardSettings& m) { m.enabled = !m.enabled; });
        const float dimTop = y;

        static const char* const regions[3] = {"BA", "BE", "BI"};
        int cur = -1;
        for (int i = 0; i < 3; i++)
            if (mc.region == regions[i]) cur = i;
        static const char* const regionNames[3] = {"BA  America", "BE  Europe", "BI  Japan"};
        const std::string shown = cur >= 0 ? regionNames[cur] : mc.region.empty() ? "None" : mc.region + "  (not a BIOS region)";
        int pick = dropdownMenu("mcregion", row("lmcregion", "Region", tip("Starts every save filename.").c_str()), nullptr,
                                shown.c_str(), {regionNames[0], regionNames[1], regionNames[2]}, cur);
        if (pick >= 0 && pick != cur) edit([&](splash::MemCardSettings& m) { m.region = regions[pick]; });

        std::string typed;
        if (textField("mcproduct",
                      row("lmcproduct", "Product code",
                          tip("e.g. SLUS-00000. The save filename is region + product + the Lua key, cut at 20 characters.").c_str()),
                      mc.product, &typed, "SLUS-00000", splash::kMemCardProductMax))
            edit([&](splash::MemCardSettings& m) { m.product = typed; });
        const int room = editor::memCardKeyRoom(mc);
        if (room > 0)
            caption("File " + editor::memCardFilename(mc, "SAVE1") + " for key SAVE1\nKeys keep their first " + std::to_string(room) +
                        " characters.",
                    color::textFaint);
        else
            caption("Region and product fill all 20 characters of the filename, so every key saves to the same file.", color::warn);
        y += space::xs;

        const ImRect titleRow = row("lmctitle", "Save title",
                                    tip("Shown in the console's memory card screen. ASCII only; \" ' - \\ ~ show as ?. MemCard.Save's "
                                        "4th argument overrides it per save.")
                                        .c_str());
        nameRect("scene.mctitle", titleRow);
        if (textField("mctitle", titleRow, mc.title, &typed, "No title", splash::kMemCardTitleMax))
            edit([&](splash::MemCardSettings& m) { m.title = typed; });

        // Icon: up to 3 frames, each drawn 4x as the console shows it, after
        // the shared 16-colour palette.
        static struct {
            std::vector<std::string> icons;
            unsigned assets = ~0u;
            std::filesystem::path root;
            editor::MemCardIcon icon;
        } cache;
        if (cache.icons != mc.icons || cache.assets != doc.assetsId() || cache.root != doc.projectRoot()) {
            cache.icons = mc.icons;
            cache.assets = doc.assetsId();
            cache.root = doc.projectRoot();
            cache.icon = editor::memCardIcon(mc.icons, doc.projectRoot());
        }
        const editor::MemCardIcon& icn = cache.icon;
        const char* iconTip = "16x16 images; 2 or 3 frames animate. All frames share one 16-colour palette. Alpha below 0.5 is "
                              "transparent. Click a frame to pick another image.";
        ImRect iv = row("lmcicon", "Icon", tip(iconTip).c_str());
        const float tile = 4.0f * splash::kMemCardIconSize, gap = 4;
        const float ty = iv.Min.y;
        const int nFrames = int(icn.frames.size());
        for (int i = 0; i < nFrames + (nFrames < int(splash::kMemCardIconsMax) ? 1 : 0); i++) {
            ImRect tr(ImVec2(iv.Min.x + i * (tile + gap), ty), ImVec2(iv.Min.x + i * (tile + gap) + tile, ty + tile));
            ImGui::PushID(i);
            if (i == nFrames) {
                Hit h = interact("mcadd", tr);
                tooltip(nFrames ? "Add a frame: 2 or 3 frames animate. Pick several images to add them at once." : iconTip);
                dl->AddRectFilled(tr.Min, tr.Max, lerpColor(color::field, color::hover, h.hover), radius::field);
                dl->AddRect(tr.Min, tr.Max, lerpColor(color::border, color::borderStrong, h.hover), radius::field);
                textCentered(dl, ImRect(tr.Min, ImVec2(tr.Max.x, tr.Max.y - 14)), f.medium, type::icon, color::textDim, icon::plus);
                textCentered(dl, ImRect(ImVec2(tr.Min.x, tr.Max.y - 26), ImVec2(tr.Max.x, tr.Max.y - 6)), f.regular, type::caption,
                             color::textFaint, nFrames ? "Add frame" : "Add icon");
                if (h.clicked) pickAsset(AssetSlot::MemCardIcon, {}, -1);
            } else {
                const editor::MemCardIcon::Frame& fr = icn.frames[size_t(i)];
                Hit h = interact("mcframe", tr);
                const std::string ft = "Frame " + std::to_string(i + 1) + ": " + fr.path +
                                       (fr.error.empty() ? "" : " (" + fr.error + ")") + ". Click to pick another image.";
                tooltip(ft.c_str());
                // Transparent texels show a checkerboard, as in image editors.
                for (int cy = 0; cy < 4; cy++)
                    for (int cx = 0; cx < 4; cx++)
                        dl->AddRectFilled(tr.Min + ImVec2(cx * 16.f, cy * 16.f), tr.Min + ImVec2(cx * 16.f + 16, cy * 16.f + 16),
                                          (cx + cy) % 2 ? rgb(0x2a2f39) : rgb(0x1f232b));
                if (!fr.rgba.empty()) {
                    const int n = splash::kMemCardIconSize;
                    for (int py = 0; py < n; py++)
                        for (int px = 0; px < n; px++) {
                            const ImU32 c = fr.rgba[size_t(py) * n + px];
                            if (c) dl->AddRectFilled(tr.Min + ImVec2(px * 4.f, py * 4.f), tr.Min + ImVec2(px * 4.f + 4, py * 4.f + 4), c);
                        }
                } else {
                    textCentered(dl, tr, f.medium, type::icon, color::warn, icon::warning);
                }
                dl->AddRect(tr.Min, tr.Max,
                            !fr.error.empty() ? color::warn : lerpColor(color::border, color::borderStrong, h.hover), radius::field);
                if (h.clicked) pickAsset(AssetSlot::MemCardIcon, {}, i);
                // Under it: its number and a button that takes it out.
                const float ly = tr.Max.y + 2;
                text(dl, ImVec2(tr.Min.x + 2, ly + 2), f.regular, type::caption, color::textFaint, std::to_string(i + 1).c_str());
                const std::string rt = "Remove frame " + std::to_string(i + 1);
                if (iconButton("mcremove", ImRect(ImVec2(tr.Max.x - 18, ly), ImVec2(tr.Max.x, ly + 18)), icon::x, false, rt.c_str()))
                    edit([&](splash::MemCardSettings& m) {
                        if (size_t(i) < m.icons.size()) m.icons.erase(m.icons.begin() + i);
                    });
            }
            ImGui::PopID();
        }
        y = ty + tile + 22 + space::xs;
        if (nFrames)
            caption(nFrames == 1 ? "1 frame: still" : std::to_string(nFrames) + " frames: animated", color::textFaint);
        if (icn.colors > icn.paletteColors)
            caption("The frames use " + std::to_string(icn.colors) + " colours and share a palette of " +
                        std::to_string(icn.paletteColors) + (icn.paletteColors == 15 ? " (one more is transparent)" : "") +
                        ", so close colours merge. The frames above show the result.",
                    color::warn);
        y += space::xs;
        // Off: dimmed like other settings that do nothing yet, still editable.
        if (!mc.enabled) dl->AddRectFilled(ImVec2(x0 + space::sm, dimTop), ImVec2(x1 - space::sm, y), rgb(0x1b1e24, 150));
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }

        // What the export would refuse or warn about, each with its fix.
        if (mc.enabled) {
            auto problemAt = [&](const std::string& id, const std::string& title, const std::string& body, const char* fixLabel) {
                bool fix = false;
                float h = problemCard(id.c_str(), ImVec2(x0, y), x1 - x0, title.c_str(), body.c_str(), fixLabel, &fix);
                y += h + space::sm;
                return fix;
            };
            for (int i = 0; i < nFrames; i++) {
                const editor::MemCardIcon::Frame& fr = icn.frames[size_t(i)];
                if (fr.error.empty()) continue;
                const bool missing = fr.error == "not found";
                const std::string title = "Icon frame " + std::to_string(i + 1) + (missing ? " not found" : " can't be used");
                const std::string body =
                    missing ? fr.path + " is not in the project. The export stops until it is found or replaced."
                    : fr.width ? fr.path + " is " + std::to_string(fr.width) + "x" + std::to_string(fr.height) +
                                     ", the BIOS icon is 16x16: resize it or pick another image. The export stops until then."
                               : fr.path + ": " + fr.error + ". The export stops until it is replaced.";
                if (problemAt("mcframefix" + std::to_string(i), title, body, missing ? "Locate file" : "Pick another image"))
                    pickAsset(AssetSlot::MemCardIcon, {}, i);
            }
            const std::vector<editor::MemCardProblem> problems = editor::memCardProblems(mc);
            for (size_t i = 0; i < problems.size(); i++) {
                const editor::MemCardProblem& p = problems[i];
                const std::string body = p.error ? p.body + ". The export stops until it is fixed." : p.body + ".";
                if (p.fix == editor::MemCardFix::None) {
                    caption(body, p.error ? color::bad : color::warn);
                    continue;
                }
                if (!problemAt("mcfix" + std::to_string(i), p.title, body, p.fixLabel)) continue;
                if (p.fix == editor::MemCardFix::AddIcon) pickAsset(AssetSlot::MemCardIcon, {}, -1);
                else edit([&](splash::MemCardSettings& m) { editor::applyMemCardFix(m, p.fix, doc.sceneStem()); });
            }
        }
    }

    // Network: the id two consoles compare before they link. Empty, the
    // engine derives one from the scene index and object count.
    {
        const float stop = y;
        const bool open = section("s_network", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::globe, color::textDim, "Network",
                                  true, true, false);
        if (!open) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
        y += 34 + space::xs;
        const ImRect idRow = row("lnetid", "Network id",
                                 "Consoles linked with Net.Begin only connect when their scenes have the same id. Name the scene "
                                 "here to keep the id stable between builds.");
        nameRect("scene.netid", idRow);
        std::string typed;
        if (textField("netid", idRow, set.networkId, &typed, "Derived from the scene"))
            doc.editSettings([&](splash::SceneSettings& s) { s.networkId = typed; });
        const uint32_t hash = splash::hashSceneId(set.networkId);
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", unsigned(hash));
        if (set.networkId.empty())
            caption("Derived at run time: it changes when objects are added, so consoles running different builds may not "
                    "connect.",
                    color::textFaint);
        else
            caption(std::string("Locked: hash ") + hex + ".", color::good);
        scriptLine("network", set.networkId.empty() ? std::string("network: derived")
                                                     : "network: locked " + set.networkId + " hash " + hex);
        y += space::xs;
        if (!open) {
            ImGui::PopClipRect();
            y = stop + 34 + space::md;
        } else {
            dl->AddRect(ImVec2(x0, stop), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
            y += space::sm + space::md;
        }
    }

    const char* hint = "Pick an object in the Scene panel to edit it.";
    ImVec2 hs = measure(f.regular, type::label, hint);
    text(dl, ImVec2((r.Min.x + r.Max.x - hs.x) * 0.5f, std::max(y + space::lg, r.Max.y - space::xl - hs.y)), f.regular, type::label,
         color::textFaint, hint);
    ImGui::PopClipRect();

    const float content = y + space::lg + hs.y + space::xl - (top0 - scroll), view = r.Max.y - top0;
    const float maxScroll = std::max(0.0f, content - view);
    if (maxScroll > 0 && ImGui::IsMouseHoveringRect(r.Min, r.Max) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        scroll -= ImGui::GetIO().MouseWheel * 48.0f;
    scroll = std::clamp(scroll, 0.0f, maxScroll);
    if (maxScroll > 0) {
        // A thin bar at the right edge: where the view sits in the settings.
        const float h = view * view / content, t = top0 + (view - h) * (scroll / maxScroll);
        dl->AddRectFilled(ImVec2(r.Max.x - 5, t + 2), ImVec2(r.Max.x - 2, t + h - 2), rgb(0xffffff, 40), 2);
    }
}

static void inspector(State& st, ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    panel(dl, r);
    float y = panelHeader(dl, r, "Inspector", nullptr);
    // The UI tab inspects its canvas and elements.
    if (st.scripts.uiShown()) {
        st.canvasView.inspector(dl, r, y, doc);
        return;
    }
    // The lock pins what the inspector shows; selecting other objects leaves it there.
    const bool pinned = doc.pinned().has_value();
    if (iconButton("lockinsp", ImRect(ImVec2(r.Max.x - 32, r.Min.y + 5), ImVec2(r.Max.x - 6, r.Min.y + 29)), icon::lock, pinned,
                   pinned ? "Pinned: click to follow the selection again" : "Keep showing this object", color::textDim,
                   pinned || doc.selection()))
        doc.pin(pinned ? std::nullopt : doc.selection());

    const std::optional<editor::ObjectPath> target = doc.pinned() ? doc.pinned() : doc.selection();
    const splash::Object* o = target ? doc.object(*target) : nullptr;
    if (!o) {
        sceneInspector(st, dl, r, y, doc);
        return;
    }
    ImGui::PushClipRect(ImVec2(r.Min.x, y), ImVec2(r.Max.x, r.Max.y - 1), true);
    // The object inspector's visible body: scripted tests check controls lie in it.
    nameRect("obj.panel", ImRect(ImVec2(r.Min.x, y), ImVec2(r.Max.x, r.Max.y - 1)));
    // Taller than the panel (several components), it scrolls with the wheel;
    // inspecting another object starts back at the top.
    static float scroll = 0;
    static editor::ObjectPath scrolled;
    if (scrolled != *target) {
        scrolled = *target;
        scroll = 0;
    }
    const float top0 = y;
    y -= scroll;

    float x0 = r.Min.x + space::md, x1 = r.Max.x - space::md;
    // Object header.
    ObjectLook look = lookOf(*o, true);
    const splash::Object* parent = doc.parent(*target);
    std::string where = "in " + (parent ? parent->name : doc.sceneStem());
    ImRect ic(ImVec2(x0, y), ImVec2(x0 + 40, y + 40));
    dl->AddRectFilled(ic.Min, ic.Max, (look.color & 0x00ffffffu) | (34u << IM_COL32_A_SHIFT), radius::card);
    textCentered(dl, ic, f.medium, type::icon + 3, look.color, look.icon);
    const editor::ObjectPath path = *target;
    // Name: double-click to rename.
    {
        ImVec2 np(ic.Max.x + space::md, y + 2);
        ImRect field(np - ImVec2(space::xs + 2, 2), ImVec2(x1 - 30 - space::md, np.y + 19));
        std::string typed;
        TextEdit res = textEdit("objname", field, f.semibold, type::title, np.x, field.Max.x - space::xs, &typed);
        if (res == TextEdit::Inactive) {
            ImVec2 ns = measure(f.semibold, type::title, o->name.c_str());
            ImRect hit(np, np + ImVec2(std::max(ns.x, 40.0f), ns.y));
            interact("objname", hit);
            nameRect("obj.name", hit);
            tooltip("Double-click to rename (F2)");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) beginTextEdit("objname", o->name);
            text(dl, np, f.semibold, type::title, color::text, o->name.c_str());
        } else if (res == TextEdit::Commit && !typed.empty() && typed != o->name) {
            renameFromUi(doc, path, typed);
        }
    }
    text(dl, ImVec2(ic.Max.x + space::md, y + 22), f.regular, type::caption, color::textFaint, where.c_str());
    if (toggle("objactive", ImRect(ImVec2(x1 - 30, y + 12), ImVec2(x1, y + 28)), o->active))
        doc.edit(path, [](splash::Object& ob) { ob.active = !ob.active; });
    tooltip("Off leaves the object out of the game entirely. To have it in the game but switched off, turn off Start active.");
    y += 40 + space::lg;
    // On, but under an off parent: it is left out all the same. The nearest
    // off ancestor, found by path (a name copy, no pointer kept past an edit).
    std::string offVia;
    if (const splash::Object* self = doc.object(path); self && self->active)
        for (size_t k = path.size(); k-- > 1;) {
            const splash::Object* a = doc.object(editor::ObjectPath(path.begin(), path.begin() + static_cast<long>(k)));
            if (a && !a->active) {
                offVia = a->name;
                break;
            }
        }
    if (!offVia.empty()) {
        const std::string s = "Off because '" + offVia + "' is off";
        const ImVec2 at(x0, y);
        text(dl, at, f.regular, type::caption, color::warn, s.c_str());
        nameRect("obj.inheritedoff", ImRect(at, at + measure(f.regular, type::caption, s.c_str())));
        y += 16 + space::md;
    }

    const float lw = 96, rowH = 32;
    auto row = [&](const char* id, const char* label, const char* tip) {
        ImRect rr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + rowH));
        y += rowH;
        return property(id, rr, lw, label, tip);
    };
    // A closed section's body still runs, under an empty clip rect: nothing
    // in it draws or takes input, and sectionEnd puts y back under the header.
    bool folded = false;
    auto fold = [&](bool open) {
        folded = !open;
        if (folded) ImGui::PushClipRect(ImVec2(0, 0), ImVec2(0, 0), false);
    };
    auto sectionEnd = [&](float top) {
        if (folded) {
            ImGui::PopClipRect();
            folded = false;
            y = top + 34 + space::md;
            return;
        }
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, y + space::sm), rgb(0xffffff, 10), radius::card);
        y += space::sm + space::md;
    };
    // Returns true on the frame its fix button is clicked. Named obj.<id>,
    // its fix button obj.<id>.fix, for scripted tests.
    auto card = [&](const char* id, const char* title, const std::string& body, const char* fix) {
        bool fixed = false;
        ImRect fr;
        const ImVec2 at(x0 + space::sm, y + space::xs);
        const float w = x1 - x0 - space::sm * 2;
        float h = problemCard(id, at, w, title, body.c_str(), fix, &fixed, &fr);
        nameRect(std::string("obj.") + id, ImRect(at, at + ImVec2(w, h)));
        nameRect(std::string("obj.") + id + ".fix", fr);
        y += h + space::sm;
        return fixed;
    };
    // Card for a referenced file that is missing or could not be read; its
    // Locate file button reopens the field's file dialog.
    auto fileCard = [&](const char* id, editor::AssetStatus st, const std::string& path, const std::string& error) {
        if (st == editor::AssetStatus::Ok) return false;
        if (st == editor::AssetStatus::Missing) return card(id, "File not found", path.empty() ? "No file is set." : path, "Locate file");
        return card(id, "File could not be read", error, "Locate file");
    };

    // Start active: the object is in the game either way; off loads it hidden,
    // with no collision, until a script or cutscene switches it on.
    if (o->active) {
        ImRect v = row("lstartactive", "Start active",
                       "Off loads the object hidden and without collision. Scripts (Entity.SetActive) and cutscenes can switch it on.");
        if (toggle("startactive", ImRect(ImVec2(v.Min.x, v.GetCenter().y - 8), ImVec2(v.Min.x + 30, v.GetCenter().y + 8)), o->startActive))
            doc.edit(path, [](splash::Object& ob) { ob.startActive = !ob.startActive; });
        y += space::md;
    }

    // Transform.
    const splash::Transform& t = o->transform;
    splash::Vec3 e = eulerDegrees(t.rotation);
    float top = y;
    fold(section("s_transform", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::move, color::textDim, "Transform", true, true, false));
    y += 34 + space::xs;
    // Edits go through the history; a scrub merges into one step per field
    // until it is released (FieldEdit::done).
    auto commit = [&](const FieldEdit& fe, const std::string& key, const std::function<void(splash::Object&)>& fn) {
        if (fe.changed) doc.edit(path, fn, key + "." + std::to_string(fe.index));
        if (fe.done) doc.endMerge();
    };
    // A component removed from its section menu goes once drawing is done,
    // since the sections below still read it this frame.
    std::optional<editor::ComponentKind> toRemove;
    bool removedFlags[static_cast<int>(editor::ComponentKind::Portal) + 1] = {};
    auto removeFlag = [&](editor::ComponentKind k) { return &removedFlags[static_cast<int>(k)]; };
    float pos[3] = {t.position.x, t.position.y, t.position.z};
    FieldEdit fe = vec3Field("pos", row("lpos", "Position", "Where the object sits, in metres, relative to its parent."),
                             fmtFixed(t.position.x).c_str(), fmtFixed(t.position.y).c_str(), fmtFixed(t.position.z).c_str(), nullptr, pos);
    commit(fe, "pos", [&](splash::Object& ob) { ob.transform.position = {pos[0], pos[1], pos[2]}; });
    float rot[3] = {e.x, e.y, e.z};
    fe = vec3Field("rot", row("lrot", "Rotation", "Rotation around each axis, in degrees."), fmtShort(e.x).c_str(), fmtShort(e.y).c_str(),
                   fmtShort(e.z).c_str(), nullptr, rot);
    commit(fe, "rot", [&](splash::Object& ob) { ob.transform.rotation = quatFromEuler({rot[0], rot[1], rot[2]}); });
    float scl[3] = {t.scale.x, t.scale.y, t.scale.z};
    fe = vec3Field("scl", row("lscl", "Scale", "Size multiplier on each axis."), fmtFixed(t.scale.x).c_str(), fmtFixed(t.scale.y).c_str(),
                   fmtFixed(t.scale.z).c_str(), nullptr, scl);
    commit(fe, "scl", [&](splash::Object& ob) { ob.transform.scale = {scl[0], scl[1], scl[2]}; });
    sectionEnd(top);

    // Import settings of the model the object's mesh came from; for the top
    // of a placed model, the first mesh below it.
    {
        std::function<const splash::Object*(const splash::Object&)> withMesh = [&](const splash::Object& ob) -> const splash::Object* {
            if (ob.mesh && !ob.mesh->mesh.empty()) return &ob;
            for (const splash::Object& c : ob.children)
                if (const splash::Object* found = withMesh(c)) return found;
            return nullptr;
        };
        const splash::Object* drawn = withMesh(*o);
        State::ImportPanel& ip = st.importPanel;
        const std::string mesh = drawn ? drawn->mesh->mesh : std::string();
        const double now = ImGui::GetTime();
        if (ip.editing && mesh != ip.mesh) ip.editing.reset();  // a scrub left unfinished on another object
        if (!ip.editing && (mesh != ip.mesh || ip.checked < 0 || now - ip.checked > 1.0)) {
            ip.mesh = mesh;
            ip.checked = now;
            ip.model = mesh.empty() ? std::filesystem::path() : editor::modelOfMesh(doc.projectRoot(), mesh);
            ip.error.clear();
            if (!ip.model.empty()) {
                try {
                    ip.settings = splash::loadImportSettings(ip.model);
                } catch (const std::exception& ex) {
                    ip.error = ex.what();
                }
            }
        }
        if (!ip.model.empty()) {
            top = y;
            const ImRect head(ImVec2(x0, y), ImVec2(x1, y + 34));
            fold(section("s_import", head, icon::box, kind::mesh, "Import settings", true, true, false));
            if (!folded) {
                const std::string file = utf8(ip.model.filename());
                ImVec2 ts = measure(f.semibold, type::body, "Import settings");
                const float fx = head.Min.x + space::sm + 41 + ts.x + space::md, fr = head.Max.x - space::sm;
                ImGui::PushClipRect(ImVec2(fx, head.Min.y), ImVec2(fr, head.Max.y), true);
                ImVec2 fsz = measure(f.regular, type::caption, file.c_str());
                text(dl, ImVec2(std::max(fx, fr - fsz.x), centerY(f.regular, type::caption, head.Min.y, head.Max.y)), f.regular, type::caption,
                     color::textFaint, file.c_str());
                ImGui::PopClipRect();
                interact("s_import#file", ImRect(ImVec2(std::max(fx, fr - fsz.x), head.Min.y), ImVec2(fr, head.Max.y)));
                tooltip(("Imported from models/" + file + (ip.settings.source.empty() ? std::string() : ", copied from " + ip.settings.source) +
                         ". These settings are kept in " + file + ".import.json beside it.")
                            .c_str());
            }
            y += 34 + space::xs;
            // Rows with room for the longer labels.
            auto irow = [&](const char* id, const char* label, const char* tip) {
                ImRect rr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + rowH));
                y += rowH;
                return property(id, rr, 150, label, tip);
            };
            if (!ip.error.empty()) {
                if (card("fiximport", "Import settings could not be read", ip.error, "Read again")) ip.checked = -1;
            } else {
                const splash::ImportSettings& cur = ip.editing ? *ip.editing : ip.settings;
                // Saved, and the model imported again next frame.
                auto apply = [&](splash::ImportSettings next) {
                    ip.editing.reset();
                    const bool was = ip.settings.mergeMeshes;
                    try {
                        splash::saveImportSettings(ip.model, next);
                    } catch (const std::exception& ex) {
                        notify(st, std::string("Import settings not saved: ") + ex.what(), true);
                        return;
                    }
                    ip.settings = next;
                    st.reimports.push_back({ip.model, was != next.mergeMeshes ? std::optional<bool>(was) : std::nullopt});
                };
                // A scrub shows its value as it goes and applies on release.
                auto scrub = [&](const FieldEdit& fe, float v, const std::function<void(splash::ImportSettings&, float)>& set) {
                    if (fe.changed) {
                        if (!ip.editing) ip.editing = ip.settings;
                        set(*ip.editing, v);
                    }
                    if (fe.done && ip.editing) apply(*ip.editing);
                };
                float sv = cur.scale;
                FieldEdit sfe = numberField("iscale", irow("liscale", "Scale",
                                                           "Multiplies the model's size: every position in it and every part's offset. "
                                                           "1 keeps the size in the file, in metres."),
                                            fmtShort(cur.scale).c_str(), nullptr, 0, nullptr, &sv);
                scrub(sfe, sv, [](splash::ImportSettings& s, float v) { s.scale = splash::clampv(v, 0.001f, 1000.0f); });
                const splash::UpAxis ups[3] = {splash::UpAxis::File, splash::UpAxis::YUp, splash::UpAxis::ZUp};
                const char* upNames[3] = {"From file", "Y up", "Z up"};
                int upCur = (int)(std::find(std::begin(ups), std::end(ups), cur.up) - std::begin(ups));
                int upPick = dropdownMenu("iup", irow("liup", "Up axis",
                                                      "Which way is up in the file. From file believes what the file says; Y up or Z up "
                                                      "overrides it, for a model that comes in lying on its back."),
                                          nullptr, upNames[upCur], {"From file", "Y up", "Z up"}, upCur);
                if (upPick >= 0 && ups[upPick] != cur.up) {
                    splash::ImportSettings next = ip.settings;
                    next.up = ups[upPick];
                    apply(next);
                }
                const bool recompute = cur.normals == splash::Normals::Recompute;
                int nPick = dropdownMenu("inormals", irow("linormals", "Normals",
                                                          "Import keeps the file's normals (flat shading where it has none). Recompute works "
                                                          "them out from the faces, smoothing edges gentler than the smoothing angle."),
                                         nullptr, recompute ? "Recompute" : "Import", {"Import", "Recompute"}, recompute ? 1 : 0);
                if (nPick >= 0 && (nPick == 1) != recompute) {
                    splash::ImportSettings next = ip.settings;
                    next.normals = nPick == 1 ? splash::Normals::Recompute : splash::Normals::Import;
                    apply(next);
                }
                // Only used with Recompute; editable either way, dimmed when unused.
                const float angleTop = y;
                float av = cur.smoothingAngle;
                FieldEdit afe = numberField("ismooth", irow("lismooth", "Smoothing angle",
                                                            recompute ? "Faces meeting at less than this many degrees are shaded smooth across "
                                                                        "the edge; sharper edges stay hard."
                                                                      : "Not used: set Normals to Recompute first. Faces meeting at less than "
                                                                        "this many degrees are shaded smooth across the edge."),
                                            fmtShort(cur.smoothingAngle).c_str(), "deg", 0, nullptr, &av);
                scrub(afe, av, [](splash::ImportSettings& s, float v) { s.smoothingAngle = splash::clampv(v, 0.0f, 180.0f); });
                if (!recompute) dl->AddRectFilled(ImVec2(x0 + space::sm, angleTop), ImVec2(x1 - space::sm, y), rgb(0x1b1e24, 150));
                ImRect mv = irow("limerge", "Merge meshes",
                                 "On makes each placed copy of the model one object drawing a single mesh. Off keeps the file's parts as "
                                 "child objects. Changing it rebuilds the copies already in the scene.");
                if (toggle("imerge", ImRect(ImVec2(mv.Min.x, mv.GetCenter().y - 8), ImVec2(mv.Min.x + 30, mv.GetCenter().y + 8)),
                           cur.mergeMeshes)) {
                    splash::ImportSettings next = ip.settings;
                    next.mergeMeshes = !next.mergeMeshes;
                    apply(next);
                }
                const splash::BitDepth depths[3] = {splash::BitDepth::Bpp4, splash::BitDepth::Bpp8, splash::BitDepth::Bpp16};
                int dCur = (int)(std::find(std::begin(depths), std::end(depths), cur.bitDepth) - std::begin(depths));
                int dPick = dropdownMenu("idepth", irow("lidepth", "Bit depth for new objects",
                                                        "Texture bit depth of the objects made from this model from now on, including "
                                                        "ones Merge meshes rebuilds. Objects already placed keep theirs."),
                                         nullptr, bppLabel(cur.bitDepth), {"4 bpp", "8 bpp", "16 bpp"}, dCur);
                if (dPick >= 0 && depths[dPick] != cur.bitDepth) {
                    splash::ImportSettings next = ip.settings;
                    next.bitDepth = depths[dPick];
                    apply(next);
                }
                const splash::DynamicLighting lights[4] = {splash::DynamicLighting::Auto, splash::DynamicLighting::On,
                                                           splash::DynamicLighting::Off, splash::DynamicLighting::Smooth};
                const char* lightNames[4] = {"Auto", "On", "Off", "Smooth"};
                int lCur = (int)(std::find(std::begin(lights), std::end(lights), cur.lighting) - std::begin(lights));
                int lPick = dropdownMenu("ilight", irow("lilight", "Lighting for new objects",
                                                        "Runtime lighting of the objects made from this model from now on. Auto: lit by "
                                                        "runtime lights that reach it. Objects already placed keep theirs."),
                                         nullptr, lightNames[lCur], {"Auto", "On", "Off", "Smooth"}, lCur);
                if (lPick >= 0 && lights[lPick] != cur.lighting) {
                    splash::ImportSettings next = ip.settings;
                    next.lighting = lights[lPick];
                    apply(next);
                }
                // Every warning of the model's last import, each with its fix.
                const std::vector<std::string>& warns = doc.importWarnings(ip.model);
                if (!warns.empty()) {
                    y += space::sm;
                    const float ix = x0 + space::sm, tx = ix + 22, tw = x1 - space::sm - tx;
                    const std::string head = "Warnings (" + std::to_string(warns.size()) + ")";
                    text(dl, ImVec2(ix, y), f.medium, type::label, color::warn, icon::warning);
                    text(dl, ImVec2(tx, y), f.semibold, type::label, color::text, head.c_str());
                    y += measure(f.semibold, type::label, head.c_str()).y + space::sm;
                    for (const std::string& w : warns) {
                        const editor::ImportWarning e = editor::explainImportWarning(w);
                        std::string problem = e.problem;
                        if (!problem.empty() && problem[0] >= 'a' && problem[0] <= 'z') problem[0] = char(problem[0] - 'a' + 'A');
                        dl->AddCircleFilled(ImVec2(ix + 8, y + 7), 2.5f, color::warn);
                        dl->AddText(f.regular, type::label, ImVec2(tx, y), color::text, problem.c_str(), nullptr, tw);
                        y += f.regular->CalcTextSizeA(type::label, FLT_MAX, tw, problem.c_str()).y + 2;
                        if (!e.fix.empty()) {
                            const std::string fix = "Fix: " + e.fix;
                            dl->AddText(f.regular, type::label, ImVec2(tx, y), color::textDim, fix.c_str(), nullptr, tw);
                            y += f.regular->CalcTextSizeA(type::label, FLT_MAX, tw, fix.c_str()).y;
                        }
                        y += space::sm;
                        ip.shown.push_back(w);
                    }
                }
            }
            sectionEnd(top);
        }
    }

    // A line printed once per change under scripted runs (the scene: line's
    // pattern). An empty line prints nothing and lets the next one print again.
    auto report = [&](const char* tag, const std::string& line) {
        if (!namedRectsOn()) return;
        static std::map<std::string, std::string> reported;
        std::string& last = reported[tag];
        if (last == line) return;
        last = line;
        if (line.empty()) return;
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    report("offvia", offVia.empty() ? std::string() : "object: " + o->name + " off via " + offVia);
    // This object's mesh colour, audio and skin lines; cleared when it has none.
    std::string meshColLine, materialLines, uvOffLine, audioLine, skinLine, interactLine, texHolesLine;

    // Mesh.
    // A labelled on/off switch; a click flips it through `flip` (one undo step).
    // Named obj.<id> for scripted tests.
    auto toggleRow = [&](const char* id, const char* label, const char* tip, bool on, void (*flip)(splash::Object&)) {
        std::string lid = std::string("l") + id;
        ImRect fr = row(lid.c_str(), label, tip);
        ImRect sw(ImVec2(fr.Min.x, fr.GetCenter().y - 8), ImVec2(fr.Min.x + 30, fr.GetCenter().y + 8));
        nameRect(std::string("obj.") + id, sw);
        if (toggle(id, sw, on)) doc.edit(path, flip);
    };
    // Shades rows from `from` down to y: settings that stay editable but do
    // nothing until another one is turned on.
    auto dimFrom = [&](float from) { dl->AddRectFilled(ImVec2(x0 + space::sm, from), ImVec2(x1 - space::sm, y), rgb(0x1b1e24, 150)); };
    if (o->mesh) {
        const splash::MeshComponent& m = *o->mesh;
        const editor::MeshInfo* mi = doc.mesh(m.mesh);
        editor::AssetStatus meshSt = mi ? mi->status : editor::AssetStatus::Missing;
        std::string tris = meshSt == editor::AssetStatus::Ok ? std::to_string(mi->triangles) + " tris" : std::string();
        std::string modelName = m.mesh.empty() ? std::string("None") : fileName(m.mesh);

        const size_t nMat = m.materials.size();
        const std::string texPath = m.materials.empty() ? std::string() : m.materials[0].texture;
        const editor::TextureInfo* ti = texPath.empty() ? nullptr : doc.texture(texPath);
        editor::AssetStatus texSt = ti ? ti->status : editor::AssetStatus::Ok;
        bool to4 = doc.couldBe4bpp(m);
        std::string texName = texPath.empty() ? std::string("None") : fileName(texPath);
        std::string texMeta = bppLabel(m.bitDepth);
        if (ti && ti->status == editor::AssetStatus::Ok)
            texMeta = std::to_string(ti->width) + " x " + std::to_string(ti->height) + "  \xc2\xb7  " + texMeta;

        top = y;
        fold(section("s_mesh", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::box, kind::mesh, "Mesh", true, true, true,
                     removeFlag(editor::ComponentKind::Mesh)));
        y += 34 + space::xs;
        const ImRect modelRow = row("lmodel", "Model", "The 3D model to draw. Click to pick a .glb, .gltf, .fbx, .obj or .blend file (.blend needs Blender installed), or drop one here.");
        bool pickModel = slotField(st, -1, path, "model", modelRow, icon::box, meshSt == editor::AssetStatus::Ok ? kind::mesh : color::warn,
                                    modelName.c_str(), tris.empty() ? nullptr : tris.c_str());
        st.modelField = folded ? ImRect() : modelRow;
        st.modelField.ClipWith(r);
        st.modelFieldPath = path;
        if (st.drop.hovering && st.modelField.Contains(st.drop.pos))
            dl->AddRect(modelRow.Min - ImVec2(1, 1), modelRow.Max + ImVec2(1, 1), color::accent, radius::field + 1, 0, 2.0f);
        if (fileCard("fixmesh", meshSt, m.mesh, mi ? mi->error : std::string())) pickModel = true;
        if (pickModel) {
            {
                std::lock_guard<std::mutex> lock(g_pickMutex);
                g_modelTarget = path;
            }
            static const SDL_DialogFileFilter modelFilter[] = {{"Models", "glb;gltf;fbx;obj;blend"}};
            SDL_ShowOpenFileDialog(onModelPicked, nullptr, nullptr, modelFilter, 1, nullptr, false);
        }
        // An untextured material's tint, from a colour well and its picker
        // (named obj.matcol.<i> and obj.matcolpick.<i>); a drag is one undo step.
        auto matColour = [&](size_t i) {
            const std::array<float, 4>& c4 = m.materials[i].color;
            const std::array<float, 3> c3{c4[0], c4[1], c4[2]};
            const std::string is = std::to_string(i);
            const ImRect colRow = row(("lmatcol." + is).c_str(), "Colour",
                                      "Tints this untextured part of the model. Ignored while it has a texture.");
            nameRect("obj.matcol." + is, colRow);
            const std::string popup = "##matcolpick" + is;
            if (colorField(("matcol." + is).c_str(), colRow, toColor(c3), toHex(c3).c_str())) ImGui::OpenPopup(popup.c_str());
            // Right-aligned under the field, so it stays inside the inspector.
            ImGui::SetNextWindowPos(ImVec2(colRow.Max.x, colRow.Max.y + space::xs), ImGuiCond_Always, ImVec2(1, 0));
            pushPopupStyle();
            if (ImGui::BeginPopup(popup.c_str())) {
                float c[3] = {c3[0], c3[1], c3[2]};
                ImGui::SetNextItemWidth(220);
                if (ImGui::ColorPicker3("##matcolwheel", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex |
                                                                ImGuiColorEditFlags_NoAlpha))
                    doc.edit(path, [&](splash::Object& ob) {
                        if (!ob.mesh || i >= ob.mesh->materials.size()) return;
                        std::array<float, 4>& col = ob.mesh->materials[i].color;
                        col = {c[0], c[1], c[2], col[3]};
                    }, "matcol." + is);
                nameRect("obj.matcolpick." + is, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                if (ImGui::IsItemDeactivated()) doc.endMerge();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(2);
        };
        // For scripted tests: the materials, one line each.
        int textured = 0;
        for (const splash::Material& mat : m.materials) textured += !mat.texture.empty();
        std::string matLines = "materials: " + std::to_string(nMat) + " textured " + std::to_string(textured);
        for (size_t i = 0; i < nMat; i++) {
            const std::array<float, 4>& c4 = m.materials[i].color;
            matLines += "\nmaterial " + std::to_string(i) + ": texture " +
                        (m.materials[i].texture.empty() ? std::string("none") : fileName(m.materials[i].texture)) + " colour " +
                        toHex({c4[0], c4[1], c4[2]});
        }
        materialLines = matLines;
        if (nMat <= 1) {
            const ImRect texRow = row("ltex", "Texture", "Image painted on the model. Click to pick an image. Converted to PS1 colours on export.");
            nameRect("obj.texture", texRow);
            if (slotField(st, static_cast<int>(AssetSlot::Texture), path, "texture", texRow, icon::grid,
                          texSt != editor::AssetStatus::Ok || to4 ? color::warn : color::textDim, texName.c_str(), texMeta.c_str(), 0) |
                fileCard("fixtex", texSt, texPath, ti ? ti->error : std::string()))
                pickAsset(AssetSlot::Texture, path, 0);
            if (nMat == 1 && texPath.empty()) matColour(0);
        } else {
            std::string cap = std::to_string(nMat) + " materials, " + std::to_string(textured) + " textured";
            if (meshSt == editor::AssetStatus::Ok && nMat > size_t(mi->submeshes)) {
                const size_t unused = nMat - size_t(mi->submeshes);
                cap += ", " + std::to_string(unused) + " unused (the model has " + std::to_string(mi->submeshes) +
                       (mi->submeshes == 1 ? " part)" : " parts)");
            }
            const ImVec2 at(x0 + space::sm, y + space::xs);
            text(dl, at, f.regular, type::caption, color::textDim, cap.c_str());
            nameRect("obj.matcount", ImRect(at, at + measure(f.regular, type::caption, cap.c_str())));
            y += 16 + space::sm;
            for (size_t i = 0; i < nMat; i++) {
                const std::string is = std::to_string(i);
                const std::string& tp = m.materials[i].texture;
                const editor::TextureInfo* mt = tp.empty() ? nullptr : doc.texture(tp);
                const editor::AssetStatus mSt = mt ? mt->status : tp.empty() ? editor::AssetStatus::Ok : editor::AssetStatus::Missing;
                std::string meta = mt && mt->status == editor::AssetStatus::Ok
                                       ? std::to_string(mt->width) + " x " + std::to_string(mt->height)
                                       : std::string();
                const std::string label = "Material " + std::to_string(i + 1);
                const ImRect texRow = row(("ltex." + is).c_str(), label.c_str(),
                                          "Image painted on this part of the model. Click to pick an image, or drop one here. Converted to PS1 colours on export.");
                nameRect("obj.texture." + is, texRow);
                if (slotField(st, static_cast<int>(AssetSlot::Texture), path, ("texture." + is).c_str(), texRow, icon::grid,
                              mSt != editor::AssetStatus::Ok || to4 ? color::warn : color::textDim,
                              tp.empty() ? "None" : fileName(tp).c_str(), meta.empty() ? nullptr : meta.c_str(), int(i)) |
                    fileCard(("fixtex." + is).c_str(), mSt, tp, mt ? mt->error : std::string()))
                    pickAsset(AssetSlot::Texture, path, int(i));
                if (tp.empty()) matColour(i);
            }
        }
        if (to4) {
            // Every textured material: the sums the 4 bpp check compared.
            int n = 0, cur = 0, at4 = 0, most = 0;
            std::string one;
            for (const splash::Material& mat : m.materials) {
                const editor::TextureInfo* t = mat.texture.empty() ? nullptr : doc.texture(mat.texture);
                if (!t) continue;
                n++;
                one = fileName(mat.texture);
                cur += m.bitDepth == splash::BitDepth::Bpp8 ? t->vramBytes8 : t->vramBytes16;
                at4 += t->vramBytes4;
                most = std::max(most, t->colors15);
            }
            std::string body = n == 1 ? one + " has " + std::to_string(most) + (most == 1 ? " colour" : " colours") + " and takes " +
                                            fmtKB(cur) + " of VRAM. At 4 bpp it keeps every colour and takes " + fmtKB(at4) + "."
                                      : "The " + std::to_string(n) + " textures have at most " + std::to_string(most) +
                                            (most == 1 ? " colour" : " colours") + " each and take " + fmtKB(cur) +
                                            " of VRAM together. At 4 bpp they keep every colour and take " + fmtKB(at4) + ".";
            if (card("fix4bpp", n == 1 ? "Texture could be 4 bpp" : "Textures could be 4 bpp", body, "Convert to 4 bpp"))
                doc.edit(path, [](splash::Object& ob) { ob.mesh->bitDepth = splash::BitDepth::Bpp4; });
        }
        // What the console will show, next to the source. A mesh texture is
        // cutout when it has texels under half alpha (splash::hasCutoutTexels).
        if (nMat <= 1 && !folded && texSt == editor::AssetStatus::Ok && !texPath.empty()) {
            const bool holes = ti && ti->holes;
            y += texturePreview(st.texPreview, dl, x0 + space::sm, x1 - space::sm, y, doc, texPath, m.bitDepth, holes) + space::sm;
            if (holes) {
                // Word-wrapped to the inspector's width.
                const char* const msg = "Texels under half alpha are see-through on the console. At 4 and 8 bpp this costs one palette colour.";
                const float wrapW = x1 - space::sm - (x0 + space::sm);
                std::vector<std::string> rows(1);
                std::string rest = msg;
                while (!rest.empty()) {
                    const size_t sp = rest.find(' ');
                    const std::string word = rest.substr(0, sp);
                    rest = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
                    const std::string tryRow = rows.back().empty() ? word : rows.back() + " " + word;
                    if (!rows.back().empty() && measure(f.regular, type::caption, tryRow.c_str()).x > wrapW) rows.push_back(word);
                    else rows.back() = tryRow;
                }
                const float top = y;
                float w = 0;
                for (const std::string& r : rows) {
                    text(dl, ImVec2(x0 + space::sm, y + space::xxs), f.regular, type::caption, color::textDim, r.c_str());
                    w = std::max(w, measure(f.regular, type::caption, r.c_str()).x);
                    y += 16;
                }
                nameRect("obj.texholes", ImRect(ImVec2(x0 + space::sm, top), ImVec2(x0 + space::sm + w, y)));
                y += space::sm;
            }
        }
        // Whether each texture of this mesh has see-through texels.
        for (const splash::Material& mat : m.materials) {
            const editor::TextureInfo* t = mat.texture.empty() ? nullptr : doc.texture(mat.texture);
            if (!t || t->status != editor::AssetStatus::Ok) continue;
            const std::string l = std::string("texture: holes ") + (t->holes ? "yes " : "no ") + mat.texture;
            if (texHolesLine.find(l) == std::string::npos) texHolesLine += (texHolesLine.empty() ? "" : "\n") + l;
        }
        // Which part a cutscene's UV offset track scrolls. One item per distinct
        // texture, numbered as the exporter numbers them (splash::uvTextureSlot).
        {
            std::vector<std::vector<size_t>> slotMats;
            for (size_t i = 0; i < nMat; i++) {
                const int sl = splash::uvTextureSlot(m, i);
                if (sl < 0) continue;
                if (size_t(sl) >= slotMats.size()) slotMats.resize(size_t(sl) + 1);
                slotMats[size_t(sl)].push_back(i);
            }
            if (!slotMats.empty()) {
                std::vector<std::string> labels;
                for (const std::vector<size_t>& ms : slotMats) {
                    std::string l = ms.size() == 1 ? "Material " : "Materials ";
                    for (size_t k = 0; k < ms.size(); k++)
                        l += (k == 0 ? "" : k + 1 == ms.size() ? " and " : ", ") + std::to_string(ms[k] + 1);
                    labels.push_back(l + " (" + fileName(m.materials[ms[0]].texture) + ")");
                }
                std::vector<const char*> items;
                for (const std::string& l : labels) items.push_back(l.c_str());
                const int cur = m.uvOffsetMaterial;
                const bool valid = cur >= 0 && size_t(cur) < labels.size();
                const ImRect uvRow = row("luvoff", "UV offset",
                                         "The part of the model a cutscene's UV offset track scrolls, by its texture. Parts sharing an image scroll together.");
                nameRect("obj.uvoff", uvRow);
                const int pick = dropdownMenu("obj.uvoff", uvRow, icon::grid, valid ? labels[size_t(cur)].c_str() : "Nothing", items,
                                              valid ? cur : -1);
                if (pick >= 0 && pick != cur) doc.edit(path, [pick](splash::Object& ob) { ob.mesh->uvOffsetMaterial = pick; });
                if (!valid) {
                    const std::string warn = "Texture " + std::to_string(cur) + " does not exist, so nothing scrolls.";
                    const ImVec2 at(x0 + space::sm, y + space::xxs);
                    text(dl, at, f.regular, type::caption, color::warn, warn.c_str());
                    nameRect("obj.uvoff.warn", ImRect(at, at + measure(f.regular, type::caption, warn.c_str())));
                    y += 16 + space::sm;
                }
                std::string ml;
                for (size_t i : splash::uvOffsetMaterials(m, cur)) ml += (ml.empty() ? "" : ",") + std::to_string(i);
                uvOffLine = "uvoffset: " + std::to_string(cur) + " materials " + (ml.empty() ? std::string("none") : ml);
            }
        }
        // Menu order follows the labels' enum: Baked vertex, Flat, Mesh colours.
        const splash::VertexColorMode modes[3] = {splash::VertexColorMode::Baked, splash::VertexColorMode::Flat,
                                                  splash::VertexColorMode::Mesh};
        int curMode = (int)(std::find(std::begin(modes), std::end(modes), m.vertexColors) - std::begin(modes));
        int pickMode = dropdownMenu("lighting", row("llight", "Lighting", "How light reaches this mesh. Baked vertex lighting costs nothing at runtime."),
                                    icon::sun, lightingLabel(m.vertexColors), {"Baked vertex", "Flat", "Mesh colours"}, curMode);
        if (pickMode >= 0 && modes[pickMode] != m.vertexColors)
            doc.edit(path, [&](splash::Object& ob) { ob.mesh->vertexColors = modes[pickMode]; });
        if (doc.missingMeshColors(m)) {
            meshColLine = "mesh: colours missing " + m.mesh;
            std::string body = modelName + " has no vertex colours, so it shows flat grey (127, 127, 127) on the console.";
            if (card("fixmeshcol", "No mesh colours", body, "Use baked lighting"))
                doc.edit(path, [](splash::Object& ob) { ob.mesh->vertexColors = splash::VertexColorMode::Baked; });
        }
        // Smoothed normals only feed baked vertex lighting.
        const bool baked = m.vertexColors == splash::VertexColorMode::Baked;
        const float smoothTop = y;
        toggleRow("smooth", "Smooth normals",
                  baked ? "Bakes the lighting as if the surface were smoothly curved, instead of shading each face flat. Off keeps the model's own normals, for hard edges."
                        : "Not used: only Baked vertex lighting uses it. Bakes the lighting as if the surface were smoothly curved.",
                  m.smoothNormals, [](splash::Object& ob) { ob.mesh->smoothNormals = !ob.mesh->smoothNormals; });
        if (!baked) dimFrom(smoothTop);
        sectionEnd(top);
    }

    // Collider.
    if (o->collider) {
        top = y;
        fold(section("s_col", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::square, kind::collider, "Collider", true, true, true,
                     removeFlag(editor::ComponentKind::Collider)));
        y += 34 + space::xs;
        // Menu order follows the tooltip: Static, Dynamic, None.
        const splash::ColliderKind kinds[3] = {splash::ColliderKind::Static, splash::ColliderKind::Dynamic, splash::ColliderKind::None};
        int cur = (int)(std::find(std::begin(kinds), std::end(kinds), o->collider->kind) - std::begin(kinds));
        int pick = dropdownMenu("shape", row("lshape", "Shape", "Static never moves. Dynamic can be moved by scripts. None turns collision off."),
                                nullptr, colliderLabel(o->collider->kind), {"Static", "Dynamic", "None"}, cur);
        if (pick >= 0) doc.edit(path, [&](splash::Object& ob) { ob.collider->kind = kinds[pick]; });
        sectionEnd(top);
    }

    // Light.
    if (o->light) {
        const splash::LightComponent& l = *o->light;
        top = y;
        bool flipLight = false;
        fold(section("s_light", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::lightbulb, kind::light, "Light", true, l.enabled, true,
                     removeFlag(editor::ComponentKind::Light), &flipLight));
        if (flipLight) doc.edit(path, [](splash::Object& ob) { ob.light->enabled = !ob.light->enabled; });
        y += 34 + space::xs;
        // Menu order follows the tooltip: Point, Spot, Directional.
        const splash::LightKind kinds[3] = {splash::LightKind::Point, splash::LightKind::Spot, splash::LightKind::Directional};
        int cur = (int)(std::find(std::begin(kinds), std::end(kinds), l.kind) - std::begin(kinds));
        int pick = dropdownMenu("lkind", row("llkind", "Type", "Point shines in every direction, spot in a cone, directional from far away."),
                                nullptr, lightKindLabel(l.kind), {"Point", "Spot", "Directional"}, cur);
        if (pick >= 0) doc.edit(path, [&](splash::Object& ob) { ob.light->kind = kinds[pick]; });
        const ImRect colRow = row("llcol", "Colour", "The colour of the light. Click to pick one.");
        if (colorField("lcol", colRow, toColor(l.color), toHex(l.color).c_str())) ImGui::OpenPopup("##lcolpick");
        // Right-aligned under the field, so it stays inside the inspector.
        ImGui::SetNextWindowPos(ImVec2(colRow.Max.x, colRow.Max.y + space::xs), ImGuiCond_Always, ImVec2(1, 0));
        pushPopupStyle();
        if (ImGui::BeginPopup("##lcolpick")) {
            float c[3] = {l.color[0], l.color[1], l.color[2]};
            ImGui::SetNextItemWidth(220);
            if (ImGui::ColorPicker3("##lcolwheel", c, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex |
                                                          ImGuiColorEditFlags_NoAlpha))
                doc.edit(path, [&](splash::Object& ob) { ob.light->color = {c[0], c[1], c[2]}; }, "lcol");
            if (ImGui::IsItemDeactivated()) doc.endMerge();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        float lv = l.intensity;
        commit(numberField("lint", row("llint", "Intensity", "How bright the light is. 1 is normal."), fmtShort(l.intensity).c_str(),
                           nullptr, 0, nullptr, &lv),
               "lint", [&](splash::Object& ob) { ob.light->intensity = std::max(0.0f, lv); });
        if (l.kind != splash::LightKind::Directional) {
            float rv = l.range;
            commit(numberField("lrange", row("llrange", "Range", "How far the light reaches before it fades out."), fmtShort(l.range).c_str(),
                               "m", 0, nullptr, &rv),
                   "lrange", [&](splash::Object& ob) { ob.light->range = std::max(0.0f, rv); });
        }
        if (l.kind == splash::LightKind::Spot) {
            float sv = l.spotAngle;
            commit(numberField("lspot", row("llspot", "Spot angle", "Width of the cone of light."), fmtShort(l.spotAngle).c_str(),
                               "\xc2\xb0", 0, nullptr, &sv),
                   "lspot", [&](splash::Object& ob) { ob.light->spotAngle = splash::clampv(sv, 1.0f, 179.0f); });
            float iv = l.innerSpotAngle;
            const ImRect innerRow = row("llinner", "Inner angle",
                                        "Inside this cone the light is at full strength; it fades out toward the spot angle. 0 fades from 80% of the spot angle.");
            nameRect("obj.linner", innerRow);
            commit(numberField("linner", innerRow, fmtShort(l.innerSpotAngle).c_str(), "\xc2\xb0", 0, nullptr, &iv), "linner",
                   [&](splash::Object& ob) { ob.light->innerSpotAngle = splash::clampv(iv, 0.0f, ob.light->spotAngle); });
        }
        sectionEnd(top);
    }

    // Script.
    if (o->script) {
        const std::string& lua = o->script->lua;
        top = y;
        fold(section("s_script", ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), icon::script, kind::script, "Script", true, true, true,
                     removeFlag(editor::ComponentKind::Script)));
        y += 34 + space::xs;
        bool found = doc.fileExists(lua);
        if (slotField(st, static_cast<int>(AssetSlot::Lua), path, "lua", row("llua", "File", "The script, blocks or Lua, that runs for this object. Click to pick one."), icon::fileCode,
                       found ? kind::script : color::warn, lua.empty() ? "None" : fileName(lua).c_str(), nullptr) |
            // No file yet: the field and New blocks / New Lua below say so, a "not found" card would not.
            fileCard("fixlua", found || lua.empty() ? editor::AssetStatus::Ok : editor::AssetStatus::Missing, lua, std::string()))
            pickAsset(AssetSlot::Lua, path);
        if (!lua.empty()) {
            if (button("editlua", ImVec2(x0 + space::sm + lw, y + space::xxs), icon::fileCode, found ? "Edit script" : "Create script",
                       ButtonKind::Secondary, nullptr, "Opens the file in the script editor, beside the Scene."))
                openScript(st, doc, lua);
        } else {
            if (auto lang = newScriptButtons("newblocks", "newlua", ImVec2(x0 + space::sm + lw, y + space::xxs), x1 - space::sm,
                                             o->interactable ? "Writes scripts/<object name>.blocks with 'when created' and 'when the player interacts' stacks, and opens it."
                                                             : "Writes scripts/<object name>.blocks with a when-created stack, and opens it.",
                                             o->interactable ? "Writes scripts/<object name>.lua with onCreate and onInteract ready to fill in, and opens it."
                                                             : "Writes scripts/<object name>.lua with onCreate ready to fill in, and opens it."))
                newScript(st, doc, &path, *lang);
        }
        y += size::field + space::sm;
        sectionEnd(top);
    }

    // Components the inspector shows a few fields of.
    auto compSection = [&](const char* id, editor::ComponentKind k) {
        ObjectLook lk = lookOf(k);
        fold(section(id, ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), lk.icon, lk.color, editor::info(k).label, true, true, true,
                     removeFlag(k)));
        y += 34 + space::xs;
    };
    auto number = [&](const char* id, const char* label, const char* tip, float v, const char* unit, const char* key, float lo, float hi,
                      const std::function<void(splash::Object&, float)>& set, int decimals = 2) {
        float nv = v;
        const std::string shown = fmtShort(v, decimals);
        std::string lid = std::string("l") + id;
        const ImRect fr = row(lid.c_str(), label, tip);
        nameRect(std::string("obj.") + id, fr);
        commit(numberField(id, fr, shown.c_str(), unit, 0, nullptr, &nv), key,
               [&](splash::Object& ob) { set(ob, splash::clampv(nv, lo, hi)); });
        return shown;
    };
    // What the last export's nav bake made of the scene: psxsplash only gives
    // the player the camera and the controller when there are nav regions.
    auto navNotes = [&]() {
        const std::optional<splash::ExportResult>& res = st.live.result();
        if (!res || !res->ok() || !res->stats.navWanted) return;
        const splash::ExportStats& s = res->stats;
        auto note = [&](const std::string& t, ImU32 col) {
            const float tx = x0 + space::sm, w = x1 - space::sm - tx;
            dl->AddText(f.regular, type::caption, ImVec2(tx, y), col, t.c_str(), nullptr, w);
            y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t.c_str()).y + space::sm;
        };
        if (s.navRegions == 0) {
            // Mesh objects the bake skips: no collider, or a collider that is not Static.
            std::vector<editor::ObjectPath> meshes;
            editor::ObjectPath at;
            std::function<void(const std::vector<splash::Object>&)> walk = [&](const std::vector<splash::Object>& objs) {
                for (size_t i = 0; i < objs.size(); i++) {
                    at.push_back(int(i));
                    const splash::Object& ob = objs[i];
                    if (ob.active && ob.mesh && !ob.skin && (!ob.collider || ob.collider->kind == splash::ColliderKind::None))
                        meshes.push_back(at);
                    walk(ob.children);
                    at.pop_back();
                }
            };
            walk(doc.scene().objects);
            std::string body =
                "The nav bake found no ground to stand on, so the game keeps the camera at the origin and ignores the "
                "controller. The player walks on meshes with a Static collider that are flat enough and wider than the player.";
            if (!meshes.empty()) {
                body += meshes.size() == 1 ? std::string(" 1 mesh has no collider: the fix gives it a Static one.")
                                           : " " + std::to_string(meshes.size()) +
                                                 " meshes have no collider: the fix gives them a Static one.";
                if (card("navempty", "The player can't walk", body, "Make meshes walkable"))
                    doc.editMany(meshes, [](splash::Object& ob) {
                        if (!ob.collider) ob.collider = splash::ColliderComponent{};
                        ob.collider->kind = splash::ColliderKind::Static;
                    });
            } else {
                note("The player can't walk. " + body, color::warn);
            }
        } else if (splash::navCoarsened(s)) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "The walkable ground is %.0f x %.0f m, so the nav bake uses %.2f m cells instead of %.2f m to keep "
                          "export fast. Walls and ledges are placed to about %.2f m.",
                          double(s.navExtentX), double(s.navExtentZ), double(s.navCellSize), double(s.navCellSizeAsked),
                          double(s.navCellSize));
            note(buf, color::textFaint);
        }
        // Regions whose one floor plane strays from the ground by more than
        // the bake's Max plane error: psxsplash stands the player on the plane.
        if (s.navRegions > 0) {
            int over = 0;
            for (const splash::ExportStats::NavRegionInfo& ri : s.navInfo)
                if (ri.planeDeviation > s.navMaxPlaneError + 1e-6f) over++;
            const std::string limit = fmtShort(s.navMaxPlaneError, 3);
            report("navplane", "nav plane: " + std::to_string(over) + " over " + limit);
            if (over > 0) {
                const std::string t = (over == 1 ? std::string("1 nav region is") : std::to_string(over) + " nav regions are") +
                                      " more than " + limit +
                                      " m off " + (over == 1 ? "its" : "their") +
                                      " floor plane, so the player may float or sink there. Try the Terrain preset or a lower "
                                      "Merge region area.";
                const float tx = x0 + space::sm, w = x1 - space::sm - tx;
                const float th = f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t.c_str()).y;
                nameRect("obj.navplane", ImRect(ImVec2(tx, y), ImVec2(tx + w, y + th)));
                note(t, color::warn);
            }
        }
    };
    // The voxel size of the nav bake, and what the bake makes of the agent in
    // those voxels (core/navregion.cpp: height and climb in cell heights,
    // radius in cell sizes). A step lower than one cell height climbs nothing.
    auto navCells = [&](const char* prefix, const char* who, const splash::NavBakeSettings& nb, float agentHeight,
                        float agentRadius, splash::NavBakeSettings* (*bakeOf)(splash::Object&)) {
        const std::string cid = std::string(prefix) + "cell", hid = std::string(prefix) + "cellh";
        const std::string over = std::string(who) == "player" ? " A Navigation component in the scene overrides it." : "";
        const std::string ctip = "Width of the nav bake's voxels. Smaller follows walls and ledges more closely and bakes slower; a "
                                 "large walkable area gets bigger cells to keep export fast." + over;
        const std::string htip = "Height of the nav bake's voxels. Step height and headroom are rounded down and up to it." + over;
        const std::string shownCs = number(cid.c_str(), "Cell size", ctip.c_str(), nb.cellSize, "m", cid.c_str(), 0.01f, 1,
               [bakeOf](splash::Object& ob, float v) { bakeOf(ob)->cellSize = v; }, 3);
        const std::string shownCh = number(hid.c_str(), "Cell height", htip.c_str(), nb.cellHeight, "m", hid.c_str(), 0.01f, 1, [bakeOf](splash::Object& ob, float v) { bakeOf(ob)->cellHeight = v; }, 3);
        const int vh = int(std::ceil(double(agentHeight) / nb.cellHeight));
        const int vr = int(std::ceil(double(agentRadius) / nb.cellSize));
        const int vc = int(std::floor(double(nb.maxStepHeight) / nb.cellHeight));
        char buf[160];
        std::snprintf(buf, sizeof buf, "Voxels: height %d, radius %d, climb %d (%.3f m)", vh, vr, vc, double(vc) * nb.cellHeight);
        const float tx = x0 + space::sm + lw, w = x1 - space::sm - tx;
        const float th = f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, buf).y;
        nameRect(std::string("obj.") + prefix + "voxels", ImRect(ImVec2(tx, y), ImVec2(x1 - space::sm, y + th)));
        dl->AddText(f.regular, type::caption, ImVec2(tx, y), vc == 0 ? color::warn : color::textFaint, buf, nullptr, w);
        y += th + space::sm;
        report((std::string(prefix) + "vox").c_str(), std::string(who) + " cells: " + shownCs + " x " + shownCh + " voxels: height " + std::to_string(vh) + " radius " + std::to_string(vr) + " climb " +
                           std::to_string(vc));
    };
    // The nav bake's region settings: SplashEdit's three presets, and the
    // fields themselves under a Bake details disclosure (closed at first).
    auto navBake = [&](const char* prefix, const char* who, const splash::NavBakeSettings& nb,
                       splash::NavBakeSettings* (*bakeOf)(splash::Object&)) {
        const std::string p = prefix;
        const std::string over = std::string(who) == "player" ? " A Navigation component in the scene overrides it." : "";
        // Preset: what the settings match now in the field column, the three
        // buttons on the line under it (they need the panel's full width).
        const ImRect fr = row(("l" + p + "preset").c_str(), "Preset",
                              ("Sets the region settings below for a kind of scene. Step height and slope are kept." + over).c_str());
        const int match = splash::matchingNavPreset(nb);
        static const char* const matchNames[3] = {"Indoor", "Terrain", "Multi-Level"};
        static const char* const matchKeys[3] = {"indoor", "terrain", "multilevel"};
        const std::string now = match < 0 ? std::string("Custom") : std::string("Matches ") + matchNames[match];
        {
            const ImVec2 sz = measure(f.regular, type::label, now.c_str());
            const ImVec2 at(fr.Min.x, std::floor(fr.GetCenter().y - sz.y * 0.5f));
            text(dl, at, f.regular, type::label, match < 0 ? color::textDim : color::text, now.c_str());
            nameRect("obj." + p + "presetnow", ImRect(at, at + sz));
            report((p + "presetnow").c_str(), std::string(who) + " preset: " + now);
        }
        struct PresetButton {
            const char* key;
            const char* label;
            const char* tip;
            splash::NavPreset preset;
        };
        static const PresetButton presets[3] = {
            {"indoor", "Indoor", "For architectural and indoor scenes with flat floors and walls.", splash::NavPreset::Indoor},
            {"terrain", "Terrain", "For natural terrain, hills and outdoor scenes.", splash::NavPreset::Terrain},
            {"multilevel", "Multi-Level", "For multi-storey buildings, bridges and overlapping floors.", splash::NavPreset::MultiLevel}};
        float bx = x0 + space::sm;
        for (const PresetButton& pb : presets) {
            const std::string bid = "obj." + p + "preset." + pb.key;
            float bw = 0;
            if (button(bid.c_str(), ImVec2(bx, y), nullptr, pb.label, ButtonKind::Secondary, &bw, pb.tip)) {
                const splash::NavPreset pr = pb.preset;
                doc.edit(path, [bakeOf, pr](splash::Object& ob) { splash::applyNavPreset(*bakeOf(ob), pr); });
            }
            bx += bw + space::sm;
        }
        y += size::field + 6 + space::sm;
        // Bake details disclosure.
        bool& open = st.navBakeDetails[p == "p" ? 0 : 1];
        {
            const std::string did = "obj." + p + "bakedetails";
            const ImRect dr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + 24));
            const Hit h = interact(did.c_str(), dr);
            nameRect(did, dr);
            tooltip("The settings the presets change: how the walkable ground is split into regions and how closely each region's floor follows the ground.");
            if (h.clicked) open = !open;
            const ImU32 col = lerpColor(color::textDim, color::text, h.hover);
            text(dl, ImVec2(dr.Min.x, centerY(f.medium, type::icon, dr.Min.y, dr.Max.y)), f.medium, type::icon - 2, col,
                 open ? icon::chevronDown : icon::chevronRight);
            text(dl, ImVec2(dr.Min.x + 18, centerY(f.regular, type::label, dr.Min.y, dr.Max.y)), f.regular, type::label, col,
                 "Bake details");
            y += 24 + space::xs;
        }
        static const char* const partNames[3] = {"Watershed", "Monotone", "Layer"};
        static const char* const partKeys[3] = {"watershed", "monotone", "layer"};
        const int part = std::clamp(int(nb.partition), 0, 2);
        std::string sMin = fmtShort(float(nb.minRegionArea), 0), sMerge = fmtShort(float(nb.mergeRegionArea), 0),
                    sSimp = fmtShort(nb.maxSimplifyError), sEdge = fmtShort(nb.maxEdgeLength), sDist = fmtShort(nb.detailSampleDist),
                    sErr = fmtShort(nb.detailMaxError, 3), sPlane = fmtShort(nb.maxPlaneError, 3);
        if (open) {
            const std::string pid = "obj." + p + "part";
            const ImRect mr = row(("l" + p + "part").c_str(), "Method",
                                  ("Watershed: classic, best for indoor and architectural scenes; can make oversized regions on rolling "
                                   "terrain. Monotone: more uniform regions, better for open terrain. Layer: keeps overlapping floors and "
                                   "bridges apart." + over).c_str());
            nameRect(pid, mr);
            const int pick = dropdownMenu(pid.c_str(), mr, nullptr, partNames[part], {partNames[0], partNames[1], partNames[2]}, part);
            if (pick >= 0 && pick != part)
                doc.edit(path, [bakeOf, pick](splash::Object& ob) { bakeOf(ob)->partition = splash::NavPartition(pick); });
            auto num = [&](const char* suffix, const char* label, const std::string& tip, float v, const char* unit, float lo, float hi,
                           void (*set)(splash::NavBakeSettings&, float), int decimals) {
                const std::string id = p + suffix;
                return number(id.c_str(), label, (tip + over).c_str(), v, unit, id.c_str(), lo, hi,
                              [bakeOf, set](splash::Object& ob, float x) { set(*bakeOf(ob), x); }, decimals);
            };
            sMin = num("minarea", "Min region area", "Regions smaller than this many voxels are removed. Typical 4 to 16.",
                       float(nb.minRegionArea), "voxels", 1, 1000,
                       [](splash::NavBakeSettings& n, float x) { n.minRegionArea = int(std::lround(x)); }, 0);
            sMerge = num("mergearea", "Merge region area",
                         "Regions smaller than this many voxels merge into their largest neighbour. The most important setting for "
                         "terrain: high values make big regions whose single floor plane can't follow hills; 4 to 8 keeps floors "
                         "accurate at the cost of more regions.",
                         float(nb.mergeRegionArea), "voxels", 1, 1000,
                         [](splash::NavBakeSettings& n, float x) { n.mergeRegionArea = int(std::lround(x)); }, 0);
            sSimp = num("simplify", "Max simplify error",
                        "How far region edges may stray from the voxel outline, in metres. Flat floors 1 to 2, terrain 0.3 to 0.6.",
                        nb.maxSimplifyError, "m", 0.1f, 10, [](splash::NavBakeSettings& n, float x) { n.maxSimplifyError = x; }, 2);
            sEdge = num("edgelen", "Max edge length",
                        "Longest region edge, in metres. Shorter gives smaller regions with better floor fits. Terrain 4 to 6, "
                        "indoors 8 to 16.",
                        nb.maxEdgeLength, "m", 0, 100, [](splash::NavBakeSettings& n, float x) { n.maxEdgeLength = x; }, 2);
            sDist = num("detdist", "Detail sample dist",
                        "Height sampling distance, in cell sizes. Lower follows slopes more closely. Typical 4 to 8.",
                        nb.detailSampleDist, "x cells", 0, 32, [](splash::NavBakeSettings& n, float x) { n.detailSampleDist = x; }, 2);
            sErr = num("deterr", "Detail max error",
                       "Largest height error allowed when sampling the floor, in metres. Typical 0.01 to 0.05.", nb.detailMaxError, "m",
                       0.001f, 1, [](splash::NavBakeSettings& n, float x) { n.detailMaxError = x; }, 3);
            sPlane = num("planeerr", "Max plane error",
                         "Regions whose floor plane is further than this from the ground get a warning: the player may float or sink "
                         "there. Typical 0.05 to 0.2.",
                         nb.maxPlaneError, "m", 0.001f, 10, [](splash::NavBakeSettings& n, float x) { n.maxPlaneError = x; }, 3);
        }
        report((p + "bake").c_str(), std::string(who) + " bake: preset " + (match < 0 ? std::string("custom") : matchKeys[match]) +
                                         " method " + partKeys[part] + " area " + sMin + "/" + sMerge + " simplify " + sSimp +
                                         " edge " + sEdge + " detail " + sDist + "/" + sErr + " plane " + sPlane);
    };
    if (o->player) {
        const splash::PlayerComponent& p = *o->player;
        top = y;
        compSection("s_player", editor::ComponentKind::Player);
        number("pheight", "Height", "Eye height of the player.", p.playerHeight, "m", "pheight", 0.1f, 100,
               [](splash::Object& ob, float v) { ob.player->playerHeight = v; });
        number("pradius", "Radius", "How wide the player is, for walls and the nav bake.", p.playerRadius, "m", "pradius", 0.01f, 100,
               [](splash::Object& ob, float v) { ob.player->playerRadius = v; });
        number("pspeed", "Move speed", "Walking speed.", p.moveSpeed, "m/s", "pspeed", 0, 1000,
               [](splash::Object& ob, float v) { ob.player->moveSpeed = v; });
        number("pjump", "Jump height", "How high a jump goes.", p.jumpHeight, "m", "pjump", 0, 1000,
               [](splash::Object& ob, float v) { ob.player->jumpHeight = v; });
        number("psprint", "Sprint speed", "Running speed, while the sprint button is held.", p.sprintSpeed, "m/s", "psprint", 0, 1000,
               [](splash::Object& ob, float v) { ob.player->sprintSpeed = v; });
        number("pgravity", "Gravity", "How fast the player falls. Earth is about 9.8; higher feels snappier.", p.gravity,
               "m/s\xc2\xb2", "pgravity", 0, 1000, [](splash::Object& ob, float v) { ob.player->gravity = v; });
        number("pstep", "Step height", "Tallest step or ledge the player walks up without jumping. For the nav bake; a Navigation component in the scene overrides it.",
               p.nav.maxStepHeight, "m", "pstep", 0, 100, [](splash::Object& ob, float v) { ob.player->nav.maxStepHeight = v; });
        number("pslope", "Max slope", "Steepest ground the player can walk on. For the nav bake; a Navigation component in the scene overrides it.",
               p.nav.walkableSlopeAngle, "deg", "pslope", 0, 90, [](splash::Object& ob, float v) { ob.player->nav.walkableSlopeAngle = v; });
        navCells("p", "player", p.nav, p.playerHeight, p.playerRadius, [](splash::Object& ob) { return &ob.player->nav; });
        navBake("p", "player", p.nav, [](splash::Object& ob) { return &ob.player->nav; });
        navNotes();
        sectionEnd(top);
    }
    if (o->navigation) {
        const splash::NavigationComponent& n = *o->navigation;
        top = y;
        compSection("s_nav", editor::ComponentKind::Navigation);
        number("nheight", "Agent height", "Headroom an agent needs to walk somewhere.", n.agentHeight, "m", "nheight", 0.1f, 100,
               [](splash::Object& ob, float v) { ob.navigation->agentHeight = v; });
        number("nradius", "Agent radius", "How far walkable ground keeps from walls.", n.agentRadius, "m", "nradius", 0.01f, 100,
               [](splash::Object& ob, float v) { ob.navigation->agentRadius = v; });
        number("nstep", "Step height", "Tallest step or ledge an agent walks up. Overrides the Player's.", n.nav.maxStepHeight, "m",
               "nstep", 0, 100, [](splash::Object& ob, float v) { ob.navigation->nav.maxStepHeight = v; });
        number("nslope", "Max slope", "Steepest walkable ground. Overrides the Player's.", n.nav.walkableSlopeAngle, "deg", "nslope",
               0, 90, [](splash::Object& ob, float v) { ob.navigation->nav.walkableSlopeAngle = v; });
        navCells("n", "navigation", n.nav, n.agentHeight, n.agentRadius, [](splash::Object& ob) { return &ob.navigation->nav; });
        navBake("n", "navigation", n.nav, [](splash::Object& ob) { return &ob.navigation->nav; });
        if (!o->player) navNotes();
        sectionEnd(top);
    }
    if (o->trigger) {
        const splash::TriggerComponent& tr = *o->trigger;
        top = y;
        compSection("s_trigger", editor::ComponentKind::Trigger);
        float sz[3] = {tr.size.x, tr.size.y, tr.size.z};
        commit(vec3Field("tsize", row("ltsize", "Size", "The box, in metres, centred on the object."), fmtFixed(tr.size.x).c_str(),
                         fmtFixed(tr.size.y).c_str(), fmtFixed(tr.size.z).c_str(), nullptr, sz),
               "tsize", [&](splash::Object& ob) { ob.trigger->size = {std::max(0.0f, sz[0]), std::max(0.0f, sz[1]), std::max(0.0f, sz[2])}; });
        bool found = !tr.lua.empty() && doc.fileExists(tr.lua);
        if (slotField(st, static_cast<int>(AssetSlot::TriggerLua), path, "tlua", row("ltlua", "Script", "The script, blocks or Lua, that gets the enter and exit calls. Click to pick one."), icon::fileCode,
                       tr.lua.empty() || found ? kind::script : color::warn, tr.lua.empty() ? "None" : fileName(tr.lua).c_str(), nullptr))
            pickAsset(AssetSlot::TriggerLua, path);
        if (!tr.lua.empty()) {
            if (button("edittlua", ImVec2(x0 + space::sm + lw, y + space::xxs), icon::fileCode, found ? "Edit script" : "Create script",
                       ButtonKind::Secondary, nullptr, "Opens the file in the script editor, beside the Scene."))
                openScript(st, doc, tr.lua);
        } else {
            if (auto lang = newScriptButtons("newtblocks", "newtlua", ImVec2(x0 + space::sm + lw, y + space::xxs), x1 - space::sm,
                                             "Writes scripts/<object name>_trigger.blocks with when-the-player-enters and -leaves stacks, and opens it.",
                                             "Writes scripts/<object name>_trigger.lua with onTriggerEnter and onTriggerExit, and opens it."))
                newScript(st, doc, &path, *lang, true);
        }
        y += size::field + space::sm;
        sectionEnd(top);
    }
    // Scripted checks name each axis of a vec3Field: <name>.x/.y/.z.
    auto nameVec3 = [&](const std::string& name, ImRect r) {
        const float gap = space::xs, w = (r.GetWidth() - gap * 2) / 3;
        for (int i = 0; i < 3; ++i)
            nameRect(name + "." + "xyz"[i],
                     ImRect(ImVec2(r.Min.x + i * (w + gap), r.Min.y), ImVec2(r.Min.x + i * (w + gap) + w, r.Max.y)));
    };
    if (o->room) {
        const splash::RoomComponent& rm = *o->room;
        top = y;
        compSection("s_room", editor::ComponentKind::Room);
        float sz[3] = {rm.size.x, rm.size.y, rm.size.z};
        const ImRect szr = row("lrsize", "Size", "The room's box, in metres. Static geometry mostly inside it belongs to the room.");
        nameVec3("obj.rsize", szr);
        commit(vec3Field("rsize", szr, fmtFixed(rm.size.x).c_str(), fmtFixed(rm.size.y).c_str(), fmtFixed(rm.size.z).c_str(), nullptr, sz),
               "rsize", [&](splash::Object& ob) { ob.room->size = {std::max(0.0f, sz[0]), std::max(0.0f, sz[1]), std::max(0.0f, sz[2])}; });
        float off[3] = {rm.offset.x, rm.offset.y, rm.offset.z};
        const ImRect ofr = row("lroffset", "Offset", "Where the box's centre sits, in metres, from the object.");
        nameVec3("obj.roffset", ofr);
        commit(vec3Field("roffset", ofr, fmtFixed(rm.offset.x).c_str(), fmtFixed(rm.offset.y).c_str(), fmtFixed(rm.offset.z).c_str(), nullptr, off),
               "roffset", [&](splash::Object& ob) { ob.room->offset = {off[0], off[1], off[2]}; });
        // What the last live export made of this room. Drawn from the first
        // frame, so nothing below moves when the export lands.
        const std::optional<splash::ExportResult>& res = st.live.result();
        std::string line = "Not exported yet";
        ImU32 tone = color::textFaint;
        if (res && !res->ok()) {
            line = "Export failed: see problems";
            tone = color::bad;
        } else if (res) {
            line = !o->active        ? "Not exported: the object is off"
                   : !offVia.empty() ? "Not exported: '" + offVia + "' is off"
                                     : "Not in the last export";
            for (const splash::ExportStats::RoomSummary& rs : res->stats.rooms)
                if (rs.name == o->name) {
                    line = std::to_string(rs.triRefs) + (rs.triRefs == 1 ? " triangle, " : " triangles, ") + std::to_string(rs.cells) +
                           (rs.cells == 1 ? " cell, " : " cells, ") + std::to_string(rs.portals) + (rs.portals == 1 ? " portal" : " portals");
                    tone = color::textDim;
                }
        }
        const ImRect sr = row("lrstats", "Exported", "From the last export: the triangles drawn with this room (shared ones near a portal "
                                                     "count in both rooms), the cells it is split into for culling, and its portals.");
        nameRect("obj.rstats", sr);
        text(dl, ImVec2(sr.Min.x, centerY(f.regular, type::body, sr.Min.y, sr.Max.y)), f.regular, type::body, tone, line.c_str());
        report("room", "room: " + o->name + " " + line);
        sectionEnd(top);
    }
    if (o->portal) {
        const splash::PortalComponent& pc = *o->portal;
        top = y;
        compSection("s_portal", editor::ComponentKind::Portal);
        // Every Room object in the scene, by name, for the two pickers. A name is
        // on when some Room carrying it is exported (on, under parents that are
        // on); the rest stay pickable but are marked, since the export skips them.
        std::vector<std::pair<std::string, bool>> roomNames;
        for (const splash::FlatObject& fo : splash::flatten(doc.scene())) {
            if (!fo.object->room) continue;
            const bool on = fo.activeInHierarchy && fo.object->active;
            auto it = std::find_if(roomNames.begin(), roomNames.end(), [&](const auto& r) { return r.first == fo.object->name; });
            if (it == roomNames.end())
                roomNames.push_back({fo.object->name, on});
            else
                it->second = it->second || on;
        }
        auto roomOff = [&](const std::string& n) {
            for (const auto& r : roomNames)
                if (r.first == n) return !r.second;
            return false;
        };
        auto roomPicker = [&](const char* id, const char* label, const char* tip, const std::string& cur, bool isA) {
            std::vector<std::string> labels{"None"};
            int at = cur.empty() ? 0 : -1;
            for (const auto& [n, on] : roomNames) {
                if (n == cur) at = int(labels.size());
                labels.push_back(on ? n : n + "  (off)");
            }
            std::vector<const char*> items;
            for (const std::string& l : labels) items.push_back(l.c_str());
            const std::string shown = cur.empty() ? std::string("None") : at < 0 ? cur + "  (no such Room)" : labels[size_t(at)];
            const std::string lid = std::string("l") + id;
            const ImRect rr = row(lid.c_str(), label, tip);
            nameRect(std::string("obj.") + id, rr);
            const int pick = dropdownMenu(id, rr, icon::layoutTemplate, shown.c_str(), items, at);
            if (pick >= 0) {
                const std::string name = pick == 0 ? std::string() : roomNames[size_t(pick) - 1].first;
                doc.edit(path, [name, isA](splash::Object& ob) { (isA ? ob.portal->roomA : ob.portal->roomB) = name; });
            }
        };
        roomPicker("pa", "Room A", "One side of the opening. The camera in either room sees into the other through it.", pc.roomA, true);
        roomPicker("pb", "Room B", "The other side. The viewport's arrow points from Room A into Room B.", pc.roomB, false);
        number("pw", "Width", "The opening's width along the object's X axis, before scale.", pc.width, "m", "pw", 0.01f, 1000,
               [](splash::Object& ob, float v) { ob.portal->width = v; });
        number("ph", "Height", "The opening's height along the object's Y axis, before scale.", pc.height, "m", "ph", 0.01f, 1000,
               [](splash::Object& ob, float v) { ob.portal->height = v; });
        // The exporter's own complaints about this portal, below the fields so they never move them.
        const std::vector<splash::FlatObject> flat = splash::flatten(doc.scene());
        std::vector<const splash::FlatObject*> rooms;
        const splash::FlatObject* self = nullptr;
        for (const splash::FlatObject& fo : flat) {
            if (fo.object == o) self = &fo;
            if (fo.activeInHierarchy && fo.object->active && fo.object->room) rooms.push_back(&fo);
        }
        size_t problems = 0;
        if (self) {
            const std::string prefix = "portal " + o->name;
            const std::vector<std::string> errs = splash::portalProblems(rooms, *self);
            problems = errs.size();
            for (std::string e : errs) {
                if (e.compare(0, prefix.size(), prefix) == 0) e.erase(0, prefix.size());
                e.erase(0, e.find_first_not_of(": "));
                if (!e.empty()) e[0] = char(std::toupper(static_cast<unsigned char>(e[0])));
                const std::string t = e + ". The export stops until this is fixed.";
                const float tx = x0 + space::sm, w = x1 - space::sm - tx;
                dl->AddText(f.regular, type::caption, ImVec2(tx, y), color::warn, t.c_str(), nullptr, w);
                y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t.c_str()).y + space::sm;
            }
        }
        char dims[64];
        std::snprintf(dims, sizeof dims, " %gx%g", double(pc.width), double(pc.height));
        report("portal", "portal: " + o->name + " a=" + pc.roomA + (roomOff(pc.roomA) ? "(off)" : "") + " b=" + pc.roomB +
                             (roomOff(pc.roomB) ? "(off)" : "") + dims +
                             (problems ? " problems " + std::to_string(problems) : std::string()));
        sectionEnd(top);
    }
    if (o->interactable) {
        const splash::InteractableComponent& in = *o->interactable;
        top = y;
        compSection("s_interact", editor::ComponentKind::Interactable);
        number("iradius", "Radius", "How close the player has to be.", in.radius, "m", "iradius", 0, 1000,
               [](splash::Object& ob, float v) { ob.interactable->radius = v; });
        {
            // Menu order: face buttons, shoulders, Start/Select, d-pad, sticks.
            // Values are psyqo's pad bits (AdvancedPad::Button), what psxsplash tests.
            static const int bits[16] = {14, 13, 15, 12, 10, 11, 8, 9, 3, 0, 4, 6, 7, 5, 1, 2};
            static const char* const names[16] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2",
                                                  "Start", "Select", "Up", "Down", "Left", "Right", "L3", "R3"};
            int cur = -1;
            for (int i = 0; i < 16; i++)
                if (bits[i] == in.button) cur = i;
            const ImRect br = row("libutton", "Button", "The pad button that uses the object.");
            nameRect("obj.ibutton", br);
            int pick = dropdownMenu("ibutton", br, nullptr, cur >= 0 ? names[cur] : "None", std::vector<const char*>(names, names + 16), cur);
            if (pick >= 0) doc.edit(path, [&](splash::Object& ob) { ob.interactable->button = bits[pick]; });
        }
        toggleRow("irepeat", "Repeatable", "On, it can be used again after the cooldown. Off, it works once.", in.repeatable,
                  [](splash::Object& ob) { ob.interactable->repeatable = !ob.interactable->repeatable; });
        const float coolTop = y;
        number("icooldown", "Cooldown",
               in.repeatable ? "Wait before it can be used again. The console runs 60 frames a second (50 on PAL)."
                             : "Not used: turn on Repeatable first. Wait before it can be used again, in frames.",
               float(in.cooldownFrames), "frames", "icooldown", 0, 65535,
               [](splash::Object& ob, float v) { ob.interactable->cooldownFrames = static_cast<uint16_t>(std::lround(v)); });
        if (!in.repeatable) dimFrom(coolTop);
        toggleRow("iprompt", "Show prompt", "Shows a UI canvas while the player is close enough to use it.", in.showPrompt,
                  [](splash::Object& ob) { ob.interactable->showPrompt = !ob.interactable->showPrompt; });
        {
            const float promptTop = y;
            std::vector<const char*> items{"None"};
            int cur = in.promptCanvas.empty() ? 0 : -1;
            for (const splash::UICanvas& c : doc.scene().canvases) {
                if (c.name == in.promptCanvas) cur = int(items.size());
                items.push_back(c.name.c_str());
            }
            std::string shown = in.promptCanvas.empty() ? std::string("None")
                                : cur < 0                ? in.promptCanvas + "  (no such canvas)"
                                                         : in.promptCanvas;
            const ImRect cr = row("licanvas", "Prompt", in.showPrompt ? "The UI canvas shown as the prompt. Its name has to fit in 15 characters."
                                                                       : "Not used: turn on Show prompt first. The UI canvas shown as the prompt.");
            nameRect("obj.icanvas", cr);
            int pick = dropdownMenu("icanvas", cr, nullptr, shown.c_str(), items, cur);
            if (pick >= 0) {
                std::string name = pick == 0 ? std::string() : std::string(items[size_t(pick)]);
                doc.edit(path, [name](splash::Object& ob) { ob.interactable->promptCanvas = name; });
            }
            if (in.promptCanvas.size() > 15) {
                const std::string t = "Only 15 characters of the canvas name are kept, so the console will not find '" + in.promptCanvas +
                                      "'. Rename the canvas.";
                const float tx = x0 + space::sm, w = x1 - space::sm - tx;
                dl->AddText(f.regular, type::caption, ImVec2(tx, y), color::warn, t.c_str(), nullptr, w);
                y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, t.c_str()).y + space::sm;
            }
            if (!in.showPrompt) dimFrom(promptTop);
        }
        ImRect fv = row("lifacing", "Require facing", "The player has to be looking toward the object to use it.");
        if (toggle("ifacing", ImRect(ImVec2(fv.Min.x, fv.GetCenter().y - 8), ImVec2(fv.Min.x + 30, fv.GetCenter().y + 8)), in.lineOfSight))
            doc.edit(path, [](splash::Object& ob) { ob.interactable->lineOfSight = !ob.interactable->lineOfSight; });
        // Facing angle only matters with Require facing on; it stays editable
        // when off, dimmed, so it can be set up front.
        float angleTop = y;
        number("iangle", "Facing angle",
               in.lineOfSight ? "How far to either side of straight ahead the object can be and still count as faced. 90 is anywhere in front, 180 is any direction."
                              : "Not used: turn on Require facing first. How far to either side of straight ahead the object can be and still count as faced.",
               in.facingAngle, "deg", "iangle", 0, 180, [](splash::Object& ob, float v) { ob.interactable->facingAngle = v; });
        if (!in.lineOfSight) dl->AddRectFilled(ImVec2(x0 + space::sm, angleTop), ImVec2(x1 - space::sm, y), rgb(0x1b1e24, 150));
        {
            // What using it runs: psxsplash calls onInteract(self) in the
            // object's own script.
            const ImRect er = row("lievent", "Event", "What the console calls when the player uses it.");
            nameRect("obj.ievent", er);
            const ImVec2 at(er.Min.x, er.Min.y - 3);
            std::string caption;
            bool warn = true;
            if (!o->script) {
                caption = "Nothing happens when it is used: the console calls onInteract(self) in this object's script.";
                interactLine = "interact: event none";
                if (auto lang = newScriptButtons("newiblocks", "newilua", at, x1 - space::sm,
                                                 "Adds a Script component with scripts/<object name>.blocks, a 'when the player interacts' stack ready, and opens it.",
                                                 "Adds a Script component with scripts/<object name>.lua, onInteract ready to fill in, and opens it."))
                    newScript(st, doc, &path, *lang, false, true);
            } else {
                const std::string& lua = o->script->lua;
                bool exists = false;
                const bool defined = !lua.empty() && interactDefined(st, doc, lua, &exists);
                if (defined) {
                    warn = false;
                    caption = "onInteract(self) in " + fileName(lua);
                    interactLine = "interact: event defined " + lua;
                    if (button("iedit", at, icon::fileCode, "Edit script", ButtonKind::Secondary, nullptr,
                               "Opens the script in the script editor, beside the Scene."))
                        openScript(st, doc, lua);
                } else {
                    interactLine = "interact: event missing " + (lua.empty() ? std::string("(no file)") : lua);
                    if (lua.empty()) {
                        caption = "The Script component has no file, so using it does nothing. Make one with New blocks or New Lua in the Script section.";
                    } else if (!exists) {
                        caption = fileName(lua) + " does not exist, so using it does nothing. Make it with Create script in the Script section.";
                    } else {
                        caption = fileName(lua) + " has no onInteract(self), so using it does nothing.";
                        if (button("iaddevent", at, icon::plus, "Add onInteract", ButtonKind::Secondary, nullptr,
                                   "Adds an empty onInteract(self) to the script and opens it."))
                            addInteractEvent(st, doc, lua);
                    }
                }
            }
            const float tx = x0 + space::sm, w = x1 - space::sm - tx;
            y += space::xs;
            dl->AddText(f.regular, type::caption, ImVec2(tx, y), warn ? color::warn : color::textDim, caption.c_str(), nullptr, w);
            y += f.regular->CalcTextSizeA(type::caption, FLT_MAX, w, caption.c_str()).y + space::sm;
        }
        sectionEnd(top);
    }
    if (o->audio) {
        const splash::AudioComponent& a = *o->audio;
        top = y;
        compSection("s_audio", editor::ComponentKind::Audio);
        bool found = doc.fileExists(a.clip);
        if (slotField(st, static_cast<int>(AssetSlot::Clip), path, "clip", row("lclip", "Clip", "A WAV file, converted to SPU ADPCM at export. Click to pick one."), icon::music,
                       found ? kind::audio : color::warn, a.clip.empty() ? "None" : fileName(a.clip).c_str(), nullptr) |
            fileCard("fixclip", found ? editor::AssetStatus::Ok : editor::AssetStatus::Missing, a.clip, std::string()))
            pickAsset(AssetSlot::Clip, path);
        // The source and what export makes of it: trim, resample, then ADPCM.
        if (const editor::AudioInfo* ai = found ? doc.audio(a.clip) : nullptr; ai && ai->status == editor::AssetStatus::Ok && ai->rate > 0) {
            const size_t bytes = splash::spuAdpcmBytes(splash::resampledCount(size_t(ai->frames), ai->rate, a.sampleRate), a.loop);
            const double secs = double(ai->frames) / double(ai->rate);
            char sec[32];
            std::snprintf(sec, sizeof sec, "%.2f", secs);
            const std::string chans = ai->channels == 1 ? "mono" : ai->channels == 2 ? "stereo" : std::to_string(ai->channels) + " ch";
            // Two lines, the source then the export, so it fits the field's width.
            const std::string src = std::to_string(ai->rate) + " Hz, " + sec + " s, " + chans;
            // Trimming can only shrink the clip, so the size is a bound then.
            const std::string bound = a.trimLeadingSilence ? "at most " : "";
            const std::string out = "-> " + std::to_string(a.sampleRate) + " Hz ADPCM, " + bound + fmtKB(int(bytes));
            const float lineH = 18;
            const ImRect ir = row("lainfo", "Source", "The WAV file as it is, and the sound RAM the exported clip takes. With Trim silence "
                                                    "it can take less: how much depends on the file's quiet start.");
            y += lineH;
            nameRect("obj.ainfo", ImRect(ir.Min, ImVec2(ir.Max.x, ir.Max.y + lineH)));
            const float ty = centerY(f.regular, type::body, ir.Min.y, ir.Max.y);
            text(dl, ImVec2(ir.Min.x, ty), f.regular, type::body, color::textDim, src.c_str());
            text(dl, ImVec2(ir.Min.x, ty + lineH), f.regular, type::body, color::textDim, out.c_str());
            audioLine = "audio: source " + std::to_string(ai->rate) + " " + sec + " " + std::to_string(ai->channels) + " adpcm " + bound +
                        std::to_string(bytes);
        }
        std::string typed;
        const ImRect nr = row("laname", "Name", "What scripts play it by, as in Audio.Play(\"name\").");
        nameRect("obj.aname", nr);
        if (textField("aname", nr, a.clipName, &typed, "No name", 255))
            doc.edit(path, [typed](splash::Object& ob) { ob.audio->clipName = typed; });
        number("arate", "Sample rate", "The clip is resampled to this at export. Lower takes less sound RAM and sounds duller; 22050 is a good middle.",
               float(a.sampleRate), "Hz", "arate", 4000, 44100,
               [](splash::Object& ob, float v) { ob.audio->sampleRate = static_cast<int>(std::lround(v)); });
        toggleRow("aloop", "Loop", "Plays again from the start when it ends, until stopped.", a.loop,
                  [](splash::Object& ob) { ob.audio->loop = !ob.audio->loop; });
        toggleRow("atrim", "Trim silence", "Cuts the quiet part at the start of the file, so it plays the moment it is triggered.",
                  a.trimLeadingSilence, [](splash::Object& ob) { ob.audio->trimLeadingSilence = !ob.audio->trimLeadingSilence; });
        sectionEnd(top);
    }
    if (o->skin) {
        const splash::SkinComponent& sk = *o->skin;
        top = y;
        compSection("s_skin", editor::ComponentKind::Skin);
        std::string clips = std::to_string(sk.clips.size()) + (sk.clips.size() == 1 ? " clip" : " clips");
        if (slotField(st, static_cast<int>(AssetSlot::SkinClips), path, "clips", row("lclips", "Clips", "Animation clips, at most 16. Click to pick the .anim files; the pick replaces the list."),
                       icon::play, kind::camera, clips.c_str(), nullptr))
            pickAsset(AssetSlot::SkinClips, path);
        number("sfps", "Bake rate", "Frames per second the clips are sampled at.", static_cast<float>(sk.fps), "fps", "sfps", 1, 30,
               [](splash::Object& ob, float v) { ob.skin->fps = static_cast<int>(std::lround(v)); });
        // What the last live export baked for this object, matched by name.
        // The height is fixed by the clip list, so nothing below moves when
        // the export lands.
        {
            const std::optional<splash::ExportResult>& lr = st.live.result();
            const splash::ExportStats::SkinInfo* si = nullptr;
            if (lr && lr->ok())
                for (const splash::ExportStats::SkinInfo& k : lr->stats.skins)
                    if (k.object == o->name && k.clips.size() == sk.clips.size()) si = &k;
            const float lineH = 18;
            const size_t lines = sk.clips.size() + 2;
            const ImRect ir = row("lsinfo", "Baked", "What the export makes of the clips at this bake rate: the bones, the triangles, "
                                                    "and the bytes each clip and the whole skin take in the splashpack.");
            y += lineH * float(lines - 1);
            nameRect("obj.skininfo", ImRect(ir.Min, ImVec2(ir.Max.x, ir.Max.y + lineH * float(lines - 1))));
            const float ty = centerY(f.regular, type::body, ir.Min.y, ir.Max.y);
            auto line = [&](size_t i, const std::string& t) {
                text(dl, ImVec2(ir.Min.x, ty + lineH * float(i)), f.regular, type::body, color::textDim, t.c_str());
            };
            if (!si) {
                line(0, "Sizes appear after the next export.");
            } else {
                line(0, "Bones " + std::to_string(si->bones) + ", " + std::to_string(si->triangles) + " triangles");
                skinLine = "skin: " + si->object + " bones " + std::to_string(si->bones) + " clips " +
                           std::to_string(si->clips.size()) + " bytes " + std::to_string(si->bytes);
                for (size_t i = 0; i < si->clips.size(); i++) {
                    const splash::ExportStats::SkinClipInfo& c = si->clips[i];
                    line(i + 1, c.name + "  " + std::to_string(c.frames) + " frames @ " + std::to_string(c.fps) + " fps, " +
                                    fmtKB(int(c.bytes)));
                    skinLine += "\nskin: clip " + c.name + " frames " + std::to_string(c.frames) + " bytes " + std::to_string(c.bytes);
                }
                line(lines - 1, "Skin data " + fmtKB(int(si->bytes)));
                // A clip this long is most of the skin's RAM; usually the rate
                // or the clip's range is the mistake.
                std::string body;
                for (const splash::ExportStats::SkinClipInfo& c : si->clips)
                    if (c.frames > 1000)
                        body += std::string(body.empty() ? "" : " ") + "Clip '" + c.name + "' bakes " + std::to_string(c.frames) +
                                " frames (" + fmtKB(int(c.bytes)) + ").";
                if (!body.empty()) card("skinlong", "Long clip", body + " Lower the bake rate or trim the clip.", nullptr);
            }
        }
        sectionEnd(top);
    }

    for (const editor::ComponentInfo& c : editor::components())
        if (removedFlags[static_cast<int>(c.kind)]) toRemove = c.kind;

    report("meshcol", meshColLine);
    report("materials", materialLines);
    report("uvoffset", uvOffLine);
    report("texholes", texHolesLine);
    report("audio", audioLine);
    report("skin", skinLine);
    report("interact", interactLine);

    // Add component.
    ImRect add(ImVec2(x0, y), ImVec2(x1, y + 34));
    Hit h = interact("addcomp", add);
    nameRect("obj.addcomp", add);
    dl->AddRect(add.Min, add.Max, h.hover > 0.5f ? color::accent : color::borderStrong, radius::card, 0, 1.0f);
    if (h.hover > 0) dl->AddRectFilled(add.Min, add.Max, rgb(0x8b7bff, (int)(28 * h.hover)), radius::card);
    const char* lbl = "Add component";
    float lwid = measure(f.medium, type::body, lbl).x + 22;
    float lx = add.GetCenter().x - lwid * 0.5f;
    text(dl, ImVec2(lx, add.Min.y + 8), f.medium, type::icon, color::textDim, icon::plus);
    text(dl, ImVec2(lx + 22, add.Min.y + 8), f.medium, type::body, color::textDim, lbl);
    if (h.clicked) st.openAddComponent = true;
    ImGui::PopClipRect();
    {
        const float content = add.Max.y + space::xl - (top0 - scroll), view = r.Max.y - top0;
        const float maxScroll = std::max(0.0f, content - view);
        if (maxScroll > 0 && ImGui::IsMouseHoveringRect(r.Min, r.Max) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            scroll -= ImGui::GetIO().MouseWheel * 48.0f;
        scroll = std::clamp(scroll, 0.0f, maxScroll);
        if (maxScroll > 0) {
            const float bh = view * view / content, t = top0 + (view - bh) * (scroll / maxScroll);
            dl->AddRectFilled(ImVec2(r.Max.x - 5, t + 2), ImVec2(r.Max.x - 2, t + bh - 2), rgb(0xffffff, 40), 2);
        }
    }

    // The Add component picker, under the button. Components already on the
    // object are left out; ones it cannot take yet show why.
    const char* pid = "##addcomponent";
    if (st.openAddComponent) {
        st.openAddComponent = false;
        st.pickQuery.clear();
        ImGui::OpenPopup(pid);
    }
    if (ImGui::IsPopupOpen(pid)) {
        std::vector<editor::ComponentKind> kinds;
        std::vector<std::pair<const char*, const char*>> keys;
        for (const editor::ComponentInfo& c : editor::components()) {
            if (editor::hasComponent(*o, c.kind)) continue;
            kinds.push_back(c.kind);
            keys.push_back({c.label, c.keywords});
        }
        std::vector<int> order = editor::rank(st.pickQuery, keys);
        std::vector<std::string> why;
        why.reserve(order.size());
        std::vector<PickerItem> items;
        for (int i : order) {
            const editor::ComponentInfo& c = editor::info(kinds[i]);
            ObjectLook lk = lookOf(c.kind);
            why.push_back(editor::cannotAdd(*o, c.kind));
            items.push_back({lk.icon, lk.color, c.label, c.blurb, why.back().empty() ? nullptr : why.back().c_str()});
        }
        int pick = picker(pid, ImVec2(add.Min.x, add.Max.y + space::xs), add.GetWidth(), "Add component", &st.pickQuery, items);
        if (pick >= 0) {
            editor::ComponentKind k = kinds[order[pick]];
            doc.edit(path, [k](splash::Object& ob) { editor::addComponent(ob, k); });
        }
    }
    if (toRemove) {
        editor::ComponentKind k = *toRemove;
        doc.edit(path, [k](splash::Object& ob) { editor::removeComponent(ob, k); });
    }
}

static std::string kb(size_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%zu", (bytes + 512) / 1024);
    return b;
}

static std::string mb(size_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", double(bytes) / (1024.0 * 1024.0));
    return b;
}

// What a block script's field lists offer from the open scene: object names
// (tree order), audio clip names, cutscene names; each once, none empty.
static BlocksView::Context blockContext(const editor::Document& doc) {
    BlocksView::Context c;
    auto add = [](std::vector<std::string>& v, const std::string& s) {
        if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    };
    std::function<void(const std::vector<splash::Object>&)> walk = [&](const std::vector<splash::Object>& objects) {
        for (const splash::Object& o : objects) {
            add(c.objects, o.name);
            if (o.audio) add(c.sounds, o.audio->clipName);
            walk(o.children);
        }
    };
    walk(doc.scene().objects);
    for (const splash::Cutscene& cs : doc.scene().cutscenes) add(c.cutscenes, cs.name);
    return c;
}

// Scripted runs print `line`; with a `tag`, only when it differs from the
// last line printed under that tag.
static void scriptLine(const char* tag, const std::string& line) {
    if (!namedRectsOn()) return;
    if (tag) {
        static std::map<std::string, std::string> printed;
        std::string& last = printed[tag];
        if (last == line) return;
        last = line;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

// The body of an output popup: a toolbar (filter, Wrap, Copy, Clear and how
// many lines show), then the lines after the clear point that match the
// filter, errors in red, kept at the newest while scrolled to the bottom.
// `who` ("play", "hw") names the controls and the scripted console: lines.
static void outputLog(State& st, const char* who, const std::deque<std::string>& lines, uint64_t total, editor::LogView& v) {
    const std::string w = who;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float rowH = size::field + 6;
    const ImRect fr(o, ImVec2(o.x + 240, o.y + rowH));
    searchField((w + "filter").c_str(), fr, "Filter", nullptr, &v.filter);
    nameRect(w + ".filter", fr);
    std::vector<size_t> shown = editor::visibleLines(lines, total, v);

    float bw = 0;
    ImVec2 bp(fr.Max.x + space::sm, o.y);
    if (button((w + "wrap").c_str(), bp, nullptr, "Wrap", v.wrap ? ButtonKind::Secondary : ButtonKind::Ghost, &bw,
               v.wrap ? "Long lines wrap. Click to scroll them sideways instead." : "Wrap long lines instead of scrolling sideways."))
        v.wrap = !v.wrap;
    nameRect(w + ".wrap", ImRect(bp, bp + ImVec2(bw, rowH)));
    bp.x += bw + space::xs;
    if (button((w + "copy").c_str(), bp, icon::copy, "Copy", ButtonKind::Ghost, &bw, "Copy the lines shown to the clipboard.")) {
        const std::string text = editor::joinLines(lines, shown);
        if (!text.empty() && !SDL_SetClipboardText(text.c_str())) notify(st, std::string("Cannot copy: ") + SDL_GetError(), true);
        scriptLine(nullptr, "console: " + w + " copied " + std::to_string(shown.size()) + " lines");
    }
    nameRect(w + ".copy", ImRect(bp, bp + ImVec2(bw, rowH)));
    bp.x += bw + space::xs;
    if (button((w + "clear").c_str(), bp, icon::trash, "Clear", ButtonKind::Ghost, &bw,
               "Hide the lines printed so far; new ones still show.")) {
        v.clearedAt = total;
        shown = editor::visibleLines(lines, total, v);
        scriptLine(nullptr, "console: " + w + " cleared");
    }
    nameRect(w + ".clear", ImRect(bp, bp + ImVec2(bw, rowH)));

    const uint64_t since = editor::linesSinceClear(total, v);
    const std::string count = std::to_string(shown.size()) + " of " + std::to_string(since) + " lines";
    Fonts& f = fonts();
    const ImVec2 cs = measure(f.regular, type::caption, count.c_str());
    text(ImGui::GetWindowDrawList(), ImVec2(o.x + width - cs.x, o.y + (rowH - cs.y) * 0.5f), f.regular, type::caption,
         color::textFaint, count.c_str());
    scriptLine((w + "shown").c_str(), "console: " + w + " shown " + std::to_string(shown.size()) + " of " + std::to_string(since));

    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rowH + space::sm));
    ImGui::PushFont(f.regular, type::caption);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color::field);
    ImGui::BeginChild((w + "lines").c_str(), ImVec2(0, 360), ImGuiChildFlags_None, v.wrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar);
    if (v.wrap) ImGui::PushTextWrapPos(0.0f);
    for (size_t i : shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, editor::containsNoCase(lines[i], "error") ? color::bad : color::textDim);
        ImGui::TextUnformatted(lines[i].c_str());
        ImGui::PopStyleColor();
    }
    if (v.wrap) ImGui::PopTextWrapPos();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}


// `s` cut to `room` pixels in `font` at `size`, ending in "..." when cut.
static std::string cutTo(ImFont* font, float size, const std::string& s, float room) {
    if (measure(font, size, s.c_str()).x <= room) return s;
    std::string shown = s;
    while (!shown.empty() && measure(font, size, (shown + "...").c_str()).x > room) {
        while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) shown.pop_back();
        if (!shown.empty()) shown.pop_back();  // a whole UTF-8 character
    }
    return shown + "...";
}

// Status bar entry for Play: building, running (click for its output), how
// it ended, or why it did not start.
static float playStatus(ImDrawList* dl, ImRect bar, float x, float maxX, State& st) {
    State::Play& p = st.play;
    Fonts& f = fonts();
    std::string label;
    ImU32 tone = color::textDim, dot = 0;
    if (p.build.valid()) {
        label = "Building for Play";
    } else if (p.emu.running()) {
        label = "pcsx-redux running";
        dot = color::good;
    } else if (!p.message.empty()) {
        label = p.message;
        tone = color::bad;
    } else if (p.emu.exitCode()) {
        int c = *p.emu.exitCode();
        label = c ? "pcsx-redux exited with code " + std::to_string(c) : "pcsx-redux closed";
        tone = c ? color::bad : color::textFaint;
    } else {
        return x;
    }
    float pad = dot ? 22 : 10;
    const std::string whole = label;
    label = cutTo(f.medium, type::caption, label, std::max(60.0f, maxX - x - pad - 10));
    ImVec2 ls = measure(f.medium, type::caption, label.c_str());
    ImRect r(ImVec2(x, bar.Min.y + 5), ImVec2(x + pad + ls.x + 10, bar.Max.y - 5));
    const bool hasOutput = !p.emu.output().empty();
    Hit h = interact("playstatus", r);
    nameRect("playstatus", r);
    if (label != whole) tooltip(whole.c_str());
    scriptLine("playstatus", "playstatus: " + whole);
    if (hasOutput) {
        dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover), radius::pill);
        if (h.clicked) ImGui::OpenPopup("##playoutput");
    }
    if (dot) dl->AddCircleFilled(ImVec2(r.Min.x + 12, r.GetCenter().y), 3.5f, dot, 12);
    text(dl, ImVec2(r.Min.x + pad, bar.Min.y + (size::statusBar - ls.y) * 0.5f), f.medium, type::caption, tone, label.c_str());

    ImGui::SetNextWindowPos(ImVec2(r.Min.x, r.Min.y - space::xs), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(720, 0));
    pushPopupStyle();
    const bool open = ImGui::BeginPopup("##playoutput", ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (!open) return r.Max.x + space::md;
    outputLog(st, "play", p.emu.output(), p.emu.lineCount(), p.playLog);
    if (p.emu.running()) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        if (button("stopplay", c, icon::square, "Stop", ButtonKind::Secondary)) p.emu.stop();
        ImGui::Dummy(ImVec2(1, size::field + 6));
    }
    ImGui::EndPopup();
    return r.Max.x + space::md;
}

// Status bar entry for Run on hardware: building, uploading, running (click
// for what the console printed and the files it asked for), or why it stopped.
static float hardwareStatus(ImDrawList* dl, ImRect bar, float x, float maxX, State& st) {
    State::Play& p = st.play;
    Fonts& f = fonts();
    using Phase = editor::HardwareRun::Phase;
    std::string label;
    ImU32 tone = color::textDim, dot = 0;
    const Phase ph = p.hw.phase();
    if (p.hwBuild.valid()) {
        label = "Building for the console";
    } else if (ph == Phase::Connecting) {
        label = "Connecting to " + p.hwPort;
    } else if (ph == Phase::Uploading) {
        label = "Uploading " + std::to_string(p.hw.progress()) + "%";
        dot = color::warn;
    } else if (ph == Phase::Running) {
        label = "Running on " + p.hwPort;
        dot = color::good;
    } else if (ph == Phase::Failed) {
        label = p.hw.error();
        tone = color::bad;
    } else if (ph == Phase::Stopped) {
        label = "Hardware run stopped";
        tone = color::textFaint;
    } else {
        return x;
    }
    float pad = dot ? 22 : 10;
    const std::string whole = label;
    label = cutTo(f.medium, type::caption, label, std::max(60.0f, maxX - x - pad - 10));
    ImVec2 ls = measure(f.medium, type::caption, label.c_str());
    ImRect r(ImVec2(x, bar.Min.y + 5), ImVec2(x + pad + ls.x + 10, bar.Max.y - 5));
    Hit h = interact("hwstatus", r);
    nameRect("hwstatus", r);
    if (label != whole) tooltip(whole.c_str());
    const bool hasOutput = p.hw.lineCount() > 0;
    if (hasOutput) {
        dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover), radius::pill);
        if (h.clicked) ImGui::OpenPopup("##hwoutput");
    }
    if (dot) dl->AddCircleFilled(ImVec2(r.Min.x + 12, r.GetCenter().y), 3.5f, dot, 12);
    text(dl, ImVec2(r.Min.x + pad, bar.Min.y + (size::statusBar - ls.y) * 0.5f), f.medium, type::caption, tone, label.c_str());

    ImGui::SetNextWindowPos(ImVec2(r.Min.x, r.Min.y - space::xs), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(720, 0));
    pushPopupStyle();
    const bool open = ImGui::BeginPopup("##hwoutput", ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (!open) return r.Max.x + space::md;
    uint64_t total = 0;
    const std::deque<std::string> lines = p.hw.lines(&total);
    outputLog(st, "hw", lines, total, p.hwLog);
    if (p.hw.active()) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        if (button("stophw", c, icon::square, "Stop", ButtonKind::Secondary)) p.hw.stop();
        ImGui::Dummy(ImVec2(1, size::field + 6));
    }
    ImGui::EndPopup();
    return r.Max.x + space::md;
}

// Status bar entry while a model imports: a spinner, which file (and
// "with Blender" while Blender converts a .blend), the seconds so far (wall
// clock: the import runs in real time), how many more wait, a bar with the
// share done while the build knows it (not while Blender converts), and
// Cancel, which stops it with nothing imported.
static float importStatus(ImDrawList* dl, ImRect bar, float x, State& st) {
    st.importCancel = ImRect();
    st.importProgress = -1;
    const editor::ModelImports::Request* rq = st.imports.current();
    if (!rq) return x;
    Fonts& f = fonts();
    const double secs = st.imports.seconds();
    const bool blender = st.imports.stage() == editor::ModelImports::Stage::Blender;
    std::string label = (rq->reimport.empty() ? "Importing " : "Reimporting ") +
                        utf8((rq->reimport.empty() ? rq->file : rq->reimport).filename()) + (blender ? " with Blender... " : "... ") +
                        std::to_string(static_cast<int>(secs)) + " s";
    if (st.imports.size() > 1) label += " (" + std::to_string(st.imports.size() - 1) + " more waiting)";
    const ImVec2 ls = measure(f.medium, type::caption, label.c_str());
    const float cy = bar.GetCenter().y;
    const float a0 = static_cast<float>(std::fmod(secs, 1.0) * 2 * IM_PI);
    dl->PathArcTo(ImVec2(x + 10, cy), 5.5f, a0, a0 + IM_PI * 1.5f, 18);
    dl->PathStroke(color::accent, ImDrawFlags_None, 2.0f);
    text(dl, ImVec2(x + 24, bar.Min.y + (size::statusBar - ls.y) * 0.5f), f.medium, type::caption, color::text, label.c_str());
    float px = x + 24 + ls.x + space::sm;
    st.importProgress = st.imports.fraction();
    if (st.importProgress >= 0) {
        const float done = std::clamp(st.importProgress, 0.0f, 1.0f);
        const ImRect track(ImVec2(px, cy - 3), ImVec2(px + 64, cy + 3));
        dl->AddRectFilled(track.Min, track.Max, color::borderStrong, radius::pill);
        if (done > 0) dl->AddRectFilled(track.Min, ImVec2(track.Min.x + track.GetWidth() * done, track.Max.y), color::accent, radius::pill);
        const std::string pct = std::to_string(static_cast<int>(done * 100)) + "%";
        const ImVec2 ps = measure(f.medium, type::caption, pct.c_str());
        text(dl, ImVec2(track.Max.x + space::sm, bar.Min.y + (size::statusBar - ps.y) * 0.5f), f.medium, type::caption, color::textDim,
             pct.c_str());
        px = track.Max.x + space::sm + ps.x + space::sm;
    }
    const char* cl = st.imports.finishing() ? "Finishing" : "Cancel";
    const ImVec2 cs = measure(f.medium, type::caption, cl);
    const float cx = px;
    const ImRect r(ImVec2(cx, bar.Min.y + 5), ImVec2(cx + cs.x + 20, bar.Max.y - 5));
    const Hit h = interact("importcancel", r);
    dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover), radius::pill);
    text(dl, ImVec2(r.Min.x + 10, bar.Min.y + (size::statusBar - cs.y) * 0.5f), f.medium, type::caption, color::text, cl);
    if (h.hover)
        tooltip(blender ? "Stop Blender. Nothing is imported and nothing is added to the project."
                        : "Stop the import. Nothing is imported and nothing is added to the project.");
    if (h.clicked) st.imports.cancel();
    st.importCancel = r;
    return r.Max.x + space::xl;
}

static void statusBar(ImDrawList* dl, ImVec2 size, const editor::Document& doc, State& st) {
    Fonts& f = fonts();
    ImRect bar(ImVec2(0, size.y - size::statusBar), size);
    dl->AddRectFilled(bar.Min, bar.Max, color::chrome);
    float x = space::md;
    const std::optional<splash::ExportResult>& res = st.live.result();
    // A failed export keeps the last good one's meters up, marked as such.
    const splash::ExportResult* shownRes = res && res->ok() ? &*res : st.live.lastGood() ? &*st.live.lastGood() : nullptr;
    if (shownRes) {
        const splash::ExportStats& s = shownRes->stats;
        splash::Budget v = splash::vramBudget(s), a = splash::spuBudget(s), r = splash::ramBudget(s),
                       t = splash::triangleBudget(s);
        std::string vs = kb(v.used) + " / " + kb(v.capacity) + " KB";
        std::string as = kb(a.used) + " / " + kb(a.capacity) + " KB";
        std::string rs = mb(r.used) + " / " + mb(r.capacity) + " MB";
        std::string ts = std::to_string(s.triangles);
        if (s.trianglesInView != uint64_t(s.triangles)) ts += ", up to " + std::to_string(s.trianglesInView) + " in view";
        x += meter("m_vram", ImVec2(x, bar.Min.y), icon::grid, "VRAM", v.fraction(), vs.c_str(),
                   "Framebuffers, texture atlases, palettes and fonts, as the export packs them.") +
             space::xl;
        x += meter("m_spu", ImVec2(x, bar.Min.y), icon::music, "SPU RAM", a.fraction(), as.c_str(),
                   "Sound samples, placed the way psxsplash uploads them.") +
             space::xl;
        x += meter("m_ram", ImVec2(x, bar.Min.y), icon::hardDrive, "RAM", r.fraction(), rs.c_str(),
                   "Scene data, the renderer's buffers, its visible-triangle list, the world streaming slots and what "
                   "psxsplash and Lua allocate for each object, in psxsplash's heap at the peak of loading.") +
             space::xl;
        x += meter("m_tris", ImVec2(x, bar.Min.y), icon::box, "Triangles", t.fraction(), ts.c_str(),
                   "Triangles in the exported meshes, and the most that can be in view at once (what the meter "
                   "measures). The psxsplash docs suggest 1000 to 2000 visible triangles per frame at 30 fps.") +
             space::xl;
        auto meterLine = [](const char* name, const splash::Budget& m) {
            scriptLine((std::string("statusbar.") + name).c_str(),
                       std::string("statusbar: ") + name + " " + std::to_string(m.used) + "/" + std::to_string(m.capacity) +
                           " " + splash::budgetBand(m.fraction()));
        };
        meterLine("vram", v);
        meterLine("spu", a);
        meterLine("ram", r);
        meterLine("tris", t);
        if (!s.rooms.empty()) {
            char rs2[64];
            std::snprintf(rs2, sizeof rs2, "%d, catch-all %d tris", int(s.rooms.size()), s.roomCatchAllTris);
            x += meter("m_rooms", ImVec2(x, bar.Min.y), icon::layoutTemplate, "Rooms", -1, rs2,
                       "Rooms the export built, and the triangles no Room box holds. Those go to the catch-all room, "
                       "which psxsplash draws every frame wherever the camera is.") +
                 space::xl;
        }
    }
    if (!res || !res->ok()) {
        std::string msg = !res ? "Measuring..." : "Export failed: see problems";
        if (res && shownRes) msg += " (meters from the last good export)";
        ImVec2 ms = measure(f.regular, type::caption, msg.c_str());
        text(dl, ImVec2(x, bar.Min.y + (size::statusBar - ms.y) * 0.5f), f.regular, type::caption,
             res ? color::bad : color::textFaint, msg.c_str());
        x += ms.x + space::xl;
    }
    if (res)
        scriptLine("statusbar.export", res->ok()  ? "statusbar: export ok"
                                       : shownRes ? "statusbar: export failed, meters from last good"
                                                  : "statusbar: export failed, no meters");
    // Right side: problems and save state. Play's and Run on hardware's
    // entries are cut to stop short of it (a failed build's first error can be
    // long), and shown whole on hover.
    const bool showNotice = st.saveError.empty() && !st.notice.empty() && ImGui::GetTime() < st.noticeUntil;
    const char* saved = !st.saveError.empty() ? st.saveError.c_str()
                        : showNotice          ? st.notice.c_str()
                        : doc.dirty()         ? "Unsaved changes"
                                              : "All changes saved";
    // A notice about a written file leads with a link that shows it in its folder.
    const bool reveal = showNotice && !st.noticeReveal.empty();
    const char* revealLabel = "Show in folder";
    const float revealW = reveal ? measure(f.medium, type::caption, revealLabel).x + 20 + space::sm : 0.0f;
    const float leftMax = size.x - space::md - (res ? 200.0f : 0.0f) - space::xl - revealW -
                          std::min(measure(f.regular, type::caption, saved).x, 240.0f);
    x = importStatus(dl, bar, x, st);
    x = playStatus(dl, bar, x, leftMax, st);
    x = hardwareStatus(dl, bar, x, leftMax, st);
    // A long message (an import error with its fix) is cut to the room left
    // of it, beside the problems pill, and shown whole on hover.
    std::string shown = saved;
    ImVec2 s = measure(f.regular, type::caption, shown.c_str());
    const float room = size.x - space::md - x - (res ? 200.0f : 0.0f) - revealW;
    const bool cut = s.x > room && room > 0;
    if (cut) {
        while (!shown.empty() && measure(f.regular, type::caption, (shown + "...").c_str()).x > room) {
            while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) shown.pop_back();
            if (!shown.empty()) shown.pop_back();  // a whole UTF-8 character
        }
        shown += "...";
        s = measure(f.regular, type::caption, shown.c_str());
    }
    float rx = size.x - space::md - s.x;
    text(dl, ImVec2(rx, bar.Min.y + (size::statusBar - s.y) * 0.5f), f.regular, type::caption,
         !st.saveError.empty() || (showNotice && st.noticeBad) ? color::bad : showNotice ? color::text : color::textFaint,
         shown.c_str());
    if (cut) {
        interact("notice", ImRect(ImVec2(rx, bar.Min.y), bar.Max));
        tooltip(saved);
    }
    if (reveal) {
        ImVec2 ls = measure(f.medium, type::caption, revealLabel);
        ImRect lr(ImVec2(rx - revealW, bar.Min.y + 5), ImVec2(rx - space::sm, bar.Max.y - 5));
        Hit lh = interact("notice.reveal", lr);
        nameRect("notice.reveal", lr);
        tooltip(("Open the folder holding " + utf8(st.noticeReveal.filename())).c_str());
        dl->AddRectFilled(lr.Min, lr.Max, lerpColor(color::raised, color::hover, lh.hover), radius::pill);
        text(dl, ImVec2(lr.Min.x + 10, bar.Min.y + (size::statusBar - ls.y) * 0.5f), f.medium, type::caption, color::accent,
             revealLabel);
        if (lh.clicked) revealInFolder(st.noticeReveal);
        rx = lr.Min.x;
    }
    if (!res) return;
    const editor::BuildReport& built = st.play.built;
    const size_t nErr = res->errors.size() + built.errors.size(), nWarn = res->warnings.size() + built.warnings.size();
    std::string prob = nErr ? std::to_string(nErr) + (nErr == 1 ? " error" : " errors")
                       : nWarn ? std::to_string(nWarn) + (nWarn == 1 ? " warning" : " warnings")
                               : "No problems";
    if (nErr && nWarn) prob += ", " + std::to_string(nWarn) + (nWarn == 1 ? " warning" : " warnings");
    scriptLine("problems", "problems: " + prob);
    ImVec2 ps = measure(f.medium, type::caption, prob.c_str());
    ImRect pr(ImVec2(rx - space::xl - ps.x - 22, bar.Min.y + 5), ImVec2(rx - space::xl + 8, bar.Max.y - 5));
    const bool any = nErr || nWarn;
    const ImU32 tone = nErr ? color::bad : nWarn ? color::warn : color::textFaint;
    Hit h = interact("problems", pr);
    nameRect("problems", pr);
    if (any) {
        dl->AddRectFilled(pr.Min, pr.Max, nErr ? rgb(0xef5a6f, (int)(30 + 30 * h.hover)) : rgb(0xf0a43a, (int)(30 + 30 * h.hover)),
                          radius::pill);
        text(dl, ImVec2(pr.Min.x + 8, bar.Min.y + 7), f.medium, type::label, tone, icon::warning);
        if (h.clicked) ImGui::OpenPopup("##problems");
    }
    text(dl, ImVec2(pr.Min.x + 26, bar.Min.y + (size::statusBar - ps.y) * 0.5f), f.medium, type::caption, tone, prob.c_str());

    ImGui::SetNextWindowPos(ImVec2(pr.Max.x, pr.Min.y - space::xs), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 0), ImVec2(560, size.y * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::md, space::md));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(space::sm, space::sm));
    const bool problemsOpen = ImGui::BeginPopup("##problems", ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    if (problemsOpen) {
        ImGui::PushFont(f.regular, type::caption);
        ImGui::PushTextWrapPos(540);
        for (const std::string& e : res->errors) {
            ImGui::PushStyleColor(ImGuiCol_Text, color::bad);
            ImGui::TextUnformatted(e.c_str());
            ImGui::PopStyleColor();
        }
        for (const std::string& w : res->warnings) {
            ImGui::PushStyleColor(ImGuiCol_Text, color::warn);
            ImGui::TextUnformatted(w.c_str());
            ImGui::PopStyleColor();
        }
        if (!built.empty()) {
            // The other scenes of the last build over the build list (and its own errors, like a file that does not load).
            if (!res->errors.empty() || !res->warnings.empty()) ImGui::Separator();
            const std::string head = "Last " + built.what + " build, " + std::to_string(built.scenes) +
                                     (built.scenes == 1 ? " scene" : " scenes");
            ImGui::PushFont(f.medium, type::caption);
            ImGui::PushStyleColor(ImGuiCol_Text, color::textDim);
            ImGui::TextUnformatted(head.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            nameRect("problems.build", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            for (const std::string& e : built.errors) {
                ImGui::PushStyleColor(ImGuiCol_Text, color::bad);
                ImGui::TextUnformatted(e.c_str());
                ImGui::PopStyleColor();
            }
            for (const std::string& w : built.warnings) {
                ImGui::PushStyleColor(ImGuiCol_Text, color::warn);
                ImGui::TextUnformatted(w.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndPopup();
    }
}

// Document-wide shortcuts. Skipped while a text field has the keyboard.
static void shortcuts(State& st, editor::Document& doc) {
    ImGuiIO& io = ImGui::GetIO();
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    auto repeat = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    // The script editor keeps the keyboard, but Play and Run on hardware still answer.
    // Not gated on WantTextInput: the editor asks for text input itself, so it is set.
    if (st.scripts.hasKeyboard()) {
        if (st.unsavedShown || st.menuOpen >= 0) return;
        if (!shift && pressed(ImGuiKey_F5)) ctrl ? toggleRun(st, doc) : togglePlay(st, doc);
        return;
    }
    if (io.WantTextInput || st.unsavedShown || st.menuOpen >= 0) return;
    // While the game has the viewport its keys are the controller; only Play/Stop stays.
    if (st.play.gameFocus && st.play.game.attached()) {
        if (!ctrl && pressed(ImGuiKey_F5)) togglePlay(st, doc);
        return;
    }
    if (ctrl && !shift && repeat(ImGuiKey_Z)) doc.undo();
    if (ctrl && ((shift && repeat(ImGuiKey_Z)) || repeat(ImGuiKey_Y))) doc.redo();
    // While the right button is held in the viewport, QWE fly the camera.
    if (!ctrl && !shift && !io.KeyAlt && st.vpButton != 1) {
        if (pressed(ImGuiKey_Q)) st.tool = 0;
        if (pressed(ImGuiKey_W)) st.tool = 1;
        if (pressed(ImGuiKey_E)) st.tool = 2;
        if (pressed(ImGuiKey_R)) st.tool = 3;
    }
    // Object commands on the selection; the UI tab takes these keys for its
    // elements, and while the Project panel has the keyboard, Delete and F2 are its own.
    const bool uiTab = st.scripts.uiShown();
    const bool assetKeys = st.assets.focused() && !timelineInFront(st);
    if (uiTab && !assetKeys && !ctrl && pressed(ImGuiKey_F2)) st.canvasView.rename(doc);
    if (const std::optional<editor::ObjectPath> sel = (uiTab || assetKeys) ? std::nullopt : doc.selection()) {
        if (!ctrl && pressed(ImGuiKey_Delete)) doc.removeObject(*sel);
        if (ctrl && !shift && pressed(ImGuiKey_D)) doc.duplicateObject(*sel);
        if (ctrl && !shift && pressed(ImGuiKey_C)) copySelection(st, doc, *sel);
        if (ctrl && !shift && pressed(ImGuiKey_X)) cutSelection(st, doc, *sel);
        if (!ctrl && pressed(ImGuiKey_F2)) {
            st.renaming = true;
            st.renameStart = true;
            st.renamePath = *sel;
        }
    }
    if (ctrl && !shift && !uiTab && pressed(ImGuiKey_V)) pasteClipboard(st, doc, doc.selection());
    if (ctrl && !shift && pressed(ImGuiKey_A)) st.openAddObject = true;
    if (ctrl && !shift && pressed(ImGuiKey_F)) st.focusTreeFilter = true;
    if (!ctrl && pressed(ImGuiKey_F5)) togglePlay(st, doc);
    if (ctrl && !shift && pressed(ImGuiKey_F5)) toggleRun(st, doc);
    if (ctrl && !shift && pressed(ImGuiKey_S)) saveDoc(st, doc);
    if (ctrl && !shift && pressed(ImGuiKey_B)) startExport(st, doc);
    if (ctrl && shift && pressed(ImGuiKey_S) && !doc.projectRoot().empty()) saveDocAs(doc);
    if (ctrl && !shift && pressed(ImGuiKey_N)) request(st, doc, State::Pending::New);
    if (ctrl && !shift && pressed(ImGuiKey_O)) request(st, doc, State::Pending::Open);
    if (ctrl && !shift && pressed(ImGuiKey_Q)) request(st, doc, State::Pending::Quit);
}

// Updates the scene for `model` imported again on the worker thread (see editor::applyReimport).
static void reimportNow(State& st, editor::Document& doc, const std::filesystem::path& model, std::optional<bool> wasMerged,
                        editor::StagedModel& staged) {
    editor::Reimported re = editor::applyReimport(doc, staged, wasMerged);
    if (!re.error.empty()) {
        notify(st, "Reimport failed: " + re.error, true);
        return;
    }
    std::string msg = "Reimported " + utf8(model.filename());
    msg += importNote(re.model.warnings);
    notify(st, msg, false);
    st.importPanel.checked = -1;
}

// Imports models again: the ones the inspector's Import settings changed last
// frame, and, about once a second, the ones whose file changed on disk. They
// join the import queue (see updateImports); one already waiting there is
// not queued twice.
static void updateModels(State& st, editor::Document& doc) {
    std::vector<State::Reimport> todo;
    todo.swap(st.reimports);
    const double now = ImGui::GetTime();
    if (st.watchedAt < 0 || now - st.watchedAt >= 1.0) {
        st.watchedAt = now;
        for (const std::filesystem::path& m : st.watcher.poll(doc.projectRoot())) todo.push_back({m, std::nullopt});
    }
    for (const State::Reimport& r : todo) {
        editor::ModelImports::Request q;
        q.reimport = r.model;
        q.wasMerged = r.wasMerged;
        q.loadId = doc.loadId();
        st.imports.add(std::move(q));
    }
}

// Runs the import queue (editor::ModelImports): Blender for a .blend, then
// the file read and baked on a worker thread, and, once it is built, the
// model placed or the scene updated here, on the UI thread. Imports asked
// for in a scene that is no longer open are dropped.
static void updateImports(State& st, editor::Document& doc, const viewport::Ps1View& view) {
    for (editor::ModelImports::Outcome& o : st.imports.update(doc)) {
        const editor::ModelImports::Request& rq = o.request;
        const bool re = !rq.reimport.empty();
        const std::string name = utf8((re ? rq.reimport : rq.file).filename());
        switch (o.kind) {
        case editor::ModelImports::Outcome::Dropped:
            notify(st, "Import of " + name + " dropped: another scene was opened", false);
            break;
        case editor::ModelImports::Outcome::Cancelled:
            notify(st, (re ? "Reimport of " : "Import of ") + name + " cancelled", false);
            break;
        case editor::ModelImports::Outcome::Failed:
            notify(st, (re ? "Reimport failed: " : "Import failed: ") + o.error, true);
            break;
        case editor::ModelImports::Outcome::Ready:
            if (re) reimportNow(st, doc, rq.reimport, rq.wasMerged, o.staged);
            else placeModelFile(st, doc, view, rq.file, rq.to, o.staged);
            break;
        }
    }
}

ImRect drawMainScreen(State& st, editor::Document& doc, viewport::Ps1View& view, ImVec2 size) {
    st.importPanel.shown.clear();
    filesPending(st, doc);
    // Whatever opened the timeline (the View menu, Edit in timeline) brings
    // its tab to the front; closing it shows the Project tab again.
    if (st.timeline.open && (!st.timelineWasOpen || st.timeline.shows != st.timelineShows)) st.bottomTab = State::BottomTab::Timeline;
    if (!st.timeline.open) st.bottomTab = State::BottomTab::Project;
    st.timelineWasOpen = st.timeline.open;
    st.timelineShows = st.timeline.shows;
    // The timeline takes Delete and Space while it has focus, before the
    // scene's shortcuts see them; only while its tab is in front.
    if (timelineInFront(st)) st.timeline.takeKeys(doc);
    shortcuts(st, doc);
    st.live.update(doc, ImGui::GetTime());
    updatePlay(st, doc);
    updateExport(st);
    updateModels(st, doc);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), size, color::base);

    ImRect bar = titleBar(st, dl, size, doc);
    float top = size::titleBar + size::gutter, bottom = size.y - size::statusBar - size::gutter;
    float g = size::gutter;
    ImRect left(ImVec2(g, top), ImVec2(g + 272, bottom));
    ImRect right(ImVec2(size.x - g - 352, top), ImVec2(size.x - g, bottom));
    ImRect mid(ImVec2(left.Max.x + g, top), ImVec2(right.Min.x - g, bottom));
    sceneTree(st, dl, left, doc, view);
    // Open scripts belong to the project; another project closes them.
    if (st.scriptsProject != doc.projectRoot()) {
        st.scripts.closeAll();
        st.scriptsProject = doc.projectRoot();
    }
    // A sequence rename rewrote the scripts that play it by name on disk; a
    // tab with unsaved changes gets the same rewrite, so saving it keeps it.
    if (auto r = st.timeline.takeRenamed()) {
        std::vector<std::filesystem::path> files;
        for (const std::string& p : editor::sceneScripts(doc)) files.push_back(doc.resolve(p));
        const int tabs = st.scripts.rewriteUnsaved(files, [&](const std::string& text) {
            return editor::renameSeqInLua(text, r->kind, r->from, r->to);
        }) + st.scripts.rewriteUnsavedBlocks(files, [&](const std::string& text) {
            return editor::renameSeqInBlocks(text, r->kind, r->from, r->to);
        });
        notify(st, r->summary + (tabs ? "; also in " + std::to_string(tabs) + " unsaved script tab" + (tabs == 1 ? "" : "s") : ""),
               false);
    }
    // Rewritten on disk (the rename, its undo or redo): clean tabs reload.
    if (st.scriptsAssets != doc.assetsId()) {
        st.scripts.reloadFromDisk();
        st.scriptsAssets = doc.assetsId();
    }
    // The bottom dock: the tab in front takes its height from the bottom of
    // the middle panel; the other is not drawn until it is picked.
    const bool tl = timelineInFront(st);
    const float tabsW = dockTabsWidth(st);
    auto tabs = [&st](ImDrawList* d, ImRect r) { dockTabs(st, d, r); };
    st.timeline.tabs = tabs;
    st.timeline.tabsWidth = tabsW;
    st.assets.tabs = tabs;
    st.assets.tabsWidth = tabsW;
    const ImRect body = st.scripts.tabStrip(dl, tl ? st.timeline.dock(mid) : st.assets.dock(mid));
    // The timeline takes this frame's scrub first, so the Scene draws the
    // pose at the new scrub head now rather than a frame late (empty when
    // closed or behind the Project tab).
    if (tl) st.timeline.draw(dl, doc);
    view.setPreview(tl ? st.timeline.preview(doc) : editor::Preview{});
    if (st.scripts.sceneShown()) {
        viewportPanel(st, dl, body, doc, view);
        st.viewportRect = body;
    } else if (st.scripts.uiShown()) {
        st.canvasView.draw(dl, body, doc);
        if (!st.canvasView.status.empty()) {
            notify(st, st.canvasView.status, st.canvasView.statusBad);
            st.canvasView.status.clear();
        }
        st.viewportRect = ImRect();
    } else {
        st.scripts.setBlockContext(blockContext(doc));
        st.scripts.draw(dl, body);
        st.viewportRect = ImRect();
        if (st.scripts.hasKeyboard()) st.play.gameFocus = false;
    }
    if (!st.scripts.notice.empty()) {
        notify(st, st.scripts.notice, st.scripts.noticeBad);
        st.scripts.notice.clear();
    }
    // A saved script may be one the scene was missing; the live game reloads it.
    std::vector<std::string> savedScripts;
    for (const std::filesystem::path& p : st.scripts.takeSaved()) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, doc.projectRoot(), ec);
        if (!ec && !rel.empty()) savedScripts.push_back(utf8(rel.generic_u8string()));
    }
    if (!savedScripts.empty()) doc.refreshAssets(savedScripts);
    if (!tl) st.assets.draw(dl, doc);
    // Project files renamed, moved or deleted (or that undone): open script
    // tabs follow their file; a move an undo or redo could not make is said.
    // A blocks tab's menu asked for its script as Lua.
    if (std::optional<std::filesystem::path> conv = st.scripts.takeConvertRequest()) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(*conv, doc.projectRoot(), ec);
        const std::string from = ec ? utf8(conv->generic_u8string()) : utf8(rel.generic_u8string());
        const editor::AssetOp op = st.assets.convertBlocks(doc, from);
        if (op.error.empty()) scriptLine(nullptr, "blocks: converted " + from + " -> " + op.path);
        else scriptLine(nullptr, "blocks: convert refused: " + op.error);
    }
    for (const auto& [from, to] : editor::takeAssetMoves()) st.scripts.moved(from, to);
    if (std::string why = editor::takeAssetHistoryError(); !why.empty()) notify(st, why, true);
    st.modelField = ImRect();
    st.assetSlots.clear();
    inspector(st, dl, right, doc);
    assetDragHint(st, dl, doc);
    if (st.drop.hovering && !st.modelField.Contains(st.drop.pos)) {
        // Anywhere but the Model field, a dropped model becomes a new object:
        // on a scene tree row, that object's child.
        std::string under;
        if (std::optional<editor::ObjectPath> rowPath = treeRowAt(st, st.drop.pos))
            for (const auto& [rect, path] : st.treeRows)
                if (path == *rowPath && doc.object(path)) {
                    dl->AddRect(rect.Min, rect.Max, color::accent, radius::card, 0, 2.0f);
                    under = doc.object(path)->name;
                }
        if (under.empty()) dl->AddRect(mid.Min, mid.Max, color::accent, radius::card, 0, 2.0f);
        Fonts& f = fonts();
        const std::string hintText = under.empty() ? std::string("Drop to add the model to the scene") : "Drop to add the model under " + under;
        const char* hint = hintText.c_str();
        ImVec2 hs = measure(f.medium, type::body, hint);
        ImRect pill(ImVec2((mid.Min.x + mid.Max.x - hs.x) * 0.5f - space::md, mid.Max.y - 56),
                    ImVec2((mid.Min.x + mid.Max.x + hs.x) * 0.5f + space::md, mid.Max.y - 24));
        dl->AddRectFilled(pill.Min, pill.Max, color::raised, radius::pill);
        dl->AddRect(pill.Min, pill.Max, color::accent, radius::pill);
        textCentered(dl, pill, f.medium, type::body, color::text, hint);
    }
    importPending(st, doc, view);
    updateImports(st, doc, view);
    assetRequests(st, doc, view);
    statusBar(dl, size, doc, st);
    playSetup(st, doc, size);
    menus(st, doc, view);
    if (st.quitRequested) {
        st.quitRequested = false;
        request(st, doc, State::Pending::Quit);
    }
    unsavedPrompt(st, doc, size);
    ImGui::End();
    return bar;
}

}  // namespace mockup
