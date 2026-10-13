// The UI tab beside the Scene: the scene's UI canvases at PS1 resolution,
// drawn the way psxsplash draws them, with the element list, mouse and
// keyboard editing, and the inspector for the selected element or canvas.
// The model is ui_canvas.hh; every edit goes through the Document.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <string>
#include <vector>

#include "editor/ui_canvas.hh"

namespace editor {
class Document;
}

namespace ui {

class CanvasView {
public:
    CanvasView();
    ~CanvasView();
    CanvasView(const CanvasView&) = delete;
    CanvasView& operator=(const CanvasView&) = delete;

    // The element list and the canvas, in `r`. Also takes the UI tab's keys.
    void draw(ImDrawList* dl, ImRect r, editor::Document& doc);
    // The inspector's body from `y` down in panel `r`: the selected element,
    // or the canvas when none is selected.
    void inspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);

    // Scripted tests (--ui): print where the canvas, the list and the
    // canvas's eye are whenever that changes.
    static bool reportLayout;

private:
    void list(ImDrawList* dl, ImRect r, editor::Document& doc);
    void canvas(ImDrawList* dl, ImRect r, editor::Document& doc);
    void keys(editor::Document& doc);
    void elementInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);
    void canvasInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);
    void upload(const editor::Document& doc, const std::vector<bool>& show);

    editor::uic::Assets m_assets;
    editor::uic::Screen m_screen;
    unsigned m_tex = 0;
    // What the texture shows: document revision, load, assets, canvases shown.
    unsigned m_texRev = ~0u, m_texLoad = ~0u, m_texAssets = ~0u;
    std::vector<bool> m_texShow;
    // Rows that have a problem, worked out once per revision.
    unsigned m_warnRev = ~0u, m_warnLoad = ~0u;
    std::vector<bool> m_rowWarn;

    // A drag on the canvas: which element, by which handle, from where.
    struct Drag {
        int canvas = -1, element = -1;
        editor::uic::Handle handle = editor::uic::Handle::None;
        ImVec2 mouse;
        editor::uic::Rect rect;
        std::array<int, 2> from{}, to{};
        bool moved = false;
    } m_drag;
    std::vector<editor::uic::Guide> m_guides;
    std::string m_dragLabel;
    bool m_snap = true;

    // A row dragged in the element list.
    int m_rowDrag = -1;
    bool m_rowDragging = false;
    float m_listScroll = 0;

    std::string m_reported, m_listLayout;
};

}  // namespace ui
