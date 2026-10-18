#include "ui/file_dialog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ui/dialog_gate.hh"
#include "ui/widgets.h"

namespace ui {

using namespace theme;

namespace {

DialogGate g_gate;
SDL_Window* g_window = nullptr;
std::mutex g_folderMutex;
std::string g_folder;
// The open dialog's own callback, called before the gate opens again.
SDL_DialogFileCallback g_callback = nullptr;
void* g_userdata = nullptr;

bool held() {
    const char* v = std::getenv("SPLASHEDIT_FILE_DIALOG");
    return v && !std::strcmp(v, "hold");
}

void SDLCALL answered(void*, const char* const* files, int filter) {
    if (g_callback) g_callback(g_userdata, files, filter);
    g_gate.leave();
}

// Takes the gate and settles the folder; false when another dialog is open.
bool begin(const char* kind, SDL_DialogFileCallback callback, void* userdata, const char* defaultLocation, std::string* folder) {
    if (!g_gate.enter()) {
        std::printf("file dialog: refused %s, one is open\n", kind);
        std::fflush(stdout);
        return false;
    }
    g_callback = callback;
    g_userdata = userdata;
    if (defaultLocation) {
        *folder = defaultLocation;
    } else {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        *folder = g_folder;
    }
    if (held()) {
        std::printf("file dialog: %s in %s\n", kind, folder->c_str());
        std::fflush(stdout);
    }
    return true;
}

const char* orNull(const std::string& s) { return s.empty() ? nullptr : s.c_str(); }

}  // namespace

void setFileDialogWindow(SDL_Window* window) { g_window = window; }

void setFileDialogFolder(const std::string& utf8Folder) {
    std::lock_guard<std::mutex> lock(g_folderMutex);
    g_folder = utf8Folder;
}

bool fileDialogOpen() { return g_gate.open(); }

bool showOpenFileDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const SDL_DialogFileFilter* filters,
                        int nfilters, const char* defaultLocation, bool allowMany) {
    std::string folder;
    if (!begin(kind, callback, userdata, defaultLocation, &folder)) return false;
    if (!held()) SDL_ShowOpenFileDialog(answered, nullptr, g_window, filters, nfilters, orNull(folder), allowMany);
    return true;
}

bool showSaveFileDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const SDL_DialogFileFilter* filters,
                        int nfilters, const char* defaultLocation) {
    std::string folder;
    if (!begin(kind, callback, userdata, defaultLocation, &folder)) return false;
    if (!held()) SDL_ShowSaveFileDialog(answered, nullptr, g_window, filters, nfilters, orNull(folder));
    return true;
}

bool showOpenFolderDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const char* defaultLocation, bool allowMany) {
    std::string folder;
    if (!begin(kind, callback, userdata, defaultLocation, &folder)) return false;
    if (!held()) SDL_ShowOpenFolderDialog(answered, nullptr, g_window, orNull(folder), allowMany);
    return true;
}

void drawFileDialogShade() {
    if (!fileDialogOpen()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    dl->AddRectFilled(ImVec2(0, 0), d, rgb(0x000000, 110));
    Fonts& f = fonts();
    const char* msg = "Waiting for the file dialog";
    const ImVec2 s = measure(f.medium, theme::type::body, msg);
    const ImRect pill(ImVec2((d.x - s.x) * 0.5f - theme::space::lg, d.y * 0.5f - 18),
                      ImVec2((d.x + s.x) * 0.5f + theme::space::lg, d.y * 0.5f + 18));
    dl->AddRectFilled(pill.Min, pill.Max, theme::color::raised, theme::radius::pill);
    dl->AddRect(pill.Min, pill.Max, theme::color::borderStrong, theme::radius::pill);
    textCentered(dl, pill, f.medium, theme::type::body, theme::color::text, msg);
}

}  // namespace ui
