// How a PS1 frame looks on a TV: where it sits in a panel (4:3 like a TV,
// or square pixels), the broadcast safe areas inside it, and an optional
// S-video or composite look. No SDL or GL, so it tests headless.
#pragma once

#include <cstdint>
#include <vector>

namespace editor::tv {

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

// Classic broadcast fractions of the picture: action-safe is the central
// 90%, title-safe the central 80%. A TV's overscan may hide anything outside.
inline constexpr float kActionSafe = 0.9f, kTitleSafe = 0.8f;

// Where a frameW x frameH frame shows in `panel`, centred and as large as
// fits, edges on whole pixels. `tvAspect`: 4:3 whatever the width, as a TV
// stretches every PS1 mode (256 to 640 wide) across the screen; otherwise
// square pixels (a 512x240 frame shows twice as wide as it is tall, and more).
Rect displayRect(Rect panel, int frameW, int frameH, bool tvAspect);

// The central `fraction` of `frame`, each side inset by the same share of
// the frame's width or height and rounded to a whole pixel.
Rect safeRect(Rect frame, float fraction);

// RGB: the frame as is. S-video: luma sharp, chroma blurred across. Composite:
// luma softened a little, chroma blurred much wider. Both can add scanlines
// to a 240-line mode.
enum class Look { RGB = 0, SVideo = 1, Composite = 2 };

// The look applied to an RGBA frame (alpha kept). Blur widths follow the
// frame width, as the signal's bandwidth is a share of the line, not of
// pixels. With `scanlines`, a 240-line frame (h <= 256) comes back 2h rows
// tall, each source line once as is and once darkened; ask for them only
// where the frame shows at 2 or more screen rows a line, or they alias.
// `outH` gets the height. RGB returns the frame unchanged.
std::vector<uint8_t> applyLook(const uint8_t* rgba, int w, int h, Look look, bool scanlines, int* outH);

// The look's name in play.cfg and scripted output: rgb, svideo, composite.
const char* lookName(Look look);
Look lookFromName(const char* name);

}  // namespace editor::tv
