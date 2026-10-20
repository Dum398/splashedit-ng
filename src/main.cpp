#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <optional>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// The implementation comes from splashcore (core/stb_impl.cpp).
#include <stb_image_write.h>

#include "editor/document.hh"
#include "editor/texture_preview.hh"
#include "gl.h"
#include "mockup/main_screen.h"
#include "ui/brand.h"
#include "ui/file_dialog.h"
#include "ui/splash.h"
#include "ui/theme.h"
#include "ui/ui_scale.hh"
#include "ui/widgets.h"
#include "ui/window_chrome.hh"
#include "viewport/ps1view.h"

namespace {

struct HitTestState {
    ui::ChromeRect titleBar;
    std::vector<ui::ChromeRect> controls;  // title bar rectangles that take clicks
    float scale = 1;                       // the UI scale they were drawn at
};
HitTestState g_hit;

// UI coordinates per window coordinate, this frame (ui_scale.hh).
float g_uiScale = 1;
// Mouse positions queued from this event id on are still in window coordinates.
ImU32 g_mouseUnscaledFrom = 0;

// Brings the mouse positions queued since the last call (SDL events, the
// backend's global mouse and scripted input alike) from window to UI
// coordinates. Called just before ImGui::NewFrame.
void scaleQueuedMouse(float c) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiInputEvent& e : g.InputEventsQueue) {
        if (e.EventId < g_mouseUnscaledFrom || e.Type != ImGuiInputEventType_MousePos) continue;
        if (e.MousePos.PosX > -FLT_MAX) e.MousePos.PosX = ui::toUi(e.MousePos.PosX, c);
        if (e.MousePos.PosY > -FLT_MAX) e.MousePos.PosY = ui::toUi(e.MousePos.PosY, c);
    }
    g_mouseUnscaledFrom = g.InputEventsNextEventId;
}

// The backend's IME hook, handed the text cursor in window coordinates.
void (*g_backendSetImeData)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) = nullptr;
void setImeDataScaled(ImGuiContext* ctx, ImGuiViewport* vp, ImGuiPlatformImeData* data) {
    ImGuiPlatformImeData d = *data;
    d.InputPos = ImVec2(ui::toWindow(d.InputPos.x, g_uiScale), ui::toWindow(d.InputPos.y, g_uiScale));
    d.InputLineHeight = ui::toWindow(d.InputLineHeight, g_uiScale);
    if (g_backendSetImeData) g_backendSetImeData(ctx, vp, &d);
}

ui::ChromeRect chromeRect(const ImRect& r) { return {r.Min.x, r.Min.y, r.Max.x, r.Max.y}; }

SDL_HitTestResult hitTest(SDL_Window* win, const SDL_Point* p, void*) {
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    const bool maximized = (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) != 0;
    switch (ui::scaledChromeHitTest(w, h, maximized, p->x, p->y, g_hit.scale, g_hit.titleBar, g_hit.controls)) {
        case ui::ChromeHit::Normal: return SDL_HITTEST_NORMAL;
        case ui::ChromeHit::Draggable: return SDL_HITTEST_DRAGGABLE;
        case ui::ChromeHit::ResizeTopLeft: return SDL_HITTEST_RESIZE_TOPLEFT;
        case ui::ChromeHit::ResizeTop: return SDL_HITTEST_RESIZE_TOP;
        case ui::ChromeHit::ResizeTopRight: return SDL_HITTEST_RESIZE_TOPRIGHT;
        case ui::ChromeHit::ResizeRight: return SDL_HITTEST_RESIZE_RIGHT;
        case ui::ChromeHit::ResizeBottomRight: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        case ui::ChromeHit::ResizeBottom: return SDL_HITTEST_RESIZE_BOTTOM;
        case ui::ChromeHit::ResizeBottomLeft: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        case ui::ChromeHit::ResizeLeft: return SDL_HITTEST_RESIZE_LEFT;
    }
    return SDL_HITTEST_NORMAL;
}

// Scripted input for screenshot mode, so interactions can be checked headless.
// Actions run one after another from frame 5, in command-line order.
struct Action {
    enum Kind { Click, DoubleClick, Drag, Key, Wheel, Text, Wait, KeyDown, KeyUp, MouseDown, MouseUp, Move, Drop, Shot, WaitImport, WaitExport, WaitPlay, ScrollTo, CopyFile, DeleteFile, Maximize, Restore, HitTest } kind;
    explicit Action(Kind k) : kind(k) {}
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int button = 0;
    float wheel = 0;
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;
    bool hold = false;  // a drag that never lets go (mid-drag screenshots)
    bool importCancel = false;  // a Click on the status bar's import Cancel, wherever it was drawn last frame
    // "@name[,dx,dy]" for x,y: the centre (plus dx, dy) of the control ui::nameRect() named, as the frame
    // before the action drew it (a Click, DoubleClick or Move). A Drag takes "@name,dx0,dy0,dx1,dy1":
    // from the centre plus dx0, dy0 to the centre plus dx1, dy1.
    std::string target;
    float dx = 0, dy = 0, dx1 = 0, dy1 = 0;
    std::string text, text2;  // Text: the characters; Drop: the file; Shot: the PNG to write; CopyFile: from, to
    int waitFrames = 0;
};

struct Args {
    const char* screenshot = nullptr;
    int width = 1600, height = 960;
    bool sized = false;  // --size given: exact window coordinates, not grown by the UI scale
    float uiScale = 0;   // --ui-scale, or 0 for the display's own
    float mouseX = -1, mouseY = -1;
    int frames = 30;
    int shotAt = -1;  // screenshot frame, overriding the padding after actions
    int viewMode = 0;
    bool overlays = true;  // --no-overlays: the scene view hides trigger, interact and nav overlays
    const char* project = nullptr;  // project root; default: the bundled example
    const char* scene = nullptr;    // relative to the project; default: first *.scene in it
    const char* select = nullptr;   // object name to select at startup
    const char* timeline = nullptr; // cutscene or animation to open in the Timeline at startup
    const char* frame = nullptr;    // GPU dump (.gpd) to open in the Frame tab at startup
    bool ui = false;                // --ui: start on the UI tab and print its layout (scripted tests)
    // --assume-maximized: draw the chrome as if maximized. Under xvfb there is
    // no window manager, so SDL_MaximizeWindow never sets the flag.
    bool assumeMaximized = false;
    std::vector<Action> actions;
    int padButton = -1;  // SDL_GamepadButton held on a virtual gamepad for the whole run
    bool splash = false;
    // --drop <file>@x,y releases a file there after the scripted input;
    // --drag-over x,y holds a dragged file there for the whole run.
    std::string dropFile;
    // --export-to <file.bin>: Export writes there instead of asking.
    std::string exportTo;
    // --play-settings <file>: a screenshot run reads and saves Play's tools there.
    std::string playSettings;
    float dropX = -1, dropY = -1;
    float dragX = -1, dragY = -1;
};

// "ctrl+shift+z", "delete", "f2", "w", "enter", "escape", "up", "down", "left", "right", "[", "]".
bool parseKey(const char* spec, Action& a) {
    std::string s = spec;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    size_t plus;
    while ((plus = s.find('+')) != std::string::npos) {
        std::string mod = s.substr(0, plus);
        if (mod == "ctrl") a.ctrl = true;
        else if (mod == "shift") a.shift = true;
        else if (mod == "alt") a.alt = true;
        else return false;
        s = s.substr(plus + 1);
    }
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') a.key = (ImGuiKey)(ImGuiKey_A + (s[0] - 'a'));
    else if (s.size() == 2 && s[0] == 'f' && s[1] >= '1' && s[1] <= '9') a.key = (ImGuiKey)(ImGuiKey_F1 + (s[1] - '1'));
    else if (s == "delete") a.key = ImGuiKey_Delete;
    else if (s == "enter") a.key = ImGuiKey_Enter;
    else if (s == "escape") a.key = ImGuiKey_Escape;
    else if (s == "backspace") a.key = ImGuiKey_Backspace;
    else if (s == "up") a.key = ImGuiKey_UpArrow;
    else if (s == "down") a.key = ImGuiKey_DownArrow;
    else if (s == "left") a.key = ImGuiKey_LeftArrow;
    else if (s == "right") a.key = ImGuiKey_RightArrow;
    else if (s == "home") a.key = ImGuiKey_Home;
    else if (s == "end") a.key = ImGuiKey_End;
    else if (s == "tab") a.key = ImGuiKey_Tab;
    else if (s == "[") a.key = ImGuiKey_LeftBracket;
    else if (s == "]") a.key = ImGuiKey_RightBracket;
    else if (s == "ctrl") a.key = ImGuiKey_LeftCtrl, a.ctrl = true;
    else if (s == "shift") a.key = ImGuiKey_LeftShift, a.shift = true;
    else if (s == "alt") a.key = ImGuiKey_LeftAlt, a.alt = true;
    else if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') a.key = (ImGuiKey)(ImGuiKey_0 + (s[0] - '0'));
    else if (s == "=") a.key = ImGuiKey_Equal;
    else if (s == "-") a.key = ImGuiKey_Minus;
    else if (s == "space") a.key = ImGuiKey_Space;
    else return false;
    return true;
}

// "@name", "@name,dx,dy" or "@name,dx,dy,button": a control by name, an
// offset from its centre, and the mouse button (0 left, 1 right, 2 middle).
void parseTarget(const char* spec, Action& a) {
    std::string s = spec + 1;
    const size_t comma = s.find(',');
    if (comma != std::string::npos) std::sscanf(s.c_str() + comma + 1, "%f,%f,%d", &a.dx, &a.dy, &a.button);
    a.target = s.substr(0, comma);
}

// Frames an action takes, including a short gap after it.
int actionFrames(const Action& a) {
    switch (a.kind) {
        case Action::Click: return 6;
        case Action::DoubleClick: return 8;
        case Action::Drag: return 20;
        case Action::Wait: return a.waitFrames;
        case Action::Move: return 14;
        case Action::Shot: return 1;
        case Action::WaitImport: return 2;
        case Action::WaitExport: return 2;
        case Action::WaitPlay: return 2;
        default: return 4;
    }
}

// A scripted pointer at x,y in UI coordinates, posted in window coordinates
// like a real one so it takes the same path through scaleQueuedMouse.
void postMousePos(ImGuiIO& io, float x, float y) {
    const float c = g_uiScale;
    const float wx = ui::scriptedWindowCoord(x, c), wy = ui::scriptedWindowCoord(y, c);
    if (c != 1) {
        // ImGui drops a position equal to the last one it has, comparing the
        // window coordinate with a UI one; step aside first so this one lands.
        ImVec2 last = io.MousePos;
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        for (int i = g.InputEventsQueue.Size - 1; i >= 0; --i)
            if (g.InputEventsQueue[i].Type == ImGuiInputEventType_MousePos) {
                last = ImVec2(g.InputEventsQueue[i].MousePos.PosX, g.InputEventsQueue[i].MousePos.PosY);
                break;
            }
        if (last.x == wx && last.y == wy) io.AddMousePosEvent(wx + 1, wy);
    }
    io.AddMousePosEvent(wx, wy);
}

// Feeds frame `f` (counted from the action's first frame) of `a` to ImGui.
void playAction(ImGuiIO& io, const Action& a, int f) {
    switch (a.kind) {
        case Action::Click:
            if (f == 0) postMousePos(io, a.x0, a.y0);
            if (f == 1) io.AddMouseButtonEvent(a.button, true);
            if (f == 2) io.AddMouseButtonEvent(a.button, false);
            break;
        case Action::DoubleClick:
            if (f == 0) postMousePos(io, a.x0, a.y0);
            if (f == 1 || f == 3) io.AddMouseButtonEvent(0, true);
            if (f == 2 || f == 4) io.AddMouseButtonEvent(0, false);
            break;
        case Action::Drag: {
            const int steps = 12;
            if (f == 0) postMousePos(io, a.x0, a.y0);
            if (f == 1) io.AddMouseButtonEvent(a.button, true);
            if (f >= 2 && f < 2 + steps) {
                float t = (float)(f - 1) / steps;
                postMousePos(io, a.x0 + (a.x1 - a.x0) * t, a.y0 + (a.y1 - a.y0) * t);
            }
            if (f == 2 + steps && !a.hold) io.AddMouseButtonEvent(a.button, false);
            break;
        }
        case Action::Key:
            if (f == 0) {
                if (a.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
                if (a.shift) io.AddKeyEvent(ImGuiMod_Shift, true);
                if (a.alt) io.AddKeyEvent(ImGuiMod_Alt, true);
                io.AddKeyEvent(a.key, true);
            }
            if (f == 1) {
                io.AddKeyEvent(a.key, false);
                if (a.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
                if (a.shift) io.AddKeyEvent(ImGuiMod_Shift, false);
                if (a.alt) io.AddKeyEvent(ImGuiMod_Alt, false);
            }
            break;
        case Action::KeyDown:
        case Action::KeyUp:
            // A key (or modifier) pressed or let go, staying that way until the matching action.
            if (f == 0) {
                bool down = a.kind == Action::KeyDown;
                if (a.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, down);
                if (a.shift) io.AddKeyEvent(ImGuiMod_Shift, down);
                if (a.alt) io.AddKeyEvent(ImGuiMod_Alt, down);
                io.AddKeyEvent(a.key, down);
            }
            break;
        case Action::MouseDown:
            if (f == 0) postMousePos(io, a.x0, a.y0);
            if (f == 1) io.AddMouseButtonEvent(a.button, true);
            break;
        case Action::MouseUp:
            if (f == 0) io.AddMouseButtonEvent(a.button, false);
            break;
        case Action::Move: {
            // From wherever the mouse is to (x1, y1), in 12 steps.
            const int steps = 12;
            if (f >= 1 && f <= steps) {
                ImVec2 from = io.MousePos;
                float t = 1.0f / (steps - f + 1);
                postMousePos(io, from.x + (a.x1 - from.x) * t, from.y + (a.y1 - from.y) * t);
            }
            break;
        }
        case Action::Wheel:
            if (f == 0) postMousePos(io, a.x0, a.y0);
            if (f == 1) io.AddMouseWheelEvent(0, a.wheel);
            break;
        case Action::Text:
            if (f == 0) io.AddInputCharactersUTF8(a.text.c_str());
            break;
        case Action::Maximize:
            if (f == 0) SDL_MaximizeWindow(SDL_GL_GetCurrentWindow());
            break;
        case Action::Restore:
            if (f == 0) SDL_RestoreWindow(SDL_GL_GetCurrentWindow());
            break;
        case Action::HitTest:
            // What a press at x,y (UI coordinates) would hit, as the OS would ask.
            if (f == 0) {
                SDL_Window* win = SDL_GL_GetCurrentWindow();
                const SDL_Point p{(int)std::floor(ui::toWindow(a.x0, g_uiScale)), (int)std::floor(ui::toWindow(a.y0, g_uiScale))};
                static const char* names[] = {"normal", "draggable", "resize-topleft", "resize-top",
                                              "resize-topright", "resize-right", "resize-bottomright",
                                              "resize-bottom", "resize-bottomleft", "resize-left"};
                const SDL_HitTestResult r = hitTest(win, &p, nullptr);
                const int i = (int)r;
                std::printf("hittest: %d,%d %s maximized=%d window=%d,%d\n", (int)a.x0, (int)a.y0,
                            i >= 0 && i < (int)(sizeof names / sizeof *names) ? names[i] : "?",
                            (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) ? 1 : 0, p.x, p.y);
                std::fflush(stdout);
            }
            break;
        case Action::CopyFile:
            // Another program writing a file the editor has open.
            if (f == 0) {
                std::error_code ec;
                std::filesystem::copy_file(a.text, a.text2, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) std::fprintf(stderr, "--copy-file: %s\n", ec.message().c_str());
            }
            break;
        case Action::DeleteFile:
            // Another program deleting a file the editor has open.
            if (f == 0) {
                std::error_code ec;
                if (!std::filesystem::remove(a.text, ec)) std::fprintf(stderr, "--delete-file: %s\n", ec ? ec.message().c_str() : "no such file");
            }
            break;
        case Action::Wait:
        case Action::Drop:  // the loop below hands these to the editor
        case Action::Shot:
        case Action::WaitImport:
        case Action::WaitExport:
        case Action::WaitPlay:
            break;
    }
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--screenshot")) a.screenshot = next();
        else if (!std::strcmp(argv[i], "--size")) a.sized = std::sscanf(next(), "%dx%d", &a.width, &a.height) == 2;
        else if (!std::strcmp(argv[i], "--ui-scale")) {
            const char* v = next();
            if (!ui::parseUiScale(v, a.uiScale)) std::fprintf(stderr, "--ui-scale %s: want a number from 0.5 to 4; using the display's scale\n", v);
        }
        else if (!std::strcmp(argv[i], "--mouse")) std::sscanf(next(), "%f,%f", &a.mouseX, &a.mouseY);
        else if (!std::strcmp(argv[i], "--drag-over")) std::sscanf(next(), "%f,%f", &a.dragX, &a.dragY);
        else if (!std::strcmp(argv[i], "--drop")) {
            std::string v = next();
            size_t at = v.rfind('@');
            if (at == std::string::npos || std::sscanf(v.c_str() + at + 1, "%f,%f", &a.dropX, &a.dropY) != 2) {
                std::fprintf(stderr, "--drop wants <file>@x,y\n");
                std::exit(2);
            }
            a.dropFile = v.substr(0, at);
        }
        else if (!std::strcmp(argv[i], "--frames")) a.frames = std::atoi(next());
        else if (!std::strcmp(argv[i], "--shot-at")) a.shotAt = std::atoi(next());
        else if (!std::strcmp(argv[i], "--clean")) a.viewMode = 1;
        else if (!std::strcmp(argv[i], "--no-overlays")) a.overlays = false;
        else if (!std::strcmp(argv[i], "--project")) a.project = next();
        else if (!std::strcmp(argv[i], "--scene")) a.scene = next();
        else if (!std::strcmp(argv[i], "--select")) a.select = next();
        else if (!std::strcmp(argv[i], "--timeline")) a.timeline = next();
        else if (!std::strcmp(argv[i], "--frame")) a.frame = next();
        else if (!std::strcmp(argv[i], "--ui")) a.ui = true;
        else if (!std::strcmp(argv[i], "--export-to")) a.exportTo = next();
        else if (!std::strcmp(argv[i], "--play-settings")) a.playSettings = next();
        else if (!std::strcmp(argv[i], "--click") || !std::strcmp(argv[i], "--dblclick")) {
            Action act(!std::strcmp(argv[i], "--click") ? Action::Click : Action::DoubleClick);
            const char* v = next();
            if (v[0] == '@') parseTarget(v, act);
            else std::sscanf(v, "%f,%f,%d", &act.x0, &act.y0, &act.button);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--click-import-cancel")) {
            // Clicks the status bar's import Cancel where it is when the click starts
            // (it moves with the budget meters beside it); nowhere when it is not shown.
            Action act(Action::Click);
            act.importCancel = true;
            act.x0 = act.y0 = -1;
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--drag") || !std::strcmp(argv[i], "--drag-hold")) {
            Action act(Action::Drag);
            act.hold = !std::strcmp(argv[i], "--drag-hold");
            const char* v = next();
            if (v[0] == '@') {
                std::string s = v + 1;
                const size_t comma = s.find(',');
                if (comma != std::string::npos)
                    std::sscanf(s.c_str() + comma + 1, "%f,%f,%f,%f", &act.dx, &act.dy, &act.dx1, &act.dy1);
                act.target = s.substr(0, comma);
            } else {
                std::sscanf(v, "%f,%f,%f,%f,%d", &act.x0, &act.y0, &act.x1, &act.y1, &act.button);
            }
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--wheel")) {
            Action act(Action::Wheel);
            std::sscanf(next(), "%f,%f,%f", &act.x0, &act.y0, &act.wheel);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--key")) {
            Action act(Action::Key);
            const char* spec = next();
            if (parseKey(spec, act)) a.actions.push_back(act);
            else std::fprintf(stderr, "unknown key '%s'\n", spec);
        } else if (!std::strcmp(argv[i], "--keydown") || !std::strcmp(argv[i], "--keyup")) {
            // Holds a key ("w", "shift", "alt+...") until --keyup of the same spec.
            Action act(!std::strcmp(argv[i], "--keydown") ? Action::KeyDown : Action::KeyUp);
            const char* spec = next();
            if (parseKey(spec, act)) a.actions.push_back(act);
            else std::fprintf(stderr, "unknown key '%s'\n", spec);
        } else if (!std::strcmp(argv[i], "--mousedown")) {
            Action act(Action::MouseDown);
            const char* v = next();
            if (v[0] == '@') parseTarget(v, act);
            else std::sscanf(v, "%f,%f,%d", &act.x0, &act.y0, &act.button);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--mouseup")) {
            Action act(Action::MouseUp);
            act.button = std::atoi(next());
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--move")) {
            Action act(Action::Move);
            const char* v = next();
            if (v[0] == '@') parseTarget(v, act);
            else std::sscanf(v, "%f,%f", &act.x1, &act.y1);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--wait")) {
            // Idle frames before the next action (lets the background export finish).
            Action act(Action::Wait);
            act.waitFrames = std::atoi(next());
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--drop-now")) {
            // --drop-now <file>@x,y: releases a file there at this point of the script.
            Action act(Action::Drop);
            std::string v = next();
            size_t at = v.rfind('@');
            if (at == std::string::npos || std::sscanf(v.c_str() + at + 1, "%f,%f", &act.x0, &act.y0) != 2) {
                std::fprintf(stderr, "--drop-now wants <file>@x,y\n");
                std::exit(2);
            }
            act.text = v.substr(0, at);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--shot")) {
            // --shot <file.png>: a screenshot at this point of the script, as well as the one at the end.
            Action act(Action::Shot);
            act.text = next();
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--wait-export")) {
            // Holds the script here until the live export has caught up with the scene, up to 90 s.
            a.actions.push_back(Action(Action::WaitExport));
        } else if (!std::strcmp(argv[i], "--scroll-to")) {
            // --scroll-to @name: wheels the panel the control lies across (a named
            // "<x>.panel" rect: the scene or object inspector) until the control is
            // inside it, then goes on; up to 90 s, or until the panel scrolls no further.
            Action act(Action::ScrollTo);
            const char* v = next();
            if (v[0] != '@') {
                std::fprintf(stderr, "--scroll-to wants @name\n");
                std::exit(2);
            }
            parseTarget(v, act);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--wait-play")) {
            // Holds the script here while a Play, Run on hardware or Export build is running, up to 90 s.
            a.actions.push_back(Action(Action::WaitPlay));
        } else if (!std::strcmp(argv[i], "--wait-import")) {
            // Holds the script here while a model import is running (in the background), up to 90 s.
            a.actions.push_back(Action(Action::WaitImport));
        } else if (!std::strcmp(argv[i], "--maximize")) {
            a.actions.push_back(Action(Action::Maximize));
        } else if (!std::strcmp(argv[i], "--assume-maximized")) {
            a.assumeMaximized = true;
        } else if (!std::strcmp(argv[i], "--restore")) {
            a.actions.push_back(Action(Action::Restore));
        } else if (!std::strcmp(argv[i], "--hittest")) {
            // --hittest x,y: prints `hittest: x,y <result> maximized=<0|1>`.
            Action act(Action::HitTest);
            std::sscanf(next(), "%f,%f", &act.x0, &act.y0);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--copy-file")) {
            // --copy-file <from>@<to>: copies over <to> at this point of the run.
            Action act(Action::CopyFile);
            std::string v = next();
            size_t at = v.rfind('@');
            if (at == std::string::npos) {
                std::fprintf(stderr, "--copy-file wants <from>@<to>\n");
                std::exit(2);
            }
            act.text = v.substr(0, at);
            act.text2 = v.substr(at + 1);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--delete-file")) {
            // --delete-file <path>: deletes it at this point of the run.
            Action act(Action::DeleteFile);
            act.text = next();
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--text")) {
            Action act(Action::Text);
            act.text = next();
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--pad")) {
            // Holds one button of a virtual gamepad ("a", "start", "dpad_up", ...) for the whole run.
            const char* spec = next();
            a.padButton = SDL_GetGamepadButtonFromString(spec);
            if (a.padButton == SDL_GAMEPAD_BUTTON_INVALID) std::fprintf(stderr, "unknown gamepad button '%s'\n", spec);
        } else if (!std::strcmp(argv[i], "--splash")) {
            // Screenshot runs skip the startup splash unless asked; it plays silent there.
            a.splash = true;
        } else if (!std::strcmp(argv[i], "--ctrl-held")) {
            // Holds Ctrl down for the whole run (Ctrl-snapping while dragging).
            Action act(Action::Key);
            act.key = ImGuiMod_Ctrl;
            act.button = -1;
            a.actions.insert(a.actions.begin(), act);
        }
    }
    return a;
}

// Writes what was just rendered to `path` as a PNG.
bool writeShot(SDL_Window* window, const char* path) {
    int fw, fh;
    SDL_GetWindowSizeInPixels(window, &fw, &fh);
    std::vector<unsigned char> px((size_t)fw * fh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (!stbi_write_png(path, fw, fh, 4, px.data(), fw * 4)) {
        std::fprintf(stderr, "failed to write %s\n", path);
        return false;
    }
    std::printf("wrote %s (%dx%d)\n", path, fw, fh);
    return true;
}

// First *.scene directly inside `project`, by name, or empty if none.
// The directory holding the executable. A packaged editor ships assets/,
// examples/, redux/ and engine/ next to itself.
std::filesystem::path baseDir() {
    const char* b = SDL_GetBasePath();  // owned by SDL
    if (!b) return {};
    return std::filesystem::path(reinterpret_cast<const char8_t*>(b));
}

// `rel` under the executable's directory when the package carries it, else
// the source-tree path compiled in for development builds.
std::string resourceDir(const char* rel, const char* compiled) {
    std::error_code ec;
    std::filesystem::path p = baseDir() / rel;
    if (std::filesystem::is_directory(p, ec)) {
        std::u8string u = p.u8string();
        return std::string(u.begin(), u.end());
    }
    return compiled;
}

// Loads the scene named on the command line, or the bundled example. Errors
// are reported and leave an empty scene open.
void openDocument(editor::Document& doc, const Args& args) {
    bool bundled = !args.project;
    std::filesystem::path project = bundled ? std::filesystem::path(reinterpret_cast<const char8_t*>(resourceDir("examples/courtyard", SPLASHEDIT_EXAMPLE_DIR).c_str())) : std::filesystem::path(args.project);
    std::filesystem::path scene = args.scene ? std::filesystem::path(args.scene) : editor::firstScene(project);
    if (scene.empty()) {
        std::fprintf(stderr, "no .scene file in %s; opening an empty scene\n", project.string().c_str());
        doc.load(project, "untitled.scene");
        return;
    }
    if (auto err = doc.load(project, scene)) {
        std::fprintf(stderr, "could not load %s: %s; opening an empty scene\n", (project / scene).string().c_str(), err->c_str());
        return;
    }
    const char* select = args.select ? args.select : bundled && !args.scene ? "Crate" : nullptr;
    if (select && !doc.selectByName(select)) std::fprintf(stderr, "no object named '%s' in the scene\n", select);
}

}  // namespace

// --dump-texture-preview <image> <4|8|16> <out.png> [--cutout]: writes the
// inspector's PS1 preview of an image and prints its numbers, no window
// (tests/quantpreviewcheck.py compares it with what psxsplash uploads).
static int dumpTexturePreview(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: splashedit --dump-texture-preview <image> <4|8|16> <out.png> [--cutout]\n");
        return 2;
    }
    editor::TexturePreviewKey key;
    key.file = argv[2];
    int bpp = std::atoi(argv[3]);
    if (bpp != 4 && bpp != 8 && bpp != 16) {
        std::fprintf(stderr, "bit depth must be 4, 8 or 16\n");
        return 2;
    }
    key.depth = static_cast<splash::BitDepth>(bpp);
    key.cutout = argc > 5 && !std::strcmp(argv[5], "--cutout");
    editor::TexturePreview p = editor::makeTexturePreview(key);
    if (!p.error.empty() || !editor::saveTexturePreviewPng(p, argv[4])) {
        std::fprintf(stderr, "preview failed: %s\n", p.error.empty() ? "cannot write png" : p.error.c_str());
        return 1;
    }
    std::printf("preview %dx%d bpp=%d palette=%d used=%d colours=%d source_colours=%d deltaE=%.3f vram=%d ms=%.1f\n",
                p.width, p.height, bpp, p.paletteSize, p.paletteUsed, p.ps1Colors, p.sourceColors, p.deltaE, p.vramBytes,
                p.convertMs);
    return 0;
}

#ifdef _WIN32
// The editor is a windowed program, so it starts with no console. Output sent
// to a file or a pipe already reaches it; anything else goes to the console
// it was started from, if there is one.
void attachParentConsole() {
    auto open = [](DWORD id) {
        HANDLE h = GetStdHandle(id);
        return h && h != INVALID_HANDLE_VALUE;
    };
    const bool out = open(STD_OUTPUT_HANDLE), err = open(STD_ERROR_HANDLE);
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f;
    if (!out) freopen_s(&f, "CONOUT$", "w", stdout);
    if (!err) freopen_s(&f, "CONOUT$", "w", stderr);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    attachParentConsole();
#endif
    if (argc >= 2 && !std::strcmp(argv[1], "--version")) {
        std::printf("SplashEdit %s\nassets %s\n", SPLASHEDIT_VERSION, resourceDir("assets", SPLASHEDIT_ASSET_DIR).c_str());
        return 0;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "--dump-texture-preview")) return dumpTexturePreview(argc, argv);
    Args args = parse(argc, argv);
    // SPLASHEDIT_UI_SCALE when --ui-scale is not given.
    if (args.uiScale <= 0) {
        const char* env = std::getenv("SPLASHEDIT_UI_SCALE");
        if (env && *env && !ui::parseUiScale(env, args.uiScale))
            std::fprintf(stderr, "SPLASHEDIT_UI_SCALE=%s: want a number from 0.5 to 4; using the display's scale\n", env);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // A controller drives port 1 in the Game view; the editor works without one.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) std::fprintf(stderr, "SDL gamepad: %s\n", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("SplashEdit", args.width, args.height, flags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    // Without --size the window grows with the UI scale of the display it
    // opened on (asked of the window: only it knows its pixel density).
    const auto uiScaleOf = [&](SDL_Window* w) {
        return ui::contentScale(SDL_GetWindowDisplayScale(w), SDL_GetWindowPixelDensity(w), args.uiScale);
    };
    if (!args.sized) {
        const float c = uiScaleOf(window);
        if (c != 1) {
            const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
            SDL_Rect usable{};
            if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) usable = SDL_Rect{};
            const ui::WindowSize size = ui::initialWindowSize(args.width, args.height, c, usable.w, usable.h);
            SDL_SetWindowSize(window, size.w, size.h);
            if (display) SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
    }
    SDL_SetWindowHitTest(window, hitTest, nullptr);
    ui::setFileDialogWindow(window);  // file dialogs are modal to it
    const std::string assetDir = resourceDir("assets", SPLASHEDIT_ASSET_DIR);
    brand::setWindowIcon(window, assetDir.c_str());
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(1);
    if (!gl::load()) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::load(assetDir.c_str());
    brand::load(assetDir.c_str());
    if (!args.screenshot || args.splash) splash::load(assetDir.c_str(), !args.screenshot);
    ImGui_ImplSDL3_InitForOpenGL(window, ctx);
    // The SDL3 backend turns SDL_BORDERLESS_WINDOWED_STYLE off for its own
    // viewport windows. SDL reads that hint whenever it rebuilds this window's
    // style (restore does). On Windows a maximized window without WS_CAPTION
    // covers the taskbar. The editor opens no viewport windows.
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    {
        // The backend places the IME box in window coordinates.
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        g_backendSetImeData = pio.Platform_SetImeDataFn;
        pio.Platform_SetImeDataFn = setImeDataScaled;
    }
    if (args.padButton >= 0) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "splashedit virtual pad";
        SDL_Joystick* j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&desc));
        if (!j || !SDL_SetJoystickVirtualButton(j, args.padButton, true))
            std::fprintf(stderr, "virtual gamepad: %s\n", SDL_GetError());
    }
    ImGui_ImplOpenGL3_Init("#version 330 core");

    viewport::Ps1View view;
    if (!view.init()) {
        std::fprintf(stderr, "viewport init failed\n");
        return 1;
    }
    mockup::State state;
    state.viewMode = args.viewMode;
    state.gizmos = args.overlays;
    state.play.bundleDir = baseDir();
    {
        // Capture frame: redux runs this beside the game (editor::reduxCommand).
        std::error_code ec;
        const std::filesystem::path script = std::filesystem::path(std::u8string(assetDir.begin(), assetDir.end())) / "framecapture.lua";
        if (std::filesystem::is_regular_file(script, ec)) state.play.captureScript = script;
    }
    {
        const std::string templates = resourceDir("templates", SPLASHEDIT_TEMPLATE_DIR);
        state.newProject.templatesDir = std::filesystem::path(std::u8string(templates.begin(), templates.end()));
    }
    state.play.exportTo = std::filesystem::path(std::u8string(args.exportTo.begin(), args.exportTo.end()));
    // Play's tool paths persist per user; screenshot runs read only the environment.
    if (!args.screenshot) {
        if (char* pref = SDL_GetPrefPath("psxsplash", "splashedit")) {
            const std::filesystem::path prefDir(reinterpret_cast<const char8_t*>(pref));
            SDL_free(pref);
            state.play.settingsFile = prefDir / "play.cfg";
            state.play.tools = editor::loadPlayTools(state.play.settingsFile);
            state.recent = editor::RecentScenes(prefDir / "recent.cfg");
            state.recent.load();
        }
    } else if (!args.playSettings.empty()) {
        state.play.settingsFile = std::filesystem::path(std::u8string(args.playSettings.begin(), args.playSettings.end()));
        state.play.tools = editor::loadPlayTools(state.play.settingsFile);
    }
    editor::Document doc;
    openDocument(doc, args);
    if (args.timeline) {
        for (const editor::SeqRef ref : editor::allSequences(doc))
            if (!state.timeline.open && *editor::seqName(doc, ref) == args.timeline) state.timeline.show(doc, ref);
        if (!state.timeline.open) std::fprintf(stderr, "no cutscene or animation named '%s' in the scene\n", args.timeline);
    }
    if (args.frame) {
        ui::namedRectsOn() = args.screenshot != nullptr;  // its scripted lines print from the load
        std::string err;
        const std::string f = args.frame;
        if (!state.frame.load(std::filesystem::path(std::u8string(f.begin(), f.end())), &err))
            std::fprintf(stderr, "--frame %s: %s\n", args.frame, err.c_str());
    }
    if (args.ui) {
        state.scripts.showUi();
        ui::CanvasView::reportLayout = true;
    }
    // A project named on the command line joins File > Recent like one opened from the menu.
    if (args.project && !args.screenshot && std::filesystem::is_regular_file(doc.scenePath()))
        state.recent.add(std::filesystem::absolute(doc.scenePath()));
    view.setDocument(doc);

    // Scripted input timeline (screenshot mode).
    int firstFrame = 5, timelineEnd = firstFrame;
    std::vector<int> actionStart;
    for (const Action& a : args.actions) {
        if (a.button == -1) {  // --ctrl-held takes no time
            actionStart.push_back(-1);
            continue;
        }
        actionStart.push_back(timelineEnd);
        timelineEnd += actionFrames(a);
    }
    ui::namedRectsOn() = args.screenshot != nullptr;  // for --click @name, and printed with the end screenshot
    if (args.screenshot && !args.actions.empty()) args.frames = std::max(args.frames, timelineEnd + 30);
    // --shot-at: the exact frame, even mid-script (actions start at frame 5).
    if (args.shotAt >= 0) args.frames = args.shotAt + 1;

    int frame = 0;
    bool running = true;
    float printedScale = 0;  // the UI scale a screenshot run last reported
    // --wait-import holding the script, since when; and a --shot due after this frame renders.
    bool held = false;
    Action::Kind heldKind = Action::WaitImport;
    std::chrono::steady_clock::time_point heldSince;
    // --scroll-to holding the script: its control, the step it is on (aim,
    // wheel, look), where the control was before the last wheel, and done.
    std::string scrollTarget;
    int scrollStep = 0;
    ImRect scrollSeen;
    bool scrollDone = false;
    // The live export is running, or its last result is for an older revision of the scene.
    const auto playBuilding = [&] { return state.play.build.valid() || state.play.hwBuild.valid() || state.play.disc.valid(); };
    const auto exportBehind = [&] { return state.live.busy() || state.live.resultRevision() != doc.revision() || !state.live.result(); };
    std::string shotDue;
    // The end screenshot waits (up to 90 s, since when) for a model import still
    // running, so a --drop has landed in it; --shot-at keeps its exact frame.
    std::optional<std::chrono::steady_clock::time_point> endHeldSince;
    // --click-import-cancel holding for the status bar to settle, since when;
    // and where the frame before last drew the Cancel.
    std::optional<std::chrono::steady_clock::time_point> aimSince;
    ImRect cancelSeen;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            // Closing the window asks about unsaved changes first; screenshot runs just stop.
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (args.screenshot) running = false;
                else state.quitRequested = true;
            }
            // Files dragged in from the desktop: hover feedback, then import where released.
            if (e.type == SDL_EVENT_DROP_BEGIN) state.drop.hovering = true;
            // (SDL reports where in window coordinates.)
            const ImVec2 dropAt(ui::toUi(e.drop.x, g_uiScale), ui::toUi(e.drop.y, g_uiScale));
            if (e.type == SDL_EVENT_DROP_POSITION) state.drop.pos = dropAt;
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && !ui::fileDialogOpen())
                state.drop.files.push_back({e.drop.data, dropAt});
            if (e.type == SDL_EVENT_DROP_COMPLETE) state.drop.hovering = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // The UI lays out in UI coordinates: a smaller display, rendered at
        // a higher density, so fonts rasterize sharp at the larger size. Read
        // every frame, so a move to another monitor rescales.
        {
            const float c = uiScaleOf(window);
            if (args.screenshot && c != printedScale) {
                std::printf("ui: scale %g\n", c);
                std::fflush(stdout);
                printedScale = c;
            }
            g_uiScale = c;
            io.DisplaySize = ImVec2(io.DisplaySize.x / c, io.DisplaySize.y / c);
            io.DisplayFramebufferScale = ImVec2(io.DisplayFramebufferScale.x * c, io.DisplayFramebufferScale.y * c);
        }
        bool aimHeld = false;
        if (args.screenshot) {
            io.DeltaTime = 1.0f / 60.0f;
            // The import Cancel where the last two frames drew it: settled once
            // they agree and the budget measurement beside it is in. Under load
            // that measurement can land mid-click and move the button away.
            const ImRect cancelNow = state.importCancel;
            const bool cancelSettled = cancelNow.GetWidth() > 0 && cancelNow.Min.x == cancelSeen.Min.x &&
                                       cancelNow.Max.x == cancelSeen.Max.x && cancelNow.Min.y == cancelSeen.Min.y &&
                                       state.live.result() && !state.live.busy();
            cancelSeen = cancelNow;
            for (size_t i = 0; i < args.actions.size(); ++i) {
                Action& a = args.actions[i];
                if (actionStart[i] < 0) {
                    if (frame == 0) io.AddKeyEvent(ImGuiMod_Ctrl, true);
                    continue;
                }
                if (a.importCancel && frame == actionStart[i] && !held) {
                    // Holds the script (up to 90 s) while an import runs and the bar settles.
                    const auto now = std::chrono::steady_clock::now();
                    if (!cancelSettled && state.importBusy()) {
                        if (!aimSince) aimSince = now;
                        aimHeld = now - *aimSince < std::chrono::seconds(90);
                        if (!aimHeld) std::fprintf(stderr, "--click-import-cancel: the status bar still moving after 90 s\n");
                    }
                    if (!aimHeld) aimSince.reset();
                    if (cancelNow.GetWidth() > 0) a.x0 = cancelNow.GetCenter().x, a.y0 = cancelNow.GetCenter().y;
                }
                if (!a.target.empty() && frame == actionStart[i] && !held) {
                    // Aims at the named control where the last frame drew it.
                    const auto it = ui::namedRects().find(a.target);
                    if (it == ui::namedRects().end()) std::fprintf(stderr, "@%s: no such control drawn\n", a.target.c_str());
                    const ImVec2 c = it == ui::namedRects().end() ? ImVec2(-100, -100) : ImVec2(it->second.GetCenter().x + a.dx, it->second.GetCenter().y + a.dy);
                    if (a.kind == Action::Move) a.x1 = c.x, a.y1 = c.y;
                    else a.x0 = c.x, a.y0 = c.y;
                    if (a.kind == Action::Drag) a.x1 = c.x - a.dx + a.dx1, a.y1 = c.y - a.dy + a.dy1;
                }
                if (frame >= actionStart[i] && frame < actionStart[i] + actionFrames(a)) playAction(io, a, frame - actionStart[i]);
                if (frame != actionStart[i] || held) continue;
                if (a.kind == Action::Drop) state.drop.files.push_back({a.text, ImVec2(a.x0, a.y0)});
                if (a.kind == Action::Shot) shotDue = a.text;
                if (a.kind == Action::WaitImport && state.importBusy()) held = true, heldKind = a.kind, heldSince = std::chrono::steady_clock::now();
                if (a.kind == Action::WaitExport && exportBehind()) held = true, heldKind = a.kind, heldSince = std::chrono::steady_clock::now();
                if (a.kind == Action::WaitPlay && playBuilding()) held = true, heldKind = a.kind, heldSince = std::chrono::steady_clock::now();
                if (a.kind == Action::ScrollTo) {
                    held = true, heldKind = a.kind, heldSince = std::chrono::steady_clock::now();
                    scrollTarget = a.target, scrollStep = 0, scrollSeen = ImRect(), scrollDone = false;
                }
            }
            if (held && heldKind == Action::ScrollTo) {
                // Where the last frame drew the control and the panel across it; the
                // wheel (48 px a notch) that would bring it 8 px inside, 0 when it is in.
                const std::map<std::string, ImRect>& rects = ui::namedRects();
                const auto it = rects.find(scrollTarget);
                const ImRect* panel = nullptr;
                if (it != rects.end())
                    for (const auto& [name, r] : rects)
                        if (name.ends_with(".panel") && it->second.GetCenter().x >= r.Min.x && it->second.GetCenter().x <= r.Max.x) {
                            panel = &r;
                            break;
                        }
                float notches = 0;
                if (panel) {
                    const ImRect& c = it->second;
                    const float m = 8;
                    if (c.Min.y < panel->Min.y + m) notches = (panel->Min.y + m - c.Min.y) / 48.0f;
                    else if (c.Max.y > panel->Max.y - m && c.GetHeight() < panel->GetHeight() - 2 * m)
                        notches = -(c.Max.y - (panel->Max.y - m)) / 48.0f;
                }
                if (it == rects.end() || !panel) {
                    std::fprintf(stderr, "--scroll-to @%s: %s\n", scrollTarget.c_str(), it == rects.end() ? "no such control drawn" : "not in a panel");
                    scrollDone = true;
                } else if (notches == 0) {
                    scrollDone = true;
                } else if (scrollStep % 3 == 0) {
                    if (scrollStep > 0 && it->second.Min.y == scrollSeen.Min.y) {
                        std::fprintf(stderr, "--scroll-to @%s: the panel scrolls no further\n", scrollTarget.c_str());
                        scrollDone = true;
                    }
                    scrollSeen = it->second;
                    postMousePos(io, panel->GetCenter().x, panel->GetCenter().y);
                } else if (scrollStep % 3 == 1) {
                    io.AddMouseWheelEvent(0, notches);
                }
                scrollStep++;
            }
            const bool holding = heldKind == Action::WaitExport ? exportBehind() : heldKind == Action::WaitPlay ? playBuilding()
                                 : heldKind == Action::ScrollTo ? !scrollDone : state.importBusy();
            if (held && (!holding || std::chrono::steady_clock::now() - heldSince > std::chrono::seconds(90))) {
                if (holding) std::fprintf(stderr, heldKind == Action::WaitExport ? "--wait-export: export still behind after 90 s\n"
                                                  : heldKind == Action::WaitPlay ? "--wait-play: build still running after 90 s\n"
                                                  : heldKind == Action::ScrollTo ? "--scroll-to: still scrolling after 90 s\n"
                                                                                 : "--wait-import: still importing after 90 s\n");
                held = false;
                ++frame;  // past the --wait-import frame
            }
            if (args.mouseX >= 0 && frame >= timelineEnd) postMousePos(io, args.mouseX, args.mouseY);
            if (args.dragX >= 0) {
                state.drop.hovering = true;
                state.drop.pos = ImVec2(args.dragX, args.dragY);
            }
            if (!args.dropFile.empty() && frame == timelineEnd) state.drop.files.push_back({args.dropFile, ImVec2(args.dropX, args.dropY)});
        }
        // While the Game view has the keyboard, arrows and Enter are the controller:
        // they must not move ImGui's keyboard focus or press the button it lands on.
        // The script editor's arrows, Tab and Enter edit text.
        if ((state.play.gameFocus && state.play.cons[size_t(state.play.focus)].game.attached()) || state.scripts.hasKeyboard())
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // A file dialog is open: it is modal, so the editor takes none of its
        // own input (real or scripted) until it answers (#159).
        if (ui::fileDialogOpen()) {
            io.ClearEventsQueue();
            io.ClearInputMouse();
            io.ClearInputKeys();
            state.drop.files.clear();
            state.drop.hovering = false;
        }
        scaleQueuedMouse(g_uiScale);
        ImGui::NewFrame();
        ui::interactiveRects().clear();
        ui::namedRects().clear();
        // The splash hides the editor until it starts fading out.
        state.maximized = args.assumeMaximized ||
                          (SDL_GetWindowFlags(SDL_GL_GetCurrentWindow()) & SDL_WINDOW_MAXIMIZED) != 0;
        const ImRect titleBar =
            splash::covering() ? ImRect() : mockup::drawMainScreen(state, doc, view, io.DisplaySize);
        g_hit.titleBar = chromeRect(titleBar);
        g_hit.scale = g_uiScale;
        splash::draw(io.DisplaySize, args.screenshot ? frame * 1000.0 / 60.0 : double(SDL_GetTicks()));
        ui::drawFileDialogShade();
        g_hit.controls.clear();
        for (const ImRect& r : ui::interactiveRects())
            if (r.Overlaps(titleBar)) g_hit.controls.push_back(chromeRect(r));
        ImGui::Render();
        if (state.quit) running = false;

        int fw, fh;
        SDL_GetWindowSizeInPixels(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!shotDue.empty()) {
            writeShot(window, shotDue.c_str());
            std::printf("model import: %s, %d frames drawn while it ran, %zu queued\n", state.importBusy() ? "running" : "idle",
                        state.importFrames(), state.imports.size());
            if (state.importCancel.GetWidth() > 0)
                std::printf("cancel button at %d,%d\n", (int)state.importCancel.GetCenter().x, (int)state.importCancel.GetCenter().y);
            if (state.importProgress >= 0) std::printf("import progress: %d%%\n", (int)(state.importProgress * 100));
            shotDue.clear();
        }
        bool endHeld = false;
        if (args.screenshot && !held && args.shotAt < 0 && frame + 1 >= args.frames && state.importBusy()) {
            const auto now = std::chrono::steady_clock::now();
            if (!endHeldSince) endHeldSince = now;
            endHeld = now - *endHeldSince < std::chrono::seconds(90);
            if (!endHeld) std::fprintf(stderr, "--screenshot: still importing after 90 s\n");
        }
        if (args.screenshot && !held && !aimHeld && !endHeld && ++frame >= args.frames) {
            if (!writeShot(window, args.screenshot)) return 1;
            // For scripted checks: menus and pickers left open by the input.
            std::printf("popups open: %s\n", ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ? "yes" : "no");
            std::printf("game attached: %s\n", state.play.cons[0].game.attached() ? "yes" : "no");
            std::printf("status: %s\n", state.notice.c_str());
            std::printf("unsaved changes: %s\n", doc.dirty() ? "yes" : "no");
            std::printf("inspector import warnings: %zu\n", state.importPanel.shown.size());
            for (const std::string& w : state.importPanel.shown) std::printf("  import warning: %s\n", w.c_str());
            std::printf("project panel open: %s\n", state.assets.open ? "yes" : "no");
            std::printf("project panel: %s\n", state.assets.shownList().c_str());
            for (const auto& [name, r] : ui::namedRects())
                std::printf("control %s at %d,%d %d,%d\n", name.c_str(), int(r.Min.x), int(r.Min.y), int(r.Max.x), int(r.Max.y));
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }

    for (auto& c : state.play.cons) c.emu.stop();
    if (state.play.relay) state.play.relay->stop();
    splash::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
