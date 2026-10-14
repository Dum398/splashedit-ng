// The script editor's ImGui side: a tab per open Lua file beside the Scene
// tab in the middle panel, and the code editor for the tab showing.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
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
    bool sceneShown() const { return m_active < 0; }
    // The tab strip over `r` while scripts are open; returns the rest of `r`.
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
    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_active = -1;
    int m_closing = -1;  // a changed tab waiting on the save prompt
    bool m_openClosePrompt = false;
    std::vector<std::filesystem::path> m_saved;
};

}  // namespace ui
