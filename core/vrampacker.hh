// VRAM layout: groups textures by depth into 256-row atlases, places the
// atlases on texture-page boundaries and allocates CLUTs. Mirrors SplashEdit
// 2.4's VRAMPacker, including its first-fit order and its deduplication.
#pragma once

#include <vector>

#include "scene.hh"
#include "texture.hh"

namespace splash {

struct Atlas {
    BitDepth bitDepth;
    int positionX = 0, positionY = 0;
    int width = 0;
    static constexpr int kHeight = 256;
    std::vector<uint16_t> pixels;  // width x 256, row-major
    std::vector<PsxTexture*> textures;
};

struct VramLayout {
    std::vector<Atlas> atlases;  // finalised (placed) atlases, in placement order
    // One texture per CLUT, in placement order: textures with equal palettes
    // (a palette group's members, or two that happen to match) share one CLUT.
    std::vector<const PsxTexture*> cluts;
    // Fog mask CLUTs for 4bpp and 8bpp cutout textures, as CLUT attribute words
    // (y << 6 | x / 16), or -1 when not asked for or out of VRAM.
    int fogMaskClut4 = -1, fogMaskClut8 = -1;
};

// `textures` in collection order (object textures in object order, then UI and
// sprite textures). Duplicates (same source, depth and cutout) receive the
// placement of the first copy; the palette group is part of that identity. Errors (does not fit) are appended to `errors`.
// `fogMask4` / `fogMask8` also place the fog mask CLUT of that depth (16 or 256
// words) after the texture CLUTs; one that does not fit is left out (-1) and
// psxsplash draws the fog over those textures the old way.
VramLayout packVram(std::vector<PsxTexture*>& textures, const VramSettings& settings,
                    std::vector<std::string>& errors, bool fogMask4 = false, bool fogMask8 = false);

}  // namespace splash
