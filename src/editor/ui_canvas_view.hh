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

#include "editor/asset_browser.hh"
#include "editor/texture_preview_view.hh"
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

    // Element clipboard commands, on the selected element, for the keys, the
    // right-click menus and the Edit menu. The text goes through the system
    // clipboard (uic::copyElementText), so it survives between projects and
    // scene object text is never taken for it.
    // They act on the whole selection, each as one undo step. Paste lands the
    // group's top-left at the pointer when it is over the canvas (and not in a
    // menu), else 8 px down and right of where the originals sit.
    bool hasElement(const editor::Document& doc) const;
    // Edit > Rename and F2: the selected element's name becomes a text field in the list.
    void rename(editor::Document& doc);
    bool canRename(const editor::Document& doc) const;
    void copy(editor::Document& doc);
    void cut(editor::Document& doc);
    // `atPointer`: false for the Edit menu. `forced`: canvas pixel to land at, for a menu opened on the canvas.
    void paste(editor::Document& doc, bool atPointer = true, const std::array<int, 2>* forced = nullptr);
    // The pointer position (canvas pixels) when it is over the canvas pane and no menu is open.
    bool pointerOnCanvas(std::array<int, 2>* at) const;
    void duplicate(editor::Document& doc);
    void remove(editor::Document& doc);

    // A refusal the app shows in the status bar (main_screen takes and clears it).
    std::string status;
    bool statusBad = false;

    // Scripted tests (--ui): print where the canvas, the list and the
    // canvas's eye are whenever that changes.
    static bool reportLayout;

private:
    void list(ImDrawList* dl, ImRect r, editor::Document& doc);
    void canvas(ImDrawList* dl, ImRect r, editor::Document& doc);
    void keys(editor::Document& doc);
    void multiInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);
    void select(editor::Document& doc, int c, const std::vector<int>& idx, int primary);
    void elementInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);
    void canvasInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc);
    void upload(const editor::Document& doc, const std::vector<bool>& show);
    void elementMenu(editor::Document& doc);
    void notice(std::string msg, bool bad);
    // Zoom to `scale` keeping canvas point under screen point `at` in place.
    void zoomAt(float scale, ImVec2 at);

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
        // The whole selection being moved: indices and where each started.
        std::vector<int> idx;
        std::vector<editor::uic::Rect> rects;
        std::vector<std::array<int, 2>> froms, tos;
        int narrowTo = -1;  // a plain click in a multi-selection: select only this if it was not a drag
    } m_drag;
    // A rubber band dragged on empty canvas: canvas pixels, and what was
    // selected before (kept with Ctrl).
    struct Band {
        bool active = false, moved = false;
        ImVec2 start, now;
        std::vector<int> base;
    } m_band;
    int m_anchor = -1;       // the row a Shift+click range starts from
    bool m_renaming = false, m_renameStart = false;
    // Where a right click on the canvas opened the element menu (canvas pixels), for Paste.
    bool m_menuAtCanvas = false;
    std::array<int, 2> m_menuAt{};

    std::vector<editor::uic::Guide> m_guides;
    std::string m_dragLabel;
    bool m_snap = true;

    // The one screen <-> canvas transform: canvas pixel (x, y) is drawn at
    // m_origin + (x, y) * m_scale. Worked out at the start of each frame from
    // the zoom (0 = fit the area) and the pan (screen pixels from centred).
    float m_zoom = 0;
    ImVec2 m_pan{0, 0};
    ImRect m_area;
    float m_fit = 1, m_scale = 1;
    ImVec2 m_origin{0, 0};
    bool m_panning = false;
    int m_panButton = 0;
    ImVec2 m_panMouse, m_panStart;
    void layoutCanvas();
    ImVec2 toScreen(float x, float y) const { return ImVec2(m_origin.x + x * m_scale, m_origin.y + y * m_scale); }
    ImVec2 toCanvas(ImVec2 p) const { return ImVec2((p.x - m_origin.x) / m_scale, (p.y - m_origin.y) / m_scale); }

    // The Image inspector's Source vs PS1 preview, and what it last reported.
    TexturePreviewView m_texPreview;
    std::string m_texPreviewReported;
    // The Image inspector's cell picker: the sheet texture as the PS1 shows
    // it (path|bit depth|assets), and the last "image sheet" line reported.
    unsigned m_sheetTex = 0;
    std::string m_sheetKey, m_sheetReported;

    std::string m_notice;
    bool m_noticeBad = false;
    double m_noticeUntil = 0;

    // A row dragged in the element list.
    int m_rowDrag = -1;
    bool m_rowDragging = false;
    float m_listScroll = 0;

    std::string m_reported, m_listLayout, m_menuReported, m_inspReported, m_headReported;

    // The canvas inspector scrolls (the Scene fonts section makes it tall);
    // another canvas starts back at the top.
    float m_cvScroll = 0;
    int m_cvScrolled = -1;
    // The Scene fonts section: the project's font files (.ttf, .otf, .png),
    // listed again every couple of seconds; each font's sample line, drawn
    // by uic::renderSample into a texture; what the section last reported.
    void fontsSection(ImDrawList* dl, float x0, float x1, float* y, editor::Document& doc);
    std::vector<std::string> fontFiles(const editor::Document& doc);
    editor::AssetIndex m_fontFiles;
    double m_fontFilesAt = -1e9;
    struct FontSample {
        unsigned tex = 0;
        std::string key;
        int w = 0, h = 0;
    };
    std::array<FontSample, editor::uic::kMaxFonts> m_samples;
    std::vector<std::string> m_fontsReported;
    std::string m_fontsCountReported;
    // Scripted tests: the canvas's Default font and the element's Font field, last reported.
    std::string m_cvFontReported, m_elFontReported;
};

}  // namespace ui
