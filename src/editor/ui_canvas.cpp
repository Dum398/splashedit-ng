#include "editor/ui_canvas.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <system_error>

#include "editor/document.hh"
#include "font.hh"
#include "texture.hh"
#include "unitymath.hh"

namespace editor::uic {

namespace {

#include "editor/ui_sysfont.inc"

using splash::UICanvas;
using splash::UIElement;
using splash::UIElementType;

constexpr int kMaxSize = 32767;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// The anchor point of one axis in pixels, as the export's bakeAxis corrects it to.
int anchorPx(float a, int res) { return splash::roundToInt(a * float(res)); }

bool stretched(const UIElement& e, int axis) { return e.anchorMin[axis] != e.anchorMax[axis]; }

uint8_t byteOf(float c) { return uint8_t(clampi(splash::roundToInt(c * 255.f), 0, 255)); }

std::string truncateBytes(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t cut = bytes;
    while (cut > 0 && (uint8_t(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

size_t utf16Units(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = uint8_t(s[i]);
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

bool sameElement(const UIElement& a, const UIElement& b) {
    return a.type == b.type && a.name == b.name && a.visible == b.visible && a.rect == b.rect && a.anchorMin == b.anchorMin &&
           a.anchorMax == b.anchorMax && a.color == b.color && a.text == b.text && a.font == b.font && a.from == b.from &&
           a.to == b.to && a.background == b.background && a.value == b.value && a.texture == b.texture &&
           a.bitDepth == b.bitDepth && a.cutout == b.cutout && a.unknown == b.unknown;
}

bool sameCanvas(const UICanvas& a, const UICanvas& b) {
    if (a.name != b.name || a.visible != b.visible || a.sortOrder != b.sortOrder || a.elements.size() != b.elements.size())
        return false;
    for (size_t i = 0; i < a.elements.size(); i++)
        if (!sameElement(a.elements[i], b.elements[i])) return false;
    return true;
}

// Every UI edit: the canvases before and after, and the UI selection with them.
class UiCommand : public Command {
public:
    UiCommand(std::vector<UICanvas> before, std::vector<UICanvas> after, Document::UiSelection selBefore,
              Document::UiSelection selAfter, std::string key)
        : m_before(std::move(before)), m_after(std::move(after)), m_selBefore(selBefore), m_selAfter(selAfter),
          m_key(std::move(key)) {}
    void apply(Document& doc) override {
        doc.canvasesMut() = m_after;
        doc.selectUi(m_selAfter);
    }
    void revert(Document& doc) override {
        doc.canvasesMut() = m_before;
        doc.selectUi(m_selBefore);
    }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const UiCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key) return false;
        m_after = n->m_after;
        m_selAfter = n->m_selAfter;
        return true;
    }

private:
    std::vector<UICanvas> m_before, m_after;
    Document::UiSelection m_selBefore, m_selAfter;
    std::string m_key;
};

void commit(Document& doc, std::vector<UICanvas> after, Document::UiSelection sel, const std::string& key = {}) {
    doc.execute(std::make_unique<UiCommand>(doc.scene().canvases, std::move(after), doc.uiSelection(), sel, key), !key.empty());
}

bool validCanvas(const Document& doc, int c) { return c >= 0 && c < int(doc.scene().canvases.size()); }
bool validElement(const Document& doc, int c, int e) {
    return validCanvas(doc, c) && e >= 0 && e < int(doc.scene().canvases[size_t(c)].elements.size());
}

// "Box (3)" -> base "Box", 3; "Box" -> "Box", 1.
int splitNumbered(const std::string& name, std::string* base) {
    if (name.size() > 4 && name.back() == ')') {
        size_t open = name.rfind(" (");
        if (open != std::string::npos && open + 2 < name.size() - 1) {
            std::string digits = name.substr(open + 2, name.size() - open - 3);
            if (std::all_of(digits.begin(), digits.end(), [](char ch) { return ch >= '0' && ch <= '9'; }) && digits.size() < 7) {
                *base = name.substr(0, open);
                return std::atoi(digits.c_str());
            }
        }
    }
    *base = name;
    return 1;
}

std::string hexOf(uint8_t r, uint8_t g, uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    return buf;
}

}  // namespace

Screen screenOf(const splash::VramSettings& vram) { return Screen{vram.resolutionX, vram.resolutionY}; }

const char* typeName(UIElementType t) {
    switch (t) {
        case UIElementType::Image: return "Image";
        case UIElementType::Box: return "Box";
        case UIElementType::Text: return "Text";
        case UIElementType::Progress: return "Progress bar";
        case UIElementType::Line: return "Line";
    }
    return "Element";
}

// ---- Layout

Rect layout(const UIElement& e, Screen s) {
    if (e.type == UIElementType::Line) {
        int x0 = std::min(e.from[0], e.to[0]), x1 = std::max(e.from[0], e.to[0]);
        int y0 = std::min(e.from[1], e.to[1]), y1 = std::max(e.from[1], e.to[1]);
        return Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }
    Rect r;
    const int res[2] = {s.w, s.h};
    int pos[2], size[2];
    for (int a = 0; a < 2; a++) {
        const int amin = anchorPx(e.anchorMin[size_t(a)], res[a]);
        pos[a] = amin + e.rect[size_t(a)];
        size[a] = stretched(e, a) ? anchorPx(e.anchorMax[size_t(a)], res[a]) - amin + e.rect[size_t(a) + 2] : e.rect[size_t(a) + 2];
    }
    r.x = pos[0];
    r.y = pos[1];
    r.w = size[0];
    r.h = size[1];
    return r;
}

Rect drawn(const UIElement& e, Screen s) {
    Rect r = layout(e, s);
    if (e.type == UIElementType::Line) return r;
    // UISystem::resolveLayout, in its int16 arithmetic.
    int x = int16_t(r.x), y = int16_t(r.y), w = int16_t(r.w), h = int16_t(r.h);
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (x + w > s.w) w = s.w - x;
    if (y + h > s.h) h = s.h - y;
    return Rect{x, y, w, h};
}

void place(UIElement& e, Rect r, Screen s) {
    if (e.type == UIElementType::Line) {
        Rect now = layout(e, s);
        const int dx = r.x - now.x, dy = r.y - now.y;
        e.from = {e.from[0] + dx, e.from[1] + dy};
        e.to = {e.to[0] + dx, e.to[1] + dy};
        return;
    }
    const int res[2] = {s.w, s.h};
    const int pos[2] = {r.x, r.y}, size[2] = {r.w, r.h};
    for (int a = 0; a < 2; a++) {
        const int amin = anchorPx(e.anchorMin[size_t(a)], res[a]);
        e.rect[size_t(a)] = pos[a] - amin;
        e.rect[size_t(a) + 2] = stretched(e, a) ? size[a] - (anchorPx(e.anchorMax[size_t(a)], res[a]) - amin) : size[a];
    }
}

// ---- Hit testing

static float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = ax + dx * t - px, cy = ay + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

// Line endpoints sit at pixel centres.
static float lineDistance(const UIElement& e, float x, float y) {
    return segmentDistance(x, y, e.from[0] + 0.5f, e.from[1] + 0.5f, e.to[0] + 0.5f, e.to[1] + 0.5f);
}

int hitTest(const UICanvas& cv, Screen s, float x, float y, float slop) {
    for (int i = int(cv.elements.size()) - 1; i >= 0; i--) {
        const UIElement& e = cv.elements[size_t(i)];
        if (!e.visible || !e.unknown.empty()) continue;
        if (e.type == UIElementType::Line) {
            if (lineDistance(e, x, y) <= slop) return i;
            continue;
        }
        Rect r = layout(e, s);
        if (x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) return i;
    }
    return -1;
}

Handle hitHandle(Rect r, float x, float y, float slop) {
    const float l = float(r.x), t = float(r.y), rt = float(r.right()), b = float(r.bottom());
    auto near = [&](float a, float v) { return std::fabs(a - v) <= slop; };
    if (near(x, l) && near(y, t)) return Handle::TopLeft;
    if (near(x, rt) && near(y, t)) return Handle::TopRight;
    if (near(x, l) && near(y, b)) return Handle::BottomLeft;
    if (near(x, rt) && near(y, b)) return Handle::BottomRight;
    const bool inX = x >= l && x <= rt, inY = y >= t && y <= b;
    if (inY && near(x, l)) return Handle::Left;
    if (inY && near(x, rt)) return Handle::Right;
    if (inX && near(y, t)) return Handle::Top;
    if (inX && near(y, b)) return Handle::Bottom;
    if (inX && inY) return Handle::Move;
    return Handle::None;
}

Handle hitLineHandle(const UIElement& e, float x, float y, float slop) {
    auto near = [&](const std::array<int, 2>& p) {
        return std::fabs(p[0] + 0.5f - x) <= slop && std::fabs(p[1] + 0.5f - y) <= slop;
    };
    if (near(e.to)) return Handle::LineTo;
    if (near(e.from)) return Handle::LineFrom;
    if (lineDistance(e, x, y) <= slop) return Handle::Move;
    return Handle::None;
}

Rect resize(Rect r, Handle h, int dx, int dy) {
    const bool left = h == Handle::Left || h == Handle::TopLeft || h == Handle::BottomLeft;
    const bool right = h == Handle::Right || h == Handle::TopRight || h == Handle::BottomRight;
    const bool top = h == Handle::Top || h == Handle::TopLeft || h == Handle::TopRight;
    const bool bottom = h == Handle::Bottom || h == Handle::BottomLeft || h == Handle::BottomRight;
    Rect o = r;
    if (left) {
        const int edge = clampi(r.x + dx, r.right() - kMaxSize, r.right() - 1);
        o.x = edge;
        o.w = r.right() - edge;
    }
    if (right) o.w = clampi(r.w + dx, 1, kMaxSize);
    if (top) {
        const int edge = clampi(r.y + dy, r.bottom() - kMaxSize, r.bottom() - 1);
        o.y = edge;
        o.h = r.bottom() - edge;
    }
    if (bottom) o.h = clampi(r.h + dy, 1, kMaxSize);
    return o;
}

// ---- Snapping

namespace {

struct Target {
    int pos;
    int from, to;  // the span along the other axis the guide covers
    bool screen;
};

// Snap targets along x (axis 0) or y (axis 1).
std::vector<Target> targets(const UICanvas& cv, int self, Screen s, int axis) {
    const int len = axis == 0 ? s.w : s.h, other = axis == 0 ? s.h : s.w;
    std::vector<Target> t = {{0, 0, other, true}, {len / 2, 0, other, true}, {len, 0, other, true}};
    for (int i = 0; i < int(cv.elements.size()); i++) {
        const UIElement& e = cv.elements[size_t(i)];
        if (i == self || !e.visible || !e.unknown.empty()) continue;
        Rect r = layout(e, s);
        const int a = axis == 0 ? r.x : r.y, n = axis == 0 ? r.w : r.h;
        const int o0 = axis == 0 ? r.y : r.x, o1 = axis == 0 ? r.bottom() : r.right();
        if (e.type == UIElementType::Line) {
            t.push_back({e.from[size_t(axis)], o0, o1, false});
            t.push_back({e.to[size_t(axis)], o0, o1, false});
            continue;
        }
        t.push_back({a, o0, o1, false});
        t.push_back({a + n / 2, o0, o1, false});
        t.push_back({a + n, o0, o1, false});
    }
    return t;
}

// The smallest shift (within threshold) that puts one of `cands` on a
// target, or 0 with *found false.
int bestShift(const std::vector<int>& cands, const std::vector<Target>& ts, int threshold, bool* found) {
    int best = 0, bestAbs = threshold + 1;
    for (int c : cands)
        for (const Target& t : ts) {
            const int d = t.pos - c;
            if (std::abs(d) < bestAbs) {
                bestAbs = std::abs(d);
                best = d;
            }
        }
    *found = bestAbs <= threshold;
    return *found ? best : 0;
}

// Guides for every target one of `cands` lands on; `lo`..`hi` is the
// snapped thing's span along the other axis.
void addGuides(std::vector<Guide>& out, bool vertical, const std::vector<int>& cands, const std::vector<Target>& ts, int lo, int hi) {
    for (const Target& t : ts)
        if (std::find(cands.begin(), cands.end(), t.pos) != cands.end()) {
            Guide g{vertical, t.pos, t.screen ? t.from : std::min(t.from, lo), t.screen ? t.to : std::max(t.to, hi)};
            if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
        }
}

}  // namespace

Snapped snapMove(const UICanvas& cv, int self, Rect r, Screen s, int threshold) {
    Snapped out{r, {}};
    const std::vector<Target> tx = targets(cv, self, s, 0), ty = targets(cv, self, s, 1);
    bool fx = false, fy = false;
    out.rect.x += bestShift({r.x, r.x + r.w / 2, r.right()}, tx, threshold, &fx);
    out.rect.y += bestShift({r.y, r.y + r.h / 2, r.bottom()}, ty, threshold, &fy);
    const Rect& q = out.rect;
    if (fx) addGuides(out.guides, true, {q.x, q.x + q.w / 2, q.right()}, tx, q.y, q.bottom());
    if (fy) addGuides(out.guides, false, {q.y, q.y + q.h / 2, q.bottom()}, ty, q.x, q.right());
    return out;
}

Snapped snapResize(const UICanvas& cv, int self, Rect r, Handle h, Screen s, int threshold) {
    Snapped out{r, {}};
    const bool left = h == Handle::Left || h == Handle::TopLeft || h == Handle::BottomLeft;
    const bool right = h == Handle::Right || h == Handle::TopRight || h == Handle::BottomRight;
    const bool top = h == Handle::Top || h == Handle::TopLeft || h == Handle::TopRight;
    const bool bottom = h == Handle::Bottom || h == Handle::BottomLeft || h == Handle::BottomRight;
    bool fx = false, fy = false;
    const std::vector<Target> tx = targets(cv, self, s, 0), ty = targets(cv, self, s, 1);
    Rect& q = out.rect;
    if (left || right) {
        const int d = bestShift({left ? r.x : r.right()}, tx, threshold, &fx);
        if (fx && left && d < r.w) q.x += d, q.w -= d;
        else if (fx && right && r.w + d >= 1) q.w += d;
        else fx = false;
    }
    if (top || bottom) {
        const int d = bestShift({top ? r.y : r.bottom()}, ty, threshold, &fy);
        if (fy && top && d < r.h) q.y += d, q.h -= d;
        else if (fy && bottom && r.h + d >= 1) q.h += d;
        else fy = false;
    }
    if (fx) addGuides(out.guides, true, {left ? q.x : q.right()}, tx, q.y, q.bottom());
    if (fy) addGuides(out.guides, false, {top ? q.y : q.bottom()}, ty, q.x, q.right());
    return out;
}

std::vector<Guide> snapPoint(const UICanvas& cv, int self, int* x, int* y, Screen s, int threshold) {
    std::vector<Guide> guides;
    const std::vector<Target> tx = targets(cv, self, s, 0), ty = targets(cv, self, s, 1);
    bool fx = false, fy = false;
    *x += bestShift({*x}, tx, threshold, &fx);
    *y += bestShift({*y}, ty, threshold, &fy);
    if (fx) addGuides(guides, true, {*x}, tx, *y, *y + 1);
    if (fy) addGuides(guides, false, {*y}, ty, *x, *x + 1);
    return guides;
}

// ---- Edits

bool editElement(Document& doc, int c, int e, const std::function<void(UIElement&)>& fn, const std::string& mergeKey) {
    if (!validElement(doc, c, e)) return false;
    std::vector<UICanvas> after = doc.scene().canvases;
    UIElement& el = after[size_t(c)].elements[size_t(e)];
    fn(el);
    if (sameElement(el, doc.scene().canvases[size_t(c)].elements[size_t(e)])) return false;
    commit(doc, std::move(after), doc.uiSelection(), mergeKey);
    return true;
}

bool editCanvas(Document& doc, int c, const std::function<void(UICanvas&)>& fn, const std::string& mergeKey) {
    if (!validCanvas(doc, c)) return false;
    std::vector<UICanvas> after = doc.scene().canvases;
    fn(after[size_t(c)]);
    if (sameCanvas(after[size_t(c)], doc.scene().canvases[size_t(c)])) return false;
    commit(doc, std::move(after), doc.uiSelection(), mergeKey);
    return true;
}

std::string uniqueElementName(const UICanvas& cv, const std::string& name) {
    std::string base;
    splitNumbered(name, &base);
    bool taken = false;
    int next = 1;
    for (const UIElement& e : cv.elements) {
        if (e.name == name) taken = true;
        std::string b;
        int n = splitNumbered(e.name, &b);
        if (b == base) next = std::max(next, n + 1);
    }
    if (!taken) return name;
    return base + " (" + std::to_string(std::max(next, 2)) + ")";
}

int addElement(Document& doc, int c, UIElementType type, Screen s) {
    if (!validCanvas(doc, c)) return -1;
    std::vector<UICanvas> after = doc.scene().canvases;
    UICanvas& cv = after[size_t(c)];
    UIElement e;
    e.type = type;
    e.name = uniqueElementName(cv, typeName(type));
    int w = 64, h = 32;
    switch (type) {
        case UIElementType::Box: e.color = {0.25f, 0.5f, 0.9f}; break;
        case UIElementType::Text:
            w = 120, h = 16;
            e.text = "Text";
            break;
        case UIElementType::Image:
            w = h = 32;
            e.color = {0.5f, 0.5f, 0.5f};  // tint 128: the texture's own colours
            break;
        case UIElementType::Progress:
            w = 100, h = 8;
            e.color = {0.2f, 0.9f, 0.2f};
            e.value = 50;
            break;
        case UIElementType::Line: break;
    }
    if (type == UIElementType::Line) {
        e.from = {s.w / 2 - 32, s.h / 2};
        e.to = {s.w / 2 + 32, s.h / 2};
    } else {
        e.rect = {(s.w - w) / 2, (s.h - h) / 2, w, h};
    }
    cv.elements.push_back(std::move(e));
    const int index = int(cv.elements.size()) - 1;
    commit(doc, std::move(after), {c, index});
    return index;
}

int duplicateElement(Document& doc, int c, int e) {
    if (!validElement(doc, c, e)) return -1;
    std::vector<UICanvas> after = doc.scene().canvases;
    UICanvas& cv = after[size_t(c)];
    UIElement copy = cv.elements[size_t(e)];
    std::string base;
    splitNumbered(copy.name, &base);
    copy.name = uniqueElementName(cv, base);
    if (copy.type == UIElementType::Line) {
        copy.from = {copy.from[0] + 8, copy.from[1] + 8};
        copy.to = {copy.to[0] + 8, copy.to[1] + 8};
    } else {
        copy.rect[0] += 8;
        copy.rect[1] += 8;
    }
    cv.elements.insert(cv.elements.begin() + e + 1, std::move(copy));
    commit(doc, std::move(after), {c, e + 1});
    return e + 1;
}

bool deleteElement(Document& doc, int c, int e) {
    if (!validElement(doc, c, e)) return false;
    std::vector<UICanvas> after = doc.scene().canvases;
    after[size_t(c)].elements.erase(after[size_t(c)].elements.begin() + e);
    commit(doc, std::move(after), {c, -1});
    return true;
}

bool moveElement(Document& doc, int c, int from, int to) {
    if (!validElement(doc, c, from)) return false;
    const int n = int(doc.scene().canvases[size_t(c)].elements.size());
    to = clampi(to, 0, n - 1);
    if (to == from) return false;
    std::vector<UICanvas> after = doc.scene().canvases;
    auto& els = after[size_t(c)].elements;
    UIElement moved = std::move(els[size_t(from)]);
    els.erase(els.begin() + from);
    els.insert(els.begin() + to, std::move(moved));
    Document::UiSelection sel = doc.uiSelection();
    if (sel.canvas == c && sel.element >= 0) {
        if (sel.element == from) sel.element = to;
        else if (from < sel.element && sel.element <= to) sel.element--;
        else if (to <= sel.element && sel.element < from) sel.element++;
    }
    commit(doc, std::move(after), sel);
    return true;
}

int addCanvas(Document& doc) {
    std::vector<UICanvas> after = doc.scene().canvases;
    UICanvas cv;
    std::string name = "Canvas";
    for (int n = 2; std::any_of(after.begin(), after.end(), [&](const UICanvas& x) { return x.name == name; }); n++)
        name = "Canvas (" + std::to_string(n) + ")";
    cv.name = name;
    for (const UICanvas& x : after) cv.sortOrder = std::max(cv.sortOrder, std::min(255, x.sortOrder + 1));
    after.push_back(std::move(cv));
    const int index = int(after.size()) - 1;
    commit(doc, std::move(after), {index, -1});
    return index;
}

bool deleteCanvas(Document& doc, int c) {
    if (!validCanvas(doc, c)) return false;
    std::vector<UICanvas> after = doc.scene().canvases;
    after.erase(after.begin() + c);
    commit(doc, std::move(after), {after.empty() ? -1 : std::min(c, int(after.size()) - 1), -1});
    return true;
}

// ---- Problems

std::vector<Problem> elementProblems(const Document& doc, int c, int ei, Screen s) {
    std::vector<Problem> out;
    if (!validElement(doc, c, ei)) return out;
    const splash::Scene& scene = doc.scene();
    const UIElement& e = scene.canvases[size_t(c)].elements[size_t(ei)];
    const std::string scr = std::to_string(s.w) + "x" + std::to_string(s.h);
    if (!e.unknown.empty()) {
        out.push_back({"Unknown element type",
                       "This build does not know this element's type. It stays in the scene file but is left out of the "
                       "export; delete it if it is not needed."});
        return out;
    }

    // Over the runtime's budget.
    int before = 0;
    for (int i = 0; i < c; i++) before += int(scene.canvases[size_t(i)].elements.size());
    int total = 0;
    for (const UICanvas& cv : scene.canvases) total += int(cv.elements.size());
    if (total > kMaxElements && before + ei >= kMaxElements)
        out.push_back({"Over the element budget",
                       "The scene has " + std::to_string(total) + " UI elements and psxsplash loads at most " +
                           std::to_string(kMaxElements) + ", so the export fails. Delete " +
                           std::to_string(total - kMaxElements) + " elements, from this canvas or another."});
    if (ei >= kMaxCanvasElements)
        out.push_back({"Over the canvas budget", "A canvas holds at most " + std::to_string(kMaxCanvasElements) +
                                                     " elements. Move this one to another canvas or delete some."});

    // Where it lands.
    if (e.type == UIElementType::Line) {
        auto off = [&](const std::array<int, 2>& p) { return p[0] < 0 || p[1] < 0 || p[0] >= s.w || p[1] >= s.h; };
        const bool f = off(e.from), t = off(e.to);
        if (f || t)
            out.push_back({"Line off screen", std::string(f && t ? "Both endpoints are" : f ? "The From point is" : "The To point is") +
                                                  " outside the " + scr + " screen, so the GPU clips the line there. Keep "
                                                  "X within 0.." + std::to_string(s.w - 1) + " and Y within 0.." +
                                                  std::to_string(s.h - 1) + "."});
    } else {
        const Rect r = layout(e, s);
        if (r.w <= 0 || r.h <= 0) {
            out.push_back({"No size", "Width and height must be at least 1 px; psxsplash draws it 1 px wide. Set W and H, or "
                                      "drag a corner handle."});
        } else if (r.right() <= 0 || r.bottom() <= 0 || r.x >= s.w || r.y >= s.h) {
            const int nx = clampi(r.x, 0, std::max(0, s.w - r.w)), ny = clampi(r.y, 0, std::max(0, s.h - r.h));
            out.push_back({"Off screen", "It is entirely outside the " + scr + " screen and never shows. Drag it back on, or "
                                         "move it to " + std::to_string(nx) + ", " + std::to_string(ny) + ".",
                           Fix::MoveOnScreen});
        } else {
            if (r.x < 0 || r.y < 0) {
                const bool l = r.x < 0;
                const int by = l ? -r.x : -r.y;
                out.push_back({l ? "Cut by the left edge" : "Cut by the top edge",
                               std::string("It starts ") + std::to_string(by) + " px past the " + (l ? "left" : "top") +
                                   " edge, so psxsplash draws it " + std::to_string(by) + " px " + (l ? "narrower" : "shorter") +
                                   (e.type == UIElementType::Image ? " and squashes the whole image into what is left" : "") +
                                   ". Move it " + std::to_string(by) + " px " + (l ? "right" : "down") + ".",
                               Fix::MoveOnScreen});
            }
            if (r.right() > s.w || r.bottom() > s.h) {
                const bool rt = r.right() > s.w;
                const int by = rt ? r.right() - s.w : r.bottom() - s.h;
                out.push_back({rt ? "Past the right edge" : "Past the bottom edge",
                               std::to_string(by) + " px of it lie past the " + (rt ? "right" : "bottom") + " edge of the " + scr +
                                   " screen and are cut off. Move it " + std::to_string(by) + " px " + (rt ? "left" : "up") +
                                   " or make it " + std::to_string(by) + " px " + (rt ? "narrower" : "shorter") + ".",
                               Fix::MoveOnScreen});
            }
        }
    }

    // Per type.
    if (e.type == UIElementType::Text) {
        if (e.text.size() > size_t(kTextMax)) {
            std::string kept = truncateBytes(e.text, kTextMax);
            out.push_back({"Text too long", "The export keeps the first " + std::to_string(kTextMax) + " bytes and drops \"" +
                                                e.text.substr(kept.size()) + "\". Shorten it by " +
                                                std::to_string(e.text.size() - kept.size()) + " characters.",
                           Fix::TrimText});
        }
        for (unsigned char ch : e.text)
            if (ch < 0x20 || ch > 0x7E) {
                out.push_back({"Characters it cannot draw",
                               "psxsplash draws ASCII only (letters, digits, punctuation); anything else shows as '?'. "
                               "Replace accented letters and symbols.",
                               Fix::AsciiText});
                break;
            }
        const splash::UIFont* font = nullptr;
        for (const splash::UIFont& f : scene.fonts)
            if (f.name == e.font) font = &f;
        if (!e.font.empty() && !font)
            out.push_back({"No such font", "The scene has no font named '" + e.font +
                                               "', so the export fails. Pick one of the scene's fonts, or System.",
                           Fix::SystemFont});
        if (font) {
            // Custom-font glyphs are white tinted by colour/128: channels above half clip.
            const uint8_t r8 = byteOf(e.color[0]), g8 = byteOf(e.color[1]), b8 = byteOf(e.color[2]);
            auto shown = [](uint8_t c) { return uint8_t(std::min(255, int(c) * 2)); };
            const bool clips = (r8 > 128 && r8 < 255) || (g8 > 128 && g8 < 255) || (b8 > 128 && b8 < 255);
            if (clips)
                out.push_back({"Shows lighter than its colour",
                               "psxsplash tints the white glyphs of a custom font by colour/128, which doubles it: this "
                               "text shows as " + hexOf(shown(r8), shown(g8), shown(b8)) + ", not " + hexOf(r8, g8, b8) +
                                   ". For " + hexOf(r8, g8, b8) + " set Colour to " +
                                   hexOf(uint8_t((r8 + 1) / 2), uint8_t((g8 + 1) / 2), uint8_t((b8 + 1) / 2)) + ".",
                               Fix::HalveColour});
        }
    }
    if (e.type == UIElementType::Image) {
        if (e.texture.empty()) {
            out.push_back({"No texture", "Pick a texture; without one the export leaves the image out."});
        } else {
            std::error_code ec;
            if (!std::filesystem::exists(doc.resolve(e.texture), ec))
                out.push_back({"Texture missing", "'" + e.texture + "' is not in the project. Put the file back or pick another."});
        }
        const uint8_t r8 = byteOf(e.color[0]), g8 = byteOf(e.color[1]), b8 = byteOf(e.color[2]);
        auto off = [](uint8_t c) { return std::abs(int(c) - 128) > 2; };
        if (off(r8) || off(g8) || off(b8))
            out.push_back({"Tinted", "psxsplash multiplies the texture by colour/128: 50% grey (#808080) shows it as it is, "
                                     "white doubles it. Set Colour to #808080 for the texture's own colours.",
                           Fix::NeutralTint});
    }
    if (e.type == UIElementType::Progress && (e.value < 0 || e.value > 100))
        out.push_back({"Value out of range", "Value " + std::to_string(e.value) +
                                                 " is outside 0..100; the game reads it as " +
                                                 std::to_string(uint8_t(e.value)) + ". Set it between 0 and 100.",
                           Fix::ClampValue});
    if (utf16Units(e.name) > 24)
        out.push_back({"Long name", "The game keeps the first 24 characters of the name, so UI.FindElement must use those. "
                                    "Shorten it to 24."});
    return out;
}

const char* fixLabel(Fix f) {
    switch (f) {
        case Fix::MoveOnScreen: return "Move onto the screen";
        case Fix::TrimText: return "Cut to 63 bytes";
        case Fix::AsciiText: return "Replace with '?'";
        case Fix::SystemFont: return "Use the system font";
        case Fix::HalveColour: return "Halve the colour";
        case Fix::NeutralTint: return "Set to #808080";
        case Fix::ClampValue: return "Clamp to 0..100";
        case Fix::None: break;
    }
    return "";
}

bool applyFix(Document& doc, int c, int e, Fix fix, Screen s) {
    return editElement(doc, c, e, [&](UIElement& el) {
        switch (fix) {
            case Fix::MoveOnScreen: {
                if (el.type == UIElementType::Line) {
                    for (auto* p : {&el.from, &el.to}) *p = {clampi((*p)[0], 0, s.w - 1), clampi((*p)[1], 0, s.h - 1)};
                    break;
                }
                Rect r = layout(el, s);
                r.w = clampi(r.w, 1, s.w);
                r.h = clampi(r.h, 1, s.h);
                r.x = clampi(r.x, 0, s.w - r.w);
                r.y = clampi(r.y, 0, s.h - r.h);
                place(el, r, s);
                break;
            }
            case Fix::TrimText: el.text = truncateBytes(el.text, kTextMax); break;
            case Fix::AsciiText: {
                std::string t;
                for (size_t i = 0; i < el.text.size(); i++) {
                    unsigned char ch = uint8_t(el.text[i]);
                    if (ch >= 0x20 && ch <= 0x7E) t += char(ch);
                    else if ((ch & 0xC0) != 0x80) t += '?';  // one '?' per character, not per byte
                }
                el.text = t;
                break;
            }
            case Fix::SystemFont: el.font.clear(); break;
            case Fix::HalveColour:
                for (float& ch : el.color) ch = float((byteOf(ch) + 1) / 2) / 255.f;
                break;
            case Fix::NeutralTint: el.color = {128 / 255.f, 128 / 255.f, 128 / 255.f}; break;
            case Fix::ClampValue: el.value = clampi(el.value, 0, 100); break;
            case Fix::None: break;
        }
    });
}

std::vector<Problem> canvasProblems(const Document& doc, int c) {
    std::vector<Problem> out;
    if (!validCanvas(doc, c)) return out;
    const splash::Scene& scene = doc.scene();
    const UICanvas& cv = scene.canvases[size_t(c)];
    if (c >= kMaxCanvases)
        out.push_back({"Over the canvas budget", "The scene has " + std::to_string(scene.canvases.size()) +
                                                     " canvases and psxsplash loads at most " + std::to_string(kMaxCanvases) +
                                                     ", so the export fails. Merge canvases or delete " +
                                                     std::to_string(scene.canvases.size() - kMaxCanvases) + "."});
    if (int(cv.elements.size()) > kMaxCanvasElements)
        out.push_back({"Too many elements", "This canvas has " + std::to_string(cv.elements.size()) + " elements and holds at most " +
                                                std::to_string(kMaxCanvasElements) + ", so the export fails. Move " +
                                                std::to_string(cv.elements.size() - kMaxCanvasElements) +
                                                " to another canvas or delete them."});
    int total = 0;
    for (const UICanvas& x : scene.canvases) total += int(x.elements.size());
    if (total > kMaxElements)
        out.push_back({"Over the element budget", "The scene has " + std::to_string(total) + " UI elements and psxsplash loads at most " +
                                                      std::to_string(kMaxElements) + ", so the export fails. Delete " +
                                                      std::to_string(total - kMaxElements) + "."});
    for (int i = 0; i < c; i++)
        if (scene.canvases[size_t(i)].name == cv.name) {
            out.push_back({"Name taken", "Canvas " + std::to_string(i + 1) + " has the same name, so UI.FindCanvas and cutscene "
                                         "tracks only ever find that one. Rename this canvas."});
            break;
        }
    if (cv.sortOrder < 0 || cv.sortOrder > 255)
        out.push_back({"Sort order out of range", "Sort order is stored in a byte: keep it within 0 (back) to 255 (front)."});
    return out;
}

// ---- Assets

struct Assets::Font {
    std::string key;
    splash::FontSheet sheet;
    std::string error;
};

Assets::Assets() = default;
Assets::~Assets() = default;

void Assets::refresh(const std::filesystem::path& root, unsigned assetsId) {
    if (root == m_root && assetsId == m_assetsId) return;
    m_root = root;
    m_assetsId = assetsId;
    m_textures.clear();
    m_fonts.clear();
}

const Assets::Texture& Assets::texture(const std::string& path, splash::BitDepth depth, bool cutout) {
    auto key = std::make_tuple(path, int(depth), cutout);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) return it->second;
    Texture& t = m_textures[key];
    try {
        splash::PsxTexture p = splash::convertTexture(splash::loadImage(m_root / path), depth, cutout);
        t.width = p.width;
        t.height = p.height;
        t.words.resize(size_t(p.width) * size_t(p.height));
        for (int y = 0; y < p.height; y++)
            for (int x = 0; x < p.width; x++) {
                uint16_t w;
                if (p.bitDepth == splash::BitDepth::Bpp16) w = p.word(x, y);
                else if (p.bitDepth == splash::BitDepth::Bpp8) w = p.palette[(p.word(x / 2, y) >> ((x & 1) * 8)) & 0xff];
                else w = p.palette[(p.word(x / 4, y) >> ((x & 3) * 4)) & 0xf];
                t.words[size_t(y) * size_t(p.width) + size_t(x)] = w;
            }
    } catch (const std::exception& ex) {
        t = Texture();
        t.error = ex.what();
    }
    return t;
}

const splash::FontSheet* Assets::font(const splash::Scene& scene, const std::string& name, std::string* error) {
    const splash::UIFont* def = nullptr;
    for (const splash::UIFont& f : scene.fonts)
        if (f.name == name) {
            def = &f;
            break;
        }
    if (!def) {
        if (error) *error = "no font named '" + name + "'";
        return nullptr;
    }
    std::string key = def->source + "|" + std::to_string(def->size) + "|" + def->bitmap + "|" + std::to_string(def->glyphWidth) +
                      "x" + std::to_string(def->glyphHeight);
    for (int a : def->advances) key += "," + std::to_string(a);
    std::unique_ptr<Font>& slot = m_fonts[name];
    if (!slot || slot->key != key) {
        slot = std::make_unique<Font>();
        slot->key = key;
        try {
            slot->sheet = splash::buildFont(*def, m_root);
        } catch (const std::exception& ex) {
            slot->error = ex.what();
        }
    }
    if (!slot->error.empty()) {
        if (error) *error = slot->error;
        return nullptr;
    }
    return &slot->sheet;
}

// ---- Rendering

namespace {

uint8_t glyphChar(unsigned char c) { return (c < 32 || c > 127) ? uint8_t('?') : uint8_t(c); }

struct Frame {
    Screen s;
    std::vector<uint16_t> px;  // 15-bit colour
    std::vector<uint8_t> cov;
    void put(int x, int y, uint16_t c) {
        if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
        px[size_t(y) * size_t(s.w) + size_t(x)] = c;
        cov[size_t(y) * size_t(s.w) + size_t(x)] = 1;
    }
    void fill(int x, int y, int w, int h, uint16_t c) {
        const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(s.w, x + w), y1 = std::min(s.h, y + h);
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) put(xx, yy, c);
    }
};

uint16_t pack5(int r, int g, int b) { return uint16_t((r & 31) | ((g & 31) << 5) | ((b & 31) << 10)); }
// A flat colour as the GPU stores it: 8-bit channels cut to 5 bits.
uint16_t flat(const std::array<float, 3>& c) { return pack5(byteOf(c[0]) >> 3, byteOf(c[1]) >> 3, byteOf(c[2]) >> 3); }
// A texel tinted by an 8-bit colour: texel * colour / 128, saturating.
uint16_t tint(uint16_t texel, const std::array<float, 3>& c) {
    int ch[3];
    for (int i = 0; i < 3; i++) ch[i] = std::min(31, (((texel >> (5 * i)) & 31) * byteOf(c[size_t(i)])) >> 7);
    return pack5(ch[0], ch[1], ch[2]);
}

void drawLine(Frame& f, int x0, int y0, int x1, int y1, uint16_t c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    // Long lines off screen: stop once both coordinates are hopelessly outside.
    for (int guard = 0; guard < 70000; guard++) {
        f.put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

void drawSystemText(Frame& f, int x, int y, const std::string& text, uint16_t col) {
    for (unsigned char raw : truncateBytes(text, kTextMax)) {
        const uint8_t ch = glyphChar(raw);
        if (ch != ' ') {
            const int idx = ch - 32, u = (idx % 32) * 8, v = (idx / 32) * 16;
            for (int gy = 0; gy < 16; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if ((kSystemFont[(v + gy) * 32 + (u + gx) / 8] >> ((u + gx) & 7)) & 1) f.put(x + gx, y + gy, col);
        }
        x += 8;
    }
}

void drawFontText(Frame& f, int x, int y, const std::string& text, const splash::FontSheet& sh, uint16_t col) {
    const int perRow = 256 / std::max(1, sh.glyphWidth);
    for (unsigned char raw : truncateBytes(text, kTextMax)) {
        const uint8_t ch = glyphChar(raw);
        const int idx = ch - 32;
        const int adv = sh.advances[size_t(idx)];
        if (ch != ' ') {
            const int u = (idx % perRow) * sh.glyphWidth, v = (idx / perRow) * sh.glyphHeight;
            const int w = (adv > 0 && adv < sh.glyphWidth) ? adv : sh.glyphWidth;
            for (int gy = 0; gy < sh.glyphHeight; gy++)
                for (int gx = 0; gx < w; gx++) {
                    const int tx = u + gx, ty = v + gy;
                    if (tx < 256 && ty < sh.height && sh.texels[size_t(ty) * 256 + size_t(tx)]) f.put(x + gx, y + gy, col);
                }
        }
        x += adv;
    }
}

}  // namespace

int textWidth(const splash::Scene& scene, Assets& assets, const std::string& font, const std::string& text) {
    const std::string t = truncateBytes(text, kTextMax);
    if (font.empty()) return int(t.size()) * 8;
    const splash::FontSheet* sh = assets.font(scene, font);
    if (!sh) return int(t.size()) * 8;
    int w = 0;
    for (unsigned char raw : t) w += sh->advances[size_t(glyphChar(raw) - 32)];
    return w;
}

std::vector<uint32_t> render(const splash::Scene& scene, const std::vector<bool>& show, Screen s, Assets& assets,
                             uint32_t background, std::vector<uint8_t>* covered) {
    Frame f{s, std::vector<uint16_t>(size_t(s.w) * size_t(s.h), 0), std::vector<uint8_t>(size_t(s.w) * size_t(s.h), 0)};
    std::vector<int> order;
    for (int c = 0; c < int(scene.canvases.size()); c++)
        if (c < int(show.size()) && show[size_t(c)]) order.push_back(c);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return scene.canvases[size_t(a)].sortOrder < scene.canvases[size_t(b)].sortOrder; });
    struct Pending {
        int x, y;
        uint16_t col;
        const std::string* text;
    };
    std::vector<Pending> systemText;
    for (int c : order)
        for (const UIElement& e : scene.canvases[size_t(c)].elements) {
            if (!e.visible || !e.unknown.empty()) continue;
            const Rect r = drawn(e, s);
            switch (e.type) {
                case UIElementType::Box: f.fill(r.x, r.y, r.w, r.h, flat(e.color)); break;
                case UIElementType::Progress: {
                    f.fill(r.x, r.y, r.w, r.h, flat(e.background));
                    const int fillW = clampi(int(uint8_t(e.value)) * r.w / 100, 0, r.w);
                    if (fillW > 0) f.fill(r.x, r.y, fillW, r.h, flat(e.color));
                    break;
                }
                case UIElementType::Line:
                    drawLine(f, int16_t(e.from[0]), int16_t(e.from[1]), int16_t(e.to[0]), int16_t(e.to[1]), flat(e.color));
                    break;
                case UIElementType::Image: {
                    if (e.texture.empty()) break;
                    const Assets::Texture& t = assets.texture(e.texture, e.bitDepth, e.cutout);
                    if (!t.error.empty() || t.width <= 0 || t.height <= 0) break;
                    // UVs run 0..size-1 across the rect, as the export writes them;
                    // the rasteriser leaves out the last column and row.
                    for (int y = 0; y < r.h; y++)
                        for (int x = 0; x < r.w; x++) {
                            const int u = int(int64_t(x) * (t.width - 1) / r.w), v = int(int64_t(y) * (t.height - 1) / r.h);
                            const uint16_t w = t.words[size_t(v) * size_t(t.width) + size_t(u)];
                            if (w) f.put(r.x + x, r.y + y, tint(w, e.color));
                        }
                    break;
                }
                case UIElementType::Text:
                    if (e.font.empty()) {
                        systemText.push_back({r.x, r.y, flat(e.color), &e.text});
                    } else if (const splash::FontSheet* sh = assets.font(scene, e.font)) {
                        drawFontText(f, r.x, r.y, e.text, *sh, tint(0x7fff, e.color));
                    }
                    break;
            }
        }
    // System-font text goes out after the ordering table, queued from the
    // front-most element back, so the back-most one ends up on top.
    for (auto it = systemText.rbegin(); it != systemText.rend(); ++it) drawSystemText(f, it->x, it->y, *it->text, it->col);

    std::vector<uint32_t> out(f.px.size(), background);
    for (size_t i = 0; i < f.px.size(); i++) {
        if (!f.cov[i]) continue;
        auto ex = [](int c5) { return uint32_t((c5 << 3) | (c5 >> 2)); };
        const uint16_t w = f.px[i];
        out[i] = 0xff000000u | (ex((w >> 10) & 31) << 16) | (ex((w >> 5) & 31) << 8) | ex(w & 31);
    }
    if (covered) *covered = std::move(f.cov);
    return out;
}

}  // namespace editor::uic
