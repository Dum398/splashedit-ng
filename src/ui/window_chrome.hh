// Where a point on the borderless main window lands: a resize edge, the
// draggable title bar, or a normal click. Decided from geometry, not hover:
// on Windows the pointer over a draggable area produces no mouse motion, so
// the UI never learns it is over a button. A maximized window has no resize
// edges, so the buttons in the top band take their clicks and the window
// cannot be resized out of the maximized state. No SDL here, so editor_tests
// can check it.
#pragma once

#include <vector>

namespace ui {

struct ChromeRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(float x, float y) const { return x >= x0 && y >= y0 && x < x1 && y < y1; }
};

enum class ChromeHit {
    Normal,
    Draggable,
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft,
};

constexpr int kResizeEdge = 6;

// titleBar: the drawn title bar; controls: rectangles inside it that take
// clicks (buttons, menus).
inline ChromeHit chromeHitTest(int w, int h, bool maximized, int x, int y, const ChromeRect& titleBar,
                               const std::vector<ChromeRect>& controls) {
    if (!maximized) {
        const bool l = x < kResizeEdge, r = x >= w - kResizeEdge;
        const bool t = y < kResizeEdge, b = y >= h - kResizeEdge;
        if (t && l) return ChromeHit::ResizeTopLeft;
        if (t && r) return ChromeHit::ResizeTopRight;
        if (b && l) return ChromeHit::ResizeBottomLeft;
        if (b && r) return ChromeHit::ResizeBottomRight;
        if (t) return ChromeHit::ResizeTop;
        if (b) return ChromeHit::ResizeBottom;
        if (l) return ChromeHit::ResizeLeft;
        if (r) return ChromeHit::ResizeRight;
    }
    const float px = (float)x, py = (float)y;
    if (!titleBar.contains(px, py)) return ChromeHit::Normal;
    for (const ChromeRect& c : controls)
        if (c.contains(px, py)) return ChromeHit::Normal;
    return ChromeHit::Draggable;
}

}  // namespace ui
