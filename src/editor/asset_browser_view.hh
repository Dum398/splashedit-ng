// The Project panel: the project folder's files, docked under the middle
// panel. A folder tree on the left; the files of the folder picked (or every
// match of the search) as a grid with texture thumbnails, or as a list. Files
// drag out onto the viewport, the scene tree or an inspector asset field;
// double-click opens; right-click has Rename, Delete, Reveal and Reimport.
// The listing comes from editor::AssetIndex, polled about once a second.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "editor/asset_browser.hh"

namespace editor {
class Document;
}

namespace ui {

class AssetBrowser {
  public:
    // What the panel asks the main screen to do, collected by takeRequests().
    struct Request {
        enum Kind {
            Open,      // double-click or Open: a script, scene or model to open or place
            Drop,      // an asset released outside the panel at `pos`
            Reimport,  // a model in models/ with a sidecar
            Select,    // select the object at `object` in the open scene
            Notice,    // a message for the status bar (`bad`: an error)
        } kind;
        std::string path;  // project-relative
        editor::AssetType type = editor::AssetType::Other;
        ImVec2 pos{};
        std::vector<int> object;
        std::string text;
        bool bad = false;
    };

    bool open = true;  // collapsed to its header when false
    // The bottom dock's tab strip, drawn at the left of the header in place
    // of the panel's own Project tab when set (`tabsWidth` wide).
    std::function<void(ImDrawList*, ImRect)> tabs;
    float tabsWidth = 0;

    // Takes the bottom of `area` (a drag bar resizes it); returns what is left above.
    ImRect dock(ImRect area);
    // Draws in the rect dock() took; rescans the project about once a second.
    void draw(ImDrawList* dl, editor::Document& doc);
    std::vector<Request> takeRequests();

    // The asset being dragged out of the panel, while the button is held past
    // the drag threshold; null otherwise.
    const editor::AssetEntry* dragging() const;
    // A click in the panel gives it the keyboard (F2, Delete, Enter) until a click elsewhere.
    bool focused() const { return m_focused; }
    bool hasKeyboard() const;
    ImRect rect() const { return m_rect; }
    // The names shown in the file area, in order, '|'-separated (scripted checks).
    std::string shownList() const;

    const editor::AssetIndex& index() const { return m_index; }
    // editor::convertBlocksToLua on `path` (project-relative) with the
    // panel's listing (scanned again first) and open scene; an error goes to
    // the status bar. The listing and references are updated after.
    editor::AssetOp convertBlocks(editor::Document& doc, const std::string& path);
    const editor::AssetReferences& references() const { return m_refs; }

  private:
    void header(ImDrawList* dl, ImRect r);
    void folderTree(ImDrawList* dl, ImRect r);
    void files(ImDrawList* dl, ImRect r, editor::Document& doc);
    void footer(ImDrawList* dl, ImRect r);
    void itemInput(const editor::AssetEntry& e, ImRect r, bool hovered);
    void menu(editor::Document& doc);
    void prompt(editor::Document& doc);
    void commitRename(editor::Document& doc, const std::string& typed);
    void openEntry(const editor::AssetEntry& e);
    void after(editor::Document& doc, const editor::AssetOp& op, const std::string& done);
    // Moves `asset` into `folder` (a drop on a folder), its references along.
    void moveInto(editor::Document& doc, const std::string& asset, const std::string& folder);
    // It is the open scene, or a folder holding it.
    bool holdsOpenScene(const editor::AssetEntry& e) const;
    unsigned thumbnail(const editor::AssetEntry& e, int* w, int* h);
    void dropThumbnails();
    void notice(const std::string& text, bool bad) { m_requests.push_back({Request::Notice, {}, {}, {}, {}, text, bad}); }

    editor::AssetIndex m_index;
    editor::AssetReferences m_refs;
    double m_scannedAt = -1;
    std::string m_openScene;

    float m_height = 220;
    ImRect m_rect;
    bool m_focused = false;
    bool m_list = false;  // list view instead of the grid
    std::string m_folder;
    std::string m_query;
    bool m_searching = false;
    uint32_t m_types = 0;
    std::map<std::string, bool> m_collapsed;  // folder tree rows closed by the user
    float m_scroll = 0, m_treeScroll = 0;
    std::string m_selected;
    std::vector<std::string> m_shown;

    // Drag out: the asset pressed and whether it moved far enough. Folders
    // drawn this frame (tree rows, tiles) take a drop: a move.
    std::string m_pressed;
    bool m_dragging = false;
    std::vector<std::pair<ImRect, std::string>> m_folderTargets;

    // Rename in place, and the menu and the prompt (delete, or a refusal).
    std::string m_renaming;
    bool m_renameStart = false;
    std::string m_menuFor;
    bool m_openMenu = false;
    ImVec2 m_menuPos;
    enum class Ask { None, Delete, CannotRename, CannotDelete } m_ask = Ask::None;
    std::string m_askFor;
    bool m_openAsk = false;

    struct Thumb {
        unsigned tex = 0;
        int w = 0, h = 0;
        std::filesystem::file_time_type mtime{};
        bool failed = false;
    };
    std::map<std::string, Thumb> m_thumbs;
    int m_thumbLoads = 0;  // this frame's, capped

    std::vector<Request> m_requests;
};

}  // namespace ui
