#include "editor/tilemap_view.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

#include "editor/document.hh"
#include "editor/texture_preview.hh"
#include "editor/tilemap.hh"
#include "gl.h"
#include "splashpack.hh"
#include "ui/icons.h"
#include "ui/texture_image.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;

namespace {

// Scripted runs print `line`; with a `tag`, only when it differs from the
// last line printed under that tag.
void scriptLine(const char* tag, const std::string& line) {
    if (!namedRectsOn()) return;
    if (tag) {
        static std::map<std::string, std::string> printed;
        std::string& last = printed[tag];
        if (last == line) return;
        last = line;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

ImU32 tint(const std::array<float, 3>& c, unsigned a) {
    auto b = [](float v) { return unsigned(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return IM_COL32(b(c[0]), b(c[1]), b(c[2]), a);
}

// Zoom steps the wheel moves through: whole steps from 1 up keep texels square.
constexpr float kZooms[] = {0.25f, 0.5f, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
// The tool strip's (and the map header's) height.
constexpr float kToolH = 30;
// The grid's alphas: a dark band under a light line.
constexpr int kGridDark = 100, kGridLight = 64;

float stepZoom(float z, int dir) {
    if (dir > 0) {
        for (float s : kZooms)
            if (s > z + 1e-3f) return s;
        return kZooms[std::size(kZooms) - 1];
    }
    for (int i = int(std::size(kZooms)) - 1; i >= 0; i--)
        if (kZooms[i] < z - 1e-3f) return kZooms[i];
    return kZooms[0];
}

}  // namespace

bool TilesetImage::cellUv(int c, ImVec2* uv0, ImVec2* uv1) const {
    if (!ready() || c < 0 || c >= cells()) return false;
    const float u = float((c % cols) * cellW), v = float((c / cols) * cellH);
    *uv0 = ImVec2(u / width, v / height);
    *uv1 = ImVec2((u + cellW) / width, (v + cellH) / height);
    return true;
}

const TilesetImage& tilesetImage(const editor::Document& doc) {
    static TilesetImage img;
    // Never destroyed: its worker thread is left to the process exit.
    static editor::TexturePreviewer& previewer = *new editor::TexturePreviewer;
    static std::shared_ptr<const editor::TexturePreview> uploaded;
    img.cols = img.rows = 0;
    const std::optional<splash::Tilemap>& tm = doc.scene().tilemap;
    const int si = tm ? splash::tilesetIndex(*tm, doc.scene().spriteSheets) : -1;
    if (si < 0) return img;
    const splash::SpriteSheet& sheet = doc.scene().spriteSheets[size_t(si)];
    if (sheet.texture.empty() || sheet.cellW < 1 || sheet.cellH < 1) return img;
    const editor::TexturePreviewKey key{doc.projectRoot() / fromUtf8(sheet.texture), sheet.bitDepth, true};
    previewer.request(key);
    std::shared_ptr<const editor::TexturePreview> pv = previewer.latest();
    if (!pv || pv->key != key || !pv->error.empty()) return img;
    if (pv != uploaded) {
        uploaded = pv;
        if (!img.tex) glGenTextures(1, &img.tex);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pv->width, pv->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pv->ps1.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        img.width = pv->width;
        img.height = pv->height;
    }
    img.cellW = sheet.cellW;
    img.cellH = sheet.cellH;
    if (img.width % sheet.cellW == 0 && img.height % sheet.cellH == 0) {
        img.cols = img.width / sheet.cellW;
        img.rows = img.height / sheet.cellH;
    }
    return img;
}

void drawTileCell(ImDrawList* dl, const TilesetImage& img, int c, ImRect r) {
    ImVec2 uv0, uv1;
    if (c < 0) {
        // A hole: nothing is drawn there.
        dl->AddRectFilled(r.Min, r.Max, rgb(0x0c0d10));
        dl->PushClipRect(r.Min, r.Max, true);
        const float step = 5;
        for (float d = -r.GetHeight(); d < r.GetWidth(); d += step)
            dl->AddLine(ImVec2(r.Min.x + d, r.Max.y), ImVec2(r.Min.x + d + r.GetHeight(), r.Min.y), rgb(0xffffff, 28));
        dl->PopClipRect();
    } else if (img.cellUv(c, &uv0, &uv1)) {
        dl->AddRectFilled(r.Min, r.Max, rgb(0x1f232b));
        addTextureImage(dl, (ImTextureID)(intptr_t)img.tex, r.Min, r.Max, uv0, uv1);
    } else {
        dl->AddRectFilled(r.Min, r.Max, rgb(0x1f232b));
        if (img.ready()) {
            dl->AddLine(r.Min + ImVec2(3, 3), r.Max - ImVec2(3, 3), color::bad, 1.5f);
            dl->AddLine(ImVec2(r.Max.x - 3, r.Min.y + 3), ImVec2(r.Min.x + 3, r.Max.y - 3), color::bad, 1.5f);
        }
    }
}

void TilemapView::draw(ImDrawList* dl, ImRect r, editor::Document& doc) {
    if (m_seenLoad != doc.loadId()) {
        m_seenLoad = doc.loadId();
        m_brush = 0;
        m_zoom = 0;
        m_pan = ImVec2(0, 0);
        m_button = -1;
    }
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window, ImDrawFlags_RoundCornersBottom);
    const std::optional<splash::Tilemap>& tm = doc.scene().tilemap;
    if (!tm) {
        textCentered(dl, r, fonts().regular, type::body, color::textFaint, "No tilemap: add one in the scene inspector");
        return;
    }
    m_brush = std::clamp(m_brush, -1, int(tm->brushes.size()) - 1);
    const float pw = 216;
    palette(dl, ImRect(r.Min, ImVec2(r.Min.x + pw, r.Max.y)), doc);
    map(dl, ImRect(ImVec2(r.Min.x + pw, r.Min.y), r.Max), doc);
}

void TilemapView::tools(ImDrawList* dl, ImRect r) {
    struct ToolButton {
        Tool tool;
        const char* icon;
        const char* name;
        const char* tip;
    };
    static const ToolButton buttons[] = {
        {Tool::Paint, icon::pencil, "paint", "Paint: click or drag to draw the brush, the right button erases.\n"
                                             "Shift+drag: a rectangle, Alt+click: pick, in any tool."},
        {Tool::Rect, icon::squareDashed, "rect", "Rectangle: drag to fill one with the brush, the right button erases one.\n"
                                                 "Shift+drag does this in any tool."},
        {Tool::Fill, icon::paintBucket, "fill", "Fill: the connected cells like the one clicked take the brush; the right "
                                                "button empties them.\nShift+drag: a rectangle, Alt+click: pick."},
        {Tool::Pick, icon::pipette, "pick", "Pick: click a cell to take its brush, then back to Paint.\n"
                                            "Alt+click picks in any tool."},
    };
    const float gap = space::xs, w = std::floor((r.GetWidth() - gap * 3) / 4);
    for (int i = 0; i < 4; i++) {
        const ToolButton& b = buttons[i];
        const ImRect br(ImVec2(r.Min.x + float(i) * (w + gap), r.Min.y), ImVec2(r.Min.x + float(i) * (w + gap) + w, r.Max.y));
        nameRect(std::string("tiles.tool.") + b.name, br);
        const bool on = m_tool == b.tool;
        if (!on) dl->AddRect(br.Min, br.Max, color::border, radius::button);
        if (iconButton((std::string("tiletool") + b.name).c_str(), br, b.icon, on, b.tip)) m_tool = b.tool;
    }
    static const char* const names[] = {"paint", "rect", "fill", "pick"};
    scriptLine("tilestool", std::string("tiles: tool ") + names[int(m_tool)]);
}

void TilemapView::palette(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    const splash::Tilemap& t = *doc.scene().tilemap;
    const TilesetImage& img = tilesetImage(doc);
    dl->AddLine(ImVec2(r.Max.x, r.Min.y + space::sm), ImVec2(r.Max.x, r.Max.y - space::sm), color::border);
    float y = r.Min.y + space::md;
    tools(dl, ImRect(ImVec2(r.Min.x + space::sm, y), ImVec2(r.Max.x - space::sm, y + kToolH)));
    y += kToolH + space::md;
    text(dl, ImVec2(r.Min.x + space::md, y), f.semibold, type::label, color::textDim, "Brushes");
    y += 22;
    ImGui::PushClipRect(r.Min, r.Max, true);
    const float rowH = 40;
    for (int i = -1; i < int(t.brushes.size()); i++) {
        ImGui::PushID(i + 1);
        const ImRect rr(ImVec2(r.Min.x + space::sm, y), ImVec2(r.Max.x - space::sm, y + rowH));
        Hit h = interact("tilebrush", rr);
        nameRect(i < 0 ? std::string("tiles.brush.erase") : "tiles.brush." + std::to_string(i), rr);
        if (i == m_brush) {
            dl->AddRectFilled(rr.Min, rr.Max, color::accentSoft, radius::field);
            dl->AddRectFilled(rr.Min + ImVec2(0, 8), ImVec2(rr.Min.x + 2, rr.Max.y - 8), color::accent, 1);
        } else if (h.hover > 0) {
            dl->AddRectFilled(rr.Min, rr.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, h.hover), radius::field);
        }
        const ImRect sw(ImVec2(rr.Min.x + space::sm, rr.Min.y + 5), ImVec2(rr.Min.x + space::sm + 30, rr.Min.y + 35));
        const float tx = sw.Max.x + space::sm;
        if (i < 0) {
            dl->AddRect(sw.Min, sw.Max, color::borderStrong, radius::field);
            textCentered(dl, sw, f.medium, type::icon, color::textDim, icon::x);
            text(dl, ImVec2(tx, rr.Min.y + 4), f.medium, type::body, i == m_brush ? color::text : color::textDim, "Eraser");
            text(dl, ImVec2(tx, rr.Min.y + 21), f.regular, type::caption, color::textFaint, "or the right button");
        } else {
            const splash::TileBrush& b = t.brushes[size_t(i)];
            drawTileCell(dl, img, b.cell, sw);
            dl->AddRect(sw.Min - ImVec2(1, 1), sw.Max + ImVec2(1, 1), tint(b.color, 200), 0, 0, 1.5f);
            ImGui::PushClipRect(ImVec2(tx, rr.Min.y), ImVec2(rr.Max.x - space::xs, rr.Max.y), true);
            text(dl, ImVec2(tx, rr.Min.y + 4), f.medium, type::body, i == m_brush ? color::text : color::textDim,
                 b.name.empty() ? "(no name)" : b.name.c_str());
            std::string meta = b.walkable ? "walkable" : "solid";
            if (b.kind != 0) {
                const std::string kn = b.kind > 0 && b.kind < int(t.kindNames.size()) && !t.kindNames[size_t(b.kind)].empty()
                                           ? t.kindNames[size_t(b.kind)]
                                           : "kind " + std::to_string(b.kind);
                meta += ", " + kn + (b.id >= 0 ? " " + std::to_string(b.id) : "");
            }
            text(dl, ImVec2(tx, rr.Min.y + 21), f.regular, type::caption, color::textFaint, meta.c_str());
            ImGui::PopClipRect();
        }
        if (h.clicked) m_brush = i;
        ImGui::PopID();
        y += rowH + 2;
    }
    ImGui::PopClipRect();
    // How to paint, at the bottom.
    static const char* const help[] = {"Left: the tool, right: erase", "Shift+drag: fill a rectangle", "Alt+click: pick the brush",
                                       "Wheel: zoom, middle drag: pan"};
    float hy = r.Max.y - space::md - 17 * float(std::size(help));
    if (hy > y + space::md)
        for (const char* l : help) {
            text(dl, ImVec2(r.Min.x + space::md, hy), f.regular, type::caption, color::textFaint, l);
            hy += 17;
        }
}

void TilemapView::endStroke(editor::Document& doc) {
    m_button = -1;
    doc.endMerge();
    const std::optional<splash::Tilemap>& tm = doc.scene().tilemap;
    if (!tm) return;
    int changed = 0;
    if (m_before.size() == tm->cells.size())
        for (size_t i = 0; i < m_before.size(); i++) changed += m_before[i] != tm->cells[i];
    // Scripted runs: one line a stroke, "tiles: paint brush 0 cells 4 at 1,1..4,1",
    // "tiles: fill brush 1 ...", "tiles: erase cells 2 ..." (a rectangle: "fill erase").
    std::string what = m_rect ? "fill " : "";
    what += m_paint < 0 ? std::string("erase") : (m_rect ? "brush " : "paint brush ") + std::to_string(m_paint);
    char line[160];
    std::snprintf(line, sizeof line, "tiles: %s cells %d at %d,%d..%d,%d", what.c_str(), changed, m_min[0], m_min[1], m_max[0],
                  m_max[1]);
    scriptLine(nullptr, line);
}

void TilemapView::map(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    ImGuiIO& io = ImGui::GetIO();
    const splash::Tilemap& t = *doc.scene().tilemap;
    const TilesetImage& img = tilesetImage(doc);
    // The tile size is the tileset's cell (16 x 16 until there is one).
    const int si = splash::tilesetIndex(t, doc.scene().spriteSheets);
    const splash::SpriteSheet* sheet = si >= 0 ? &doc.scene().spriteSheets[size_t(si)] : nullptr;
    const int tw = sheet ? std::clamp(sheet->cellW, 1, 255) : 16, th = sheet ? std::clamp(sheet->cellH, 1, 255) : 16;
    // The header, level with the tool strip: view toggles, right-aligned.
    const ImRect hdr(r.Min + ImVec2(space::md, space::md), ImVec2(r.Max.x - space::md, r.Min.y + space::md + kToolH));
    {
        const char* label = "Grid";
        const float gw = 16 + space::xs + measure(f.medium, type::label, label).x + space::md * 2;
        const ImRect gb(ImVec2(hdr.Max.x - gw, hdr.Min.y), hdr.Max);
        nameRect("tiles.grid", gb);
        Hit gh = interact("tilegrid", gb);
        tooltip("Grid lines on the tile edges. A view setting: not saved in the scene.");
        if (m_grid) dl->AddRectFilled(gb.Min, gb.Max, color::accentSoft, radius::button);
        else if (gh.hover > 0) dl->AddRectFilled(gb.Min, gb.Max, rgb(0x2a2f39, int(255 * gh.hover)), radius::button);
        if (!m_grid) dl->AddRect(gb.Min, gb.Max, color::border, radius::button);
        const ImU32 fg = m_grid ? color::accentHover : lerpColor(color::textDim, color::text, gh.hover);
        const float cy = gb.GetCenter().y;
        textCentered(dl, ImRect(ImVec2(gb.Min.x + space::md, gb.Min.y), ImVec2(gb.Min.x + space::md + 16, gb.Max.y)), f.medium, type::icon,
                     fg, icon::grid);
        const ImVec2 ls = measure(f.medium, type::label, label);
        text(dl, ImVec2(gb.Min.x + space::md + 16 + space::xs, cy - ls.y * 0.5f), f.medium, type::label, fg, label);
        if (gh.clicked) m_grid = !m_grid;
        scriptLine("tilesgrid", std::string("tiles: grid ") + (m_grid ? "on" : "off"));
    }
    const ImRect area(ImVec2(r.Min.x + space::md, hdr.Max.y + space::sm), r.Max - ImVec2(space::md, space::md + 22));
    nameRect("tiles.map", area);
    const float mw = float(t.width * tw), mh = float(t.height * th);
    if (m_zoom <= 0) {
        // Fit: whole steps when the map fits at 1:1 or more.
        const float fit = std::min(area.GetWidth() / mw, area.GetHeight() / mh);
        m_zoom = fit >= 1 ? std::floor(fit) : kZooms[0];
        for (float s : kZooms)
            if (s <= fit) m_zoom = std::max(m_zoom, s);
        m_pan = ImVec2(0, 0);
    }
    const float z = m_zoom;
    auto originOf = [&](ImVec2 pan) {
        return ImVec2(std::round(area.GetCenter().x - mw * z * 0.5f + pan.x), std::round(area.GetCenter().y - mh * z * 0.5f + pan.y));
    };

    // Zoom about the pointer; pan with the middle button.
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    Hit h = interact("tilemaparea", area);
    const bool over = !popup && h.hovered;
    if (over && io.MouseWheel != 0 && m_button < 0) {
        const float nz = stepZoom(z, io.MouseWheel > 0 ? 1 : -1);
        if (nz != z) {
            // Keep the map point under the pointer where it is.
            const ImVec2 o = originOf(m_pan);
            const ImVec2 p((io.MousePos.x - o.x) / z, (io.MousePos.y - o.y) / z);
            const ImVec2 no(area.GetCenter().x - mw * nz * 0.5f, area.GetCenter().y - mh * nz * 0.5f);
            m_pan = ImVec2(io.MousePos.x - p.x * nz - no.x, io.MousePos.y - p.y * nz - no.y);
            m_zoom = nz;
        }
    }
    if (!m_panning && over && m_button < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        m_panning = true;
        m_panMouse = io.MousePos;
        m_panStart = m_pan;
    }
    if (m_panning) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_panning = false;
        else m_pan = m_panStart + (io.MousePos - m_panMouse);
    }
    const float zz = m_zoom;
    const ImVec2 o = originOf(m_pan);
    // Screen x of the left edge of column x (and y of row y): tiles and grid
    // lines both come from these, so they line up at every zoom.
    auto sx = [&](int x) { return o.x + std::round(float(x * tw) * zz); };
    auto sy = [&](int y) { return o.y + std::round(float(y * th) * zz); };
    auto cellAt = [&](ImVec2 p, int* cx, int* cy) {
        *cx = int(std::floor((p.x - o.x) / (tw * zz)));
        *cy = int(std::floor((p.y - o.y) / (th * zz)));
        return *cx >= 0 && *cy >= 0 && *cx < t.width && *cy < t.height;
    };

    // The map: a checkerboard where nothing is drawn, then the tiles.
    ImGui::PushClipRect(area.Min, area.Max, true);
    dl->AddRectFilled(area.Min, area.Max, rgb(0x121419), radius::field);
    const ImVec2 end(sx(t.width), sy(t.height));
    const int x0 = std::max(0, int(std::floor((area.Min.x - o.x) / (tw * zz)))), x1 = std::min(t.width - 1, int((area.Max.x - o.x) / (tw * zz)));
    const int y0 = std::max(0, int(std::floor((area.Min.y - o.y) / (th * zz)))), y1 = std::min(t.height - 1, int((area.Max.y - o.y) / (th * zz)));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const ImU32 c = (x + y) & 1 ? rgb(0x1a1d23) : rgb(0x16181d);
            dl->AddRectFilled(ImVec2(sx(x), sy(y)), ImVec2(sx(x + 1), sy(y + 1)), c);
        }
    auto brushOf = [&](int b) -> const splash::TileBrush* { return b >= 0 && b < int(t.brushes.size()) ? &t.brushes[size_t(b)] : nullptr; };
    if (img.ready())
        addTextureImages(dl, [&] {
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    const splash::TileBrush* b = brushOf(t.at(x, y));
                    ImVec2 uv0, uv1;
                    if (b && img.cellUv(b->cell, &uv0, &uv1))
                        dl->AddImage((ImTextureID)(intptr_t)img.tex, ImVec2(sx(x), sy(y)), ImVec2(sx(x + 1), sy(y + 1)), uv0, uv1);
                }
        });
    // What the texture does not show: cells with a missing brush or a cell
    // past the sheet (red), and the objects (their brush's tint, in a corner).
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const int bi = t.at(x, y);
            if (bi < 0) continue;
            const splash::TileBrush* b = brushOf(bi);
            const ImRect cr(ImVec2(sx(x), sy(y)), ImVec2(sx(x + 1), sy(y + 1)));
            if (!b || (img.ready() && b->cell >= img.cells())) {
                dl->AddRectFilled(cr.Min, cr.Max, rgb(0xef5a6f, 90));
                continue;
            }
            if (b->kind != 0) {
                const float d = std::clamp(cr.GetWidth() * 0.3f, 3.0f, 8.0f);
                dl->AddRectFilled(cr.Min + ImVec2(1, 1), cr.Min + ImVec2(1 + d, 1 + d), tint(b->color, 230));
            }
        }
    // Grid lines on the tile edges (left out when tiles are tiny): a light
    // pixel line over a dark band a pixel wider each side, so it reads over
    // dark and busy tiles alike. Whole-pixel rectangles keep them crisp.
    if (m_grid && tw * zz >= 6 && th * zz >= 6) {
        const float gy0 = std::max(o.y, area.Min.y), gy1 = std::min(end.y, area.Max.y);
        const float gx0 = std::max(o.x, area.Min.x), gx1 = std::min(end.x, area.Max.x);
        for (int pass = 0; pass < 2; pass++) {
            const float w = pass ? 0 : 1;
            const ImU32 c = pass ? rgb(0xffffff, kGridLight) : rgb(0x000000, kGridDark);
            for (int x = x0; x <= x1 + 1; x++) dl->AddRectFilled(ImVec2(sx(x) - w, gy0), ImVec2(sx(x) + 1 + w, gy1), c);
            for (int y = y0; y <= y1 + 1; y++) dl->AddRectFilled(ImVec2(gx0, sy(y) - w), ImVec2(gx1, sy(y) + 1 + w), c);
        }
    }
    dl->AddRect(o - ImVec2(1, 1), end + ImVec2(1, 1), color::borderStrong);
    // Scripted runs aim at cells by name: tiles.cell.<x>.<y>, the ones in view.
    if (namedRectsOn() && (x1 - x0 + 1) * (y1 - y0 + 1) <= 4096)
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                nameRect("tiles.cell." + std::to_string(x) + "." + std::to_string(y), ImRect(ImVec2(sx(x), sy(y)), ImVec2(sx(x + 1), sy(y + 1))));

    // Painting. Left paints the brush, right erases; Shift makes it a
    // rectangle filled on release; Alt+click picks the brush under the pointer.
    int hx = -1, hy = -1;
    const bool onMap = cellAt(io.MousePos, &hx, &hy);
    if (over && m_button < 0 && !m_panning) {
        const bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left), right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        // Shift (a rectangle) and Alt (pick) work whatever the tool; the right
        // button erases in every tool (in Fill, the whole region).
        const bool rect = io.KeyShift || (m_tool == Tool::Rect && !io.KeyAlt);
        if (left && (io.KeyAlt || (m_tool == Tool::Pick && !io.KeyShift))) {
            if (onMap) {
                const int b = t.at(hx, hy);
                m_brush = b >= 0 && b < int(t.brushes.size()) ? b : -1;
                scriptLine(nullptr, "tiles: pick brush " + std::to_string(m_brush) + " at " + std::to_string(hx) + "," + std::to_string(hy));
                // As in an image editor's eyedropper: one pick, then back to painting with it.
                if (m_tool == Tool::Pick) m_tool = Tool::Paint;
            }
        } else if ((left || right) && m_tool == Tool::Fill && !rect) {
            if (onMap) {
                doc.endMerge();
                const int paint = left ? m_brush : -1;
                const int n = editor::floodTiles(doc, hx, hy, paint);
                scriptLine(nullptr, std::string("tiles: flood ") + (paint < 0 ? std::string("erase") : "brush " + std::to_string(paint)) +
                                        " cells " + std::to_string(n) + " at " + std::to_string(hx) + "," + std::to_string(hy));
            }
        } else if ((left || right) && (onMap || rect)) {
            m_button = left ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
            m_paint = left ? m_brush : -1;
            m_rect = rect;
            m_from[0] = m_last[0] = std::clamp(hx, 0, t.width - 1);
            m_from[1] = m_last[1] = std::clamp(hy, 0, t.height - 1);
            m_min[0] = m_max[0] = m_from[0];
            m_min[1] = m_max[1] = m_from[1];
            m_before = t.cells;
            m_key = "tilestroke" + std::to_string(++m_strokes);
            doc.endMerge();
            if (!m_rect) editor::paintTiles(doc, {{m_from[0], m_from[1]}}, m_paint, m_key);
        }
    }
    const splash::Tilemap& tn = *doc.scene().tilemap;  // the stroke may have changed it
    if (m_button >= 0) {
        const int cx = std::clamp(hx, 0, tn.width - 1), cy = std::clamp(hy, 0, tn.height - 1);
        if (m_rect) {
            m_min[0] = std::min(m_from[0], cx), m_max[0] = std::max(m_from[0], cx);
            m_min[1] = std::min(m_from[1], cy), m_max[1] = std::max(m_from[1], cy);
        } else if (cx != m_last[0] || cy != m_last[1]) {
            // Every cell between the last one and this, so a quick stroke leaves no gaps.
            editor::paintTiles(doc, editor::tileLine(m_last[0], m_last[1], cx, cy), m_paint, m_key);
            m_last[0] = cx, m_last[1] = cy;
            m_min[0] = std::min(m_min[0], cx), m_max[0] = std::max(m_max[0], cx);
            m_min[1] = std::min(m_min[1], cy), m_max[1] = std::max(m_max[1], cy);
        }
        if (m_rect) {
            const ImRect rr(ImVec2(sx(m_min[0]), sy(m_min[1])), ImVec2(sx(m_max[0] + 1), sy(m_max[1] + 1)));
            dl->AddRectFilled(rr.Min, rr.Max, m_paint < 0 ? rgb(0xef5a6f, 50) : color::accentSoft);
            dl->AddRect(rr.Min, rr.Max, m_paint < 0 ? color::bad : color::accent, 0, 0, 2.0f);
        }
        if (!ImGui::IsMouseDown(m_button)) {
            if (m_rect) editor::fillTiles(doc, m_min[0], m_min[1], m_max[0], m_max[1], m_paint);
            endStroke(doc);
        }
    }
    if (onMap && over && m_button < 0) {
        const ImRect cr(ImVec2(sx(hx), sy(hy)), ImVec2(sx(hx + 1), sy(hy + 1)));
        dl->AddRect(cr.Min, cr.Max, io.KeyAlt || m_tool == Tool::Pick ? color::warn : rgb(0xffffff, 160), 0, 0, 1.5f);
    }
    ImGui::PopClipRect();

    // Under the map: the cell under the pointer, the map and the zoom.
    std::string status;
    const splash::Tilemap& ts = *doc.scene().tilemap;
    if (onMap) {
        const int b = ts.at(hx, hy);
        const splash::TileBrush* br = b >= 0 && b < int(ts.brushes.size()) ? &ts.brushes[size_t(b)] : nullptr;
        status = std::to_string(hx) + ", " + std::to_string(hy) + "   " +
                 (b < 0 ? std::string("empty") : br ? br->name + (br->walkable ? ", walkable" : ", solid") : "missing brush " + std::to_string(b));
    }
    char right[96];
    std::snprintf(right, sizeof right, "%d x %d tiles of %d x %d px   %g%%", ts.width, ts.height, tw, th, double(zz * 100));
    const float ty = area.Max.y + 4;
    text(dl, ImVec2(area.Min.x, ty), f.regular, type::caption, color::textDim, status.c_str());
    const ImVec2 rs = measure(f.regular, type::caption, right);
    text(dl, ImVec2(area.Max.x - rs.x, ty), f.regular, type::caption, color::textFaint, right);
    if (!img.ready()) {
        const char* why = !sheet ? "No tileset: pick a sprite sheet in the inspector's Tilemap section" : "Converting the tileset...";
        textCentered(dl, ImRect(area.Min, ImVec2(area.Max.x, area.Min.y + 40)), f.regular, type::caption, color::warn, why);
    }
    char view[128];
    std::snprintf(view, sizeof view, "tiles: view %dx%d tile %dx%d zoom %g brush %d tileset %s", ts.width, ts.height, tw, th,
                  double(zz), m_brush, img.ready() ? "ready" : "none");
    scriptLine("tilesview", view);
}

}  // namespace ui
