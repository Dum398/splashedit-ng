#include "editor/blocks_view.hh"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
namespace blk = editor::blk;
using blk::Path;

namespace {

constexpr float kPaletteW = 272;
constexpr float kStatusH = 28;
constexpr float kDragStart = 4;  // pixels the pointer moves before a press is a drag
constexpr float kBlockRadius = 5;
const char* const kFieldId = "##blockfield";
const char* const kPickId = "##blockbutton";

ImU32 categoryColor(std::string_view c) {
    if (c == "Events") return blocks::events;
    if (c == "Control") return blocks::control;
    if (c == "Objects") return blocks::objects;
    if (c == "Sound and scenes") return blocks::sound;
    if (c == "Variables") return blocks::variables;
    if (c == "Operators") return blocks::operators;
    if (c == "Input") return blocks::input;
    return blocks::debug;
}

ImRect screen(const blk::Rect& r, ImVec2 o) { return ImRect(ImVec2(o.x + r.x0, o.y + r.y0), ImVec2(o.x + r.x1, o.y + r.y1)); }

float textY(const ImRect& r, float size) { return std::round(r.GetCenter().y - size * 0.5f - 1); }

void hexagon(ImDrawList* dl, ImRect r, ImU32 col, bool fill = true, float thick = 1) {
    const float h = r.GetHeight() * 0.5f;
    const ImVec2 p[6] = {ImVec2(r.Min.x, r.Min.y + h), ImVec2(r.Min.x + h, r.Min.y), ImVec2(r.Max.x - h, r.Min.y),
                         ImVec2(r.Max.x, r.Min.y + h), ImVec2(r.Max.x - h, r.Max.y), ImVec2(r.Min.x + h, r.Max.y)};
    if (fill) dl->AddConvexPolyFilled(p, 6, col);
    else dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, thick);
}

// An input well: an empty slot, a field, a literal.
void well(ImDrawList* dl, ImRect r, bool boolean, bool round, ImU32 edge) {
    if (boolean) {
        hexagon(dl, r, color::field);
        hexagon(dl, r, edge, false);
        return;
    }
    const float rad = round ? r.GetHeight() * 0.5f : radius::field;
    dl->AddRectFilled(r.Min, r.Max, color::field, rad);
    dl->AddRect(r.Min, r.Max, edge, rad);
}

// Corners another block touches are square: `f`'s corners without them.
ImDrawFlags trimCorners(ImDrawFlags f, const blk::BlockBox& b) {
    if (b.above) f &= ~(ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight);
    if (b.below) f &= ~(ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight);
    if (b.inBody) f &= ~(ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
    return f;
}

// No corner flag at all means every corner to ImGui.
ImDrawFlags roundFlags(ImDrawFlags f) { return (f & ImDrawFlags_RoundCornersAll) ? f : ImDrawFlags_RoundCornersNone; }

struct DrawOpts {
    Path selected;
    Path hovered;       // the block under the pointer
    Path hoveredField;  // whose field is under it
    bool names = false;
};

void drawLayout(ImDrawList* dl, const blk::Layout& l, ImVec2 o, const DrawOpts& d) {
    Fonts& f = fonts();
    const float fs = type::label;
    for (const blk::BlockBox& b : l.blocks) {
        const ImRect rr = screen(b.rect, o);
        const bool hovered = !d.hovered.empty() && d.hovered == b.path;
        const ImU32 cat = categoryColor(b.category);
        if (b.op.empty()) {
            // An empty block in a list (a hand-edited file): a dashed hole.
            dl->AddRect(rr.Min, rr.Max, color::borderStrong, kBlockRadius, 0, 1.5f);
            continue;
        }
        const bool literal = b.op == "number" || b.op == "text" || b.op == "bool";
        if (literal) {
            const bool fieldHover = !d.hoveredField.empty() && d.hoveredField == b.path;
            well(dl, rr, b.op == "bool", b.op == "number",
                 fieldHover ? color::accentHover : lerpColor(color::borderStrong, cat, 0.55f));
            const blk::Piece& pc = b.pieces.front();
            const float tw = measure(f.medium, fs, pc.text.c_str()).x;
            text(dl, ImVec2(std::round(rr.GetCenter().x - tw * 0.5f), textY(rr, fs)), f.medium, fs, color::text, pc.text.c_str());
        } else {
            const ImU32 fill = hovered ? lerpColor(cat, IM_COL32_WHITE, 0.08f) : cat;
            if (b.kind == splash::BlockKind::Boolean) {
                hexagon(dl, ImRect(rr.Min + ImVec2(0, 1), rr.Max + ImVec2(0, 1)), blocks::shadow);
                hexagon(dl, rr, fill);
            } else if (b.kind == splash::BlockKind::Reporter) {
                const float rad = rr.GetHeight() * 0.5f;
                dl->AddRectFilled(rr.Min + ImVec2(0, 1), rr.Max + ImVec2(0, 1), blocks::shadow, rad);
                dl->AddRectFilled(rr.Min, rr.Max, fill, rad);
            } else {
                const size_t n = b.parts.size();
                for (size_t k = 0; k < n; k++) {
                    const ImRect pr = screen(b.parts[k], o);
                    ImDrawFlags corners = ImDrawFlags_RoundCornersAll;
                    float rad = kBlockRadius;
                    if (b.kind == splash::BlockKind::CBlock) {
                        // Rows at even indices, the arm beside each mouth at odd ones, the bottom arm last.
                        if (k == n - 1) corners = trimCorners(ImDrawFlags_RoundCornersBottom, b) | ImDrawFlags_RoundCornersTopRight;
                        else if (k % 2 == 1) corners = ImDrawFlags_RoundCornersNone;
                        else if (k == 0) corners = trimCorners(ImDrawFlags_RoundCornersTop, b) | ImDrawFlags_RoundCornersBottomRight;
                        else corners = ImDrawFlags_RoundCornersRight;
                    } else {
                        corners = trimCorners(corners, b);
                    }
                    corners = roundFlags(corners);
                    if (k == n - 1) dl->AddRectFilled(pr.Min + ImVec2(0, 2), pr.Max + ImVec2(0, 2), blocks::shadow, rad, corners);
                    if (b.kind == splash::BlockKind::Hat && k == 0) {
                        // The cap: a rounder top over the row.
                        const float mid = std::round(pr.Min.y + pr.GetHeight() * 0.5f);
                        dl->AddRectFilled(pr.Min, ImVec2(pr.Max.x, mid + 1), fill, 12, ImDrawFlags_RoundCornersTop);
                        dl->AddRectFilled(ImVec2(pr.Min.x, mid), pr.Max, fill, rad, roundFlags(trimCorners(ImDrawFlags_RoundCornersBottom, b)));
                    } else {
                        dl->AddRectFilled(pr.Min, pr.Max, fill, rad, corners);
                    }
                }
                // The seam under it, where the next block starts.
                const ImRect last = screen(b.parts.back(), o);
                dl->AddLine(ImVec2(last.Min.x + 3, last.Max.y - 0.5f), ImVec2(last.Max.x - 3, last.Max.y - 0.5f), blocks::seam);
            }
            for (const blk::Piece& pc : b.pieces) {
                const ImRect r = screen(pc.rect, o);
                if (pc.kind == blk::Piece::Text) {
                    text(dl, ImVec2(r.Min.x, textY(r, fs)), f.medium, fs, blocks::text, pc.text.c_str());
                } else if (pc.kind == blk::Piece::Field) {
                    const bool fieldHover = !d.hoveredField.empty() && d.hoveredField == b.path;
                    well(dl, r, false, false, fieldHover ? color::accentHover : lerpColor(color::border, cat, 0.35f));
                    text(dl, ImVec2(r.Min.x + 7, textY(r, fs)), f.medium, fs, pc.placeholder ? color::textFaint : color::text,
                         pc.text.c_str());
                } else if (!pc.filled) {
                    well(dl, r, blk::booleanSlot(b.op, pc.arg), false, lerpColor(color::border, cat, 0.35f));
                }
            }
        }
        if (!d.selected.empty() && d.selected == b.path) {
            const float rad = b.kind == splash::BlockKind::Reporter ? rr.GetHeight() * 0.5f : kBlockRadius;
            dl->AddRect(rr.Min - ImVec2(2, 2), rr.Max + ImVec2(2, 2), color::accent, rad + 2, ImDrawFlags_None, 2.0f);
        }
        if (d.names) {
            const std::string ps = b.path.str();
            nameRect("blocks.block." + ps, rr);
            for (size_t i = 0; i < b.slots.size(); i++)
                if (b.slots[i].w() > 0) nameRect("blocks.slot." + ps + "." + std::to_string(i), screen(b.slots[i], o));
            if (b.hasField) nameRect("blocks.field." + ps, screen(b.field, o));
        }
    }
}

// The pointer's block: a literal in an input moves with the block it sits in.
Path dragOwner(const splash::BlockScript& s, Path p) {
    const splash::Block* b = blk::blockAt(s, p);
    if (b && blk::isLiteral(*b) && !p.steps.empty() && p.steps.back().kind == 'a') p = p.parent();
    return p;
}

int numberFilter(ImGuiInputTextCallbackData* d) {
    const ImWchar c = d->EventChar;
    return (c >= '0' && c <= '9') || c == '.' || c == '-' ? 0 : 1;
}

struct PaletteItem {
    std::string_view op;
    std::string_view category;
};

}  // namespace

void BlocksView::reset(splash::BlockScript s) {
    m_history.reset(std::move(s));
    m_drag = {};
    m_press = {};
    m_selected = m_editing = m_picking = {};
}

bool BlocksView::commit(splash::BlockScript s) {
    if (!m_history.commit(std::move(s))) return false;
    m_drag = {};
    m_press = {};
    m_selected = m_editing = m_picking = {};
    return true;
}

bool BlocksView::edit(splash::BlockScript s, Result* out) {
    if (!m_history.commit(std::move(s))) return false;
    out->ops.push_back({Result::Op::Edit, splash::saveBlocks(m_history.current())});
    return true;
}

BlocksView::Result BlocksView::draw(ImDrawList* dl, ImRect r, bool* focused) {
    Result res;
    Fonts& f = fonts();
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = f.medium;
    const float fs = type::label;
    const blk::TextWidth tw = [font, fs](std::string_view s) {
        return font->CalcTextSizeA(fs, FLT_MAX, 0, s.data(), s.data() + s.size()).x;
    };
    const blk::Metrics m;
    const ImVec2 mouse = io.MousePos;
    ImGui::PushID("blocks");

    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !popup) *focused = r.Contains(mouse);

    if (!error.empty()) {
        const char* what = "This file is not a block script the editor can read.";
        const float y = r.GetCenter().y - 30;
        const float w = measure(f.semibold, type::title, what).x;
        text(dl, ImVec2(r.GetCenter().x - w * 0.5f, y), f.semibold, type::title, color::text, what);
        const float ew = std::min(measure(f.regular, type::label, error.c_str()).x, r.GetWidth() - 40);
        dl->PushClipRect(ImVec2(r.Min.x + 20, y + 24), ImVec2(r.Max.x - 20, y + 48), true);
        text(dl, ImVec2(r.GetCenter().x - ew * 0.5f, y + 28), f.regular, type::label, color::bad, error.c_str());
        dl->PopClipRect();
        ImGui::PopID();
        return res;
    }

    const ImRect status(ImVec2(r.Min.x, r.Max.y - kStatusH), r.Max);
    const ImRect palette(r.Min, ImVec2(r.Min.x + kPaletteW, status.Min.y));
    const ImRect canvas(ImVec2(palette.Max.x + 1, r.Min.y), ImVec2(r.Max.x, status.Min.y));
    const splash::BlockScript& cur = m_history.current();
    auto originNow = [&] { return ImVec2(std::round(canvas.Min.x + space::xl - m_scroll.x), std::round(canvas.Min.y + space::xl - m_scroll.y)); };

    // --- The palette's contents, in palette coordinates ---
    // Category chips wrap at the top; the list under them scrolls.
    const std::vector<std::string_view>& cats = splash::blockCategories();
    std::vector<ImRect> chips;
    {
        float x = palette.Min.x + space::md, y = palette.Min.y + space::md;
        for (std::string_view c : cats) {
            const std::string name(c);
            const float w = space::sm + 8 + space::xs + 2 + measure(f.medium, type::caption, name.c_str()).x + space::sm + 2;
            if (x + w > palette.Max.x - space::md && x > palette.Min.x + space::md) {
                x = palette.Min.x + space::md;
                y += 22 + space::xs;
            }
            chips.push_back(ImRect(ImVec2(x, y), ImVec2(x + w, y + 22)));
            x += w + space::xs;
        }
    }
    const float listTop = (chips.empty() ? palette.Min.y : chips.back().Max.y) + space::md;
    const ImRect list(ImVec2(palette.Min.x, listTop), palette.Max);
    splash::BlockScript pal;
    std::vector<PaletteItem> palItems;
    std::vector<float> catY;  // each category header's top
    {
        float y = space::xs;
        for (std::string_view c : cats) {
            catY.push_back(y);
            y += 26;
            for (const splash::BlockSpec& s : splash::blockCatalogue()) {
                if (s.category != c) continue;
                splash::BlockStack st{space::md + 2, y, {blk::newBlock(s.op)}};
                y += blk::stackSize(st, tw, m).second + space::sm + 2;
                pal.stacks.push_back(std::move(st));
                palItems.push_back({s.op, c});
            }
            y += space::md;
        }
        m_paletteHeight = y;
    }
    auto clampPalette = [&] { m_paletteScroll = std::clamp(m_paletteScroll, 0.0f, std::max(0.0f, m_paletteHeight - list.GetHeight())); };
    clampPalette();
    const blk::Layout palL = blk::layout(pal, tw, m);
    auto paletteOrigin = [&] { return ImVec2(list.Min.x, std::round(list.Min.y - m_paletteScroll)); };

    // --- Input ---
    Hit canvasHit = interact("##canvas", canvas);
    Hit paletteHit = interact("##palette", list);
    const bool typing = !m_editing.empty() && textEditing(kFieldId);
    const blk::Layout curL = blk::layout(cur, tw, m);

    // The keyboard first, so a frame's undo comes before its edits.
    if (*focused && !typing && m_picking.empty() && !m_drag.on) {
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z, true)) {
            if (m_history.undo()) {
                res.ops.push_back({Result::Op::Undo, splash::saveBlocks(m_history.current())});
                m_selected = m_editing = {};
            }
        } else if (ctrl && ((shift && ImGui::IsKeyPressed(ImGuiKey_Z, true)) || ImGui::IsKeyPressed(ImGuiKey_Y, true))) {
            if (m_history.redo()) {
                res.ops.push_back({Result::Op::Redo, splash::saveBlocks(m_history.current())});
                m_selected = m_editing = {};
            }
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) res.save = true;
        if (!ctrl && !m_selected.empty() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
            if (auto d = blk::deleteFragment(m_history.current(), m_selected)) edit(std::move(*d), &res);
            m_selected = {};
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_selected = {};
    } else if (m_drag.on && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_drag = {};
        m_press = {};
    }

    // Wheel: the canvas scrolls (Shift for sideways), the palette scrolls.
    if (!popup && io.MouseWheel != 0 && canvas.Contains(mouse)) {
        if (io.KeyShift) m_scroll.x -= io.MouseWheel * 48;
        else m_scroll.y -= io.MouseWheel * 48;
    }
    if (!popup && io.MouseWheelH != 0 && canvas.Contains(mouse)) m_scroll.x -= io.MouseWheelH * 48;
    if (!popup && io.MouseWheel != 0 && list.Contains(mouse)) {
        m_paletteScroll -= io.MouseWheel * 48;
        clampPalette();
    }
    // Middle-drag pans too.
    if (canvasHit.hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) m_scroll -= io.MouseDelta;

    // A press: on a palette block, on a canvas block (or field), or on empty canvas (a pan).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !popup && !m_drag.on) {
        if (canvasHit.hovered) {
            const ImVec2 o = originNow();
            m_press = {};
            m_press.on = true;
            m_press.at = mouse;
            m_press.hit = blk::hitTest(curL, mouse.x - o.x, mouse.y - o.y);
            m_press.pan = m_press.hit.kind == blk::Hit::None;
            m_press.scrollAt = m_scroll;
        } else if (paletteHit.hovered) {
            const ImVec2 po = paletteOrigin();
            for (size_t i = 0; i < palL.stacks.size(); i++) {
                const ImRect sr = screen(palL.stacks[i].rect, po);
                if (!sr.Contains(mouse)) continue;
                m_press = {};
                m_press.on = true;
                m_press.palette = true;
                m_press.op = std::string(palItems[i].op);
                m_press.blockMin = sr.Min;
                m_press.at = mouse;
                break;
            }
        }
    }
    if (m_press.on && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 d = mouse - m_press.at;
        if (m_press.pan) {
            m_scroll = m_press.scrollAt - d;
        } else if (!m_drag.on && d.x * d.x + d.y * d.y > kDragStart * kDragStart) {
            if (m_press.palette) {
                m_drag = {};
                m_drag.on = true;
                m_drag.fragment.blocks.push_back(blk::newBlock(m_press.op));
                m_drag.rest = cur;
                m_drag.grab = m_press.at - m_press.blockMin;
            } else {
                const Path owner = dragOwner(cur, m_press.hit.path);
                const blk::BlockBox* box = curL.find(owner);
                std::optional<blk::Taken> t = box ? blk::takeStack(cur, owner) : std::nullopt;
                if (t) {
                    const ImVec2 o = originNow();
                    m_drag = {};
                    m_drag.on = true;
                    m_drag.fromCanvas = true;
                    m_drag.fragment = std::move(t->fragment);
                    m_drag.rest = std::move(t->rest);
                    m_drag.grab = m_press.at - ImVec2(o.x + box->rect.x0, o.y + box->rect.y0);
                    m_selected = m_editing = {};
                } else {
                    m_press.on = false;
                }
            }
        }
    }
    // Where the fragment would land, in canvas coordinates.
    blk::Target target;
    ImVec2 dragAt;
    const blk::Layout* shownL = &curL;
    blk::Layout restL;
    if (m_drag.on) {
        const ImVec2 o = originNow();
        dragAt = ImVec2(std::round(mouse.x - m_drag.grab.x - o.x), std::round(mouse.y - m_drag.grab.y - o.y));
        restL = blk::layout(m_drag.rest, tw, m);
        shownL = &restL;
        if (canvas.Contains(mouse) && !palette.Contains(mouse))
            target = blk::findTarget(m_drag.rest, restL, m_drag.fragment, dragAt.x, dragAt.y, tw, m);
    }
    if (m_press.on && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (m_drag.on) {
            if (palette.Contains(mouse)) {
                // Dropped on the palette: gone.
                if (m_drag.fromCanvas) edit(m_drag.rest, &res);
            } else if (canvas.Contains(mouse)) {
                blk::Target t = target;
                if (t.kind == blk::Target::None) t = {blk::Target::Canvas, {}, 0, dragAt.x, dragAt.y};
                Path placed;
                if (auto out = blk::dropStack(m_drag.rest, m_drag.fragment, t, &placed)) {
                    if (edit(std::move(*out), &res)) m_selected = placed;
                }
            }
            m_drag = {};
            target = {};
        } else if (!m_press.palette && !m_press.pan) {
            const blk::Hit& h = m_press.hit;
            const splash::Block* b = blk::blockAt(cur, h.path);
            const splash::BlockSpec* spec = b ? splash::findBlockSpec(b->op) : nullptr;
            if (h.kind == blk::Hit::Field && spec) {
                if (spec->field == splash::BlockField::Bool) {
                    if (auto out = blk::setField(cur, h.path, b->field == "true" ? "false" : "true")) edit(std::move(*out), &res);
                } else if (spec->field == splash::BlockField::Button) {
                    m_picking = h.path;
                    m_pickQuery.clear();
                    ImGui::OpenPopup(kPickId);
                } else {
                    m_editing = h.path;
                    beginTextEdit(kFieldId, b->field);
                }
                m_selected = dragOwner(cur, h.path);
            } else if (h.kind != blk::Hit::None) {
                m_selected = dragOwner(cur, h.path);
            }
        } else if (m_press.pan && m_press.at.x == mouse.x && m_press.at.y == mouse.y) {
            m_selected = {};
        }
        m_press = {};
    }
    // Edits above may have changed the script: lay out what is shown now.
    blk::Layout nowL;
    if (!m_drag.on && !res.ops.empty()) {
        nowL = blk::layout(m_history.current(), tw, m);
        shownL = &nowL;
    }
    const splash::BlockScript& shown = m_drag.on ? m_drag.rest : m_history.current();
    const ImVec2 o = originNow();

    // --- Canvas ---
    dl->AddRectFilled(canvas.Min, canvas.Max, color::base, radius::window, ImDrawFlags_RoundCornersNone);
    ImGui::PushClipRect(canvas.Min, canvas.Max, true);
    {
        const float step = 24;
        const float sx = canvas.Min.x + std::fmod(std::fmod(-m_scroll.x, step) + step, step);
        const float sy = canvas.Min.y + std::fmod(std::fmod(-m_scroll.y, step) + step, step);
        for (float y = sy; y < canvas.Max.y; y += step)
            for (float x = sx; x < canvas.Max.x; x += step) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 2, y + 2), blocks::grid);
    }
    nameRect("blocks.canvas", canvas);
    if (shown.stacks.empty() && !m_drag.on) {
        const char* hint = "Drag blocks here from the left. A script starts with an Events block.";
        const float w = measure(f.regular, type::body, hint).x;
        text(dl, ImVec2(std::round(canvas.GetCenter().x - w * 0.5f), std::round(canvas.GetCenter().y - 10)), f.regular, type::body,
             color::textFaint, hint);
    }
    DrawOpts opts;
    opts.selected = m_selected;
    opts.names = namedRectsOn();
    if (!m_drag.on && !m_press.on && canvasHit.hovered && !popup) {
        const blk::Hit h = blk::hitTest(*shownL, mouse.x - o.x, mouse.y - o.y);
        if (h.kind != blk::Hit::None) opts.hovered = dragOwner(shown, h.path);
        if (h.kind == blk::Hit::Field) opts.hoveredField = h.path;
    }
    drawLayout(dl, *shownL, o, opts);
    // Where the dragged blocks would go.
    if (m_drag.on && target.kind != blk::Target::None) {
        const std::pair<float, float> fsz = blk::stackSize(m_drag.fragment, tw, m);
        // The fragment's outline where it would go, and a bar where it joins.
        auto bar = [&](float x, float y, bool above) {
            const ImVec2 g0(o.x + x, o.y + (above ? y - fsz.second : y)), g1(g0.x + fsz.first, g0.y + fsz.second);
            dl->AddRectFilled(g0, g1, rgb(0xffffff, 18), kBlockRadius);
            dl->AddRect(g0, g1, color::accentHover, kBlockRadius, ImDrawFlags_None, 1.5f);
            dl->AddRectFilled(ImVec2(o.x + x - 1, o.y + y - 3), ImVec2(o.x + x + fsz.first + 1, o.y + y + 3), blocks::seam, 3);
            dl->AddRectFilled(ImVec2(o.x + x, o.y + y - 2), ImVec2(o.x + x + fsz.first, o.y + y + 2), color::text, 2);
        };
        if (target.kind == blk::Target::Slot) {
            if (const blk::BlockBox* b = shownL->find(target.path)) {
                const ImRect sr = screen(b->slots[size_t(target.index)], o);
                dl->AddRectFilled(sr.Min - ImVec2(2, 2), sr.Max + ImVec2(2, 2), color::accentSoft, radius::field + 2);
                dl->AddRect(sr.Min - ImVec2(2, 2), sr.Max + ImVec2(2, 2), color::accent, radius::field + 2, ImDrawFlags_None, 2.0f);
            }
        } else if (target.kind == blk::Target::StackTop) {
            const blk::StackBox& sb = shownL->stacks[size_t(target.path.stack)];
            bar(sb.rect.x0, sb.rect.y0, true);
        } else if (const blk::BlockBox* b = shownL->find(target.path)) {
            if (target.kind == blk::Target::After) bar(b->rect.x0, b->rect.y1, false);
            else bar(b->mouths[size_t(target.index)].x0, b->mouths[size_t(target.index)].y0, false);
        }
    }
    // A field being typed into, over its block.
    if (!m_editing.empty() && !m_drag.on) {
        const blk::BlockBox* b = shownL->find(m_editing);
        if (!b || !b->hasField) {
            m_editing = {};
        } else {
            const ImRect fr = screen(b->field, o);
            const ImRect er(fr.Min, ImVec2(std::max(fr.Max.x, fr.Min.x + 120), fr.Max.y));
            const splash::Block* blkp = blk::blockAt(m_history.current(), m_editing);
            const splash::BlockSpec* spec = blkp ? splash::findBlockSpec(blkp->op) : nullptr;
            const bool number = spec && spec->field == splash::BlockField::Number;
            std::string out;
            TextEdit te = textEdit(kFieldId, er, f.medium, fs, er.Min.x + 7, er.Max.x - 7, &out, number ? 32 : 255,
                                   number ? numberFilter : nullptr);
            if (te == TextEdit::Commit) {
                const Path p = m_editing;
                m_editing = {};
                if (auto s = blk::setField(m_history.current(), p, out)) edit(std::move(*s), &res);
            } else if (te == TextEdit::Cancel || te == TextEdit::Inactive) {
                m_editing = {};
            }
        }
    }
    ImGui::PopClipRect();

    // --- Palette ---
    dl->AddRectFilled(palette.Min, palette.Max, color::panel);
    dl->AddLine(ImVec2(palette.Max.x + 0.5f, palette.Min.y), ImVec2(palette.Max.x + 0.5f, palette.Max.y), color::border);
    // The category whose blocks are at the top of the list.
    size_t atTop = 0;
    for (size_t i = 0; i < catY.size(); i++)
        if (catY[i] <= m_paletteScroll + 1) atTop = i;
    for (size_t i = 0; i < cats.size(); i++) {
        const std::string name(cats[i]);
        ImGui::PushID(int(i));
        Hit h = interact("chip", chips[i]);
        ImGui::PopID();
        nameRect("blocks.category." + name, chips[i]);
        const bool on = i == atTop;
        dl->AddRectFilled(chips[i].Min, chips[i].Max, on ? color::active : h.hovered ? color::hover : color::raised, radius::pill);
        const float cy = chips[i].GetCenter().y;
        dl->AddCircleFilled(ImVec2(chips[i].Min.x + space::sm + 4, cy), 4, categoryColor(cats[i]));
        text(dl, ImVec2(chips[i].Min.x + space::sm + 8 + space::xs + 2, std::round(cy - type::caption * 0.5f - 1)), f.medium,
             type::caption, on ? color::text : color::textDim, name.c_str());
        if (h.clicked) {
            m_paletteScroll = catY[i];
            clampPalette();
        }
    }
    dl->AddLine(ImVec2(palette.Min.x, list.Min.y - 0.5f), ImVec2(palette.Max.x, list.Min.y - 0.5f), color::border);
    ImGui::PushClipRect(list.Min, list.Max, true);
    {
        const ImVec2 po = paletteOrigin();
        for (size_t i = 0; i < cats.size(); i++) {
            const float y = po.y + catY[i];
            const std::string name(cats[i]);
            dl->AddCircleFilled(ImVec2(list.Min.x + space::md + 6, y + 13), 4, categoryColor(cats[i]));
            text(dl, ImVec2(list.Min.x + space::md + 16, std::round(y + 13 - type::label * 0.5f - 1)), f.semibold, type::label,
                 color::textDim, name.c_str());
        }
        DrawOpts po2;
        if (paletteHit.hovered && !m_drag.on) {
            for (size_t i = 0; i < palL.stacks.size(); i++)
                if (screen(palL.stacks[i].rect, po).Contains(mouse)) {
                    Path p;
                    p.stack = int(i);
                    p.steps = {{'b', 0}};
                    po2.hovered = p;
                }
        }
        drawLayout(dl, palL, po, po2);
        for (size_t i = 0; i < palL.stacks.size(); i++) {
            const ImRect sr = screen(palL.stacks[i].rect, po);
            if (sr.Min.y >= list.Min.y && sr.Max.y <= list.Max.y) nameRect("blocks.palette." + std::string(palItems[i].op), sr);
        }
    }
    ImGui::PopClipRect();
    // Dragging canvas blocks over the palette: letting go deletes them.
    if (m_drag.on && m_drag.fromCanvas && palette.Contains(mouse)) {
        dl->AddRectFilled(palette.Min, palette.Max, rgb(0xef5a6f, 28));
        const char* what = "Drop to delete";
        const ImVec2 ts = measure(f.medium, type::body, what);
        const float iw = measure(f.medium, type::icon, icon::trash).x;
        const float x = std::round(palette.GetCenter().x - (iw + space::sm + ts.x) * 0.5f), y = palette.GetCenter().y;
        text(dl, ImVec2(x, y - type::icon * 0.5f - 1), f.medium, type::icon, color::bad, icon::trash);
        text(dl, ImVec2(x + iw + space::sm, y - type::body * 0.5f - 1), f.medium, type::body, color::text, what);
    }

    // The dragged blocks, over everything in the tab.
    if (m_drag.on) {
        splash::BlockScript one;
        one.stacks.push_back({0, 0, m_drag.fragment.blocks});
        const blk::Layout fl = blk::layout(one, tw, m);
        ImGui::PushClipRect(r.Min, status.Min.y > r.Min.y ? ImVec2(r.Max.x, status.Min.y) : r.Max, true);
        drawLayout(dl, fl, mouse - m_drag.grab, DrawOpts{});
        ImGui::PopClipRect();
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (opts.hovered.stack >= 0 || (paletteHit.hovered && !popup)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // --- Button picker ---
    if (!m_picking.empty()) {
        std::vector<std::string_view> names;
        std::vector<PickerItem> items;
        std::string q = m_pickQuery;
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        for (std::string_view b : splash::blockButtons())
            if (q.empty() || b.find(q) != std::string_view::npos) names.push_back(b);
        std::vector<std::string> labels(names.begin(), names.end());
        for (const std::string& l : labels) items.push_back({icon::gamepad, blocks::input, l.c_str(), "", nullptr});
        ImVec2 at = mouse;
        if (const blk::BlockBox* b = curL.find(m_picking)) at = ImVec2(o.x + b->field.x0, o.y + b->field.y1 + space::xs);
        const int pick = picker(kPickId, at, 220, "Button", &m_pickQuery, items);
        if (pick >= 0 && size_t(pick) < labels.size()) {
            if (auto s = blk::setField(m_history.current(), m_picking, labels[size_t(pick)])) edit(std::move(*s), &res);
            m_picking = {};
        }
        if (!ImGui::IsPopupOpen(kPickId)) m_picking = {};
    }

    // --- Status line: the Lua check ---
    if (!m_checkedAny || !(m_checked == m_history.current())) {
        m_checked = m_history.current();
        m_checkedAny = true;
        m_gen = splash::blocksToLua(m_checked);
    }
    dl->AddLine(ImVec2(status.Min.x, status.Min.y), ImVec2(status.Max.x, status.Min.y), color::border);
    {
        const float cy = status.GetCenter().y;
        const float x = status.Min.x + space::md;
        bool events = false;
        for (const splash::BlockStack& st : m_checked.stacks) {
            const splash::BlockSpec* s = st.blocks.empty() ? nullptr : splash::findBlockSpec(st.blocks[0].op);
            events = events || (s && s->kind == splash::BlockKind::Hat);
        }
        char right[64];
        std::snprintf(right, sizeof right, "%zu stack%s     Blocks", m_checked.stacks.size(), m_checked.stacks.size() == 1 ? "" : "s");
        const ImVec2 rs = measure(f.regular, type::caption, right);
        text(dl, ImVec2(status.Max.x - space::md - rs.x, cy - rs.y * 0.5f), f.regular, type::caption, color::textFaint, right);
        std::string msg;
        ImU32 dot = color::good, col = color::textDim;
        if (!m_gen.errors.empty()) {
            msg = m_gen.errors[0];
            if (m_gen.errors.size() > 1) msg += "  (" + std::to_string(m_gen.errors.size() - 1) + " more)";
            dot = col = color::bad;
        } else if (!events) {
            msg = "No events yet: nothing runs until a stack starts with an Events block";
            dot = color::textFaint;
        } else {
            msg = "Makes Lua for the PS1";
        }
        dl->AddCircleFilled(ImVec2(x + 4, cy), 4, dot);
        dl->PushClipRect(ImVec2(x, status.Min.y), ImVec2(status.Max.x - space::md * 2 - rs.x, status.Max.y), true);
        text(dl, ImVec2(x + 14, cy - type::caption * 0.5f - 1), f.regular, type::caption, col, msg.c_str());
        dl->PopClipRect();
    }
    ImGui::PopID();
    return res;
}

}  // namespace ui
