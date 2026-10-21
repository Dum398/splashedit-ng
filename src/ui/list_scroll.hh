// A scrolled list's arithmetic (the Scene panel's rows): how far it may
// scroll, the scroll that brings a row into view, the thumb, and the scroll
// while a row is dragged near an edge. Kept apart from the widgets (no
// ImGui) so editor_tests can check it. Positions are in px; `s` is how far
// the content is scrolled up, `content` its height and `view` the visible height.
#pragma once

#include <algorithm>

namespace ui {

inline float maxScroll(float content, float view) { return std::max(0.0f, content - view); }

inline float clampScroll(float s, float content, float view) { return std::clamp(s, 0.0f, maxScroll(content, view)); }

// The scroll nearest `s` that shows the row from `top` to `bottom` (content
// px from the list's top) whole; a row taller than the view shows its top.
inline float scrollToShow(float s, float top, float bottom, float view) {
    if (top < s) return top;
    if (bottom > s + view) return std::min(top, bottom - view);
    return s;
}

// The thumb at the view's right edge: its top (from the view's top) and height.
struct Thumb {
    float top, height;
};
constexpr float kMinThumb = 24;

inline Thumb thumb(float s, float content, float view) {
    const float m = maxScroll(content, view);
    if (m <= 0 || view <= 0) return {0, view};
    const float h = std::min(view, std::max(kMinThumb, view * view / content));
    return {(view - h) * std::clamp(s / m, 0.0f, 1.0f), h};
}

// Dragging the thumb `dy` px scrolls the content this far.
inline float thumbDragScroll(float dy, float content, float view) {
    const float travel = view - thumb(0, content, view).height;
    return travel > 0 ? dy * maxScroll(content, view) / travel : 0;
}

// A row dragged with the pointer at `y`: within `zone` of the view's top
// (`viewTop`) or bottom, or past them, the list scrolls, up to `maxSpeed`
// px a second at the edge and beyond; 0 elsewhere. Negative scrolls up.
inline float edgeScrollSpeed(float y, float viewTop, float viewBottom, float zone, float maxSpeed) {
    if (zone <= 0 || viewBottom - viewTop < 2 * zone) return 0;
    if (y < viewTop + zone) return -maxSpeed * std::min(1.0f, (viewTop + zone - y) / zone);
    if (y > viewBottom - zone) return maxSpeed * std::min(1.0f, (y - (viewBottom - zone)) / zone);
    return 0;
}

}  // namespace ui
