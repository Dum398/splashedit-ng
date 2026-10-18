#include "editor/ui_canvas_view.hh"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <mutex>

#include "editor/document.hh"
#include "editor/project.hh"
#include "font.hh"
#include "gl.h"
#include "ui/icons.h"
#include "ui/texture_image.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
namespace uic = editor::uic;
using splash::UICanvas;
using splash::UIElement;
using splash::UIElementType;

bool CanvasView::reportLayout = false;

namespace {

constexpr ImU32 kGuide = rgb(0xff4fd8);

// The texture file dialog answers on its own thread; draw() applies it.
std::mutex g_texMutex;
std::string g_texPicked;
int g_texCanvas = -1, g_texElement = -1;

void SDLCALL onTexturePicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_texMutex);
    g_texPicked = files[0];
}
constexpr const char* kDragKey = "uicanvas-drag";

float centerY(ImFont* font, float size, float y0, float y1) {
    return std::floor(y0 + (y1 - y0 - measure(font, size, "Ag").y) * 0.5f);
}

const char* typeIcon(UIElementType t) {
    switch (t) {
        case UIElementType::Image: return icon::image;
        case UIElementType::Box: return icon::square;
        case UIElementType::Text: return icon::type;
        case UIElementType::Progress: return icon::gauge;
        case UIElementType::Line: return icon::slash;
    }
    return icon::square;
}

ImU32 typeColor(UIElementType t) {
    switch (t) {
        case UIElementType::Image: return rgb(0x7aa7ff);
        case UIElementType::Box: return rgb(0x9fd36b);
        case UIElementType::Text: return rgb(0xf2c25c);
        case UIElementType::Progress: return rgb(0x46c98a);
        case UIElementType::Line: return rgb(0xb49dff);
    }
    return color::textDim;
}

ImU32 toColor(const std::array<float, 3>& c) {
    auto b = [](float v) { return unsigned(std::clamp(std::lround(v * 255.f), 0l, 255l)); };
    return IM_COL32(b(c[0]), b(c[1]), b(c[2]), 255);
}

std::string toHex(const std::array<float, 3>& c) {
    auto b = [](float v) { return unsigned(std::clamp(std::lround(v * 255.f), 0l, 255l)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", b(c[0]), b(c[1]), b(c[2]));
    return buf;
}

void pushMenuStyle() {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleColor(ImGuiCol_Header, color::hover);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color::hover);
    ImGui::PushStyleColor(ImGuiCol_Text, color::text);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::sm, space::sm));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(space::sm, space::xs));
}
void popMenuStyle() {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

// A menu row with an icon; true when picked.
bool menuItem(const char* ic, ImU32 icol, const char* label, const char* meta = nullptr, bool enabled = true) {
    Fonts& f = fonts();
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    const bool picked = ImGui::Selectable("##item", false, enabled ? 0 : ImGuiSelectableFlags_Disabled, ImVec2(w, 22));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 tc = enabled ? color::text : color::textFaint;
    if (ic) dl->AddText(f.medium, type::icon, ImVec2(p.x + 2, p.y + 11 - type::icon * 0.5f - 1), enabled ? icol : color::textFaint, ic);
    dl->AddText(f.regular, type::body, ImVec2(p.x + 26, p.y + 11 - type::body * 0.5f - 1), tc, label);
    if (meta) {
        const float mw = measure(f.regular, type::caption, meta).x;
        dl->AddText(f.regular, type::caption, ImVec2(p.x + w - mw - 4, p.y + 11 - type::caption * 0.5f), color::textFaint, meta);
    }
    ImGui::PopID();
    return picked && enabled;
}

const UIElementType kTypes[] = {UIElementType::Box, UIElementType::Text, UIElementType::Image, UIElementType::Progress,
                                UIElementType::Line};

// The UI selection made valid for the scene: a canvas when there is one,
// an element only when it exists.
editor::Document::UiSelection validSelection(const editor::Document& doc) {
    editor::Document::UiSelection sel = doc.uiSelection();
    const auto& cvs = doc.scene().canvases;
    if (cvs.empty()) return {-1, -1};
    if (sel.canvas < 0 || sel.canvas >= int(cvs.size())) sel = {0, -1};
    const int n = int(cvs[size_t(sel.canvas)].elements.size());
    if (sel.element >= n) sel.element = -1;
    sel.more.erase(std::remove_if(sel.more.begin(), sel.more.end(), [&](int i) { return i < 0 || i >= n || i == sel.element; }),
                   sel.more.end());
    return sel;
}

struct AnchorPreset {
    const char* label;
    float minX, minY, maxX, maxY;
};
const AnchorPreset kAnchors[] = {
    {"Top left", 0, 0, 0, 0},           {"Top", 0.5f, 0, 0.5f, 0},          {"Top right", 1, 0, 1, 0},
    {"Left", 0, 0.5f, 0, 0.5f},         {"Centre", 0.5f, 0.5f, 0.5f, 0.5f}, {"Right", 1, 0.5f, 1, 0.5f},
    {"Bottom left", 0, 1, 0, 1},        {"Bottom", 0.5f, 1, 0.5f, 1},       {"Bottom right", 1, 1, 1, 1},
    {"Stretch across, top", 0, 0, 1, 0}, {"Stretch across, bottom", 0, 1, 1, 1}, {"Stretch down, left", 0, 0, 0, 1},
    {"Stretch down, right", 1, 0, 1, 1}, {"Stretch over the screen", 0, 0, 1, 1},
};

std::string anchorLabel(const UIElement& e) {
    for (const AnchorPreset& a : kAnchors)
        if (e.anchorMin[0] == a.minX && e.anchorMin[1] == a.minY && e.anchorMax[0] == a.maxX && e.anchorMax[1] == a.maxY)
            return a.label;
    char buf[64];
    std::snprintf(buf, sizeof buf, "Custom (%.2f, %.2f)-(%.2f, %.2f)", e.anchorMin[0], e.anchorMin[1], e.anchorMax[0], e.anchorMax[1]);
    return buf;
}

// A field well showing `value`, with a chevron; true when clicked.
bool pickField(const char* id, ImRect r, const char* ic, ImU32 icol, const char* value) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Hit h = interact(id, r);
    dl->AddRectFilled(r.Min, r.Max, color::field, radius::field);
    dl->AddRect(r.Min, r.Max, lerpColor(color::border, color::borderStrong, h.hover), radius::field);
    float x = r.Min.x + space::sm;
    const float cy = r.GetCenter().y;
    if (ic) {
        dl->AddText(f.medium, type::icon, ImVec2(x, cy - type::icon * 0.5f - 1), icol, ic);
        x += 22;
    }
    ImGui::PushClipRect(r.Min, ImVec2(r.Max.x - 20, r.Max.y), true);
    dl->AddText(f.regular, type::body, ImVec2(x, cy - type::body * 0.5f - 1), color::text, value);
    ImGui::PopClipRect();
    dl->AddText(f.medium, type::caption, ImVec2(r.Max.x - 18, cy - type::caption * 0.5f), color::textDim, icon::chevronDown);
    return h.clicked;
}

// A card for a problem; with a fix, its button. Returns its height.
float problem(ImDrawList* dl, const char* id, ImVec2 pos, float width, const uic::Problem& p, bool* fixed) {
    if (p.fix != uic::Fix::None) return problemCard(id, pos, width, p.title.c_str(), p.body.c_str(), uic::fixLabel(p.fix), fixed);
    Fonts& f = fonts();
    const float pad = space::md, textW = width - pad * 2 - 24;
    const ImVec2 bs = f.regular->CalcTextSizeA(type::label, FLT_MAX, textW, p.body.c_str());
    const float h = pad + 18 + space::xs + bs.y + pad;
    ImRect r(pos, pos + ImVec2(width, h));
    dl->AddRectFilled(r.Min, r.Max, color::warnSoft, radius::card);
    dl->AddRect(r.Min, r.Max, rgb(0xf0a43a, 70), radius::card);
    text(dl, r.Min + ImVec2(pad, pad), f.medium, type::icon, color::warn, icon::warning);
    text(dl, ImVec2(r.Min.x + pad + 24, r.Min.y + pad), f.semibold, type::body, color::text, p.title.c_str());
    dl->AddText(f.regular, type::label, ImVec2(r.Min.x + pad + 24, r.Min.y + pad + 18 + space::xs), color::textDim, p.body.c_str(),
                nullptr, textW);
    return h;
}

// The zoom one step in (dir > 0) or out from `scale`: whole multiples above
// 100% so pixels stay square and sharp.
float stepZoom(float scale, int dir) {
    static const float steps[] = {0.25f, 0.5f, 0.75f, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
    if (dir > 0) {
        for (float z : steps)
            if (z > scale + 1e-3f) return z;
        return steps[std::size(steps) - 1];
    }
    for (int i = int(std::size(steps)) - 1; i >= 0; i--)
        if (steps[i] < scale - 1e-3f) return steps[i];
    return steps[0];
}

}  // namespace

CanvasView::CanvasView() = default;
CanvasView::~CanvasView() {
    if (m_tex) glDeleteTextures(1, &m_tex);
    if (m_sheetTex) glDeleteTextures(1, &m_sheetTex);
    for (FontSample& s : m_samples)
        if (s.tex) glDeleteTextures(1, &s.tex);
}

// ---- The tab

void CanvasView::draw(ImDrawList* dl, ImRect r, editor::Document& doc) {
    m_screen = uic::screenOf(splash::VramSettings{});
    // A texture picked for an image element: adopted into the project, then set.
    {
        std::string picked;
        int pc = -1, pe = -1;
        {
            std::lock_guard<std::mutex> lock(g_texMutex);
            picked.swap(g_texPicked);
            pc = g_texCanvas, pe = g_texElement;
        }
        if (!picked.empty()) {
            editor::AdoptedAsset a = editor::adoptAsset(doc.projectRoot(), std::filesystem::path(std::u8string(picked.begin(), picked.end())),
                                                        "textures");
            if (!a.path.empty()) {
                uic::editElement(doc, pc, pe, [&](UIElement& el) { el.texture = a.path; });
                if (a.copied) doc.refreshAssets({a.path});
            }
        }
    }
    m_assets.refresh(doc.projectRoot(), doc.assetsId());
    const editor::Document::UiSelection sel = validSelection(doc);
    if (!(sel == doc.uiSelection())) doc.selectUi(sel);
    // The list gives up width on a narrow tab so the canvas keeps some.
    const float listW = std::clamp(std::floor(r.GetWidth() * 0.4f), 200.0f, 264.0f);
    ImRect left(r.Min, ImVec2(std::min(r.Max.x, r.Min.x + listW), r.Max.y));
    ImRect right(ImVec2(left.Max.x + size::gutter, r.Min.y), r.Max);
    ImGui::PushID("uicanvasview");
    list(dl, left, doc);
    canvas(dl, right, doc);
    keys(doc);
    elementMenu(doc);
    ImGui::PopID();
}

void CanvasView::list(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window);
    const auto& cvs = doc.scene().canvases;
    const editor::Document::UiSelection sel = doc.uiSelection();
    const float x0 = r.Min.x + space::sm, x1 = r.Max.x - space::sm;
    float y = r.Min.y + space::sm;

    // Canvas picker and its eye.
    ImRect pick(ImVec2(x0, y), ImVec2(x1 - 30, y + size::field));
    ImRect eye(ImVec2(x1 - 26, y), ImVec2(x1, y + size::field));
    const UICanvas* cv = sel.canvas >= 0 ? &cvs[size_t(sel.canvas)] : nullptr;
    std::string shown = cv ? cv->name + (cv->name.empty() ? "(unnamed canvas)" : "") : std::string("No canvas");
    if (pickField("canvaspick", pick, icon::layoutTemplate, rgb(0x5ccfe0), shown.c_str())) ImGui::OpenPopup("##canvasmenu");
    tooltip("The canvas being edited. A scene can have up to 24; higher sort orders draw in front.");
    if (cv) {
        if (iconButton("canvaseye", eye, cv->visible ? icon::eye : icon::eyeOff, !cv->visible,
                       cv->visible ? "Shown when the scene starts: click to start hidden (UI.SetCanvasVisible shows it)"
                                   : "Hidden when the scene starts: click to show it from the start"))
            uic::editCanvas(doc, sel.canvas, [](UICanvas& c) { c.visible = !c.visible; });
    }
    ImGui::SetNextWindowPos(ImVec2(pick.Min.x, pick.Max.y + space::xs));
    pushMenuStyle();
    if (ImGui::BeginPopup("##canvasmenu")) {
        for (int i = 0; i < int(cvs.size()); i++) {
            ImGui::PushID(i);
            std::string meta = std::to_string(cvs[size_t(i)].elements.size()) + " el" + (cvs[size_t(i)].visible ? "" : ", hidden");
            if (!cvs[size_t(i)].name.empty() && cvs[size_t(i)].name == doc.scene().settings.loadingCanvas) meta += ", loading screen";
            if (menuItem(i == sel.canvas ? icon::chevronRight : icon::layoutTemplate, rgb(0x5ccfe0), cvs[size_t(i)].name.c_str(),
                         meta.c_str()))
                doc.selectUi({i, -1});
            ImGui::PopID();
        }
        if (!cvs.empty()) ImGui::Separator();
        if (menuItem(icon::plus, color::textDim, "New canvas", nullptr, cvs.size() < size_t(uic::kMaxCanvases))) uic::addCanvas(doc);
        if (menuItem(icon::trash, color::bad, "Delete this canvas", nullptr, cv != nullptr)) uic::deleteCanvas(doc, sel.canvas);
        ImGui::EndPopup();
    }
    popMenuStyle();
    y += size::field + space::sm;

    if (!cv) {
        const char* msg = "This scene has no UI canvas yet. A canvas holds the HUD, menus and prompts drawn over the game.";
        dl->AddText(f.regular, type::label, ImVec2(x0, y + space::sm), color::textDim, msg, nullptr, x1 - x0);
        y += f.regular->CalcTextSizeA(type::label, FLT_MAX, x1 - x0, msg).y + space::lg;
        if (button("newcanvas", ImVec2(x0, y), icon::plus, "New canvas", ButtonKind::Primary)) uic::addCanvas(doc);
        return;
    }
    const int c = sel.canvas;
    const int n = int(cv->elements.size());

    // Toolbar.
    const float bs = size::field;
    float bx = x0;
    auto tool = [&](const char* id, const char* ic, const char* tip, bool enabled, ImU32 col = color::textDim) {
        bool hit = iconButton(id, ImRect(ImVec2(bx, y), ImVec2(bx + bs, y + bs)), ic, false, tip, col, enabled);
        bx += bs + space::xs;
        return hit;
    };
    const ImVec2 addPos(bx, y + bs + space::xs);
    if (tool("addel", icon::plus, "Add an element", true, color::accent)) ImGui::OpenPopup("##addmenu");
    if (tool("dupel", icon::copy, "Duplicate (Ctrl+D)", sel.element >= 0)) uic::duplicateElement(doc, c, sel.element);
    if (tool("delel", icon::trash, "Delete (Del)", sel.element >= 0)) uic::deleteElement(doc, c, sel.element);
    bx += space::sm;
    if (tool("upel", icon::arrowUp, "Move up: drawn further back (Ctrl+[)", sel.element > 0))
        uic::moveElement(doc, c, sel.element, sel.element - 1);
    if (tool("downel", icon::arrowDown, "Move down: drawn further in front (Ctrl+])", sel.element >= 0 && sel.element + 1 < n))
        uic::moveElement(doc, c, sel.element, sel.element + 1);
    ImGui::SetNextWindowPos(addPos);
    pushMenuStyle();
    if (ImGui::BeginPopup("##addmenu")) {
        for (UIElementType t : kTypes)
            if (menuItem(typeIcon(t), typeColor(t), uic::typeName(t))) uic::addElement(doc, c, t, m_screen);
        ImGui::EndPopup();
    }
    popMenuStyle();
    y += bs + space::sm;
    text(dl, ImVec2(x0 + space::xs, y), f.regular, type::caption, color::textFaint, "Lower rows draw in front");
    y += 18;

    // Rows, scrolled; the budget under them.
    const float rowH = size::row;
    const float footH = 28;
    ImRect area(ImVec2(r.Min.x, y), ImVec2(r.Max.x, r.Max.y - footH));
    const float content = n * rowH;
    if (area.Contains(ImGui::GetIO().MousePos) && ImGui::GetIO().MouseWheel != 0)
        m_listScroll -= ImGui::GetIO().MouseWheel * rowH * 3;
    m_listScroll = std::clamp(m_listScroll, 0.0f, std::max(0.0f, content - area.GetHeight()));
    const float top = area.Min.y - m_listScroll;
    if (doc.revision() != m_warnRev || doc.loadId() != m_warnLoad || int(m_rowWarn.size()) != n) {
        m_rowWarn.assign(size_t(n), false);
        for (int i = 0; i < n; i++) m_rowWarn[size_t(i)] = !uic::elementProblems(doc, c, i, m_screen).empty();
        m_warnRev = doc.revision();
        m_warnLoad = doc.loadId();
    }
    ImGui::PushClipRect(area.Min, area.Max, true);
    int clickedRow = -1, eyeRow = -1, rightRow = -2;
    const std::vector<int> picked = uic::selectedElements(doc);
    auto isPicked = [&](int i) { return std::binary_search(picked.begin(), picked.end(), i); };
    for (int i = 0; i < n; i++) {
        ImRect rr(ImVec2(x0 - space::xs, top + i * rowH), ImVec2(x1 + space::xs, top + (i + 1) * rowH));
        if (rr.Max.y < area.Min.y || rr.Min.y > area.Max.y) continue;
        const UIElement& e = cv->elements[size_t(i)];
        TreeRow row;
        row.icon = typeIcon(e.type);
        row.iconColor = e.unknown.empty() ? typeColor(e.type) : color::textFaint;
        row.label = e.name.empty() ? "(unnamed)" : e.name.c_str();
        row.meta = e.unknown.empty() ? nullptr : "unknown type";
        row.selected = isPicked(i);
        row.warning = m_rowWarn[size_t(i)];
        row.hidden = !e.visible;
        ImGui::PushID(i);
        bool eyeHit = false;
        Hit h = treeRow("row", rr, row, &eyeHit);
        if (m_renaming && i == sel.element && picked.size() == 1) {
            // F2: the name becomes a text field.
            if (m_renameStart) {
                beginTextEdit("uirename", e.name);
                m_renameStart = false;
            }
            const float lx = rr.Min.x + space::xs + 16 + 22;
            ImRect field(ImVec2(lx - space::xs - 2, rr.Min.y + 2), ImVec2(rr.Max.x - 2, rr.Max.y - 2));
            std::string typed;
            TextEdit res = textEdit("uirename", field, fonts().medium, type::body, lx, field.Max.x - space::xs, &typed);
            if (res == TextEdit::Commit && !typed.empty() && typed != e.name)
                uic::editElement(doc, c, i, [&](UIElement& el) { el.name = typed; });
            if (res != TextEdit::Editing) m_renaming = false;
            eyeHit = false;
        }
        ImGui::PopID();
        if (eyeHit) eyeRow = i;
        else if (h.clicked && !m_rowDragging) clickedRow = i;
        if (h.hovered && !m_rowDragging && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightRow = i;
        if (h.held && !m_rowDragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 5)) {
            m_rowDrag = i;
            m_rowDragging = true;
        }
    }
    // Where a dragged row would go: the gap nearest the pointer.
    int gap = -1;
    if (m_rowDragging && m_rowDrag >= 0 && m_rowDrag < n) {
        gap = std::clamp(int(std::floor((ImGui::GetIO().MousePos.y - top) / rowH + 0.5f)), 0, n);
        const float gy = top + gap * rowH;
        dl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), color::accent, 2.0f);
        dl->AddCircleFilled(ImVec2(x0, gy), 3.5f, color::accent);
    }
    ImGui::PopClipRect();
    if (eyeRow >= 0) uic::editElement(doc, c, eyeRow, [](UIElement& e) { e.visible = !e.visible; });
    if (clickedRow >= 0) {
        ImGuiIO& io = ImGui::GetIO();
        m_renaming = false;
        if (io.KeyShift && m_anchor >= 0 && m_anchor < n) {
            // A range from the row last clicked plain or with Ctrl.
            std::vector<int> range;
            for (int i = std::min(m_anchor, clickedRow); i <= std::max(m_anchor, clickedRow); i++) range.push_back(i);
            select(doc, c, range, clickedRow);
        } else if (io.KeyCtrl) {
            std::vector<int> v = picked;
            const bool was = isPicked(clickedRow);
            if (was) v.erase(std::find(v.begin(), v.end(), clickedRow));
            else v.push_back(clickedRow);
            select(doc, c, v, was ? -1 : clickedRow);
            m_anchor = clickedRow;
        } else {
            select(doc, c, {clickedRow}, clickedRow);
            m_anchor = clickedRow;
        }
    }
    // Right click: on a row selects it, below the rows selects none; then the element menu.
    if (rightRow == -2 && !m_rowDragging && ImGui::IsMouseHoveringRect(area.Min, area.Max) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        rightRow = -1;
    if (rightRow >= -1) {
        if (rightRow < 0 || !isPicked(rightRow)) select(doc, c, rightRow < 0 ? std::vector<int>{} : std::vector<int>{rightRow}, rightRow);
        m_menuAtCanvas = false;
        ImGui::OpenPopup("##elmenu");
    }
    if (m_rowDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (gap >= 0) {
            const int to = gap > m_rowDrag ? gap - 1 : gap;
            if (to != m_rowDrag) uic::moveElement(doc, c, m_rowDrag, to);
            else select(doc, c, {m_rowDrag}, m_rowDrag);
        }
        m_rowDragging = false;
        m_rowDrag = -1;
    }
    if (n == 0)
        dl->AddText(f.regular, type::label, ImVec2(x0 + space::xs, area.Min.y + space::sm), color::textFaint,
                    "No elements. Add one with +.");

    int total = 0;
    for (const UICanvas& x : cvs) total += int(x.elements.size());
    char foot[96];
    std::snprintf(foot, sizeof foot, "%d in this canvas, %d of %d in the scene", n, total, uic::kMaxElements);
    text(dl, ImVec2(x0 + space::xs, r.Max.y - footH + 6), f.regular, type::caption,
         total > uic::kMaxElements ? color::bad : color::textFaint, foot);

    char lay[96];
    std::snprintf(lay, sizeof lay, "list %.0f,%.0f row %.0f eye %.0f,%.0f", x0 - space::xs, area.Min.y, rowH, eye.GetCenter().x,
                  eye.GetCenter().y);
    m_listLayout = lay;
}

void CanvasView::upload(const editor::Document& doc, const std::vector<bool>& show) {
    if (m_tex && doc.revision() == m_texRev && doc.loadId() == m_texLoad && doc.assetsId() == m_texAssets && show == m_texShow) return;
    std::vector<uint8_t> covered;
    std::vector<uint32_t> px = uic::render(doc.scene(), show, m_screen, m_assets, 0, &covered);
    if (!m_tex) glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_screen.w, m_screen.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    m_texRev = doc.revision();
    m_texLoad = doc.loadId();
    m_texAssets = doc.assetsId();
    m_texShow = show;
}

// The transform from the zoom and pan. Fit: the largest whole scale that
// fits, centred; below 1, the fraction that fits (not under 25%). Otherwise the zoom, panned, kept so some of the screen stays
// in view. The origin is always on a whole pixel, so whole scales draw the
// texture pixel for pixel.
void CanvasView::layoutCanvas() {
    const uic::Screen s = m_screen;
    if (m_zoom <= 0) m_pan = ImVec2(0, 0);
    m_scale = m_zoom > 0 ? m_zoom : m_fit;
    const ImVec2 size(s.w * m_scale, s.h * m_scale);
    const ImVec2 centred(std::floor(m_area.GetCenter().x - size.x * 0.5f), std::floor(m_area.GetCenter().y - size.y * 0.5f));
    const float keep = 48;
    const float hx = std::floor(m_area.Max.x - keep - centred.x), hy = std::floor(m_area.Max.y - keep - centred.y);
    m_pan.x = std::clamp(m_pan.x, std::min(hx, std::floor(m_area.Min.x + keep - size.x - centred.x)), hx);
    m_pan.y = std::clamp(m_pan.y, std::min(hy, std::floor(m_area.Min.y + keep - size.y - centred.y)), hy);
    m_origin = centred + m_pan;
}

void CanvasView::zoomAt(float scale, ImVec2 at) {
    scale = std::clamp(scale, 0.25f, 32.0f);
    const ImVec2 cp = toCanvas(at);
    const uic::Screen s = m_screen;
    const ImVec2 size(s.w * scale, s.h * scale);
    const ImVec2 centred(std::floor(m_area.GetCenter().x - size.x * 0.5f), std::floor(m_area.GetCenter().y - size.y * 0.5f));
    m_zoom = scale;
    m_pan = ImVec2(std::round(at.x - cp.x * scale - centred.x), std::round(at.y - cp.y * scale - centred.y));
    layoutCanvas();
}

void CanvasView::canvas(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    ImGuiIO& io = ImGui::GetIO();
    dl->AddRectFilled(r.Min, r.Max, color::chrome, radius::window);
    const editor::Document::UiSelection sel = doc.uiSelection();
    const auto& cvs = doc.scene().canvases;
    const uic::Screen s = m_screen;

    // Header: resolution, the zoom controls, the snap switch, the hints.
    const float headH = size::panelHeader;
    ImRect head(r.Min, ImVec2(r.Max.x, r.Min.y + headH));
    ImRect area(ImVec2(r.Min.x, head.Max.y), r.Max);
    m_area = area;
    const float pad = area.GetWidth() < 400 ? space::sm : 24;
    float fit = std::min((area.GetWidth() - pad * 2) / s.w, (area.GetHeight() - pad * 2) / s.h);
    m_fit = fit >= 1 ? std::floor(fit) : std::max(fit, 0.25f);  // whole pixels when it fits
    layoutCanvas();

    // Right to left: the snap switch, fit, zoom in, the zoom, zoom out. When
    // the header is narrow the gaps and the zoom label tighten, then the
    // hints, the resolution and last the zoom label give way; the buttons stay.
    char res[64];
    std::snprintf(res, sizeof res, "%d x %d", s.w, s.h);
    const float resW = measure(f.medium, type::label, res).x;
    const float bw = 26, by0 = head.Min.y + 5, by1 = head.Max.y - 5;
    const float fullW = 6 + bw + 12 + bw + 2 + bw + 2 + 50 + 2 + bw;  // 178
    const bool compact = head.Min.x + space::md + resW + space::sm + fullW > head.Max.x;
    const float snapGap = compact ? 6 : 12, pctW0 = compact ? 40 : 50;
    const float needPct = 6 + bw + snapGap + bw + 2 + bw + 2 + pctW0 + 2 + bw;
    const float pctW = head.Min.x + space::xs + needPct <= head.Max.x ? pctW0 : 0;
    float x = head.Max.x - 6;
    const ImRect snapR(ImVec2(x - bw, by0), ImVec2(x, by1));
    x -= bw + snapGap;
    const ImRect fitR(ImVec2(x - bw, by0), ImVec2(x, by1));
    x -= bw + 2;
    const ImRect inR(ImVec2(x - bw, by0), ImVec2(x, by1));
    x -= bw + 2;
    const ImRect pctR(ImVec2(x - pctW, by0), ImVec2(x, by1));
    if (pctW > 0) x -= pctW + 2;
    const ImRect outR(ImVec2(x - bw, by0), ImVec2(x, by1));
    const float tools = outR.Min.x;
    const bool showRes = head.Min.x + space::md + resW + space::sm <= tools;
    const float textX = showRes ? head.Min.x + space::md + resW + space::lg : head.Min.x + space::md;
    if (showRes)
        text(dl, ImVec2(head.Min.x + space::md, centerY(f.medium, type::label, head.Min.y, head.Max.y)), f.medium, type::label,
             color::textDim, res);
    if (iconButton("snap", snapR, icon::magnet, m_snap,
                   m_snap ? "Snapping to the screen and other elements: on" : "Snapping: off (Ctrl snaps while dragging)"))
        m_snap = !m_snap;
    const bool canZoom = sel.canvas >= 0;
    if (iconButton("zoomfit", fitR, icon::maximize, m_zoom <= 0, "Fit the screen in the view (Ctrl+0)", color::textDim, canZoom))
        m_zoom = 0, m_pan = ImVec2(0, 0);
    if (iconButton("zoomin", inR, icon::plus, false, "Zoom in (Ctrl+=, wheel)", color::textDim, canZoom))
        zoomAt(stepZoom(m_scale, 1), area.GetCenter());
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(m_scale * 100)));
    if (pctW > 0) {
        Hit ph = interact("zoom100", pctR);
        if (ph.hovered) dl->AddRectFilled(pctR.Min, pctR.Max, color::hover, radius::field);
        textCentered(dl, pctR, f.medium, type::caption, m_scale == 1 ? color::text : color::textDim, pct);
        if (ph.hovered) {
            if (showRes) tooltip("Actual pixels: 100% (Ctrl+1)");
            else tooltip((std::string("Screen ") + res + ". Actual pixels: 100% (Ctrl+1)").c_str());
        }
        if (ph.clicked && canZoom) m_zoom = 1, m_pan = ImVec2(0, 0);
    }
    if (iconButton("zoomout", outR, icon::minus, false, "Zoom out (Ctrl+-, wheel)", color::textDim, canZoom))
        zoomAt(stepZoom(m_scale, -1), area.GetCenter());
    const char* hint = "Drag to move, handles resize, arrows nudge, wheel zooms, middle-drag or Space pans";
    const float hw = measure(f.regular, type::caption, hint).x;
    const bool showHint = tools - space::md - hw >= textX;
    if (showHint)
        text(dl, ImVec2(tools - space::md - hw, centerY(f.regular, type::caption, head.Min.y, head.Max.y)), f.regular, type::caption,
             color::textFaint, hint);
    if (reportLayout) {
        // Each header item as x0-x1, "-" when it gave way.
        auto span = [](bool shown, float a, float b) {
            char t[32];
            if (shown) std::snprintf(t, sizeof t, "%.0f-%.0f", a, b);
            else std::snprintf(t, sizeof t, "-");
            return std::string(t);
        };
        const float rx = head.Min.x + space::md, hx = tools - space::md - hw;
        std::string line = "uicanvas: header " + span(true, head.Min.x, head.Max.x) + " res " + span(showRes, rx, rx + resW) +
                           " hint " + span(showHint, hx, hx + hw) + " zoomout " + span(true, outR.Min.x, outR.Max.x) + " pct " +
                           span(pctW > 0, pctR.Min.x, pctR.Max.x) + " pcttext " +
                           span(pctW > 0, pctR.GetCenter().x - measure(f.medium, type::caption, pct).x * 0.5f,
                                pctR.GetCenter().x + measure(f.medium, type::caption, pct).x * 0.5f) +
                           " zoomin " + span(true, inR.Min.x, inR.Max.x) + " zoomfit " + span(true, fitR.Min.x, fitR.Max.x) + " snap " +
                           span(true, snapR.Min.x, snapR.Max.x);
        if (m_headReported != line) {
            m_headReported = line;
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    }

    // Zoom about the pointer with the wheel; pan with the middle button or
    // Space + left. Before anything reads the transform this frame.
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    const bool overArea = canZoom && !popup && ImGui::IsMouseHoveringRect(area.Min, area.Max) && !ImGui::IsAnyItemActive();
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
    if (overArea && io.MouseWheel != 0 && m_drag.element < 0) zoomAt(stepZoom(m_scale, io.MouseWheel > 0 ? 1 : -1), io.MousePos);
    if (!m_panning && overArea && m_drag.element < 0) {
        int button = -1;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) button = ImGuiMouseButton_Middle;
        else if (spaceHeld && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) button = ImGuiMouseButton_Left;
        if (button >= 0) {
            m_panning = true;
            m_panButton = button;
            m_panMouse = io.MousePos;
            if (m_zoom <= 0) m_zoom = m_scale;
            m_panStart = m_pan;
        }
    }
    if (m_panning) {
        if (!ImGui::IsMouseDown(m_panButton)) {
            m_panning = false;
        } else {
            const ImVec2 d = io.MousePos - m_panMouse;
            m_pan = ImVec2(std::round(m_panStart.x + d.x), std::round(m_panStart.y + d.y));
        }
    }
    layoutCanvas();
    const float scale = m_scale;
    const ImVec2 size(s.w * scale, s.h * scale);
    const ImVec2 origin = m_origin;

    if (sel.canvas < 0) {
        textCentered(dl, area, f.regular, type::body, color::textFaint, "No canvas to show");
        return;
    }
    const UICanvas& cv = cvs[size_t(sel.canvas)];

    // The screen: a checkerboard where nothing draws, then the canvases the
    // game shows at the start, and the one being edited even when hidden.
    std::vector<bool> show(cvs.size());
    for (size_t i = 0; i < cvs.size(); i++) show[i] = cvs[i].visible || int(i) == sel.canvas;
    upload(doc, show);
    ImGui::PushClipRect(area.Min, area.Max, true);
    const ImVec2 o = origin, end = origin + size;
    const float cell = 8 * scale;
    dl->AddRectFilled(o, end, rgb(0x0c0d10));
    for (int cy = 0; cy * cell < size.y; cy++)
        for (int cx = (cy & 1); cx * cell < size.x; cx += 2)
            dl->AddRectFilled(ImVec2(o.x + cx * cell, o.y + cy * cell),
                              ImVec2(std::min(end.x, o.x + (cx + 1) * cell), std::min(end.y, o.y + (cy + 1) * cell)), rgb(0x16181d));
    ui::addTextureImage(dl, (ImTextureID)(intptr_t)m_tex, o, end);
    dl->AddRect(o - ImVec2(1, 1), end + ImVec2(1, 1), color::borderStrong);
    // Centre marks.
    dl->AddLine(ImVec2(o.x + size.x * 0.5f, o.y - 6), ImVec2(o.x + size.x * 0.5f, o.y - 1), color::textFaint);
    dl->AddLine(ImVec2(o.x - 6, o.y + size.y * 0.5f), ImVec2(o.x - 1, o.y + size.y * 0.5f), color::textFaint);
    if (!cv.visible) {
        const char* note = "Hidden at start: shown here to edit";
        ImVec2 ns = measure(f.medium, type::caption, note);
        ImRect chip(ImVec2(o.x, end.y + 6), ImVec2(o.x + ns.x + space::md * 2, end.y + 6 + 20));
        dl->AddRectFilled(chip.Min, chip.Max, color::warnSoft, radius::pill);
        textCentered(dl, chip, f.medium, type::caption, color::warn, note);
    }

    const ImVec2 m = io.MousePos;
    const ImVec2 mc = toCanvas(m);
    const float mx = mc.x, my = mc.y;
    const float slop = 5.0f / scale;
    const int threshold = std::max(1, int(std::lround(6.0f / scale)));

    // Mouse.
    Hit h = interact("canvasarea", area);
    const std::vector<int> sels = uic::selectedElements(doc);
    const bool single = sels.size() == 1;
    auto inSel = [&](int i) { return std::binary_search(sels.begin(), sels.end(), i); };
    const UIElement* selEl = single ? &cv.elements[size_t(sels[0])] : nullptr;
    auto handleAt = [&](const UIElement& e) {
        if (e.type == UIElementType::Line) return uic::hitLineHandle(e, mx, my, slop);
        uic::Handle hd = uic::hitHandle(uic::layout(e, s), mx, my, slop);
        return hd;
    };
    uic::Handle hoverHandle = uic::Handle::None;
    if (selEl && selEl->unknown.empty()) hoverHandle = handleAt(*selEl);
    const int hoverEl = h.hovered ? uic::hitTest(cv, s, mx, my, slop) : -1;
    if (h.hovered && !m_panning && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_drag.element < 0) {
        // Right click: selects what is under the pointer (keeping a selection it is part of), then the element menu.
        if (hoverEl >= 0 && !inSel(hoverEl)) select(doc, sel.canvas, {hoverEl}, hoverEl);
        m_menuAtCanvas = true;
        m_menuAt = {int(std::floor(mx)), int(std::floor(my))};
        ImGui::OpenPopup("##elmenu");
    }
    if (h.hovered && !m_panning && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_drag = Drag();
        m_band = Band();
        m_renaming = false;
        int el = -1;
        uic::Handle hd = uic::Handle::None;
        if (!io.KeyCtrl && hoverHandle != uic::Handle::None && hoverHandle != uic::Handle::Move) {
            el = sels[0], hd = hoverHandle;
        } else if (hoverEl >= 0) {
            el = hoverEl, hd = uic::Handle::Move;
        } else if (!io.KeyCtrl && hoverHandle == uic::Handle::Move) {
            el = sels[0], hd = uic::Handle::Move;  // a line's own body
        }
        if (io.KeyCtrl && el >= 0) {
            // Ctrl+click toggles the element in the selection.
            std::vector<int> v = sels;
            const bool was = inSel(el);
            if (was) v.erase(std::find(v.begin(), v.end(), el));
            else v.push_back(el);
            select(doc, sel.canvas, v, was ? -1 : el);
            m_anchor = el;
            el = -1;
        } else if (el >= 0) {
            if (hd == uic::Handle::Move && inSel(el) && sels.size() > 1) m_drag.narrowTo = el;  // moves them all
            else select(doc, sel.canvas, {el}, el);
            m_anchor = el;
        } else {
            // Empty canvas: a rubber band; plain, it starts from nothing selected, with Ctrl it adds.
            m_band.active = true;
            m_band.start = m_band.now = ImVec2(mx, my);
            if (io.KeyCtrl) m_band.base = sels;
            else doc.selectUi({sel.canvas, -1});
        }
        if (el >= 0) {
            const UIElement& e = cv.elements[size_t(el)];
            m_drag.canvas = sel.canvas;
            m_drag.element = el;
            m_drag.handle = hd;
            m_drag.mouse = m;
            m_drag.rect = uic::layout(e, s);
            m_drag.from = e.from;
            m_drag.to = e.to;
            m_drag.idx = hd == uic::Handle::Move ? uic::selectedElements(doc) : std::vector<int>{el};
            for (int i : m_drag.idx) {
                const UIElement& q = cv.elements[size_t(i)];
                m_drag.rects.push_back(uic::boundsOf(q, s));
                m_drag.froms.push_back(q.from);
                m_drag.tos.push_back(q.to);
            }
        }
    }
    m_guides.clear();
    m_dragLabel.clear();
    if (m_drag.element >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (m_drag.moved) doc.endMerge();
            else if (m_drag.narrowTo >= 0) select(doc, sel.canvas, {m_drag.narrowTo}, m_drag.narrowTo);
            m_drag = Drag();
        } else if (m_drag.canvas == sel.canvas && std::all_of(m_drag.idx.begin(), m_drag.idx.end(),
                                                               [&](int i) { return i < int(cv.elements.size()); })) {
            const ImVec2 dm = m - m_drag.mouse;
            if (!m_drag.moved && std::fabs(dm.x) + std::fabs(dm.y) >= 2) m_drag.moved = true;
            if (m_drag.moved) {
                const int dx = int(std::lround(dm.x / scale)), dy = int(std::lround(dm.y / scale));
                const bool snap = m_snap != io.KeyCtrl;
                const int th = snap ? threshold : -1;
                const int ei = m_drag.element;
                const uic::Handle hd = m_drag.handle;
                char label[64] = {};
                if (hd == uic::Handle::LineFrom || hd == uic::Handle::LineTo) {
                    std::array<int, 2> p = hd == uic::Handle::LineFrom ? m_drag.from : m_drag.to;
                    p[0] += dx;
                    p[1] += dy;
                    if (snap) m_guides = uic::snapPoint(cv, ei, &p[0], &p[1], s, th);
                    uic::editElement(doc, sel.canvas, ei, [&](UIElement& e) { (hd == uic::Handle::LineFrom ? e.from : e.to) = p; }, kDragKey);
                    std::snprintf(label, sizeof label, "%d, %d", p[0], p[1]);
                } else if (hd == uic::Handle::Move) {
                    // The whole selection moves as one box; it snaps to what is not part of it.
                    int bx0 = INT_MAX, by0 = INT_MAX, bx1 = INT_MIN, by1 = INT_MIN;
                    for (const uic::Rect& q : m_drag.rects)
                        bx0 = std::min(bx0, q.x), by0 = std::min(by0, q.y), bx1 = std::max(bx1, q.right()), by1 = std::max(by1, q.bottom());
                    uic::Rect nr{bx0 + dx, by0 + dy, bx1 - bx0, by1 - by0};
                    if (snap) {
                        UICanvas others = cv;
                        for (size_t k = m_drag.idx.size(); k-- > 0;) others.elements.erase(others.elements.begin() + m_drag.idx[k]);
                        uic::Snapped sn = uic::snapMove(others, -1, nr, s, th);
                        nr = sn.rect;
                        m_guides = sn.guides;
                    }
                    const int mx2 = nr.x - bx0, my2 = nr.y - by0;
                    uic::editElements(doc, sel.canvas, m_drag.idx, [&](UIElement& e, size_t k) {
                        if (e.type == UIElementType::Line) {
                            e.from = {m_drag.froms[k][0] + mx2, m_drag.froms[k][1] + my2};
                            e.to = {m_drag.tos[k][0] + mx2, m_drag.tos[k][1] + my2};
                        } else {
                            const uic::Rect& q = m_drag.rects[k];
                            uic::place(e, uic::Rect{q.x + mx2, q.y + my2, q.w, q.h}, s);
                        }
                    }, kDragKey);
                    std::snprintf(label, sizeof label, "%d, %d", nr.x, nr.y);
                } else {
                    uic::Rect nr = uic::resize(m_drag.rect, hd, dx, dy);
                    if (snap) {
                        uic::Snapped sn = uic::snapResize(cv, ei, nr, hd, s, th);
                        nr = sn.rect;
                        m_guides = sn.guides;
                    }
                    uic::editElement(doc, sel.canvas, ei, [&](UIElement& e) { uic::place(e, nr, s); }, kDragKey);
                    std::snprintf(label, sizeof label, "%d x %d", nr.w, nr.h);
                }
                m_dragLabel = label;
            }
        } else {
            m_drag = Drag();
        }
    }
    if (m_band.active) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_band = Band();
        } else {
            m_band.now = ImVec2(mx, my);
            const ImVec2 sp = toScreen(m_band.start.x, m_band.start.y);
            if (!m_band.moved && std::fabs(m.x - sp.x) + std::fabs(m.y - sp.y) >= 3) m_band.moved = true;
            if (m_band.moved) {
                std::vector<int> v = uic::elementsInBox(cv, s, m_band.start.x, m_band.start.y, mx, my);
                v.insert(v.end(), m_band.base.begin(), m_band.base.end());
                const editor::Document::UiSelection want = uic::selectionOf(sel.canvas, v);
                if (!(want == doc.uiSelection())) doc.selectUi(want);
            }
        }
    }

    // Cursor.
    const uic::Handle cur = m_drag.element >= 0 ? m_drag.handle : (h.hovered ? hoverHandle : uic::Handle::None);
    switch (cur) {
        case uic::Handle::TopLeft:
        case uic::Handle::BottomRight: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
        case uic::Handle::TopRight:
        case uic::Handle::BottomLeft: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
        case uic::Handle::Left:
        case uic::Handle::Right: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
        case uic::Handle::Top:
        case uic::Handle::Bottom: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
        case uic::Handle::LineFrom:
        case uic::Handle::LineTo:
        case uic::Handle::Move:
            if (m_drag.element >= 0 && m_drag.moved) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            break;
        case uic::Handle::None: break;
    }
    if (m_panning || (spaceHeld && overArea)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Outlines: every element faintly, the one under the pointer brighter,
    // the selection with its handles.
    const UICanvas& now = doc.scene().canvases[size_t(sel.canvas)];
    const std::vector<int> sels2 = uic::selectedElements(doc);
    for (int i = 0; i < int(now.elements.size()); i++) {
        const UIElement& e = now.elements[size_t(i)];
        if (!e.unknown.empty()) continue;
        const bool selected = std::binary_search(sels2.begin(), sels2.end(), i), hovered = i == hoverEl && m_drag.element < 0;
        if (!e.visible && !selected) continue;
        const ImU32 col = selected ? color::accent : hovered ? rgb(0xffffff, 150) : rgb(0xffffff, 26);
        if (e.type == UIElementType::Line) {
            dl->AddLine(toScreen(e.from[0] + 0.5f, e.from[1] + 0.5f), toScreen(e.to[0] + 0.5f, e.to[1] + 0.5f), col,
                        selected || hovered ? 1.5f : 1.0f);
        } else {
            const uic::Rect lr = uic::layout(e, s);
            dl->AddRect(toScreen(float(lr.x), float(lr.y)) - ImVec2(0.5f, 0.5f), toScreen(float(lr.right()), float(lr.bottom())) + ImVec2(0.5f, 0.5f),
                        col, 0, 0, selected ? 1.5f : 1.0f);
        }
    }
    for (const uic::Guide& g : m_guides) {
        if (g.vertical) dl->AddLine(toScreen(float(g.pos), float(g.from) - 4), toScreen(float(g.pos), float(g.to) + 4), kGuide, 1.0f);
        else dl->AddLine(toScreen(float(g.from) - 4, float(g.pos)), toScreen(float(g.to) + 4, float(g.pos)), kGuide, 1.0f);
    }
    if (sels2.size() == 1 && now.elements[size_t(sels2[0])].unknown.empty()) {
        const UIElement& e = now.elements[size_t(sels2[0])];
        auto knob = [&](ImVec2 p) {
            dl->AddRectFilled(p - ImVec2(4, 4), p + ImVec2(4, 4), color::text);
            dl->AddRect(p - ImVec2(4, 4), p + ImVec2(4, 4), color::accent, 0, 0, 1.5f);
        };
        if (e.type == UIElementType::Line) {
            knob(toScreen(e.from[0] + 0.5f, e.from[1] + 0.5f));
            knob(toScreen(e.to[0] + 0.5f, e.to[1] + 0.5f));
        } else {
            const uic::Rect lr = uic::layout(e, s);
            const float l = float(lr.x), t = float(lr.y), rt = float(lr.right()), b = float(lr.bottom());
            for (ImVec2 p : {ImVec2(l, t), ImVec2(rt, t), ImVec2(l, b), ImVec2(rt, b), ImVec2((l + rt) * 0.5f, t),
                             ImVec2((l + rt) * 0.5f, b), ImVec2(l, (t + b) * 0.5f), ImVec2(rt, (t + b) * 0.5f)})
                knob(toScreen(p.x, p.y));
        }
    }
    if (m_band.active && m_band.moved) {
        const ImVec2 a = toScreen(m_band.start.x, m_band.start.y), b = toScreen(m_band.now.x, m_band.now.y);
        dl->AddRectFilled(a, b, (color::accent & 0x00ffffffu) | (40u << IM_COL32_A_SHIFT));
        dl->AddRect(a, b, color::accent, 0, 0, 1.0f);
    }
    if (!m_dragLabel.empty()) {
        ImVec2 ls = measure(f.medium, type::caption, m_dragLabel.c_str());
        ImRect chip(m + ImVec2(16, 16), m + ImVec2(16 + ls.x + space::md, 16 + 20));
        dl->AddRectFilled(chip.Min, chip.Max, color::raised, radius::field);
        dl->AddRect(chip.Min, chip.Max, color::borderStrong, radius::field);
        textCentered(dl, chip, f.medium, type::caption, color::text, m_dragLabel.c_str());
    }
    if (!m_notice.empty() && ImGui::GetTime() < m_noticeUntil) {
        ImVec2 ns = measure(f.medium, type::caption, m_notice.c_str());
        const float cx = area.GetCenter().x;
        ImRect chip(ImVec2(cx - ns.x * 0.5f - space::md, area.Max.y - 40), ImVec2(cx + ns.x * 0.5f + space::md, area.Max.y - 16));
        dl->AddRectFilled(chip.Min, chip.Max, m_noticeBad ? color::warnSoft : color::raised, radius::pill);
        dl->AddRect(chip.Min, chip.Max, color::borderStrong, radius::pill);
        textCentered(dl, chip, f.medium, type::caption, m_noticeBad ? color::warn : color::text, m_notice.c_str());
    }
    ImGui::PopClipRect();

    if (reportLayout) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "uicanvas: screen %.0f,%.0f scale %g %s", o.x, o.y, scale, m_listLayout.c_str());
        if (m_reported != buf) {
            m_reported = buf;
            std::printf("%s\n", buf);
            std::fflush(stdout);
        }
    }
}

// ---- Element commands

bool CanvasView::hasElement(const editor::Document& doc) const { return !uic::selectedElements(doc).empty(); }

void CanvasView::notice(std::string msg, bool bad) {
    if (reportLayout) {
        std::printf("uicanvas: notice %s\n", msg.c_str());
        std::fflush(stdout);
    }
    m_notice = std::move(msg);
    m_noticeBad = bad;
    m_noticeUntil = ImGui::GetTime() + (bad ? 5 : 2.5);
}

namespace {
// "Box" for one element, "3 elements" for more.
std::string describe(const editor::Document& doc, int c, const std::vector<int>& idx) {
    if (idx.size() == 1) return doc.scene().canvases[size_t(c)].elements[size_t(idx[0])].name;
    return std::to_string(idx.size()) + " elements";
}
}  // namespace

bool CanvasView::canRename(const editor::Document& doc) const { return uic::selectedElements(doc).size() == 1; }

void CanvasView::rename(editor::Document& doc) {
    const std::vector<int> sels = uic::selectedElements(doc);
    if (sels.size() != 1) return;
    m_renaming = true;
    m_renameStart = true;
}

void CanvasView::copy(editor::Document& doc) {
    const std::vector<int> sels = uic::selectedElements(doc);
    if (sels.empty()) return;
    const int c = doc.uiSelection().canvas;
    const std::string text = uic::copyElementsText(doc, c, sels);
    if (!SDL_SetClipboardText(text.c_str())) return notice(std::string("Cannot copy: ") + SDL_GetError(), true);
    notice("Copied " + describe(doc, c, sels), false);
}

void CanvasView::cut(editor::Document& doc) {
    const std::vector<int> sels = uic::selectedElements(doc);
    if (sels.empty()) return;
    const int c = doc.uiSelection().canvas;
    const std::string text = uic::copyElementsText(doc, c, sels);
    // Only removed once the clipboard holds it, so a failed copy loses nothing.
    if (!SDL_SetClipboardText(text.c_str())) return notice(std::string("Cannot cut: ") + SDL_GetError(), true);
    notice("Cut " + describe(doc, c, sels), false);
    uic::deleteElements(doc, c, sels);
}

bool CanvasView::pointerOnCanvas(std::array<int, 2>* at) const {
    if (m_scale <= 0 || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return false;
    if (!ImGui::IsMouseHoveringRect(m_area.Min, m_area.Max)) return false;
    const ImVec2 c = toCanvas(ImGui::GetIO().MousePos);
    *at = {int(std::floor(c.x)), int(std::floor(c.y))};
    return true;
}

void CanvasView::paste(editor::Document& doc, bool atPointer, const std::array<int, 2>* forced) {
    const editor::Document::UiSelection sel = doc.uiSelection();
    if (sel.canvas < 0) return;
    char* raw = SDL_GetClipboardText();
    const std::string text = raw ? raw : "";
    SDL_free(raw);
    const std::vector<int> sels = uic::selectedElements(doc);
    const int after = sels.empty() ? -1 : sels.back();
    // At the pointer when it is over the canvas, snapped the way a drag is; else offset from the originals.
    std::array<int, 2> pos{};
    bool placed = forced != nullptr;
    if (forced) pos = *forced;
    else if (atPointer) placed = pointerOnCanvas(&pos);
    if (placed && m_snap) {
        if (const std::optional<uic::Rect> b = uic::clipboardBounds(text, m_screen)) {
            const uic::Snapped sn = uic::snapMove(doc.scene().canvases[size_t(sel.canvas)], -1, uic::Rect{pos[0], pos[1], b->w, b->h},
                                                  m_screen, std::max(1, int(std::lround(6.0f / m_scale))));
            pos = {sn.rect.x, sn.rect.y};
        }
    }
    const int r = uic::pasteElements(doc, sel.canvas, after, text, m_screen, placed ? &pos : nullptr);
    if (r == -2) {
        status = "Cannot paste: a canvas holds at most " + std::to_string(uic::kMaxCanvasElements) + " UI elements";
        statusBad = true;
        notice(status, true);
    } else if (r < 0) {
        notice(text.find("splashedit-objects") != std::string::npos ? "The clipboard holds scene objects: paste them on the Scene tab"
                                                                     : "The clipboard holds no UI elements",
               true);
    }
}

void CanvasView::duplicate(editor::Document& doc) {
    const std::vector<int> sels = uic::selectedElements(doc);
    if (sels.empty()) return;
    if (uic::duplicateElements(doc, doc.uiSelection().canvas, sels) == -2) {
        status = "Cannot duplicate: a canvas holds at most " + std::to_string(uic::kMaxCanvasElements) + " UI elements";
        statusBad = true;
        notice(status, true);
    }
}

void CanvasView::remove(editor::Document& doc) {
    const std::vector<int> sels = uic::selectedElements(doc);
    if (!sels.empty()) uic::deleteElements(doc, doc.uiSelection().canvas, sels);
}

void CanvasView::select(editor::Document& doc, int c, const std::vector<int>& idx, int primary) {
    doc.selectUi(uic::selectionOf(c, idx, primary));
}

// The right-click menu of the canvas and the element list.
void CanvasView::elementMenu(editor::Document& doc) {
    pushMenuStyle();
    if (ImGui::BeginPopup("##elmenu")) {
        const editor::Document::UiSelection sel = doc.uiSelection();
        const bool has = uic::selectedElements(doc).size() == 1;  // the order commands act on one
        const bool any = hasElement(doc);
        const int n = sel.canvas >= 0 ? int(doc.scene().canvases[size_t(sel.canvas)].elements.size()) : 0;
        bool canPaste = false;
        if (sel.canvas >= 0 && SDL_HasClipboardText()) {
            char* raw = SDL_GetClipboardText();
            canPaste = raw && splash::readUIElementsText(raw).has_value();
            SDL_free(raw);
        }
        if (reportLayout) {
            // Scripted tests: where the first row is and how far apart rows are.
            char buf[96];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            std::snprintf(buf, sizeof buf, "uicanvas: menu %.0f,%.0f step %.0f", p.x + 40, p.y + 11, 22 + ImGui::GetStyle().ItemSpacing.y);
            if (m_menuReported != buf) {
                m_menuReported = buf;
                std::printf("%s\n", buf);
                std::fflush(stdout);
            }
        }
        if (menuItem(icon::x, color::textDim, "Cut", "Ctrl+X", any)) cut(doc);
        if (menuItem(icon::copy, color::textDim, "Copy", "Ctrl+C", any)) copy(doc);
        if (menuItem(icon::plus, color::textDim, "Paste", "Ctrl+V", canPaste)) paste(doc, false, m_menuAtCanvas ? &m_menuAt : nullptr);
        if (menuItem(icon::layers, color::textDim, "Duplicate", "Ctrl+D", any)) duplicate(doc);
        if (menuItem(icon::trash, color::bad, "Delete", "Del", any)) remove(doc);
        ImGui::Separator();
        if (menuItem(icon::arrowDown, color::textDim, "Bring forward", "Ctrl+]", has && sel.element + 1 < n))
            uic::moveElement(doc, sel.canvas, sel.element, sel.element + 1);
        if (menuItem(icon::arrowUp, color::textDim, "Send backward", "Ctrl+[", has && sel.element > 0))
            uic::moveElement(doc, sel.canvas, sel.element, sel.element - 1);
        ImGui::EndPopup();
    }
    popMenuStyle();
}

void CanvasView::keys(editor::Document& doc) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    const editor::Document::UiSelection sel = doc.uiSelection();
    if (sel.canvas < 0) return;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    auto pressed = [](ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); };
    if (ctrl && !shift && pressed(ImGuiKey_V, false)) paste(doc);
    if (ctrl && !shift && pressed(ImGuiKey_0, false)) m_zoom = 0, m_pan = ImVec2(0, 0);
    if (ctrl && !shift && pressed(ImGuiKey_1, false)) m_zoom = 1, m_pan = ImVec2(0, 0);
    if (ctrl && (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd))) zoomAt(stepZoom(m_scale, 1), m_area.GetCenter());
    if (ctrl && (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract))) zoomAt(stepZoom(m_scale, -1), m_area.GetCenter());
    const std::vector<int> sels = uic::selectedElements(doc);
    if (sels.empty()) return;
    const int c = sel.canvas, e = sel.element;
    const int n = int(doc.scene().canvases[size_t(c)].elements.size());
    if (ctrl && !shift && pressed(ImGuiKey_C, false)) copy(doc);
    if (ctrl && !shift && pressed(ImGuiKey_X, false)) {
        cut(doc);
        return;
    }
    if (!ctrl && m_drag.element < 0) {
        const int step = shift ? 8 : 1;
        int dx = 0, dy = 0;
        if (pressed(ImGuiKey_LeftArrow)) dx -= step;
        if (pressed(ImGuiKey_RightArrow)) dx += step;
        if (pressed(ImGuiKey_UpArrow)) dy -= step;
        if (pressed(ImGuiKey_DownArrow)) dy += step;
        if (dx || dy)
            uic::editElements(doc, c, sels, [&](UIElement& el, size_t) {
                if (el.type == UIElementType::Line) {
                    el.from = {el.from[0] + dx, el.from[1] + dy};
                    el.to = {el.to[0] + dx, el.to[1] + dy};
                    return;
                }
                uic::Rect r = uic::layout(el, m_screen);
                r.x += dx;
                r.y += dy;
                uic::place(el, r, m_screen);
            });
        if (pressed(ImGuiKey_Delete, false)) remove(doc);
        if (pressed(ImGuiKey_Escape, false)) doc.selectUi({c, -1});
    }
    if (ctrl && !shift && pressed(ImGuiKey_D, false)) duplicate(doc);
    if (sels.size() != 1) return;
    if (ctrl && pressed(ImGuiKey_RightBracket, false)) uic::moveElement(doc, c, e, shift ? n - 1 : e + 1);
    if (ctrl && pressed(ImGuiKey_LeftBracket, false) && e > 0) uic::moveElement(doc, c, e, shift ? 0 : e - 1);
}

// ---- Inspector

void CanvasView::inspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc) {
    const editor::Document::UiSelection sel = validSelection(doc);
    if (reportLayout) {
        // Scripted tests: what the inspector shows when that changes.
        const size_t n = uic::selectedElements(doc).size();
        const std::string what = n > 1 ? std::to_string(n) + " elements"
                                 : n == 1 ? "element " + doc.scene().canvases[size_t(sel.canvas)].elements[size_t(sel.element)].name
                                          : "canvas";
        if (m_inspReported != what) {
            m_inspReported = what;
            std::printf("uicanvas: inspector %s\n", what.c_str());
            std::fflush(stdout);
        }
    }
    ImGui::PushID("uicanvasinspector");
    ImGui::PushClipRect(ImVec2(r.Min.x, y), ImVec2(r.Max.x, r.Max.y - 1), true);
    if (sel.canvas >= 0 && uic::selectedElements(doc).size() > 1) multiInspector(dl, r, y, doc);
    else if (sel.canvas >= 0 && sel.element >= 0) elementInspector(dl, r, y, doc);
    else if (sel.canvas >= 0) canvasInspector(dl, r, y, doc);
    else textCentered(dl, ImRect(ImVec2(r.Min.x, y), r.Max), fonts().regular, type::body, color::textFaint, "No canvas");
    ImGui::PopClipRect();
    ImGui::PopID();
}

namespace {

// Inspector layout shared by the element and canvas inspectors.
struct Rows {
    ImDrawList* dl;
    float x0, x1, y;
    float lw = 92, rowH = 32;
    ImRect row(const char* id, const char* label, const char* tip) {
        ImRect rr(ImVec2(x0 + space::sm, y), ImVec2(x1 - space::sm, y + rowH));
        y += rowH;
        return property(id, rr, lw, label, tip);
    }
    bool sectionHead(const char* id, const char* ic, ImU32 col, const char* title) {
        const bool open = section(id, ImRect(ImVec2(x0, y), ImVec2(x1, y + 34)), ic, col, title, true, true, false);
        y += 34 + space::xs;
        return open;
    }
    void gap() { y += space::md; }
};

// An editable text well; true with `out` set when an edit is committed.
bool textField(const char* id, ImRect field, const std::string& value, const char* placeholder, std::string* out) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string typed;
    TextEdit res = textEdit(id, field, f.regular, type::body, field.Min.x + space::sm, field.Max.x - space::sm, &typed);
    if (res == TextEdit::Inactive) {
        Hit h = interact(id, field);
        dl->AddRectFilled(field.Min, field.Max, color::field, radius::field);
        dl->AddRect(field.Min, field.Max, lerpColor(color::border, color::borderStrong, h.hover), radius::field);
        ImGui::PushClipRect(field.Min, field.Max - ImVec2(space::xs, 0), true);
        const bool empty = value.empty();
        dl->AddText(f.regular, type::body, ImVec2(field.Min.x + space::sm, field.GetCenter().y - type::body * 0.5f - 1),
                    empty ? color::textFaint : color::text, empty ? placeholder : value.c_str());
        ImGui::PopClipRect();
        if (h.clicked) beginTextEdit(id, value);
        return false;
    }
    if (res == TextEdit::Commit && typed != value) {
        *out = typed;
        return true;
    }
    return false;
}

// A colour well with a picker under it. Edits merge under `key` until the picker lets go.
// Returns whether the picker is open. Scripted tests: the well is ui.<id>, the picker ui.<id>pick.
bool colourRow(editor::Document& doc, Rows& rows, const char* id, const char* label, const char* tip, std::array<float, 3> value,
               const std::function<void(UIElement&, const std::array<float, 3>&)>& set, int c, int e) {
    ImRect field = rows.row((std::string("l") + id).c_str(), label, tip);
    nameRect(std::string("ui.") + id, field);
    std::string popup = std::string("##pick") + id;
    if (colorField(id, field, toColor(value), toHex(value).c_str())) ImGui::OpenPopup(popup.c_str());
    ImGui::SetNextWindowPos(ImVec2(field.Max.x, field.Max.y + space::xs), ImGuiCond_Always, ImVec2(1, 0));
    pushMenuStyle();
    bool open = false;
    if (ImGui::BeginPopup(popup.c_str())) {
        open = true;
        float v[3] = {value[0], value[1], value[2]};
        ImGui::SetNextItemWidth(220);
        if (ImGui::ColorPicker3("##wheel", v, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoAlpha))
            uic::editElement(doc, c, e, [&](UIElement& el) { set(el, {v[0], v[1], v[2]}); }, std::string("uicol") + id);
        if (ImGui::IsItemDeactivated()) doc.endMerge();
        nameRect(std::string("ui.") + id + "pick", ImGui::GetCurrentWindow()->Rect());
        ImGui::EndPopup();
    }
    popMenuStyle();
    return open;
}

}  // namespace

void CanvasView::elementInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc) {
    Fonts& f = fonts();
    const editor::Document::UiSelection sel = validSelection(doc);
    const int c = sel.canvas, ei = sel.element;
    const UICanvas& cv = doc.scene().canvases[size_t(c)];
    const UIElement& e = cv.elements[size_t(ei)];
    const uic::Screen s = m_screen;
    Rows rows{dl, r.Min.x + space::md, r.Max.x - space::md, y};
    const float x0 = rows.x0, x1 = rows.x1;

    // Header: type badge, name (click to rename), where, visible.
    ImRect ic(ImVec2(x0, rows.y), ImVec2(x0 + 40, rows.y + 40));
    const ImU32 tc = typeColor(e.type);
    dl->AddRectFilled(ic.Min, ic.Max, (tc & 0x00ffffffu) | (34u << IM_COL32_A_SHIFT), radius::card);
    textCentered(dl, ic, f.medium, type::icon + 3, tc, typeIcon(e.type));
    std::string name;
    if (textField("uiname", ImRect(ImVec2(ic.Max.x + space::md, rows.y), ImVec2(x1 - 30 - space::md, rows.y + 22)), e.name, "(unnamed)",
                  &name))
        uic::editElement(doc, c, ei, [&](UIElement& el) { el.name = name; });
    char where[128];
    std::snprintf(where, sizeof where, "%s in %s, %d of %d", uic::typeName(e.type), cv.name.c_str(), ei + 1, int(cv.elements.size()));
    text(dl, ImVec2(ic.Max.x + space::md, rows.y + 25), f.regular, type::caption, color::textFaint, where);
    if (toggle("uivisible", ImRect(ImVec2(x1 - 30, rows.y + 12), ImVec2(x1, rows.y + 28)), e.visible))
        uic::editElement(doc, c, ei, [](UIElement& el) { el.visible = !el.visible; });
    tooltip("Visible when the scene starts. Lua's UI.SetVisible and cutscene tracks switch it while playing.");
    rows.y += 40 + space::lg;

    // Problems first, each with what to do. While a colour picker is open they
    // stay as they were when it opened: a Tinted warning showing or going as
    // the colour passes #808080 would move the picker under the pointer
    // (#157). They catch up when it closes.
    std::vector<uic::Problem> problems = uic::elementProblems(doc, c, ei, s);
    if (m_colourOpen && m_heldProblems.canvas == c && m_heldProblems.element == ei)
        problems = m_heldProblems.problems;
    else
        m_heldProblems = {c, ei, problems};
    m_colourOpen = false;
    for (size_t i = 0; i < problems.size(); i++) {
        bool fixed = false;
        ImGui::PushID(int(i));
        rows.y += problem(dl, "fix", ImVec2(x0, rows.y), x1 - x0, problems[i], &fixed) + space::sm;
        ImGui::PopID();
        if (fixed) uic::applyFix(doc, c, ei, problems[i].fix, s);
    }
    if (!problems.empty()) rows.y += space::xs;
    if (!e.unknown.empty()) return;

    auto number = [&](const char* id, ImRect field, int value, const char* unit, ImU32 axis, const char* axisLabel,
                      const std::function<void(UIElement&, int)>& set) {
        float v = float(value);
        nameRect(std::string("ui.") + id, field);  // scripted tests: ui.posa, ui.sizeb, ...
        FieldEdit fe = numberField(id, field, std::to_string(value).c_str(), unit, axis, axisLabel, &v, true);
        if (fe.changed) uic::editElement(doc, c, ei, [&](UIElement& el) { set(el, int(std::lround(v))); }, std::string("uinum") + id);
        if (fe.done) doc.endMerge();
    };
    auto pair = [&](const char* id, const char* label, const char* tip, int a, int b, const char* la, const char* lb,
                    const std::function<void(UIElement&, int)>& setA, const std::function<void(UIElement&, int)>& setB) {
        ImRect field = rows.row((std::string("l") + id).c_str(), label, tip);
        const float half = (field.GetWidth() - space::xs) * 0.5f;
        number((std::string(id) + "a").c_str(), ImRect(field.Min, ImVec2(field.Min.x + half, field.Max.y)), a, nullptr, color::axisX, la, setA);
        number((std::string(id) + "b").c_str(), ImRect(ImVec2(field.Max.x - half, field.Min.y), field.Max), b, nullptr, color::axisY, lb, setB);
    };

    // Layout.
    if (rows.sectionHead("uilayout", icon::move, color::textDim, "Layout")) {
        if (e.type == UIElementType::Line) {
            pair("from", "From", "One end of the line, in screen pixels.", e.from[0], e.from[1], "X", "Y",
                 [](UIElement& el, int v) { el.from[0] = v; }, [](UIElement& el, int v) { el.from[1] = v; });
            pair("to", "To", "The other end, in screen pixels.", e.to[0], e.to[1], "X", "Y", [](UIElement& el, int v) { el.to[0] = v; },
                 [](UIElement& el, int v) { el.to[1] = v; });
        } else {
            const uic::Rect lr = uic::layout(e, s);
            auto setRect = [&](int which) {
                return [this, which](UIElement& el, int v) {
                    uic::Rect q = uic::layout(el, m_screen);
                    (which == 0 ? q.x : which == 1 ? q.y : which == 2 ? q.w : q.h) = which >= 2 ? std::max(1, v) : v;
                    uic::place(el, q, m_screen);
                };
            };
            pair("pos", "Position", "Top-left corner on the screen, in pixels. The scene stores it as an offset from the anchor.",
                 lr.x, lr.y, "X", "Y", setRect(0), setRect(1));
            pair("size", "Size", "Width and height in pixels.", lr.w, lr.h, "W", "H", setRect(2), setRect(3));
            ImRect af = rows.row("lanchor", "Anchor",
                                 "What the position is kept relative to if the screen size changes. Stretched sides follow both edges.");
            const std::string al = anchorLabel(e);
            if (pickField("anchor", af, nullptr, 0, al.c_str())) ImGui::OpenPopup("##anchormenu");
            ImGui::SetNextWindowPos(ImVec2(af.Min.x, af.Max.y + space::xs));
            pushMenuStyle();
            if (ImGui::BeginPopup("##anchormenu")) {
                for (const AnchorPreset& a : kAnchors)
                    if (menuItem(al == a.label ? icon::chevronRight : nullptr, color::accent, a.label))
                        uic::editElement(doc, c, ei, [&](UIElement& el) {
                            const uic::Rect keep = uic::layout(el, m_screen);
                            el.anchorMin = {a.minX, a.minY};
                            el.anchorMax = {a.maxX, a.maxY};
                            uic::place(el, keep, m_screen);  // stays where it is on screen
                        });
                ImGui::EndPopup();
            }
            popMenuStyle();
        }
        rows.gap();
    }

    // Appearance, per type.
    const char* title = e.type == UIElementType::Text ? "Text" : e.type == UIElementType::Image ? "Image"
                      : e.type == UIElementType::Progress ? "Progress bar" : e.type == UIElementType::Line ? "Line" : "Box";
    if (rows.sectionHead("uilook", typeIcon(e.type), tc, title)) {
        if (e.type == UIElementType::Text) {
            ImRect tf = rows.row("ltext", "Text", "ASCII, at most 63 bytes. Lua's UI.SetText changes it while playing.");
            std::string t;
            if (textField("text", tf, e.text, "(empty)", &t)) uic::editElement(doc, c, ei, [&](UIElement& el) { el.text = t; });
            // No font of its own: its canvas's default font, else the system
            // font. With a canvas default there is no way to pick the system
            // font for one element (as in Unity's PSXCanvas).
            const UICanvas& ecv = doc.scene().canvases[size_t(c)];
            const std::string own = ecv.defaultFont.empty() ? std::string("System (8 x 16)") : "Canvas default (" + ecv.defaultFont + ")";
            ImRect ff = rows.row("lfont", "Font",
                                 "The system font (8 x 16, fixed width) or one of the scene's fonts. Without one of its own, "
                                 "a text element uses its canvas's default font.");
            nameRect("ui.font", ff);
            const std::string fl = e.font.empty() ? own : e.font;
            if (pickField("font", ff, icon::type, rgb(0xf2c25c), fl.c_str())) ImGui::OpenPopup("##fontmenu");
            ImGui::SetNextWindowPos(ImVec2(ff.Min.x, ff.Max.y + space::xs));
            pushMenuStyle();
            if (ImGui::BeginPopup("##fontmenu")) {
                const bool pickOwn = menuItem(e.font.empty() ? icon::chevronRight : nullptr, color::accent, own.c_str());
                nameRect("ui.font/" + own, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                if (pickOwn) uic::editElement(doc, c, ei, [](UIElement& el) { el.font.clear(); });
                for (const splash::UIFont& uf : doc.scene().fonts) {
                    std::string meta = uf.bitmap.empty() ? std::to_string(uf.size) + " px" : "bitmap";
                    const bool picked = menuItem(e.font == uf.name ? icon::chevronRight : nullptr, color::accent, uf.name.c_str(), meta.c_str());
                    nameRect("ui.font/" + uf.name, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                    if (picked) uic::editElement(doc, c, ei, [&](UIElement& el) { el.font = uf.name; });
                }
                ImGui::EndPopup();
            }
            popMenuStyle();
            // The width of the font it draws with (its own or its canvas's default).
            const int tw = uic::textWidth(doc.scene(), m_assets, splash::effectiveFont(doc.scene().canvases[size_t(c)], e), e.text);
            if (reportLayout) {
                // Scripted tests: what the Font field shows and the width it resolves to.
                const std::string line = fl + " width " + std::to_string(tw);
                if (m_elFontReported != line) {
                    std::printf("uicanvas: element font %s\n", (m_elFontReported = line).c_str());
                    std::fflush(stdout);
                }
            }
            char info[96];
            std::snprintf(info, sizeof info, "%d px wide, %d of %d bytes", tw, int(std::min<size_t>(e.text.size(), 999)), uic::kTextMax);
            text(dl, ImVec2(x0 + space::sm + rows.lw, rows.y - 4), f.regular, type::caption, color::textFaint, info);
            rows.y += 16;
        }
        if (e.type == UIElementType::Image) {
            // Sheet: a scene sprite sheet this image shows one cell of (its
            // texture, bit depth and cutout then stand in for the image's own).
            const auto& sheets = doc.scene().spriteSheets;
            ImRect shf = rows.row("lsheet", "Sheet",
                                  "None, or a scene sprite sheet: the image then shows one of its cells, from the sheet's "
                                  "texture (packed once for both). Lua's UI.SetFrame steps through the cells while playing.");
            nameRect("ui.sheet", shf);
            {
                std::vector<const char*> items{"None"};
                int cur = 0;
                for (size_t k = 0; k < sheets.size(); k++) {
                    items.push_back(sheets[k].name.c_str());
                    if (sheets[k].name == e.sheet) cur = int(k) + 1;
                }
                const std::string shown = e.sheet.empty() ? std::string("None") : cur ? e.sheet : e.sheet + " (missing)";
                const int picked = dropdownMenu("ui.sheet", shf, icon::image, shown.c_str(), items, e.sheet.empty() ? 0 : cur);
                if (picked == 0) uic::setImageSheet(doc, c, ei, "");
                else if (picked > 0) uic::setImageSheet(doc, c, ei, sheets[size_t(picked - 1)].name);
            }
            const splash::SpriteSheet* sheet = uic::imageSheet(doc.scene(), e);
            std::string sheetLine = "none";
            if (!e.sheet.empty() && sheet) {
                static const uic::Assets::Texture none;
                const uic::Assets::Texture& t = sheet->texture.empty() ? none : m_assets.texture(sheet->texture, sheet->bitDepth, true);
                const int cw = std::max(1, sheet->cellW), ch = std::max(1, sheet->cellH);
                const int cols = t.width / cw, rows_ = t.height / ch, cells = cols * rows_;
                const uic::Rect lr = uic::layout(e, s);
                rows.y += 4;
                char info[128];
                if (t.error.empty() && cells > 0)
                    std::snprintf(info, sizeof info, "cell %d of %d, %d x %d texels%s", e.cell, cells, cw, ch,
                                  e.cell >= cells ? ": outside the sheet" : "");
                else
                    std::snprintf(info, sizeof info, "%s", sheet->texture.empty() ? "The sheet has no texture" : "The sheet's texture cannot be read");
                text(dl, ImVec2(x0 + space::sm + rows.lw, rows.y - 4), f.regular, type::caption, e.cell >= cells ? color::bad : color::textFaint, info);
                if (button("ui.snapsize", ImVec2(x1 - space::sm - 100, rows.y - 8), nullptr, "Snap size", ButtonKind::Ghost, nullptr,
                           "Sets the size to one cell of the sheet, one texel per screen pixel."))
                    uic::snapImageToCell(doc, c, ei, m_screen);
                rows.y += 26;
                sheetLine = e.sheet + " cell " + std::to_string(e.cell) + " of " + std::to_string(cells) + " grid " + std::to_string(cols) +
                            "x" + std::to_string(rows_) + " size " + std::to_string(lr.w) + "x" + std::to_string(lr.h);
                if (t.error.empty() && cells > 0) {
                    // The cell picker: the whole sheet as the PS1 shows it, the
                    // grid over it and the image's cell outlined. Click a cell.
                    if (m_sheetKey != sheet->texture + "|" + std::to_string(int(sheet->bitDepth)) + "|" + std::to_string(doc.assetsId()) || !m_sheetTex) {
                        m_sheetKey = sheet->texture + "|" + std::to_string(int(sheet->bitDepth)) + "|" + std::to_string(doc.assetsId());
                        std::vector<uint32_t> px(size_t(t.width) * size_t(t.height), 0);
                        auto ex = [](int c5) { return uint32_t((c5 << 3) | (c5 >> 2)); };
                        for (size_t k = 0; k < px.size(); k++)
                            if (const uint16_t w = t.words[k])
                                px[k] = 0xff000000u | (ex((w >> 10) & 31) << 16) | (ex((w >> 5) & 31) << 8) | ex(w & 31);
                        if (!m_sheetTex) glGenTextures(1, &m_sheetTex);
                        glBindTexture(GL_TEXTURE_2D, m_sheetTex);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                        glBindTexture(GL_TEXTURE_2D, 0);
                    }
                    const float pad = space::sm, availW = x1 - x0 - 2 * space::sm - 2 * pad, maxH = 240;
                    const float fit = std::min(availW / t.width, maxH / t.height);
                    const float z = fit >= 1 ? std::floor(fit) : fit;
                    const ImVec2 box0(x0 + space::sm, rows.y);
                    const ImRect box(box0, ImVec2(x1 - space::sm, rows.y + t.height * z + 2 * pad));
                    dl->AddRectFilled(box.Min, box.Max, rgb(0x14171c), radius::field);
                    dl->AddRect(box.Min, box.Max, color::border, radius::field);
                    const ImVec2 so(std::floor(box.GetCenter().x - t.width * z * 0.5f), box.Min.y + pad);
                    const ImRect sr(so, so + ImVec2(t.width * z, t.height * z));
                    nameRect("ui.sheetcells", sr);
                    dl->PushClipRect(sr.Min, sr.Max, true);
                    const float sq = std::max(4.0f, std::min(8.0f, cw * z / 2));
                    for (float cy = sr.Min.y; cy < sr.Max.y; cy += sq)
                        for (float cx = sr.Min.x; cx < sr.Max.x; cx += sq)
                            dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + sq, cy + sq),
                                              (int((cx - sr.Min.x) / sq) + int((cy - sr.Min.y) / sq)) % 2 ? rgb(0x2a2f39) : rgb(0x1f232b));
                    dl->PopClipRect();
                    ui::addTextureImage(dl, (ImTextureID)(intptr_t)m_sheetTex, sr.Min, sr.Max);
                    for (int k = 1; k < cols; k++)
                        dl->AddLine(ImVec2(sr.Min.x + k * cw * z, sr.Min.y), ImVec2(sr.Min.x + k * cw * z, sr.Max.y), rgb(0xffffff, 70));
                    for (int k = 1; k < rows_; k++)
                        dl->AddLine(ImVec2(sr.Min.x, sr.Min.y + k * ch * z), ImVec2(sr.Max.x, sr.Min.y + k * ch * z), rgb(0xffffff, 70));
                    auto cellRect = [&](int k) {
                        const ImVec2 m(sr.Min.x + (k % cols) * cw * z, sr.Min.y + (k / cols) * ch * z);
                        return ImRect(m, m + ImVec2(cw * z, ch * z));
                    };
                    for (int k = 0; k < cells && k < 256; k++) nameRect("ui.sheetcell/" + std::to_string(k), cellRect(k));
                    const Hit h = interact("ui.sheetcells", sr);
                    if (h.hovered) {
                        const ImVec2 m = ImGui::GetIO().MousePos;
                        const int hc = std::clamp(int((m.x - sr.Min.x) / (cw * z)), 0, cols - 1) +
                                       std::clamp(int((m.y - sr.Min.y) / (ch * z)), 0, rows_ - 1) * cols;
                        const ImRect hr = cellRect(hc);
                        dl->AddRectFilled(hr.Min, hr.Max, rgb(0xffffff, 28));
                        if (h.clicked && hc != e.cell && hc < 256) uic::setImageCell(doc, c, ei, hc);
                    }
                    if (e.cell >= 0 && e.cell < cells) {
                        const ImRect now = cellRect(e.cell);
                        dl->AddRect(now.Min - ImVec2(1, 1), now.Max + ImVec2(1, 1), color::accentHover, 0.0f, 0, 2.0f);
                    }
                    rows.y = box.Max.y + space::xs;
                    text(dl, ImVec2(x0 + space::sm, rows.y), f.regular, type::caption, color::textFaint,
                         "PS1 colours at the sheet's bit depth. Click a cell to show it.");
                    rows.y += 18 + space::sm;
                }
            } else if (!e.sheet.empty()) {
                sheetLine = e.sheet + " missing";
            }
            if (reportLayout && m_sheetReported != sheetLine) {
                // Scripted tests: what the Sheet row and the cell picker show.
                std::printf("uicanvas: image sheet %s\n", (m_sheetReported = sheetLine).c_str());
                std::fflush(stdout);
            }
            if (e.sheet.empty()) {
                ImRect pf = rows.row("ltex", "Texture", "A project-relative image file. Browse copies one from outside into textures/.");
                std::string p;
                ImRect tfield(pf.Min, ImVec2(pf.Max.x - size::field - space::xs, pf.Max.y));
                if (textField("tex", tfield, e.texture, "textures/hud.png", &p)) uic::editElement(doc, c, ei, [&](UIElement& el) { el.texture = p; });
                if (iconButton("texbrowse", ImRect(ImVec2(pf.Max.x - size::field, pf.Min.y), pf.Max), icon::folderOpen, false, "Pick an image")) {
                    {
                        std::lock_guard<std::mutex> lock(g_texMutex);
                        g_texCanvas = c, g_texElement = ei;
                    }
                    static const SDL_DialogFileFilter images[] = {{"Images", "png;jpg;jpeg;bmp;tga;gif"}};
                    SDL_ShowOpenFileDialog(onTexturePicked, nullptr, nullptr, images, 1, nullptr, false);
                }
                if (!e.texture.empty()) {
                    const uic::Assets::Texture& t = m_assets.texture(e.texture, e.bitDepth, e.cutout);
                    if (t.error.empty() && t.width > 0) {
                        char info[64];
                        std::snprintf(info, sizeof info, "%d x %d texels", t.width, t.height);
                        text(dl, ImVec2(x0 + space::sm + rows.lw, rows.y - 4), f.regular, type::caption, color::textFaint, info);
                        const uic::Rect lr = uic::layout(e, s);
                        if ((lr.w != t.width || lr.h != t.height) &&
                            button("native", ImVec2(x1 - space::sm - 120, rows.y - 8), nullptr, "Native size", ButtonKind::Ghost, nullptr,
                                   "Sets the size to the texture's, one texel per screen pixel."))
                            uic::editElement(doc, c, ei, [&](UIElement& el) {
                                uic::Rect q = uic::layout(el, m_screen);
                                q.w = t.width;
                                q.h = t.height;
                                uic::place(el, q, m_screen);
                            });
                        rows.y += 22;
                    }
                }
                ImRect bf = rows.row("lbpp", "Bit depth", "4 and 8 bpp use a palette (16 and 256 colours) and less VRAM.");
                const int cur = e.bitDepth == splash::BitDepth::Bpp4 ? 0 : e.bitDepth == splash::BitDepth::Bpp8 ? 1 : 2;
                const int picked = dropdownMenu("bpp", bf, nullptr, cur == 0 ? "4 bpp" : cur == 1 ? "8 bpp" : "16 bpp", {"4 bpp", "8 bpp", "16 bpp"}, cur);
                if (picked >= 0 && picked != cur)
                    uic::editElement(doc, c, ei, [&](UIElement& el) {
                        el.bitDepth = picked == 0 ? splash::BitDepth::Bpp4 : picked == 1 ? splash::BitDepth::Bpp8 : splash::BitDepth::Bpp16;
                    });
                ImRect cf = rows.row("lcut", "Cutout", "Transparent texels (alpha under half) are left out instead of drawn black.");
                if (toggle("cut", ImRect(ImVec2(cf.Min.x, cf.GetCenter().y - 8), ImVec2(cf.Min.x + 30, cf.GetCenter().y + 8)), e.cutout))
                    uic::editElement(doc, c, ei, [](UIElement& el) { el.cutout = !el.cutout; });
                if (!e.texture.empty()) {
                    // What the console will show, next to the source, at the bit depth
                    // and cutout above: like the mesh inspector, after its settings.
                    rows.y += texturePreview(m_texPreview, dl, x0 + space::sm, x1 - space::sm, rows.y, doc, e.texture, e.bitDepth, e.cutout) +
                              space::sm;
                    if (reportLayout && m_texPreview.shown != m_texPreviewReported) {
                        // Scripted tests: what the preview shows when that changes.
                        m_texPreviewReported = m_texPreview.shown;
                        if (!m_texPreview.shown.empty()) {
                            std::printf("uicanvas: texpreview %s\n", m_texPreview.shown.c_str());
                            std::fflush(stdout);
                        }
                    }
                }
            }
        }
        if (e.type == UIElementType::Progress) {
            ImRect vf = rows.row("lval", "Value", "How full the bar is, 0 to 100. Lua's UI.SetProgress changes it while playing.");
            number("val", vf, e.value, "%", 0, nullptr, [](UIElement& el, int v) { el.value = v; });
            m_colourOpen |= colourRow(doc, rows, "bg", "Background", "The empty part of the bar.", e.background,
                                      [](UIElement& el, const std::array<float, 3>& v) { el.background = v; }, c, ei);
        }
        m_colourOpen |= colourRow(doc, rows, "col", e.type == UIElementType::Image ? "Tint" : e.type == UIElementType::Progress ? "Fill" : "Colour",
                                  e.type == UIElementType::Image ? "Multiplies the texture by colour/128: #808080 shows it as it is."
                                                                 : "Cut to 15-bit colour on the PS1, as the preview shows.",
                                  e.color, [](UIElement& el, const std::array<float, 3>& v) { el.color = v; }, c, ei);
        rows.gap();
    }
}

// Several elements selected: only what they share. Position is the top-left of
// the box around them; editing it moves them all together.
void CanvasView::multiInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc) {
    Fonts& f = fonts();
    const int c = doc.uiSelection().canvas;
    const std::vector<int> sels = uic::selectedElements(doc);
    const UICanvas& cv = doc.scene().canvases[size_t(c)];
    Rows rows{dl, r.Min.x + space::md, r.Max.x - space::md, y};
    const float x0 = rows.x0, x1 = rows.x1;
    ImRect ic(ImVec2(x0, rows.y), ImVec2(x0 + 40, rows.y + 40));
    dl->AddRectFilled(ic.Min, ic.Max, (color::accent & 0x00ffffffu) | (34u << IM_COL32_A_SHIFT), radius::card);
    textCentered(dl, ic, f.medium, type::icon + 3, color::accent, icon::layers);
    char head[64];
    std::snprintf(head, sizeof head, "%d elements", int(sels.size()));
    text(dl, ImVec2(ic.Max.x + space::md, rows.y + 2), f.medium, type::body, color::text, head);
    text(dl, ImVec2(ic.Max.x + space::md, rows.y + 25), f.regular, type::caption, color::textFaint, ("in " + cv.name).c_str());
    rows.y += 40 + space::lg;

    int x = INT_MAX, yy = INT_MAX;
    for (int i : sels) {
        const uic::Rect b = uic::boundsOf(cv.elements[size_t(i)], m_screen);
        x = std::min(x, b.x), yy = std::min(yy, b.y);
    }
    if (rows.sectionHead("uilayout", icon::move, color::textDim, "Layout")) {
        ImRect field = rows.row("lpos", "Position", "Top-left corner of the box around all of them, in pixels. Changing it moves them together.");
        const float half = (field.GetWidth() - space::xs) * 0.5f;
        auto number = [&](const char* id, ImRect fr, int value, ImU32 axis, const char* axisLabel, bool isX) {
            float v = float(value);
            FieldEdit fe = numberField(id, fr, std::to_string(value).c_str(), nullptr, axis, axisLabel, &v, true);
            if (fe.changed) {
                const int d = int(std::lround(v)) - value;
                uic::editElements(doc, c, sels, [&](UIElement& el, size_t) {
                    uic::Rect q = uic::boundsOf(el, m_screen);
                    if (isX) q.x += d;
                    else q.y += d;
                    uic::place(el, q, m_screen);
                }, std::string("uinum") + id);
            }
            if (fe.done) doc.endMerge();
        };
        number("mposa", ImRect(field.Min, ImVec2(field.Min.x + half, field.Max.y)), x, color::axisX, "X", true);
        number("mposb", ImRect(ImVec2(field.Max.x - half, field.Min.y), field.Max), yy, color::axisY, "Y", false);
        rows.gap();
    }
}

void CanvasView::canvasInspector(ImDrawList* dl, ImRect r, float y, editor::Document& doc) {
    Fonts& f = fonts();
    const int c = validSelection(doc).canvas;
    const UICanvas& cv = doc.scene().canvases[size_t(c)];
    if (m_cvScrolled != c) {
        m_cvScrolled = c;
        m_cvScroll = 0;
    }
    const float top0 = y;
    Rows rows{dl, r.Min.x + space::md, r.Max.x - space::md, y - m_cvScroll};
    const float x0 = rows.x0, x1 = rows.x1;
    ImRect ic(ImVec2(x0, rows.y), ImVec2(x0 + 40, rows.y + 40));
    dl->AddRectFilled(ic.Min, ic.Max, rgb(0x5ccfe0, 34), radius::card);
    textCentered(dl, ic, f.medium, type::icon + 3, rgb(0x5ccfe0), icon::layoutTemplate);
    std::string name;
    if (textField("cvname", ImRect(ImVec2(ic.Max.x + space::md, rows.y), ImVec2(x1, rows.y + 22)), cv.name, "(unnamed)", &name))
        uic::editCanvas(doc, c, [&](UICanvas& x) { x.name = name; });
    char where[96];
    std::snprintf(where, sizeof where, "UI canvas %d of %d, %d elements", c + 1, int(doc.scene().canvases.size()), int(cv.elements.size()));
    text(dl, ImVec2(ic.Max.x + space::md, rows.y + 25), f.regular, type::caption, color::textFaint, where);
    rows.y += 40 + space::lg;

    for (const uic::Problem& p : uic::canvasProblems(doc, c)) rows.y += problem(dl, "cvfix", ImVec2(x0, rows.y), x1 - x0, p, nullptr) + space::sm;

    if (rows.sectionHead("cvsec", icon::layoutTemplate, rgb(0x5ccfe0), "Canvas")) {
        ImRect vf = rows.row("lcvvis", "Visible", "Shown when the scene starts. UI.SetCanvasVisible switches it while playing.");
        if (toggle("cvvis", ImRect(ImVec2(vf.Min.x, vf.GetCenter().y - 8), ImVec2(vf.Min.x + 30, vf.GetCenter().y + 8)), cv.visible))
            uic::editCanvas(doc, c, [](UICanvas& x) { x.visible = !x.visible; });
        ImRect sf = rows.row("lcvsort", "Sort order", "0 is at the back, 255 in front; canvases with higher orders draw over lower ones.");
        float v = float(cv.sortOrder);
        FieldEdit fe = numberField("cvsort", sf, std::to_string(cv.sortOrder).c_str(), nullptr, 0, nullptr, &v, true);
        if (fe.changed)
            uic::editCanvas(doc, c, [&](UICanvas& x) { x.sortOrder = std::clamp(int(std::lround(v)), 0, 255); }, "uicvsort");
        if (fe.done) doc.endMerge();

        // Default font: what its text elements without a font of their own draw with.
        const std::string dfont = cv.defaultFont;  // a copy: a pick below replaces the canvas
        if (reportLayout && m_cvFontReported != (dfont.empty() ? "none" : dfont)) {
            m_cvFontReported = dfont.empty() ? "none" : dfont;
            std::printf("uicanvas: default font %s\n", m_cvFontReported.c_str());
            std::fflush(stdout);
        }
        ImRect df = rows.row("lcvfont", "Default font", "Text elements without a font of their own use this one.");
        nameRect("cv.font", df);
        const std::string dl0 = dfont.empty() ? std::string("System (8 x 16)") : dfont;
        if (pickField("cvfont", df, icon::type, rgb(0xf2c25c), dl0.c_str())) ImGui::OpenPopup("##cvfontmenu");
        ImGui::SetNextWindowPos(ImVec2(df.Min.x, df.Max.y + space::xs));
        pushMenuStyle();
        if (ImGui::BeginPopup("##cvfontmenu")) {
            const char* sys = "System (8 x 16)";
            const bool pickSys = menuItem(dfont.empty() ? icon::chevronRight : nullptr, color::accent, sys);
            nameRect(std::string("cv.font/") + sys, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            if (pickSys) uic::editCanvas(doc, c, [](UICanvas& x) { x.defaultFont.clear(); });
            for (const splash::UIFont& uf : doc.scene().fonts) {
                std::string meta = uf.bitmap.empty() ? std::to_string(uf.size) + " px" : "bitmap";
                const bool picked = menuItem(dfont == uf.name ? icon::chevronRight : nullptr, color::accent, uf.name.c_str(), meta.c_str());
                nameRect("cv.font/" + uf.name, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                if (picked) uic::editCanvas(doc, c, [&](UICanvas& x) { x.defaultFont = uf.name; });
            }
            ImGui::EndPopup();
        }
        popMenuStyle();
        rows.gap();
    }
    fontsSection(dl, x0, x1, &rows.y, doc);
    dl->AddText(f.regular, type::caption, ImVec2(x0, rows.y), color::textFaint,
                "Select an element on the canvas or in the list to edit it.", nullptr, x1 - x0);
    rows.y += 40;

    // Taller than the panel, it scrolls with the wheel.
    const float content = rows.y - (top0 - m_cvScroll), view = r.Max.y - top0;
    const float maxScroll = std::max(0.0f, content - view);
    if (maxScroll > 0 && ImGui::IsMouseHoveringRect(r.Min, r.Max) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        m_cvScroll -= ImGui::GetIO().MouseWheel * 48.0f;
    m_cvScroll = std::clamp(m_cvScroll, 0.0f, maxScroll);
    if (maxScroll > 0) {
        const float h = view * view / content, t = top0 + (view - h) * (m_cvScroll / maxScroll);
        dl->AddRectFilled(ImVec2(r.Max.x - 5, t + 2), ImVec2(r.Max.x - 2, t + h - 2), rgb(0xffffff, 40), 2);
    }
}

std::vector<std::string> CanvasView::fontFiles(const editor::Document& doc) {
    // The asset browser's listing of the project; walked again at most every two seconds.
    const double now = ImGui::GetTime();
    if (m_fontFiles.project() != doc.projectRoot() || now - m_fontFilesAt > 2.0) {
        m_fontFilesAt = now;
        if (!doc.projectRoot().empty()) m_fontFiles.scan(doc.projectRoot());
    }
    std::vector<std::string> out;
    for (const editor::AssetEntry& e : m_fontFiles.entries())
        if (e.type == editor::AssetType::Font) out.push_back(e.path);
    for (const editor::AssetEntry& e : m_fontFiles.entries())
        if (e.type != editor::AssetType::Folder && uic::isBitmapFontFile(e.path)) out.push_back(e.path);
    return out;
}

// The Scene fonts section: each font's name, file, size (or bitmap cells), what
// the export makes of it and a sample line; Add font under them.
void CanvasView::fontsSection(ImDrawList* dl, float x0, float x1, float* yp, editor::Document& doc) {
    Fonts& f = fonts();
    const std::vector<splash::UIFont>& fl = doc.scene().fonts;
    const int n = int(fl.size());
    auto say = [](const std::string& line) {
        std::printf("uicanvas: %s\n", line.c_str());
        std::fflush(stdout);
    };

    // What each font builds to: the info line, the scripted line.
    struct Built {
        const splash::FontSheet* sheet = nullptr;
        std::string error, info, line;
    };
    std::vector<Built> built(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const splash::UIFont& uf = fl[size_t(i)];
        Built& b = built[size_t(i)];
        const bool bitmap = !uf.bitmap.empty();
        if (uf.source.empty() && uf.bitmap.empty()) b.error = "no source file: pick a .ttf, .otf or .png";
        else b.sheet = m_assets.font(doc.scene(), uf.name, &b.error);
        if (b.sheet) {
            const splash::FontSheet& sh = *b.sheet;
            const size_t vram = sh.packed4bpp().size();
            char info[160];
            std::snprintf(info, sizeof info, "%d x %d cells of %d x %d, sheet 256 x %d, VRAM %zu bytes", 256 / std::max(1, sh.glyphWidth),
                          sh.height / std::max(1, sh.glyphHeight), sh.glyphWidth, sh.glyphHeight, sh.height, vram);
            b.info = info;
            b.line = "fonts: " + std::to_string(i) + " " + uf.name +
                     (bitmap ? " bitmap " + uf.bitmap + " " + std::to_string(uf.glyphWidth) + "x" + std::to_string(uf.glyphHeight)
                             : " ttf " + uf.source + " " + std::to_string(uf.size) + "px") +
                     " vram " + std::to_string(vram);
        } else {
            if (b.error.empty()) b.error = "cannot be built";
            b.info = b.error;
            b.line = "fonts: " + std::to_string(i) + " " + uf.name + " error " + b.error;
        }
    }
    if (reportLayout) {
        // Scripted tests: each font's line and the count, when they change.
        m_fontsReported.resize(size_t(n));
        for (int i = 0; i < n; i++)
            if (m_fontsReported[size_t(i)] != built[size_t(i)].line) say(m_fontsReported[size_t(i)] = built[size_t(i)].line);
        const std::string count = "fonts: count " + std::to_string(n) + (n < uic::kMaxFonts ? " add enabled" : " add disabled");
        if (m_fontsCountReported != count) say(m_fontsCountReported = count);
    }

    Rows rows{dl, x0, x1, *yp};
    if (!rows.sectionHead("cvfonts", icon::type, rgb(0xf2c25c), "Scene fonts")) {
        *yp = rows.y;
        return;
    }
    if (fl.empty()) {
        text(dl, ImVec2(x0 + space::sm, rows.y), f.regular, type::label, color::textFaint, "Only the system font (8 x 16).");
        rows.y += 24;
    }
    std::vector<std::string> files;
    bool listed = false;
    auto list = [&]() -> const std::vector<std::string>& {
        if (!listed) files = fontFiles(doc), listed = true;
        return files;
    };
    int remove = -1;
    for (int i = 0; i < n; i++) {
        const splash::UIFont& uf = fl[size_t(i)];
        const Built& b = built[size_t(i)];
        const std::string id = "cvfont." + std::to_string(i);
        ImGui::PushID(i);

        // A line per font: which one, how many text elements use it, Delete.
        const int uses = uic::fontUses(doc.scene(), uf.name);
        char head[96];
        std::snprintf(head, sizeof head, "Font %d of %d   %s", i + 1, n,
                      uses == 0 ? "unused" : uses == 1 ? "used by 1 text element" : (std::to_string(uses) + " text elements").c_str());
        text(dl, ImVec2(x0 + space::sm, rows.y + 8), f.medium, type::caption, color::textDim, head);
        const char* delTip = "Removes the font. Text elements and canvases that use it go back to the system font (a text element "
                             "to its canvas's default font, if that is another). Ctrl+Z brings them back.";
        const float dw = buttonWidth(icon::trash, "Delete");
        if (button((id + ".delete").c_str(), ImVec2(x1 - space::sm - dw, rows.y), icon::trash, "Delete", ButtonKind::Ghost, nullptr, delTip))
            remove = i;
        rows.y += size::field + 6 + space::xs;

        // Name: text elements refer to the font by it, so a rename renames theirs.
        ImRect nf = rows.row("lfname", "Name", "What text elements pick the font by. Renaming it renames it in every text element that uses it.");
        nameRect(id + ".name", nf);
        std::string typed;
        if (textField("fname", nf, uf.name, "(unnamed)", &typed)) {
            std::string why;
            if (!uic::renameFont(doc, i, typed, &why) && !why.empty()) {
                notice("Font not renamed: " + why, true);
                if (reportLayout) say("fonts: rename refused: " + why);
            }
        }

        // The file: a TTF/OTF rasterised at Size, or a 256-wide bitmap PNG.
        const bool bitmap = !uf.bitmap.empty();
        ImRect sf = rows.row("lfsrc", "Source",
                             "A project .ttf or .otf, rasterised at Size, or a 256-wide .png of glyph cells (characters 32 to 127 in rows).");
        nameRect(id + ".source", sf);
        const std::string shown = bitmap ? uf.bitmap : uf.source.empty() ? std::string("pick a source") : uf.source;
        if (pickField("fsrc", sf, bitmap ? icon::image : icon::type, rgb(0xf2c25c), shown.c_str())) ImGui::OpenPopup("##fontsrcmenu");
        ImGui::SetNextWindowPos(ImVec2(sf.Min.x, sf.Max.y + space::xs));
        pushMenuStyle();
        if (ImGui::BeginPopup("##fontsrcmenu")) {
            const std::vector<std::string>& fs = list();
            if (fs.empty()) menuItem(nullptr, color::textDim, "No .ttf, .otf or .png in the project", nullptr, false);
            for (const std::string& p : fs) {
                const bool png = uic::isBitmapFontFile(p);
                const bool cur = png ? p == uf.bitmap : p == uf.source;
                if (menuItem(cur ? icon::chevronRight : png ? icon::image : icon::type, color::accent, p.c_str(), png ? "bitmap" : "TTF/OTF"))
                    uic::editFont(doc, i, [&](splash::UIFont& x) { uic::setFontFile(x, p); });
            }
            ImGui::EndPopup();
        }
        popMenuStyle();

        if (!bitmap) {
            ImRect zf = rows.row("lfsize", "Size", "Pixels per em the TTF/OTF is rasterised at, 4 to 64.");
            nameRect(id + ".size", zf);
            float v = float(uf.size);
            FieldEdit fe = numberField("fsize", zf, std::to_string(uf.size).c_str(), "px", 0, nullptr, &v, true);
            if (fe.changed)
                uic::editFont(doc, i, [&](splash::UIFont& x) { x.size = std::clamp(int(std::lround(v)), 4, 64); }, "uifontsize" + id);
            if (fe.done) doc.endMerge();
        } else {
            ImRect gf = rows.row("lfcell", "Glyph", "The size of one character's cell in the PNG, in pixels (4 to 64 each).");
            const float half = (gf.GetWidth() - space::xs) * 0.5f;
            ImRect wf(gf.Min, ImVec2(gf.Min.x + half, gf.Max.y)), hf(ImVec2(gf.Max.x - half, gf.Min.y), gf.Max);
            nameRect(id + ".gw", wf);
            nameRect(id + ".gh", hf);
            float w = float(uf.glyphWidth), h = float(uf.glyphHeight);
            FieldEdit fw = numberField("fgw", wf, std::to_string(uf.glyphWidth).c_str(), nullptr, color::axisX, "W", &w, true);
            if (fw.changed)
                uic::editFont(doc, i, [&](splash::UIFont& x) { x.glyphWidth = std::clamp(int(std::lround(w)), 4, 64); }, "uifontgw" + id);
            if (fw.done) doc.endMerge();
            FieldEdit fh = numberField("fgh", hf, std::to_string(uf.glyphHeight).c_str(), nullptr, color::axisY, "H", &h, true);
            if (fh.changed)
                uic::editFont(doc, i, [&](splash::UIFont& x) { x.glyphHeight = std::clamp(int(std::lround(h)), 4, 64); }, "uifontgh" + id);
            if (fh.done) doc.endMerge();
        }

        // What the export makes of it, or why it cannot: one line, the whole reason in its tooltip.
        {
            // Two lines, wrapped, reserved whether or not the text needs them.
            const ImVec2 at(x0 + space::sm + rows.lw, rows.y - 4);
            ImRect line(at, ImVec2(x1 - space::sm, at.y + 32));
            ImGui::PushClipRect(line.Min, line.Max, true);
            dl->AddText(f.regular, type::caption, at, b.sheet ? color::textFaint : color::warn, b.info.c_str(), nullptr, line.GetWidth());
            ImGui::PopClipRect();
            nameRect(id + ".info", line);
            interact("finfo", line);
            tooltip(b.info.c_str());
            rows.y += 34;
        }

        // A sample line drawn the way the canvas draws custom-font text.
        rows.rowH = 52;
        ImRect pf = rows.row("lfsample", "Sample", "The font as the PS1 draws it: white, one texel per pixel, shown at twice the size when it fits.");
        rows.rowH = 32;
        pf = ImRect(ImVec2(pf.Min.x, rows.y - 52 + 4), ImVec2(pf.Max.x, rows.y - 4));
        nameRect(id + ".sample", pf);
        dl->AddRectFilled(pf.Min, pf.Max, color::field, radius::field);
        dl->AddRect(pf.Min, pf.Max, color::border, radius::field);
        if (b.sheet && i < uic::kMaxFonts) {
            FontSample& smp = m_samples[size_t(i)];
            const std::string key = uf.name + "|" + uf.source + "|" + uf.bitmap + "|" + std::to_string(uf.size) + "|" +
                                    std::to_string(uf.glyphWidth) + "x" + std::to_string(uf.glyphHeight) + "|" +
                                    std::to_string(doc.assetsId()) + "|" + doc.projectRoot().string();
            if (!smp.tex || smp.key != key) {
                uic::Screen size;
                std::vector<uint32_t> px = uic::renderSample(doc.scene(), m_assets, uf.name, "AaBbCc 0123 !?", 0, &size);
                if (!smp.tex) glGenTextures(1, &smp.tex);
                glBindTexture(GL_TEXTURE_2D, smp.tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.w, size.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                smp.key = key;
                smp.w = size.w;
                smp.h = size.h;
            }
            const float room = pf.GetHeight() - 8, wide = pf.GetWidth() - 2 * space::sm;
            const float scale = (smp.h * 2 <= room && smp.w * 2 <= wide) ? 2.0f : 1.0f;
            const ImVec2 o(std::floor(pf.Min.x + space::sm), std::floor(pf.GetCenter().y - smp.h * scale * 0.5f));
            ImGui::PushClipRect(pf.Min + ImVec2(1, 1), pf.Max - ImVec2(1, 1), true);
            ui::addTextureImage(dl, (ImTextureID)(intptr_t)smp.tex, o, o + ImVec2(smp.w * scale, smp.h * scale));
            ImGui::PopClipRect();
        } else {
            textCentered(dl, pf, f.regular, type::caption, color::textFaint, "No sample: the font cannot be built.");
        }
        rows.y += space::md;
        ImGui::PopID();
    }

    // Add font: from the project's first TTF/OTF; the export takes three at most.
    const bool canAdd = n < uic::kMaxFonts;
    const char* addTip = "A new font from the project's first .ttf or .otf; pick another file under Source.";
    if (canAdd) {
        if (button("cvfonts.add", ImVec2(x0 + space::sm, rows.y), icon::plus, "Add font", ButtonKind::Secondary, nullptr, addTip)) {
            std::string first;
            for (const std::string& p : list())
                if (!uic::isBitmapFontFile(p)) {
                    first = p;
                    break;
                }
            uic::addFont(doc, first);
        }
    } else {
        // Shown but dead: the reason under it.
        ImRect br(ImVec2(x0 + space::sm, rows.y), ImVec2(x0 + space::sm + buttonWidth(icon::plus, "Add font"), rows.y + size::field + 6));
        interact("cvfonts.add", br);
        nameRect("cvfonts.add", br);
        tooltip("The export takes at most 3 fonts per scene. Delete one to add another.");
        dl->AddRect(br.Min, br.Max, color::border, radius::button);
        text(dl, ImVec2(br.Min.x + space::md, br.GetCenter().y - type::icon * 0.5f - 1), f.medium, type::icon, color::textFaint, icon::plus);
        text(dl, ImVec2(br.Min.x + space::md + measure(f.medium, type::icon, icon::plus).x + space::sm - 2,
                        br.GetCenter().y - type::body * 0.5f - 1),
             f.medium, type::body, color::textFaint, "Add font");
    }
    rows.y += size::field + 6 + space::xs;
    // Reserved either way so the layout under it does not move.
    if (!canAdd) text(dl, ImVec2(x0 + space::sm, rows.y), f.regular, type::caption, color::textFaint, "At most 3 fonts per scene.");
    rows.y += 18 + space::md;
    if (remove >= 0) uic::deleteFont(doc, remove);
    *yp = rows.y;
}

}  // namespace ui
