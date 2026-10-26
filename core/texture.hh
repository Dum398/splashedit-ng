// Texture conversion to PS1 form: 16bpp direct colour, or 4/8bpp indexed with
// a CLUT. Mirrors SplashEdit 2.4's PSXTexture2D + TextureQuantizer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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
    std::string paletteGroup;  // Material::paletteGroup it converted with; part of its identity

    // Placement, filled by the VRAM packer.
    uint8_t packingX = 0, packingY = 0;  // inside the atlas; X in words
    uint8_t texpageX = 0, texpageY = 0;
    uint16_t clutPackingX = 0, clutPackingY = 0;  // X in units of 16 words

    uint16_t word(int x, int y) const { return imageData[size_t(y) * quantizedWidth + x]; }
};

PsxTexture convertTexture(const Image& img, BitDepth depth, bool cutout = false);

// The CLUT convertTexture gives a 4bpp or 8bpp texture, built over the
// texels of several images together: the shared palette of a palette group.
// For one image it is exactly that texture's palette.
std::vector<uint16_t> buildPalette(const std::vector<const Image*>& images, BitDepth depth, bool cutout);
// A 4bpp or 8bpp texture mapped onto a given CLUT (from buildPalette with the
// same depth and cutout); convertTexture(img, depth, cutout) is this with
// buildPalette({&img}, depth, cutout).
PsxTexture convertTexture(const Image& img, BitDepth depth, bool cutout, const std::vector<uint16_t>& palette);

// The members of a palette group as the exporter finds them: every distinct
// texture of an active object's textured material whose paletteGroup is
// `name` and that converts at `depth` (4 or 8 bpp) with `cutout`, in object
// and material order. `cutoutOf` says whether a texture has cutout texels
// (hasCutoutTexels), nullopt when it cannot be loaded: such a texture is left
// out (its own material reports it).
std::vector<std::string> paletteGroupMembers(const Scene& scene, const std::string& name, BitDepth depth, bool cutout,
                                             const std::function<std::optional<bool>(const std::string&)>& cutoutOf);
// The group's members and the CLUT they share (buildPalette over them), the
// one the exporter converts each member onto. `image` loads a texture and
// throws when it cannot.
struct PaletteGroup {
    std::vector<std::string> members;
    std::vector<uint16_t> palette;
};
using ImageSource = std::function<const Image&(const std::string&)>;
PaletteGroup paletteGroup(const Scene& scene, const std::string& name, BitDepth depth, bool cutout, const ImageSource& image);

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
