#include "ui/widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include "budget.hh"
#include "ui/colour_hex.hh"
#include "ui/icons.h"
#include "ui/scrub.hh"

using namespace theme;

namespace ui {

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImVec4 fa = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 fb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(fa.x + (fb.x - fa.x) * t, fa.y + (fb.y - fa.y) * t,
                                                 fa.z + (fb.z - fa.z) * t, fa.w + (fb.w - fa.w) * t));
}

float anim(ImGuiID id, bool target, float speed) {
    static std::unordered_map<ImGuiID, float> values;
    float& v = values[id];
    float dt = ImGui::GetIO().DeltaTime;
    float goal = target ? 1.0f : 0.0f;
    v += (goal - v) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(goal - v) < 0.001f) v = goal;
    return v;
}

ImVec2 measure(ImFont* font, float size, const char* t) { return font->CalcTextSizeA(size, FLT_MAX, 0, t); }

void text(ImDrawList* dl, ImVec2 pos, ImFont* font, float size, ImU32 col, const char* t) {
    dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), col, t);
}

// Vertical position that centres a line of `size` text inside [y0, y1].
static float centerY(ImFont* font, float size, float y0, float y1) {
    float h = measure(font, size, "Ag").y;
    return std::floor(y0 + (y1 - y0 - h) * 0.5f);
}

void textCentered(ImDrawList* dl, ImRect r, ImFont* font, float size, ImU32 col, const char* t) {
    ImVec2 s = measure(font, size, t);
    text(dl, ImVec2(r.Min.x + (r.GetWidth() - s.x) * 0.5f, centerY(font, size, r.Min.y, r.Max.y)), font, size, col, t);
}

std::vector<ImRect>& interactiveRects() {
    static std::vector<ImRect> rects;
    return rects;
}

Hit interact(const char* id, ImRect r, int itemFlags) {
    interactiveRects().push_back(r);
    Hit h;
    ImGuiID gid = ImGui::GetID(id);
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::ItemSize(r.GetSize());
    if (ImGui::ItemAdd(r, gid, nullptr, itemFlags)) h.clicked = ImGui::ButtonBehavior(r, gid, &h.hovered, &h.held);
    h.hover = anim(gid, h.hovered || h.held);
    return h;
}

bool& namedRectsOn() {
    static bool on = false;
    return on;
}

std::map<std::string, ImRect>& namedRects() {
    static std::map<std::string, ImRect> rects;
    return rects;
}

void nameRect(const std::string& name, ImRect r) {
    if (namedRectsOn()) namedRects()[name] = r;
}

void tooltip(const char* t) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) return;
    Fonts& f = fonts();
    // Long tips wrap instead of running off the screen.
    const float wrap = 360;
    ImVec2 s = f.regular->CalcTextSizeA(type::label, FLT_MAX, wrap, t);
    ImVec2 m = ImGui::GetIO().MousePos;
    ImRect r(ImVec2(m.x + 14, m.y + 18), ImVec2(m.x + 14 + s.x + space::md * 2, m.y + 18 + s.y + space::sm * 2));
    // Keep it on screen near the right and bottom edges.
    ImVec2 d = ImGui::GetIO().DisplaySize;
    ImVec2 shift(std::min(0.0f, d.x - space::xs - r.Max.x), std::min(0.0f, d.y - space::xs - r.Max.y));
    r.Min += shift;
    r.Max += shift;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(r.Min + ImVec2(0, 2), r.Max + ImVec2(0, 4), rgb(0x000000, 70), radius::card);
    dl->AddRectFilled(r.Min, r.Max, color::active, radius::card);
    dl->AddRect(r.Min, r.Max, color::borderStrong, radius::card);
    ImVec2 tp = r.Min + ImVec2(space::md, space::sm);
    dl->AddText(f.regular, type::label, ImVec2(std::floor(tp.x), std::floor(tp.y)), color::text, t, nullptr, wrap);
}

float buttonWidth(const char* ic, const char* label) {
    Fonts& f = fonts();
    const float iconW = ic ? measure(f.medium, type::icon, ic).x : 0;
    const float labelW = label ? measure(f.medium, type::body, label).x : 0;
    return space::md * 2 + iconW + ((ic && label) ? space::sm - 2 : 0) + labelW;
}

bool button(const char* id, ImVec2 pos, const char* ic, const char* label, ButtonKind kind, float* outWidth, const char* tip) {
    Fonts& f = fonts();
    float iconW = ic ? measure(f.medium, type::icon, ic).x : 0;
    float gap = (ic && label) ? space::sm - 2 : 0;
    float w = buttonWidth(ic, label);
    ImRect r(pos, pos + ImVec2(w, size::field + 6));
    Hit h = interact(id, r);
    nameRect(id, r);  // --click @id
    if (tip) tooltip(tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg, fg, border = 0;
    switch (kind) {
        case ButtonKind::Primary:
            bg = lerpColor(color::accent, color::accentHover, h.hover);
            fg = rgb(0xffffff);
            break;
        case ButtonKind::Secondary:
            bg = lerpColor(color::raised, color::hover, h.hover);
            fg = color::text;
            border = color::borderStrong;
            break;
        default:
            bg = lerpColor(rgb(0x2a2f39, 0), color::hover, h.hover);
            fg = lerpColor(color::textDim, color::text, h.hover);
            break;
    }
    if (h.held) bg = lerpColor(bg, rgb(0x000000), 0.15f);
    if (kind == ButtonKind::Primary) {
        // Soft glow under the primary action, growing on hover.
        for (int i = 3; i >= 1; --i)
            dl->AddRectFilled(r.Min - ImVec2(i, i - 1), r.Max + ImVec2(i, i + 1), rgb(0x8b7bff, (int)((10 + 14 * h.hover) / i)),
                              radius::button + i);
    }
    dl->AddRectFilled(r.Min, r.Max, bg, radius::button);
    if (border) dl->AddRect(r.Min, r.Max, border, radius::button);
    if (kind == ButtonKind::Primary)
        dl->AddLine(r.Min + ImVec2(radius::button, 0.5f), ImVec2(r.Max.x - radius::button, r.Min.y + 0.5f), rgb(0xffffff, 40));

    float x = r.Min.x + space::md;
    if (ic) {
        text(dl, ImVec2(x, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon, fg, ic);
        x += iconW + gap;
    }
    if (label) text(dl, ImVec2(x, centerY(f.medium, type::body, r.Min.y, r.Max.y)), f.medium, type::body, fg, label);
    if (outWidth) *outWidth = w;
    return h.clicked;
}

bool iconButton(const char* id, ImRect r, const char* ic, bool toggled, const char* tip, ImU32 iconColor, bool enabled) {
    Hit h = interact(id, r);
    if (tip) tooltip(tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!enabled) {
        textCentered(dl, r, fonts().medium, type::icon, (color::textFaint & 0x00ffffffu) | (110u << IM_COL32_A_SHIFT), ic);
        return false;
    }
    if (toggled)
        dl->AddRectFilled(r.Min, r.Max, color::accentSoft, radius::button);
    else if (h.hover > 0)
        dl->AddRectFilled(r.Min, r.Max, rgb(0x2a2f39, (int)(255 * h.hover)), radius::button);
    ImU32 fg = toggled ? color::accentHover : lerpColor(iconColor, color::text, h.hover);
    textCentered(dl, r, fonts().medium, type::icon, fg, ic);
    return h.clicked;
}

int segmented(const char* id, ImVec2 pos, std::initializer_list<const char*> items, int selected, float* outWidth) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = size::field + 2;
    float total = 4;
    for (const char* it : items) total += measure(f.medium, type::label, it).x + space::md * 2;
    ImRect outer(pos, pos + ImVec2(total, h));
    dl->AddRectFilled(outer.Min, outer.Max, rgb(0x0e1014, 210), radius::button + 2);
    dl->AddRect(outer.Min, outer.Max, rgb(0xffffff, 14), radius::button + 2);
    float x = pos.x + 2;
    int i = 0, result = selected;
    for (const char* it : items) {
        float w = measure(f.medium, type::label, it).x + space::md * 2;
        ImRect r(ImVec2(x, pos.y + 2), ImVec2(x + w, pos.y + h - 2));
        ImGui::PushID(i);
        Hit hit = interact(id, r);
        ImGui::PopID();
        if (hit.clicked) result = i;
        if (namedRectsOn()) nameRect(std::string(id) + "/" + it, r);
        if (i == selected)
            dl->AddRectFilled(r.Min, r.Max, color::active, radius::button);
        else if (hit.hover > 0)
            dl->AddRectFilled(r.Min, r.Max, rgb(0x2a2f39, (int)(200 * hit.hover)), radius::button);
        textCentered(dl, r, f.medium, type::label, i == selected ? color::text : lerpColor(color::textDim, color::text, hit.hover), it);
        x += w;
        ++i;
    }
    if (outWidth) *outWidth = total;
    return result;
}

static int resizeString(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(d->UserData);
        str->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = str->data();
    }
    return 0;
}

bool searchField(const char* id, ImRect r, const char* placeholder, const char* shortcut, std::string* query, bool focus, bool searchIcon) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textX = r.Min.x + space::sm + (searchIcon ? 22 : 0);
    // The clear button registers first so it wins over the field.
    const bool hasText = query && !query->empty();
    ImRect clear(ImVec2(r.Max.x - space::xs - 22, r.Min.y + 3), ImVec2(r.Max.x - space::xs, r.Max.y - 3));
    Hit c{};
    if (hasText) {
        c = interact((std::string(id) + "#clear").c_str(), clear);
        if (c.clicked) query->clear();
    }
    ImGuiID inputId = 0;
    bool editing = false;
    if (query) {
        ImGui::PushID(id);
        inputId = ImGui::GetID("##input");
        ImGui::PopID();
        editing = ImGui::GetActiveID() == inputId;
    }
    Hit h{};
    if (!query) h = interact(id, r);
    const bool hovered = query ? ImGui::IsMouseHoveringRect(r.Min, r.Max) : h.hovered;
    dl->AddRectFilled(r.Min, r.Max, color::field, radius::field + 1);
    dl->AddRect(r.Min, r.Max, editing ? color::accent : lerpColor(color::border, color::borderStrong, hovered ? 1.0f : h.hover),
                radius::field + 1);
    if (searchIcon)
        text(dl, ImVec2(r.Min.x + space::sm, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 1,
             hasText || editing ? color::textDim : color::textFaint, icon::search);
    if (query) {
        const float right = hasText ? clear.Min.x : r.Max.x - space::sm;
        float padY = centerY(f.regular, type::body, r.Min.y, r.Max.y) - r.Min.y;
        ImGui::SetCursorScreenPos(ImVec2(textX, r.Min.y));
        ImGui::PushFont(f.regular, type::body);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, padY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, 0u);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0u);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0u);
        ImGui::PushStyleColor(ImGuiCol_Text, color::text);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, rgb(0x8b7bff, 110));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, color::accentHover);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, 0u);
        ImGui::SetNextItemWidth(std::max(8.0f, right - textX));
        if (focus) ImGui::SetKeyboardFocusHere();
        ImGui::PushID(id);
        // Escape clears the text, then a second Escape leaves the field.
        ImGui::InputText("##input", query->data(), query->capacity() + 1,
                         ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EscapeClearsAll, resizeString, query);
        interactiveRects().push_back(r);
        ImGui::PopID();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        editing = ImGui::GetActiveID() == inputId;
    }
    if (!query || (query->empty() && !editing))
        text(dl, ImVec2(textX, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body, color::textFaint,
             placeholder);
    if (hasText) {
        textCentered(dl, clear, f.medium, type::icon - 2, lerpColor(color::textFaint, color::text, c.hover), icon::x);
    } else if (shortcut && !editing) {
        ImVec2 s = measure(f.medium, type::caption, shortcut);
        ImRect k(ImVec2(r.Max.x - space::sm - s.x - 10, r.Min.y + 5), ImVec2(r.Max.x - space::sm, r.Max.y - 5));
        dl->AddRect(k.Min, k.Max, color::borderStrong, radius::field);
        textCentered(dl, k, f.medium, type::caption, color::textFaint, shortcut);
    }
    return editing;
}

Hit treeRow(const char* id, ImRect r, const TreeRow& row, bool* visibilityClicked) {
    Fonts& f = fonts();
    // The eye registers before the row so it wins the click where they overlap.
    ImRect eye(ImVec2(r.Max.x - space::sm - 18, r.Min.y), ImVec2(r.Max.x - space::sm + 4, r.Max.y));
    Hit e{};
    if (visibilityClicked) {
        e = interact((std::string(id) + "#eye").c_str(), eye);
        if (e.clicked) *visibilityClicked = true;
    }
    Hit h = interact(id, r);
    const float rowHover = std::max(h.hover, e.hover);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (row.selected) {
        dl->AddRectFilled(r.Min, r.Max, color::accentSoft, radius::field);
        dl->AddRectFilled(r.Min, ImVec2(r.Min.x + 2, r.Max.y), color::accent, 1);
    } else if (rowHover > 0) {
        dl->AddRectFilled(r.Min, r.Max, rgb(0x2a2f39, (int)(160 * rowHover)), radius::field);
    }
    float x = r.Min.x + space::xs + row.depth * 16.0f;
    const bool faint = row.hidden || row.inheritedOff || row.readOnly;
    ImU32 dimmed = faint ? color::textFaint : row.labelColor ? row.labelColor : color::text;
    if (row.hasChildren)
        text(dl, ImVec2(x, centerY(f.medium, type::label, r.Min.y, r.Max.y)), f.medium, type::label, color::textFaint,
             row.expanded ? icon::chevronDown : row.opensUp ? icon::chevronUp : icon::chevronRight);
    x += 16;
    if (row.icon) {
        text(dl, ImVec2(x, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 1,
             faint ? color::textFaint : row.iconColor, row.icon);
        x += 22;
    }
    text(dl, ImVec2(x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), row.selected ? f.medium : f.regular, type::body, dimmed,
         row.label);
    float right = r.Max.x - space::sm;
    // Visibility toggle shows on hover, or always when the object is hidden.
    if (visibilityClicked && (rowHover > 0.01f || row.hidden)) {
        float a = row.hidden ? 1.0f : rowHover;
        ImU32 col = lerpColor(rgb(0xa0a7b6, (int)(255 * a)), color::text, e.hover);
        text(dl, ImVec2(right - 14, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 2, col,
             row.hidden ? icon::eyeOff : icon::eye);
        right -= 22;
    }
    if (row.warning) {
        text(dl, ImVec2(right - 14, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 2, color::warn,
             icon::warning);
        right -= 22;
    }
    if (row.meta) {
        ImVec2 s = measure(f.regular, type::caption, row.meta);
        text(dl, ImVec2(right - s.x, centerY(f.regular, type::caption, r.Min.y, r.Max.y)), f.regular, type::caption,
             color::textFaint, row.meta);
    }
    return h;
}

static void drawToggle(ImDrawList* dl, const char* id, ImRect r, bool on, float hover);

bool section(const char* id, ImRect r, const char* ic, ImU32 iconColor, const char* title, bool open, bool enabled,
             bool removable, bool* removed, bool* toggled) {
    Fonts& f = fonts();
    nameRect(id, r);  // scripted tests fold a section by clicking @id
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The ellipsis and the switch register before the header so they win the hover where they overlap.
    ImRect dots(ImVec2(r.Max.x - space::sm - 20, r.Min.y + 5), ImVec2(r.Max.x - space::xs, r.Max.y - 5));
    std::string menuId = std::string(id) + "#menu";
    float dotsHover = 0;
    if (removable && removed) {
        Hit d = interact((std::string(id) + "#dots").c_str(), dots);
        dotsHover = d.hover;
        if (d.clicked) ImGui::OpenPopup(menuId.c_str());
    }
    const float switchRight = r.Max.x - space::sm - (removable ? 26 : 0);
    ImRect sw(ImVec2(switchRight - 26, r.Min.y + 9), ImVec2(switchRight, r.Max.y - 9));
    std::string switchId = std::string(id) + "#on";
    Hit s{};
    if (toggled) {
        s = interact(switchId.c_str(), sw);
        if (s.clicked) *toggled = true;
    }
    Hit h = interact(id, r);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID openKey = ImGui::GetID((std::string(id) + "#open").c_str());
    open = store->GetBool(openKey, open);
    if (h.clicked) {
        open = !open;
        store->SetBool(openKey, open);
    }
    dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover * 0.6f), radius::card,
                      open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    float x = r.Min.x + space::sm;
    text(dl, ImVec2(x, centerY(f.medium, type::label, r.Min.y, r.Max.y)), f.medium, type::label, color::textFaint,
         open ? icon::chevronDown : icon::chevronRight);
    x += 18;
    text(dl, ImVec2(x, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 1, iconColor, ic);
    x += 23;
    text(dl, ImVec2(x, centerY(f.semibold, type::body, r.Min.y, r.Max.y)), f.semibold, type::body,
         enabled ? color::text : color::textFaint, title);
    float right = r.Max.x - space::sm;
    if (removable) {
        if (dotsHover > 0) dl->AddRectFilled(dots.Min, dots.Max, lerpColor(rgb(0x2a2f39, 0), color::active, dotsHover), radius::field);
        text(dl, ImVec2(right - 14, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 1,
             lerpColor(color::textFaint, dotsHover > 0 ? color::text : color::textDim, std::max(h.hover, dotsHover)), icon::ellipsis);
        right -= 26;
    }
    if (toggled) drawToggle(dl, switchId.c_str(), sw, enabled, s.hover);
    if (removable && removed) {
        const float pad = space::xs, rowH = size::field + 4, w = 176;
        ImVec2 size(w, pad * 2 + rowH);
        ImGui::SetNextWindowPos(ImVec2(dots.Max.x - w, dots.Max.y + space::xs));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        if (ImGui::BeginPopup(menuId.c_str(), ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoSavedSettings)) {
            ImDrawList* pl = ImGui::GetWindowDrawList();
            ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + size);
            pl->AddRectFilled(box.Min + ImVec2(0, 2), box.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
            pl->AddRectFilled(box.Min, box.Max, color::raised, radius::card);
            pl->AddRect(box.Min, box.Max, color::borderStrong, radius::card);
            ImRect ir(box.Min + ImVec2(pad, pad), box.Max - ImVec2(pad, pad));
            Hit m = interact("remove", ir);
            if (m.hover > 0) pl->AddRectFilled(ir.Min, ir.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, m.hover), radius::field);
            text(pl, ImVec2(ir.Min.x + space::sm, centerY(f.medium, type::icon, ir.Min.y, ir.Max.y)), f.medium, type::icon - 2, color::bad,
                 icon::x);
            text(pl, ImVec2(ir.Min.x + space::sm + 20, centerY(f.regular, type::body, ir.Min.y, ir.Max.y)), f.regular, type::body,
                 m.hover > 0.5f ? color::text : color::textDim, "Remove component");
            if (m.clicked) {
                *removed = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
    }
    return open;
}

ImRect property(const char* id, ImRect row, float labelWidth, const char* label, const char* tip) {
    Fonts& f = fonts();
    ImRect lr(row.Min, ImVec2(row.Min.x + labelWidth, row.Max.y));
    // Hovered for its tip only: Tab goes from field to field past it.
    Hit h = interact(id, lr, ImGuiItemFlags_NoTabStop | ImGuiItemFlags_NoNav);
    if (tip) tooltip(tip);
    text(ImGui::GetWindowDrawList(), ImVec2(lr.Min.x, centerY(f.regular, type::label, row.Min.y, row.Max.y)), f.regular, type::label,
         lerpColor(color::textDim, color::text, h.hover), label);
    return ImRect(ImVec2(row.Min.x + labelWidth, row.Min.y + (row.GetHeight() - size::field) * 0.5f),
                  ImVec2(row.Max.x, row.Min.y + (row.GetHeight() + size::field) * 0.5f));
}

static void well(ImDrawList* dl, ImRect r, float hover) {
    dl->AddRectFilled(r.Min, r.Max, color::field, radius::field);
    dl->AddRect(r.Min, r.Max, lerpColor(color::border, color::borderStrong, hover), radius::field);
}

// Tab (or Enter) landed on the item `gid` just drawn as Inputable: the
// field goes straight to typing, as ImGui's own inputs do.
static bool keyboardActivated(ImGuiID gid) {
    const ImGuiContext& g = *GImGui;
    return g.NavActivateId == gid && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput);
}

// The one in-place text editor; only one field edits at a time.
struct TextEditState {
    ImGuiID id = 0;
    char buf[256] = {};
    int frames = 0;
};
static TextEditState& textState() {
    static TextEditState s;
    return s;
}

void beginTextEdit(const char* id, const std::string& initial) {
    TextEditState& s = textState();
    s.id = ImGui::GetID(id);
    std::snprintf(s.buf, sizeof s.buf, "%s", initial.c_str());
    s.frames = 0;
}

bool textEditing(const char* id) { return textState().id != 0 && textState().id == ImGui::GetID(id); }

TextEdit textEdit(const char* id, ImRect field, ImFont* font, float size, float textX, float textRight, std::string* out,
                  size_t maxLen, ImGuiInputTextCallback charFilter) {
    TextEditState& s = textState();
    if (s.id == 0 || s.id != ImGui::GetID(id)) return TextEdit::Inactive;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A focused well: the field colours with an accent border.
    dl->AddRectFilled(field.Min, field.Max, color::field, radius::field);
    dl->AddRect(field.Min, field.Max, color::accent, radius::field);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s.id = 0;
        return TextEdit::Cancel;
    }
    // Frame padding puts ImGui's text exactly where the static text sits.
    float padY = centerY(font, size, field.Min.y, field.Max.y) - field.Min.y;
    ImGui::SetCursorScreenPos(ImVec2(textX, field.Min.y));
    ImGui::PushFont(font, size);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0u);
    ImGui::PushStyleColor(ImGuiCol_Text, color::text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, rgb(0x8b7bff, 110));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, color::accentHover);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, 0u);
    ImGui::SetNextItemWidth(std::max(8.0f, textRight - textX));
    if (s.frames == 0) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(id);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (charFilter) flags |= ImGuiInputTextFlags_CallbackCharFilter;
    bool enter = ImGui::InputText("##textedit", s.buf, std::min(sizeof s.buf, maxLen + 1), flags, charFilter);
    bool deactivated = ImGui::IsItemDeactivated();
    bool active = ImGui::IsItemActive();
    // Tab and Shift+Tab move to the next or previous field: commit now, so a
    // field that starts typing first (Shift+Tab: one drawn earlier) does not
    // take over the editor before this one has stored its text.
    const bool tabbed = active && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    ImGui::PopID();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    ++s.frames;
    if (enter || deactivated || tabbed || (s.frames > 2 && !active)) {
        *out = s.buf;
        s.id = 0;
        return TextEdit::Commit;
    }
    return TextEdit::Editing;
}

bool textField(const char* id, ImRect r, const std::string& value, std::string* out, const char* placeholder, size_t maxLen,
               ImGuiInputTextCallback charFilter) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A value already past the cap stays editable, so it can be shortened.
    const size_t cap = maxLen ? std::max(maxLen, value.size()) : 255;
    std::string count;
    if (maxLen) count = std::to_string(textEditing(id) ? std::strlen(textState().buf) : value.size()) + "/" + std::to_string(maxLen);
    const float countW = count.empty() ? 0 : measure(f.regular, type::caption, count.c_str()).x;
    const float x = r.Min.x + space::sm, right = r.Max.x - space::sm - (countW > 0 ? countW + space::sm : 0);
    auto drawCount = [&](size_t len) {
        if (count.empty()) return;
        text(dl, ImVec2(r.Max.x - space::sm - countW, centerY(f.regular, type::caption, r.Min.y, r.Max.y)), f.regular,
             type::caption, len > maxLen ? color::warn : color::textFaint, count.c_str());
    };
    if (textEditing(id)) {
        std::string typed;
        TextEdit res = textEdit(id, r, f.regular, type::body, x, right, &typed, cap, charFilter);
        drawCount(std::strlen(textState().buf));
        if (res == TextEdit::Commit && typed != value) {
            *out = typed;
            return true;
        }
        return false;
    }
    Hit h = interact(id, r, ImGuiItemFlags_Inputable);
    if (h.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (h.clicked || keyboardActivated(ImGui::GetID(id))) beginTextEdit(id, value);
    well(dl, r, h.hover);
    drawCount(value.size());
    const bool empty = value.empty() && placeholder;
    const ImVec4 clip(r.Min.x, r.Min.y, right, r.Max.y);
    dl->AddText(f.regular, type::body, ImVec2(x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), empty ? color::textFaint : color::text,
                empty ? placeholder : value.c_str(), nullptr, 0.0f, &clip);
    return false;
}

// Scrub state of the number field being dragged.
struct ScrubState {
    ImGuiID id = 0;
    float value = 0;
    float start = 0;
    bool dragging = false;
};
static ScrubState& scrubState() {
    static ScrubState s;
    return s;
}

FieldEdit numberField(const char* id, ImRect r, const char* value, const char* unit, ImU32 axis, const char* axisLabel, float* edit,
                      bool integer) {
    FieldEdit out;
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiID gid = ImGui::GetID(id);
    float x = r.Min.x + space::sm;
    ImRect tag(r.Min, ImVec2(r.Min.x + 18, r.Max.y));
    if (axis) x = tag.Max.x + space::xs + 2;
    float unitW = unit ? measure(f.regular, type::label, unit).x : 0;
    float right = unit ? r.Max.x - space::sm - unitW - space::xs : r.Max.x - space::xs;
    auto decorations = [&] {
        if (axis) {
            dl->AddRectFilled(tag.Min, tag.Max, axis & 0x40ffffff, radius::field, ImDrawFlags_RoundCornersLeft);
            textCentered(dl, tag, f.semibold, type::caption, axis, axisLabel);
        }
        if (unit)
            text(dl, ImVec2(r.Max.x - space::sm - unitW, centerY(f.regular, type::label, r.Min.y, r.Max.y)), f.regular, type::label,
                 color::textFaint, unit);
    };

    // Typing.
    if (edit && textEditing(id)) {
        std::string typed;
        TextEdit res = textEdit(id, r, f.regular, type::body, x, right, &typed);
        decorations();
        if (res == TextEdit::Commit) {
            char* end = nullptr;
            float v = std::strtof(typed.c_str(), &end);
            while (end && *end == ' ') ++end;
            if (end && end != typed.c_str() && *end == 0 && std::isfinite(v) && v != *edit) {
                *edit = v;
                out.changed = true;
            }
        }
        out.done = res == TextEdit::Commit || res == TextEdit::Cancel;
        return out;
    }

    Hit h = interact(id, r, edit ? ImGuiItemFlags_Inputable : 0);
    if (h.hovered || h.held) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (edit) {
        ScrubState& sc = scrubState();
        ImGuiContext& g = *GImGui;
        if (h.held && g.ActiveId == gid && g.ActiveIdIsJustActivated) sc = {gid, *edit, *edit, false};
        if (h.held && sc.id == gid) {
            if (!sc.dragging && ImGui::IsMouseDragPastThreshold(0)) sc.dragging = true;
            float dx = ImGui::GetIO().MouseDelta.x;
            if (sc.dragging && dx != 0) {
                // scrubRate(): one per pixel for whole numbers, else growing with the value.
                const float shown = scrubTo(sc.value, dx, integer, ImGui::GetIO().KeyShift);
                if (shown != *edit) {
                    *edit = shown;
                    out.changed = true;
                }
            }
        } else if (sc.id == gid) {
            out.done = sc.dragging;
            sc.id = 0;
        }
        // Double-click, or Tab or Enter onto it: type a value (#158).
        if ((h.hovered && ImGui::IsMouseDoubleClicked(0)) || keyboardActivated(gid)) {
            char b[32];
            std::snprintf(b, sizeof b, "%.6g", static_cast<double>(*edit));
            beginTextEdit(id, b);
        }
    }
    bool scrubbing = edit && scrubState().id == gid && scrubState().dragging;
    well(dl, r, scrubbing ? 1.0f : h.hover);
    if (scrubbing) dl->AddRect(r.Min, r.Max, rgb(0x8b7bff, 150), radius::field);
    decorations();
    text(dl, ImVec2(x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body, color::text, value);
    return out;
}

FieldEdit vec3Field(const char* id, ImRect r, const char* x, const char* y, const char* z, const char* unit, float* edit) {
    FieldEdit out;
    float gap = space::xs;
    float w = (r.GetWidth() - gap * 2) / 3;
    const char* vals[3] = {x, y, z};
    const ImU32 cols[3] = {color::axisX, color::axisY, color::axisZ};
    const char* labels[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        ImRect c(ImVec2(r.Min.x + i * (w + gap), r.Min.y), ImVec2(r.Min.x + i * (w + gap) + w, r.Max.y));
        ImGui::PushID(i);
        FieldEdit e = numberField(id, c, vals[i], unit, cols[i], labels[i], edit ? edit + i : nullptr);
        ImGui::PopID();
        if (e.changed || e.done) {
            out = e;
            out.index = i;
        }
    }
    return out;
}

void dropdown(const char* id, ImRect r, const char* ic, const char* value) {
    Fonts& f = fonts();
    Hit h = interact(id, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover), radius::field);
    dl->AddRect(r.Min, r.Max, color::borderStrong, radius::field);
    float x = r.Min.x + space::sm;
    if (ic) {
        text(dl, ImVec2(x, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::icon - 2, color::textDim, ic);
        x += 20;
    }
    text(dl, ImVec2(x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body, color::text, value);
    text(dl, ImVec2(r.Max.x - 20, centerY(f.medium, type::label, r.Min.y, r.Max.y)), f.medium, type::label, color::textDim,
         icon::chevronDown);
}

int dropdownMenu(const char* id, ImRect r, const char* ic, const char* value, std::initializer_list<const char*> items, int current) {
    return dropdownMenu(id, r, ic, value, std::vector<const char*>(items), current);
}

int dropdownMenu(const char* id, ImRect r, const char* ic, const char* value, const std::vector<const char*>& items, int current) {
    Fonts& f = fonts();
    dropdown(id, r, ic, value);
    std::string popupId = std::string(id) + "#menu";
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup(popupId.c_str());

    const float pad = space::xs, rowH = size::field + 4;
    ImVec2 size(r.GetWidth(), pad * 2 + rowH * static_cast<float>(items.size()));
    // A long list that would run off the bottom opens above the field instead.
    float top = r.Max.y + space::xs;
    if (top + size.y > ImGui::GetIO().DisplaySize.y && r.Min.y - space::xs - size.y >= 0) top = r.Min.y - space::xs - size.y;
    ImGui::SetNextWindowPos(ImVec2(r.Min.x, top));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    int picked = -1;
    if (ImGui::BeginPopup(popupId.c_str(), ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings)) {
        // Drawn like the tooltip card: a raised surface with a soft shadow.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + size);
        dl->AddRectFilled(box.Min + ImVec2(0, 2), box.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
        dl->AddRectFilled(box.Min, box.Max, color::raised, radius::card);
        dl->AddRect(box.Min, box.Max, color::borderStrong, radius::card);
        int i = 0;
        for (const char* item : items) {
            ImRect ir(ImVec2(box.Min.x + pad, box.Min.y + pad + rowH * static_cast<float>(i)),
                      ImVec2(box.Max.x - pad, box.Min.y + pad + rowH * static_cast<float>(i + 1)));
            ImGui::PushID(i);
            Hit h = interact("item", ir);
            ImGui::PopID();
            nameRect(std::string(id) + "/" + item, ir);  // --click @id/item
            if (i == current) {
                dl->AddRectFilled(ir.Min, ir.Max, color::accentSoft, radius::field);
                dl->AddRectFilled(ir.Min + ImVec2(0, 5), ImVec2(ir.Min.x + 2, ir.Max.y - 5), color::accent, 1);
            } else if (h.hover > 0) {
                dl->AddRectFilled(ir.Min, ir.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, h.hover), radius::field);
            }
            text(dl, ImVec2(ir.Min.x + space::sm + (ic ? 20 : 0), centerY(f.regular, type::body, ir.Min.y, ir.Max.y)),
                 i == current ? f.medium : f.regular, type::body, i == current || h.hover > 0.5f ? color::text : color::textDim, item);
            if (h.clicked) {
                picked = i;
                ImGui::CloseCurrentPopup();
            }
            ++i;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    return picked == current ? -1 : picked;
}

// Keyboard highlight and scroll of the open picker; only one is open at a time.
struct PickerState {
    int hi = 0;     // highlighted row, index into the items
    int first = 0;  // first visible row
    char buf[64] = {};
};
static PickerState& pickerState() {
    static PickerState s;
    return s;
}

int picker(const char* id, ImVec2 pos, float width, const char* placeholder, std::string* query, const std::vector<PickerItem>& items) {
    Fonts& f = fonts();
    const float pad = space::xs, searchH = 36, rowH = 40;
    const int rows = 10;  // at most; the box shrinks to the matches
    const int shown = std::clamp(static_cast<int>(items.size()), 1, rows);
    ImVec2 size(width, pad * 2 + searchH + space::xs + rowH * static_cast<float>(shown));
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    pos.x = std::clamp(pos.x, space::sm, std::max(space::sm, disp.x - size.x - space::sm));
    pos.y = std::clamp(pos.y, space::sm, std::max(space::sm, disp.y - size.y - space::sm));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    int picked = -1;
    if (!ImGui::BeginPopup(id, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(2);
        return -1;
    }
    PickerState& s = pickerState();
    const int n = static_cast<int>(items.size());
    auto available = [&](int i) { return i >= 0 && i < n && !items[i].unavailable; };
    auto firstAvailable = [&] {
        for (int i = 0; i < n; ++i)
            if (available(i)) return i;
        return 0;
    };
    bool appearing = ImGui::IsWindowAppearing();
    if (appearing) {
        std::snprintf(s.buf, sizeof s.buf, "%s", query->c_str());
        s.hi = firstAvailable();
        s.first = 0;
    }
    if (s.hi >= n) s.hi = firstAvailable();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + size);
    dl->AddRectFilled(box.Min + ImVec2(0, 3), box.Max + ImVec2(0, 10), rgb(0x000000, 90), radius::window);
    dl->AddRectFilled(box.Min, box.Max, color::raised, radius::window);
    dl->AddRect(box.Min, box.Max, color::borderStrong, radius::window);

    // Search well.
    ImRect sr(box.Min + ImVec2(pad, pad), ImVec2(box.Max.x - pad, box.Min.y + pad + searchH));
    dl->AddRectFilled(sr.Min, sr.Max, color::field, radius::field + 1);
    dl->AddRect(sr.Min, sr.Max, color::accent, radius::field + 1);
    float tx = sr.Min.x + space::sm + 22;
    text(dl, ImVec2(sr.Min.x + space::sm, centerY(f.medium, type::icon, sr.Min.y, sr.Max.y)), f.medium, type::icon - 1, color::textDim,
         icon::search);
    if (!s.buf[0])
        text(dl, ImVec2(tx, centerY(f.regular, type::body, sr.Min.y, sr.Max.y)), f.regular, type::body, color::textFaint, placeholder);
    float padY = centerY(f.regular, type::body, sr.Min.y, sr.Max.y) - sr.Min.y;
    ImGui::SetCursorScreenPos(ImVec2(tx, sr.Min.y));
    ImGui::PushFont(f.regular, type::body);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0u);
    ImGui::PushStyleColor(ImGuiCol_Text, color::text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, rgb(0x8b7bff, 110));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, color::accentHover);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, 0u);
    ImGui::SetNextItemWidth(std::max(8.0f, sr.Max.x - space::sm - tx));
    if (appearing) ImGui::SetKeyboardFocusHere();
    // CallbackHistory makes the field own Up/Down, so they move the highlight
    // below instead of ImGui's keyboard nav taking focus out of the popup.
    bool enter = ImGui::InputText("##search", s.buf, sizeof s.buf,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                  [](ImGuiInputTextCallbackData*) { return 0; });
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (*query != s.buf) {
        *query = s.buf;
        s.hi = 0;  // the list is re-ranked next frame; its best match is first
        s.first = 0;
    }

    // Keyboard: move over the available rows, the list following.
    auto step = [&](int dir) {
        for (int i = s.hi + dir; i >= 0 && i < n; i += dir)
            if (available(i)) {
                s.hi = i;
                if (s.hi < s.first) s.first = s.hi;
                if (s.hi >= s.first + rows) s.first = s.hi - rows + 1;
                return;
            }
    };
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) step(1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) step(-1);
    // The wheel scrolls the list; a highlight it scrolls away comes along.
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
        s.first = std::clamp(s.first - static_cast<int>(ImGui::GetIO().MouseWheel), 0, std::max(0, n - rows));
        if (s.hi < s.first || s.hi >= s.first + rows) {
            s.hi = s.hi < s.first ? s.first : s.first + rows - 1;
            while (s.hi < n && !available(s.hi)) ++s.hi;
            if (s.hi >= n) s.hi = firstAvailable();
        }
    }
    s.first = std::clamp(s.first, 0, std::max(0, n - rows));
    if (enter && available(s.hi)) picked = s.hi;

    // Rows.
    float y = sr.Max.y + space::xs;
    if (n == 0)
        textCentered(dl, ImRect(ImVec2(box.Min.x, y), ImVec2(box.Max.x, y + rowH)), f.regular, type::body, color::textFaint,
                     "No matches");
    const bool mouseMoved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
    for (int i = s.first; i < std::min(n, s.first + rows); ++i, y += rowH) {
        const PickerItem& it = items[i];
        ImRect ir(ImVec2(box.Min.x + pad, y), ImVec2(box.Max.x - pad, y + rowH));
        ImGui::PushID(i);
        Hit h = interact("row", ir);
        ImGui::PopID();
        if (namedRectsOn()) nameRect(std::string(id) + "/" + it.label, ir);  // --click @id/label
        bool ok = !it.unavailable;
        if (h.hovered && ok && mouseMoved) s.hi = i;
        if (i == s.hi && ok) {
            dl->AddRectFilled(ir.Min, ir.Max, color::accentSoft, radius::field);
            dl->AddRectFilled(ir.Min + ImVec2(0, 8), ImVec2(ir.Min.x + 2, ir.Max.y - 8), color::accent, 1);
        }
        ImRect ic(ImVec2(ir.Min.x + space::sm, ir.Min.y + 6), ImVec2(ir.Min.x + space::sm + 28, ir.Max.y - 6));
        dl->AddRectFilled(ic.Min, ic.Max, ok ? (it.iconColor & 0x00ffffffu) | (34u << IM_COL32_A_SHIFT) : color::field, radius::field);
        textCentered(dl, ic, f.medium, type::icon - 1, ok ? it.iconColor : color::textFaint, it.icon);
        float lx = ic.Max.x + space::md;
        dl->PushClipRect(ir.Min, ImVec2(ir.Max.x - space::sm, ir.Max.y), true);
        text(dl, ImVec2(lx, ir.Min.y + 4), f.medium, type::body, ok ? color::text : color::textFaint, it.label);
        text(dl, ImVec2(lx, ir.Min.y + 22), f.regular, type::caption, ok ? color::textDim : color::warn,
             ok ? it.blurb : it.unavailable);
        dl->PopClipRect();
        if (h.clicked && ok) picked = i;
    }
    bool closing = picked >= 0 || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (closing) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    // Closing hands focus back to the window under it; keep ImGui's nav
    // cursor hidden so it does not land on the first title-bar button.
    if (closing) ImGui::SetNavCursorVisible(false);
    return picked;
}

bool assetField(const char* id, ImRect r, const char* ic, ImU32 iconColor, const char* name, const char* meta) {
    Fonts& f = fonts();
    Hit h = interact(id, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    well(dl, r, h.hover);
    ImRect thumb(r.Min + ImVec2(3, 3), ImVec2(r.Min.x + r.GetHeight() - 3, r.Max.y - 3));
    dl->AddRectFilled(thumb.Min, thumb.Max, color::raised, radius::field - 1);
    textCentered(dl, thumb, f.medium, type::icon - 3, iconColor, ic);
    float x = thumb.Max.x + space::sm;
    text(dl, ImVec2(x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body, color::text, name);
    if (meta) {
        ImVec2 s = measure(f.regular, type::caption, meta);
        text(dl, ImVec2(r.Max.x - space::sm - s.x, centerY(f.regular, type::caption, r.Min.y, r.Max.y)), f.regular, type::caption,
             color::textFaint, meta);
    }
    return h.clicked;
}

static void drawToggle(ImDrawList* dl, const char* id, ImRect r, bool on, float hover) {
    float t = anim(ImGui::GetID((std::string(id) + "#k").c_str()), on, 18.0f);
    ImU32 track = lerpColor(color::active, color::accent, t);
    if (hover > 0 && !on) track = lerpColor(track, color::borderStrong, hover);
    float rad = r.GetHeight() * 0.5f;
    dl->AddRectFilled(r.Min, r.Max, track, rad);
    float cx = r.Min.x + rad + (r.GetWidth() - rad * 2) * t;
    dl->AddCircleFilled(ImVec2(cx, r.Min.y + rad + 0.5f), rad - 2.5f, rgb(0x000000, 50), 20);
    dl->AddCircleFilled(ImVec2(cx, r.Min.y + rad), rad - 2.5f, rgb(0xf4f5f8), 20);
}

bool toggle(const char* id, ImRect r, bool on) {
    Hit h = interact(id, r);
    drawToggle(ImGui::GetWindowDrawList(), id, r, on, h.hover);
    return h.clicked;
}

ColorFieldHit colorField(const char* id, ImRect r, const std::array<float, 3>& value, const char* name) {
    Fonts& f = fonts();
    const std::string hex = colourHex(value);
    auto u8 = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    Hit h = interact(id, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    well(dl, r, h.hover);
    ImRect sw(r.Min + ImVec2(3, 3), ImVec2(r.Min.x + 34, r.Max.y - 3));
    dl->AddRectFilled(sw.Min, sw.Max, IM_COL32(u8(value[0]), u8(value[1]), u8(value[2]), 255), radius::field - 1);
    text(dl, ImVec2(sw.Max.x + space::sm, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body, color::text,
         hex.c_str());
    ColorFieldHit out;
    out.clicked = h.clicked;

    // The right-click menu. Only one is open at a time, so its state is shared:
    // where it opened and the clipboard's colour as it was then.
    static ImVec2 menuPos;
    static std::optional<std::array<float, 3>> clip;
    static std::string clipHex;
    ImGui::PushID(id);
    const char* popup = "##colourmenu";
    if (h.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        clip.reset();
        if (char* raw = SDL_GetClipboardText()) {
            clip = parseColourHex(raw);
            SDL_free(raw);
        }
        clipHex = clip ? colourHex(*clip) : std::string();
        // Under the field, from the pointer across, so the field stays in view.
        menuPos = ImVec2(ImGui::GetIO().MousePos.x, r.Max.y + space::xs);
        ImGui::OpenPopup(popup);
    }
    if (ImGui::IsPopupOpen(popup)) {
        std::vector<MenuItem> items;
        items.push_back({"Copy", hex, true, [hex] { SDL_SetClipboardText(hex.c_str()); }});
        items.push_back({"Paste", clipHex, clip.has_value(), [&out] { out.pasted = clip; }});
        float w, mh;
        menuSize(items, &w, &mh);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        ImVec2 pos = menuPos;
        if (pos.x + w > d.x - space::xs) pos.x -= w;
        if (pos.y + mh > d.y - space::xs) pos.y = r.Min.y - space::xs - mh;  // no room below: above it
        pos.x = std::max(pos.x, space::xs);
        pos.y = std::max(pos.y, space::xs);
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(ImVec2(w, mh));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        const bool shown = ImGui::BeginPopup(popup, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);
        if (shown) {
            const std::string menuName = std::string(name ? name : id) + ".menu";
            std::function<void()> run = menuRows(items, w, mh, menuName.c_str());
            if (run || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (run) run();
        }
    }
    ImGui::PopID();
    return out;
}

void slider(const char* id, ImRect r, float t, const char* value) {
    Fonts& f = fonts();
    Hit h = interact(id, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 vs = measure(f.regular, type::body, value);
    float valueW = 52;
    ImRect track(ImVec2(r.Min.x, r.GetCenter().y - 2), ImVec2(r.Max.x - valueW - space::sm, r.GetCenter().y + 2));
    dl->AddRectFilled(track.Min, track.Max, color::active, 2);
    float kx = track.Min.x + track.GetWidth() * t;
    dl->AddRectFilled(track.Min, ImVec2(kx, track.Max.y), color::accent, 2);
    float kr = 6 + 1.5f * h.hover;
    dl->AddCircleFilled(ImVec2(kx, track.GetCenter().y), kr + 4, rgb(0x8b7bff, (int)(50 * h.hover)), 24);
    dl->AddCircleFilled(ImVec2(kx, track.GetCenter().y), kr, rgb(0xf4f5f8), 24);
    ImRect vr(ImVec2(r.Max.x - valueW, r.Min.y), r.Max);
    well(dl, vr, 0);
    text(dl, ImVec2(vr.Max.x - space::sm - vs.x, centerY(f.regular, type::body, r.Min.y, r.Max.y)), f.regular, type::body,
         color::text, value);
}

float problemCard(const char* id, ImVec2 pos, float width, const char* title, const char* body, const char* fixLabel, bool* fixed,
                  ImRect* fixRect) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad = space::md;
    float textW = width - pad * 2 - 24;
    ImVec2 bodySize = f.regular->CalcTextSizeA(type::label, FLT_MAX, textW, body);
    const float buttonH = fixLabel ? space::md + size::field + 6 : 0;
    float h = pad + 18 + space::xs + bodySize.y + buttonH + pad;
    ImRect r(pos, pos + ImVec2(width, h));
    dl->AddRectFilled(r.Min, r.Max, color::warnSoft, radius::card);
    dl->AddRect(r.Min, r.Max, rgb(0xf0a43a, 70), radius::card);
    text(dl, r.Min + ImVec2(pad, pad), f.medium, type::icon, color::warn, icon::warning);
    float x = r.Min.x + pad + 24;
    text(dl, ImVec2(x, r.Min.y + pad), f.semibold, type::body, color::text, title);
    dl->AddText(f.regular, type::label, ImVec2(x, r.Min.y + pad + 18 + space::xs), color::textDim, body, nullptr, textW);
    if (!fixLabel) {
        if (fixed) *fixed = false;
        if (fixRect) *fixRect = ImRect();
        return h;
    }
    const ImVec2 bp(x, r.Max.y - pad - size::field - 6);
    float bw = 0;
    bool hit = button(id, bp, icon::wand, fixLabel, ButtonKind::Secondary, &bw);
    if (fixed) *fixed = hit;
    if (fixRect) *fixRect = ImRect(bp, bp + ImVec2(bw, size::field + 6));
    return h;
}

float meter(const char* id, ImVec2 pos, const char* ic, const char* label, float fraction, const char* value, const char* tip) {
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A negative fraction is a plain readout: no bar.
    const splash::BudgetBand band = splash::budgetBandOf(fraction);
    ImU32 col = fraction < 0                           ? color::textDim
                : band == splash::BudgetBand::Good ? color::good
                : band == splash::BudgetBand::Warn ? color::warn
                                                   : color::bad;
    float barW = fraction < 0 ? -space::sm : 64;
    float labelW = measure(f.medium, type::caption, label).x;
    float valueW = measure(f.regular, type::caption, value).x;
    float w = 18 + labelW + space::sm + barW + space::sm + valueW;
    ImRect r(pos, pos + ImVec2(w, size::statusBar));
    Hit h = interact(id, r);
    tooltip(tip);
    if (h.hover > 0)
        dl->AddRectFilled(r.Min + ImVec2(-6, 4), r.Max + ImVec2(6, -4), rgb(0x2a2f39, (int)(255 * h.hover)), radius::field);
    float x = pos.x;
    text(dl, ImVec2(x, centerY(f.medium, type::icon, r.Min.y, r.Max.y)), f.medium, type::label, col, ic);
    x += 18;
    text(dl, ImVec2(x, centerY(f.medium, type::caption, r.Min.y, r.Max.y)), f.medium, type::caption, color::textDim, label);
    x += labelW + space::sm;
    float cy = r.GetCenter().y;
    if (fraction >= 0) {
        dl->AddRectFilled(ImVec2(x, cy - 2.5f), ImVec2(x + barW, cy + 2.5f), color::active, 3);
        dl->AddRectFilled(ImVec2(x, cy - 2.5f), ImVec2(x + barW * std::min(fraction, 1.0f), cy + 2.5f), col, 3);
    }
    x += barW + space::sm;
    text(dl, ImVec2(x, centerY(f.regular, type::caption, r.Min.y, r.Max.y)), f.regular, type::caption, color::text, value);
    return w;
}

// --- Popup menus ---

// A themed popup menu of `items` (open, sized and placed by the caller).
// With a `name`, scripted checks find each row as <name>/<label>.
std::function<void()> menuRows(const std::vector<MenuItem>& items, float w, float h, const char* name) {
    auto centerY = [](ImFont* font, float size, float y0, float y1) { return y0 + (y1 - y0 - measure(font, size, "Ag").y) * 0.5f; };
    Fonts& f = fonts();
    const float pad = space::xs, rowH = size::field + 4, sepH = 9;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImVec2(w, h));
    dl->AddRectFilled(box.Min + ImVec2(0, 2), box.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
    dl->AddRectFilled(box.Min, box.Max, color::raised, radius::card);
    dl->AddRect(box.Min, box.Max, color::borderStrong, radius::card);
    float y = box.Min.y + pad;
    std::function<void()> run;
    int i = 0;
    for (const MenuItem& e : items) {
        ImGui::PushID(i++);
        if (e.label.empty()) {
            dl->AddLine(ImVec2(box.Min.x + space::sm, y + sepH * 0.5f), ImVec2(box.Max.x - space::sm, y + sepH * 0.5f), color::border);
            y += sepH;
        } else {
            ImRect ir(ImVec2(box.Min.x + pad, y), ImVec2(box.Max.x - pad, y + rowH));
            Hit hit = interact("item", ir);
            if (name) nameRect(std::string(name) + "/" + e.label, ir);
            const bool on = e.enabled && hit.hover > 0;
            if (on) dl->AddRectFilled(ir.Min, ir.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, hit.hover), radius::field);
            const ImU32 tc = !e.enabled ? color::textFaint : on && hit.hover > 0.5f ? color::text : color::textDim;
            text(dl, ImVec2(ir.Min.x + space::md, centerY(f.regular, type::body, ir.Min.y, ir.Max.y)), f.regular, type::body, tc,
                 e.label.c_str());
            if (!e.shortcut.empty()) {
                ImVec2 ss = measure(f.regular, type::label, e.shortcut.c_str());
                text(dl, ImVec2(ir.Max.x - space::sm - ss.x, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular,
                     type::label, color::textFaint, e.shortcut.c_str());
            }
            if (hit.clicked && e.enabled && e.run) run = e.run;
            y += rowH;
        }
        ImGui::PopID();
    }
    return run;
}

void menuSize(const std::vector<MenuItem>& items, float* w, float* h) {
    Fonts& f = fonts();
    *w = 200;
    *h = space::xs * 2;
    for (const MenuItem& e : items) {
        if (e.label.empty()) {
            *h += 9;
            continue;
        }
        *h += size::field + 4;
        *w = std::max(*w, space::md + measure(f.regular, type::body, e.label.c_str()).x + space::xl +
                              measure(f.regular, type::label, e.shortcut.c_str()).x + space::md);
    }
}

}  // namespace ui
