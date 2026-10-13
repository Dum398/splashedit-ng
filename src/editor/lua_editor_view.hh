// The script editor's ImGui side: a tab per open Lua file beside the Scene
// tab in the middle panel, and the code editor for the tab showing.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

class ScriptTabs {
public:
    ScriptTabs();
    ~ScriptTabs();

    // Shows `file` in its tab, opening one when needed. False with `error`
    // when it cannot be read.
    bool open(const std::filesystem::path& file, std::string* error);
    bool empty() const { return m_tabs.empty(); }
    // The Scene tab (the viewport) is showing.
    bool sceneShown() const { return m_active == kScene; }
    // The UI tab (the canvas editor) is showing.
    bool uiShown() const { return m_active == kUi; }
    void showUi() { m_active = kUi; }
    // The tab strip over `r`: Scene, UI, then a tab per open script.
    // Returns the rest of `r`.
    ImRect tabStrip(ImDrawList* dl, ImRect r);
    // The editor of the script showing, in `r`.
    void draw(ImDrawList* dl, ImRect r);
    // The editor has the keyboard: the main screen's shortcuts and ImGui's
    // keyboard navigation stay out of it.
    bool hasKeyboard() const;

    int dirtyCount() const;
    // Saves every changed script; false (with the first error) if one fails.
    bool saveAll(std::string* error);
    void closeAll();
    // Files moved on disk (absolute; a folder moves what is under it): the
    // tabs showing them follow.
    void moved(const std::filesystem::path& from, const std::filesystem::path& to);

    // Something rewrote script files on disk (a sequence rename, its undo):
    // each tab without unsaved changes whose file now reads differently
    // reloads it, the cursor kept where it can be.
    void reloadFromDisk();
    // Runs `fn` over the text of each tab of `files` with unsaved changes, as
    // one edit of that tab (undoable there). Returns how many tabs changed.
    int rewriteUnsaved(const std::vector<std::filesystem::path>& files,
                       const std::function<std::string(const std::string&)>& fn);

    // Files written since the last call, for the caller to refresh.
    std::vector<std::filesystem::path> takeSaved();
    // A save result for the status bar; empty when there is none.
    std::string notice;
    bool noticeBad = false;

    struct Tab;

private:
    void close(int i);
    void closePrompt();
    void pollDisk();
    double m_polledAt = -1;
    static constexpr int kScene = -1, kUi = -2;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_active = kScene;  // a script tab's index, or kScene / kUi
    int m_closing = -1;  // a changed tab waiting on the save prompt
    bool m_openClosePrompt = false;
    std::vector<std::filesystem::path> m_saved;
};

}  // namespace ui
