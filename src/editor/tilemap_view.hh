// The Tilemap tab beside Scene and UI: the scene's tilemap drawn with its
// tileset's cells, a brush palette, and painting with the mouse. The model is
// scene.hh's Tilemap; every edit goes through editor/tilemap.hh (one undo step
// a stroke).
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

namespace editor {
class Document;
}

namespace ui {

// The scene tilemap's tileset in the console's colours, on the GPU: shared by
// the Tilemap tab and the inspector's Tilemap section.
struct TilesetImage {
    unsigned tex = 0;  // GL texture, nearest filtered
    int width = 0, height = 0;
    int cellW = 0, cellH = 0, cols = 0, rows = 0;
    bool ready() const { return tex && cols > 0 && rows > 0; }
    int cells() const { return cols * rows; }
    // The texture coordinates of tileset cell `c`; false when it is not one.
    bool cellUv(int c, ImVec2* uv0, ImVec2* uv1) const;
};
// Converts the tileset in the background as it changes; not ready() while it
// converts, or when the scene has no tilemap or the tileset names no sheet.
const TilesetImage& tilesetImage(const editor::Document& doc);
// Tileset cell `c` drawn in `r` (a hatched box for a hole, -1; a red cross for
// a cell the sheet does not have).
void drawTileCell(ImDrawList* dl, const TilesetImage& img, int c, ImRect r);

class TilemapView {
public:
    // The palette and the map, in `r`.
    void draw(ImDrawList* dl, ImRect r, editor::Document& doc);
    // The brush that paints (-1: the eraser).
    int brush() const { return m_brush; }
    // What a left click on the map does. Shift (a rectangle) and Alt (pick)
    // work in every tool; the right button erases in every tool.
    enum class Tool { Paint, Rect, Fill, Pick };
    Tool tool() const { return m_tool; }

private:
    void tools(ImDrawList* dl, ImRect r);
    void palette(ImDrawList* dl, ImRect r, editor::Document& doc);
    void map(ImDrawList* dl, ImRect r, editor::Document& doc);
    void endStroke(editor::Document& doc);

    int m_brush = 0;
    Tool m_tool = Tool::Paint;
    unsigned m_seenLoad = ~0u;
    // The view: zoom (screen pixels per map pixel, 0 = fit on the next frame) and pan from centred.
    float m_zoom = 0;
    ImVec2 m_pan{0, 0};
    bool m_panning = false;
    ImVec2 m_panMouse{0, 0}, m_panStart{0, 0};
    // A stroke: the button held (-1: none), what it paints, the last cell, the
    // cells before it (to count what it changed) and its undo merge key.
    int m_button = -1;
    bool m_rect = false;  // Shift: a rectangle from m_from to the pointer
    int m_paint = 0;
    int m_from[2] = {0, 0}, m_last[2] = {0, 0}, m_min[2] = {0, 0}, m_max[2] = {0, 0};
    std::vector<int> m_before;
    int m_strokes = 0;
    std::string m_key;
    std::string m_reported;
};

}  // namespace ui
