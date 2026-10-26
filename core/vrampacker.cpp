#include "vrampacker.hh"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>

namespace splash {

namespace {

constexpr int kVramW = 1024, kVramH = 512;

struct Rect {
    int x, y, w, h;
    // UnityEngine.Rect.Overlaps: touching edges do not overlap.
    bool overlaps(const Rect& o) const { return o.x + o.w > x && o.x < x + w && o.y + o.h > y && o.y < y + h; }
};

int atlasWidthFor(BitDepth d) { return d == BitDepth::Bpp16 ? 256 : d == BitDepth::Bpp8 ? 128 : 64; }

bool tryPlace(Atlas& atlas, PsxTexture* t) {
    // The original loops with byte counters; for sizes that fit they behave
    // like ints.
    for (int y = 0; y <= Atlas::kHeight - t->height; y++)
        for (int x = 0; x <= atlas.width - t->quantizedWidth; x++) {
            Rect cand{x, y, t->quantizedWidth, t->height};
            bool hit = false;
            for (PsxTexture* o : atlas.textures)
                if (Rect{o->packingX, o->packingY, o->quantizedWidth, o->height}.overlaps(cand)) {
                    hit = true;
                    break;
                }
            if (!hit) {
                t->packingX = uint8_t(x);
                t->packingY = uint8_t(y);
                atlas.textures.push_back(t);
                return true;
            }
        }
    return false;
}

}  // namespace

VramLayout packVram(std::vector<PsxTexture*>& all, const VramSettings& s, std::vector<std::string>& errors,
                    bool fogMask4, bool fogMask8) {
    std::vector<Rect> reserved;
    for (const auto& a : s.prohibited) reserved.push_back({a.x, a.y, a.w, a.h});
    reserved.push_back({0, 0, s.resolutionX, s.resolutionY});
    // SplashEdit 2.4 reserves the second buffer even with single buffering
    // (and throws there); the dual layout is what it ever exported.
    if (s.verticalBuffering)
        reserved.push_back({0, 256, s.resolutionX, s.resolutionY});
    else
        reserved.push_back({s.resolutionX, 0, s.resolutionX, s.resolutionY});
    reserved.push_back({960, 0, 64, kVramH});

    std::vector<Atlas> atlases;
    using Key = std::tuple<std::string, int, bool, std::string>;
    std::map<Key, PsxTexture*> unique;
    std::vector<std::pair<PsxTexture*, PsxTexture*>> duplicates;

    for (BitDepth depth : {BitDepth::Bpp16, BitDepth::Bpp8, BitDepth::Bpp4}) {
        std::vector<PsxTexture*> group;
        for (PsxTexture* t : all)
            if (t->bitDepth == depth) group.push_back(t);
        if (group.empty()) continue;
        std::stable_sort(group.begin(), group.end(), [](PsxTexture* a, PsxTexture* b) {
            return a->quantizedWidth * a->height > b->quantizedWidth * b->height;
        });
        int aw = atlasWidthFor(depth);
        atlases.push_back({depth, 0, 0, aw, {}, {}});
        for (PsxTexture* t : group) {
            Key k{t->source, int(t->bitDepth), t->cutout, t->paletteGroup};
            auto it = unique.find(k);
            if (it != unique.end()) {
                duplicates.push_back({t, it->second});
                continue;
            }
            if (!tryPlace(atlases.back(), t)) {
                atlases.push_back({depth, 0, 0, aw, {}, {}});
                if (!tryPlace(atlases.back(), t)) {
                    errors.push_back("texture " + t->source + " does not fit in an atlas");
                    continue;
                }
            }
            unique[k] = t;
        }
    }

    VramLayout out;
    std::vector<Rect> placedAtlases, cluts;
    auto valid = [&](const Rect& r) {
        if (r.x + r.w > kVramW || r.y + r.h > kVramH) return false;
        for (const Rect& a : placedAtlases)
            if (a.overlaps(r)) return false;
        for (const Rect& a : reserved)
            if (a.overlaps(r)) return false;
        for (const Rect& a : cluts)
            if (a.overlaps(r)) return false;
        return true;
    };
    for (BitDepth depth : {BitDepth::Bpp16, BitDepth::Bpp8, BitDepth::Bpp4})
        for (Atlas& a : atlases) {
            if (a.bitDepth != depth) continue;
            bool placed = false;
            for (int y = 0; y <= kVramH - Atlas::kHeight && !placed; y += 256)
                for (int x = 0; x <= kVramW - a.width; x += 64) {
                    Rect cand{x, y, a.width, Atlas::kHeight};
                    if (valid(cand)) {
                        a.positionX = x;
                        a.positionY = y;
                        placedAtlases.push_back(cand);
                        placed = true;
                        break;
                    }
                }
            if (!placed) {
                errors.push_back("an atlas does not fit in VRAM");
                continue;
            }
            for (PsxTexture* t : a.textures) {
                t->texpageX = uint8_t(a.positionX / 64);
                t->texpageY = uint8_t(a.positionY / 256);
            }
            out.atlases.push_back(a);
        }

    std::map<std::vector<uint16_t>, const PsxTexture*> clutOf;  // palette -> the texture that placed it
    for (Atlas& a : out.atlases)
        for (PsxTexture* t : a.textures) {
            if (!t->hasPalette) continue;
            auto [same, fresh] = clutOf.emplace(t->palette, t);
            if (!fresh) {
                t->clutPackingX = same->second->clutPackingX;
                t->clutPackingY = same->second->clutPackingY;
                continue;
            }
            out.cluts.push_back(t);
            if (t->palette.empty()) continue;
            bool placed = false;
            for (int x = 0; x < kVramW && !placed; x += 16)
                for (int y = 0; y < kVramH; y++) {
                    Rect cand{x, y, int(t->palette.size()), 1};
                    if (valid(cand)) {
                        cluts.push_back(cand);
                        t->clutPackingX = uint16_t(x / 16);
                        t->clutPackingY = uint16_t(y);
                        placed = true;
                        break;
                    }
                }
            if (!placed) errors.push_back("CLUT for " + t->source + " does not fit in VRAM");
        }

    auto placeFogMask = [&](int length, int& out) {
        for (int x = 0; x < kVramW; x += 16)
            for (int y = 0; y < kVramH; y++) {
                Rect cand{x, y, length, 1};
                if (valid(cand)) {
                    cluts.push_back(cand);
                    out = (y << 6) | (x / 16);
                    return;
                }
            }
    };
    if (fogMask4) placeFogMask(16, out.fogMaskClut4);
    if (fogMask8) placeFogMask(256, out.fogMaskClut8);

    for (Atlas& a : out.atlases) {
        a.pixels.assign(size_t(a.width) * Atlas::kHeight, 0);
        for (PsxTexture* t : a.textures)
            for (int y = 0; y < t->height; y++)
                for (int x = 0; x < t->quantizedWidth; x++)
                    a.pixels[size_t(y + t->packingY) * a.width + x + t->packingX] = t->word(x, y);
    }

    for (auto& [dup, orig] : duplicates) {
        dup->packingX = orig->packingX;
        dup->packingY = orig->packingY;
        dup->texpageX = orig->texpageX;
        dup->texpageY = orig->texpageY;
        dup->clutPackingX = orig->clutPackingX;
        dup->clutPackingY = orig->clutPackingY;
    }
    return out;
}

}  // namespace splash
