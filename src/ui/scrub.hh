// How far a drag on a number field moves its value. Kept apart from the
// widget (no ImGui) so editor_tests can check it.
#pragma once

#include <algorithm>
#include <cmath>

namespace ui {

struct ScrubRate {
    float perPixel;  // value change per pixel dragged
    float step;      // the value is rounded to a multiple of this
};

// A whole-number field (pixels, frames) moves one per pixel, a tenth with
// Shift. Any other grows with the value: 0.01 per pixel below 1, then 1% of
// it, rounded to the power of ten under that (a tenth with Shift).
inline ScrubRate scrubRate(float value, bool integer, bool fine) {
    if (integer) return {fine ? 0.1f : 1.0f, 1.0f};
    const float perPixel = std::max(0.01f, std::fabs(value) * 0.01f) * (fine ? 0.1f : 1.0f);
    return {perPixel, std::pow(10.0f, std::floor(std::log10(perPixel)))};
}

// One frame of a drag: `dx` pixels move the unrounded value `acc` (kept
// across the drag); returns the value to show and store.
inline float scrubTo(float& acc, float dx, bool integer, bool fine) {
    const ScrubRate r = scrubRate(acc, integer, fine);
    acc += dx * r.perPixel;
    return std::round(acc / r.step) * r.step;
}

}  // namespace ui
