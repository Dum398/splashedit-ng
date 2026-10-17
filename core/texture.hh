// Texture conversion to PS1 form: 16bpp direct colour, or 4/8bpp indexed with
// a CLUT. Mirrors SplashEdit 2.4's PSXTexture2D + TextureQuantizer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

// RGBA float image, rows BOTTOM-UP (pixels[y * width + x], y = 0 is the
// bottom row), matching Texture2D.GetPixels.
struct Image {
    int width = 0, height = 0;
    struct Px {
        float r, g, b, a;
    };
    std::vector<Px> pixels;
};

Image loadImage(const std::filesystem::path& file);
void savePng(const Image& img, const std::filesystem::path& file);

// PS1 15-bit colour word with the semi-transparency bit.
inline uint16_t packVram(uint16_t r, uint16_t g, uint16_t b, bool stp) {
    return uint16_t((stp ? 0x8000 : 0) | ((b & 31) << 10) | ((g & 31) << 5) | (r & 31));
}

struct PsxTexture {
    std::string source;  // project path, the identity used for deduplication
    BitDepth bitDepth = BitDepth::Bpp8;
    bool cutout = false;
    int width = 0, height = 0;
    int quantizedWidth = 0;  // in 16-bit VRAM words
    // quantizedWidth x height words, row 0 = top of the image.
    std::vector<uint16_t> imageData;
    // CLUT words; empty for 16bpp. hasPalette distinguishes "no CLUT" (16bpp)
    // from an empty CLUT the way the original's null/empty list did.
    bool hasPalette = false;
    std::vector<uint16_t> palette;

    // Placement, filled by the VRAM packer.
    uint8_t packingX = 0, packingY = 0;  // inside the atlas; X in words
    uint8_t texpageX = 0, texpageY = 0;
    uint16_t clutPackingX = 0, clutPackingY = 0;  // X in units of 16 words

    uint16_t word(int x, int y) const { return imageData[size_t(y) * quantizedWidth + x]; }
};

PsxTexture convertTexture(const Image& img, BitDepth depth, bool cutout = false);

// True when any texel is below the cutout alpha threshold (half alpha): the
// texture has holes, which convertTexture keeps only with cutout on.
bool hasCutoutTexels(const Image& img);

// What the console shows for a converted texture, back in Image form (5-bit
// channels expanded to the nearest 8-bit level; transparent texels get a = 0).
Image decodeTexture(const PsxTexture& t);

// How far a converted texture is from its source, over the texels the
// console draws (with cutout, source texels below the alpha threshold are
// skipped). deltaE is the mean OKLab distance x100; deltaEBlur is the same
// after a 3x3 box blur of both images, which is closer to what dithering
// looks like at viewing distance than a per-texel score.
struct TextureError {
    double psnr = 0;  // RGB, dB
    double deltaE = 0;
    double deltaEBlur = 0;
    int colorsUsed = 0;  // distinct palette entries actually referenced
};
TextureError measureTexture(const Image& source, const PsxTexture& t);

}  // namespace splash
