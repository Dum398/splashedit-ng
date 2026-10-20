// A software PS1 GPU for the frame debugger: replays a decoded GPU dump
// (gpudump.hh) into 1024x512 16-bit VRAM, and keeps, per VRAM pixel, which
// command last wrote it.
//
// Written from the psx-spx GPU pages: drawing area and offset clipping,
// triangles with a top-left fill rule (quads as triangles 1-2-3 and 2-3-4),
// 4/8/15-bit textures with CLUTs, the texture window, the four
// semi-transparency modes (a textured pixel only when its texel has bit 15),
// mask bit set and check, dithering of gouraud-shaded and modulated polygons
// and of lines, raw textures, rectangles (with the v2 flips), lines and
// polylines, fills and the VRAM copies. Not modelled: the texture cache,
// interlaced drawing, the "draw to display area" bit, 24-bit display.
#pragma once

#include <cstdint>
#include <vector>

#include "gpudump.hh"

namespace gpuraster {

constexpr int kVramWidth = 1024, kVramHeight = 512;

// How closely to follow which renderer. The defaults follow the psx-spx
// description of the hardware; redux() matches pcsx-redux's software GPU,
// which does not dither and shades textured gouraud polygons a pixel pair at
// a time (both pixels take the colour of the first, pairs counted from the
// span's first pixel).
struct Options {
    bool dither = true;
    bool reduxPairs = false;
    static Options redux() { return Options{false, true}; }
};

struct Gpu {
    Options options;
    std::vector<uint16_t> vram = std::vector<uint16_t>(kVramWidth * kVramHeight, 0);
    // The index (in the CommandList) of the command that last wrote each pixel, -1 for none.
    std::vector<int32_t> owner = std::vector<int32_t>(kVramWidth * kVramHeight, -1);
    // Commands replayed so far (replay() continues from here).
    size_t done = 0;

    uint16_t at(int x, int y) const { return vram[size_t(y & 511) * kVramWidth + size_t(x & 1023)]; }
    void clear();
    // Runs one command; it draws with the state it carries (Command::state). `id` goes into owner.
    void execute(const gpudump::Command& c, int32_t id);
};

// Brings `gpu` to the point after commands [0, k): continues when it is at
// or before k, else starts over.
void replay(const gpudump::CommandList& list, size_t k, Gpu& gpu);

// The 15-bit colour of a VRAM pixel as 8-bit RGB (mask bit dropped).
inline void toRgb(uint16_t p, uint8_t& r, uint8_t& g, uint8_t& b) {
    const int r5 = p & 31, g5 = (p >> 5) & 31, b5 = (p >> 10) & 31;
    r = uint8_t(r5 << 3 | r5 >> 2);
    g = uint8_t(g5 << 3 | g5 >> 2);
    b = uint8_t(b5 << 3 | b5 >> 2);
}

// RGBA8 of a VRAM rectangle (wrapping at the VRAM edges), top-down.
std::vector<uint8_t> rgba(const Gpu& gpu, int x, int y, int w, int h);

// Where a drawing command's vertices land in VRAM (drawing offset applied),
// for outlining it: polygon corners in drawing order (a quad as 1,2,4,3),
// a rectangle's or fill's or copy's four corners, a line's points.
std::vector<std::pair<int, int>> outline(const gpudump::Command& c);

}  // namespace gpuraster
