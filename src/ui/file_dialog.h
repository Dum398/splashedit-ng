// The native file dialogs, modal: each is parented to the editor's window
// (where the platform supports it), only one is open at a time, and while it
// is the editor takes no input of its own (main.cpp drops it) and shades its
// window. Every SDL_Show*Dialog call in the editor goes through here.
#pragma once

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_video.h>

#include <string>

namespace ui {

// The window dialogs are modal to; set once at startup.
void setFileDialogWindow(SDL_Window* window);
// The folder a dialog opens in when its caller names none: the open project.
void setFileDialogFolder(const std::string& utf8Folder);
// A dialog is open: the editor ignores its own input until it answers.
bool fileDialogOpen();

// As SDL_ShowOpenFileDialog and friends, modal to the editor's window and
// opening in `defaultLocation` (else the project folder). False, with nothing
// shown, while another dialog is open. `kind` names the dialog for scripted
// tests: with SPLASHEDIT_FILE_DIALOG=hold in the environment no dialog shows;
// the editor prints "file dialog: <kind> in <folder>" (or "file dialog: refused
// <kind>, one is open") and acts as if one stayed open.
bool showOpenFileDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const SDL_DialogFileFilter* filters,
                        int nfilters, const char* defaultLocation, bool allowMany);
bool showSaveFileDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const SDL_DialogFileFilter* filters,
                        int nfilters, const char* defaultLocation);
bool showOpenFolderDialog(const char* kind, SDL_DialogFileCallback callback, void* userdata, const char* defaultLocation, bool allowMany);

// While a dialog is open: shades the window and says what it waits for.
void drawFileDialogShade();

}  // namespace ui
