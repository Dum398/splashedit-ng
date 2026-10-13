#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// The implementation comes from splashcore (core/stb_impl.cpp).
#include <stb_image_write.h>

#include "editor/document.hh"
#include "gl.h"
#include "mockup/main_screen.h"
#include "ui/brand.h"
#include "ui/splash.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "viewport/ps1view.h"

namespace {

struct HitTestState {
    ImRect titleBar;
    std::vector<ImRect> controls;  // title bar rectangles that take clicks
};
HitTestState g_hit;

SDL_HitTestResult hitTest(SDL_Window* win, const SDL_Point* p, void*) {
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    const int edge = 6;
    bool l = p->x < edge, r = p->x >= w - edge, t = p->y < edge, b = p->y >= h - edge;
    if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (t) return SDL_HITTEST_RESIZE_TOP;
    if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    if (l) return SDL_HITTEST_RESIZE_LEFT;
    if (r) return SDL_HITTEST_RESIZE_RIGHT;
    // Decided from geometry, not hover: on Windows the pointer over a draggable
    // area produces no mouse motion, so the UI never learns it is over a button.
    ImVec2 pt((float)p->x, (float)p->y);
    if (!g_hit.titleBar.Contains(pt)) return SDL_HITTEST_NORMAL;
    for (const ImRect& r : g_hit.controls)
        if (r.Contains(pt)) return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

// Scripted input for screenshot mode, so interactions can be checked headless.
// Actions run one after another from frame 5, in command-line order.
struct Action {
    enum Kind { Click, DoubleClick, Drag, Key, Wheel, Text, Wait, KeyDown, KeyUp, MouseDown, MouseUp, Move } kind;
    explicit Action(Kind k) : kind(k) {}
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int button = 0;
    float wheel = 0;
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;
    bool hold = false;  // a drag that never lets go (mid-drag screenshots)
    std::string text;
    int waitFrames = 0;
};

struct Args {
    const char* screenshot = nullptr;
    int width = 1600, height = 960;
    float mouseX = -1, mouseY = -1;
    int frames = 30;
    int viewMode = 0;
    const char* project = nullptr;  // project root; default: the bundled example
    const char* scene = nullptr;    // relative to the project; default: first *.scene in it
    const char* select = nullptr;   // object name to select at startup
    std::vector<Action> actions;
    int padButton = -1;  // SDL_GamepadButton held on a virtual gamepad for the whole run
    bool splash = false;
    // --drop <file>@x,y releases a file there after the scripted input;
    // --drag-over x,y holds a dragged file there for the whole run.
    std::string dropFile;
    // --export-to <file.bin>: Export writes there instead of asking.
    std::string exportTo;
    float dropX = -1, dropY = -1;
    float dragX = -1, dragY = -1;
};

// "ctrl+shift+z", "delete", "f2", "w", "enter", "escape", "up", "down".
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
    else if (s == "ctrl") a.key = ImGuiKey_LeftCtrl, a.ctrl = true;
    else if (s == "shift") a.key = ImGuiKey_LeftShift, a.shift = true;
    else if (s == "alt") a.key = ImGuiKey_LeftAlt, a.alt = true;
    else return false;
    return true;
}

// Frames an action takes, including a short gap after it.
int actionFrames(const Action& a) {
    switch (a.kind) {
        case Action::Click: return 6;
        case Action::DoubleClick: return 8;
        case Action::Drag: return 20;
        case Action::Wait: return a.waitFrames;
        case Action::Move: return 14;
        default: return 4;
    }
}

// Feeds frame `f` (counted from the action's first frame) of `a` to ImGui.
void playAction(ImGuiIO& io, const Action& a, int f) {
    switch (a.kind) {
        case Action::Click:
            if (f == 0) io.AddMousePosEvent(a.x0, a.y0);
            if (f == 1) io.AddMouseButtonEvent(a.button, true);
            if (f == 2) io.AddMouseButtonEvent(a.button, false);
            break;
        case Action::DoubleClick:
            if (f == 0) io.AddMousePosEvent(a.x0, a.y0);
            if (f == 1 || f == 3) io.AddMouseButtonEvent(0, true);
            if (f == 2 || f == 4) io.AddMouseButtonEvent(0, false);
            break;
        case Action::Drag: {
            const int steps = 12;
            if (f == 0) io.AddMousePosEvent(a.x0, a.y0);
            if (f == 1) io.AddMouseButtonEvent(a.button, true);
            if (f >= 2 && f < 2 + steps) {
                float t = (float)(f - 1) / steps;
                io.AddMousePosEvent(a.x0 + (a.x1 - a.x0) * t, a.y0 + (a.y1 - a.y0) * t);
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
            if (f == 0) io.AddMousePosEvent(a.x0, a.y0);
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
                io.AddMousePosEvent(from.x + (a.x1 - from.x) * t, from.y + (a.y1 - from.y) * t);
            }
            break;
        }
        case Action::Wheel:
            if (f == 0) io.AddMousePosEvent(a.x0, a.y0);
            if (f == 1) io.AddMouseWheelEvent(0, a.wheel);
            break;
        case Action::Text:
            if (f == 0) io.AddInputCharactersUTF8(a.text.c_str());
            break;
        case Action::Wait:
            break;
    }
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--screenshot")) a.screenshot = next();
        else if (!std::strcmp(argv[i], "--size")) std::sscanf(next(), "%dx%d", &a.width, &a.height);
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
        else if (!std::strcmp(argv[i], "--clean")) a.viewMode = 1;
        else if (!std::strcmp(argv[i], "--project")) a.project = next();
        else if (!std::strcmp(argv[i], "--scene")) a.scene = next();
        else if (!std::strcmp(argv[i], "--select")) a.select = next();
        else if (!std::strcmp(argv[i], "--export-to")) a.exportTo = next();
        else if (!std::strcmp(argv[i], "--click") || !std::strcmp(argv[i], "--dblclick")) {
            Action act(!std::strcmp(argv[i], "--click") ? Action::Click : Action::DoubleClick);
            std::sscanf(next(), "%f,%f,%d", &act.x0, &act.y0, &act.button);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--drag") || !std::strcmp(argv[i], "--drag-hold")) {
            Action act(Action::Drag);
            act.hold = !std::strcmp(argv[i], "--drag-hold");
            std::sscanf(next(), "%f,%f,%f,%f,%d", &act.x0, &act.y0, &act.x1, &act.y1, &act.button);
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
            std::sscanf(next(), "%f,%f,%d", &act.x0, &act.y0, &act.button);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--mouseup")) {
            Action act(Action::MouseUp);
            act.button = std::atoi(next());
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--move")) {
            Action act(Action::Move);
            std::sscanf(next(), "%f,%f", &act.x1, &act.y1);
            a.actions.push_back(act);
        } else if (!std::strcmp(argv[i], "--wait")) {
            // Idle frames before the next action (lets the background export finish).
            Action act(Action::Wait);
            act.waitFrames = std::atoi(next());
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

int main(int argc, char** argv) {
    Args args = parse(argc, argv);
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
    SDL_SetWindowHitTest(window, hitTest, nullptr);
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
    state.play.bundleDir = baseDir();
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
    }
    editor::Document doc;
    openDocument(doc, args);
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
    if (args.screenshot && !args.actions.empty()) args.frames = std::max(args.frames, timelineEnd + 30);

    int frame = 0;
    bool running = true;
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
            if (e.type == SDL_EVENT_DROP_POSITION) state.drop.pos = ImVec2(e.drop.x, e.drop.y);
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data)
                state.drop.files.push_back({e.drop.data, ImVec2(e.drop.x, e.drop.y)});
            if (e.type == SDL_EVENT_DROP_COMPLETE) state.drop.hovering = false;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (args.screenshot) {
            io.DeltaTime = 1.0f / 60.0f;
            for (size_t i = 0; i < args.actions.size(); ++i) {
                const Action& a = args.actions[i];
                if (actionStart[i] < 0) {
                    if (frame == 0) io.AddKeyEvent(ImGuiMod_Ctrl, true);
                    continue;
                }
                if (frame >= actionStart[i] && frame < actionStart[i] + actionFrames(a)) playAction(io, a, frame - actionStart[i]);
            }
            if (args.mouseX >= 0 && frame >= timelineEnd) io.AddMousePosEvent(args.mouseX, args.mouseY);
            if (args.dragX >= 0) {
                state.drop.hovering = true;
                state.drop.pos = ImVec2(args.dragX, args.dragY);
            }
            if (!args.dropFile.empty() && frame == timelineEnd) state.drop.files.push_back({args.dropFile, ImVec2(args.dropX, args.dropY)});
        }
        // While the Game view has the keyboard, arrows and Enter are the controller:
        // they must not move ImGui's keyboard focus or press the button it lands on.
        // The script editor's arrows, Tab and Enter edit text.
        if ((state.play.gameFocus && state.play.game.attached()) || state.scripts.hasKeyboard())
            io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::NewFrame();
        ui::interactiveRects().clear();
        // The splash hides the editor until it starts fading out.
        g_hit.titleBar = splash::covering() ? ImRect() : mockup::drawMainScreen(state, doc, view, io.DisplaySize);
        splash::draw(io.DisplaySize, args.screenshot ? frame * 1000.0 / 60.0 : double(SDL_GetTicks()));
        g_hit.controls.clear();
        for (const ImRect& r : ui::interactiveRects())
            if (r.Overlaps(g_hit.titleBar)) g_hit.controls.push_back(r);
        ImGui::Render();
        if (state.quit) running = false;

        int fw, fh;
        SDL_GetWindowSizeInPixels(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (args.screenshot && ++frame >= args.frames) {
            std::vector<unsigned char> px((size_t)fw * fh * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            stbi_flip_vertically_on_write(1);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            if (!stbi_write_png(args.screenshot, fw, fh, 4, px.data(), fw * 4)) {
                std::fprintf(stderr, "failed to write %s\n", args.screenshot);
                return 1;
            }
            std::printf("wrote %s (%dx%d)\n", args.screenshot, fw, fh);
            // For scripted checks: menus and pickers left open by the input.
            std::printf("popups open: %s\n", ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ? "yes" : "no");
            std::printf("game attached: %s\n", state.play.game.attached() ? "yes" : "no");
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }

    state.play.emu.stop();
    splash::shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
