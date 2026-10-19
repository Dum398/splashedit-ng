// UI scale for high-DPI monitors (#136). The editor lays itself out in UI
// coordinates; one UI unit covers `c` window units, where `c` is the part of
// the OS scale the ImGui SDL3 backend does not already handle. On macOS and
// Wayland the window is measured in points and the backend renders at the
// pixel density, so c is 1 there; on Windows and X11 at 150% the window is
// measured in pixels (density 1) while the display asks for 1.5. No SDL here,
// so editor_tests can check it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "ui/window_chrome.hh"

namespace ui {

constexpr float kMinUiScale = 0.5f, kMaxUiScale = 4.0f;

// A --ui-scale / SPLASHEDIT_UI_SCALE value: a number in [0.5, 4]. False (and
// `out` untouched) for anything else.
inline bool parseUiScale(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s || *end != '\0' || !std::isfinite(v) || v < kMinUiScale || v > kMaxUiScale) return false;
    out = v;
    return true;
}

// displayScale: SDL_GetWindowDisplayScale; density: SDL_GetWindowPixelDensity;
// override: a parsed --ui-scale, or 0 for none. Anything SDL could not answer
// (0, NaN) counts as 1.
inline float contentScale(float displayScale, float density, float override = 0) {
    if (override > 0) return override;
    if (!std::isfinite(displayScale) || displayScale <= 0) return 1;
    if (!std::isfinite(density) || density <= 0) density = 1;
    const float c = displayScale / density;
    if (!std::isfinite(c) || c <= 0) return 1;
    return std::clamp(c, kMinUiScale, kMaxUiScale);
}

inline float toUi(float windowCoord, float c) { return windowCoord / c; }
inline float toWindow(float uiCoord, float c) { return uiCoord * c; }

// Where a scripted pointer goes, in window coordinates, so that it lands on UI
// coordinate `ui`: ImGui floors the window coordinate, the UI pass divides it
// by c and ImGui floors again. Rounding up survives both floors for c >= 1.
inline float scriptedWindowCoord(float ui, float c) { return std::ceil(ui * c); }

inline ChromeRect toWindow(const ChromeRect& r, float c) { return {r.x0 * c, r.y0 * c, r.x1 * c, r.y1 * c}; }

// The resize band, in window units: kResizeEdge UI units deep, at least one.
inline int resizeEdge(float c) { return std::max(1, (int)std::lround(kResizeEdge * c)); }

// chromeHitTest for rectangles drawn in UI coordinates at scale c; the window
// size and the point are window coordinates, as SDL hands them over.
inline ChromeHit scaledChromeHitTest(int w, int h, bool maximized, int x, int y, float c, const ChromeRect& titleBar,
                                     const std::vector<ChromeRect>& controls) {
    std::vector<ChromeRect> scaled;
    scaled.reserve(controls.size());
    for (const ChromeRect& r : controls) scaled.push_back(toWindow(r, c));
    return chromeHitTest(w, h, maximized, x, y, toWindow(titleBar, c), scaled, resizeEdge(c));
}

// The window opened without --size: the default grown by c, but no bigger
// than the display's usable area (when known, i.e. > 0).
struct WindowSize {
    int w = 0, h = 0;
};
inline WindowSize initialWindowSize(int w, int h, float c, int usableW, int usableH) {
    WindowSize s{(int)std::lround(w * c), (int)std::lround(h * c)};
    if (usableW > 0) s.w = std::min(s.w, usableW);
    if (usableH > 0) s.h = std::min(s.h, usableH);
    return s;
}

}  // namespace ui
