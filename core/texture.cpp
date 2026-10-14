#include "texture.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "stb_image.h"
#include "stb_image_write.h"

namespace splash {

namespace {

constexpr float kCutoutAlpha = 0.5f;

// Floyd-Steinberg error is scaled by this before it spreads. A trade-off, not
// an optimum: on the texstats corpus (tests/README.md) per-texel deltaE rises
// with strength while the blurred deltaE is lowest near 0.9, and texels are
// often magnified on screen, where dither noise shows.
constexpr float kDitherStrength = 0.75f;

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

struct Lab {
    float l = 0, a = 0, b = 0;
    friend Lab operator+(Lab p, Lab q) { return {p.l + q.l, p.a + q.a, p.b + q.b}; }
    friend Lab operator-(Lab p, Lab q) { return {p.l - q.l, p.a - q.a, p.b - q.b}; }
    friend Lab operator*(Lab p, float s) { return {p.l * s, p.a * s, p.b * s}; }
};

float dist2(Lab p, Lab q) {
    float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

Lab oklab(float r, float g, float b) {
    r = srgbToLinear(r);
    g = srgbToLinear(g);
    b = srgbToLinear(b);
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

void oklabToSrgb(Lab c, float& r, float& g, float& b) {
    float l = c.l + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.l - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.l - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    r = linearToSrgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    g = linearToSrgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    b = linearToSrgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

// 5-bit level nearest to an 8-bit-derived channel, and back.
unsigned to5(float c) { return unsigned(std::lround(std::clamp(c, 0.f, 1.f) * 31.f)); }
float expand5(unsigned v) { return float((v * 255 + 15) / 31) / 255.f; }

// An opaque colour as a VRAM word. 0x0000 is the console's transparent
// texel, so black becomes 0x8000: still black, and the set STP bit keeps it
// drawn.
uint16_t opaqueWord(float r, float g, float b) {
    uint16_t v = packVram(uint16_t(to5(r)), uint16_t(to5(g)), uint16_t(to5(b)), false);
    return v == 0 ? 0x8000 : v;
}

Lab wordLab(uint16_t w) { return oklab(expand5(w & 31), expand5((w >> 5) & 31), expand5((w >> 10) & 31)); }

// Nearest-colour search over a set of OKLab centres, sorted by L so a search
// can stop once the lightness gap alone is too far. It returns exactly what a
// scan of every centre in index order with dist2 returns: the smallest
// distance, ties to the lowest index. Pruning only skips a centre when
// (p.l - c.l)^2, a lower bound of its float dist2 (adding non-negative floats
// never goes down), is already greater than the bound.
class NearestSearch {
public:
    explicit NearestSearch(const std::vector<Lab>& centres) : sorted_(centres.size()) {
        for (size_t j = 0; j < sorted_.size(); j++) sorted_[j] = {centres[j], int(j)};
        std::sort(sorted_.begin(), sorted_.end(), [](const Entry& a, const Entry& b) {
            return a.c.l < b.c.l || (a.c.l == b.c.l && a.index < b.index);
        });
    }

    // Index of the nearest centre and its dist2; with `second`, also the
    // smallest dist2 among the other centres (INFINITY if there is none).
    // Written with raw pointers and dist2 spelled out (same expression, same
    // rounding) so it stays quick in unoptimised builds too.
    int find(Lab p, float& bd) const { return search<false>(p, bd, nullptr); }
    int find(Lab p, float& bd, float& second) const { return search<true>(p, bd, &second); }

private:
    template <bool kSecond>
    int search(Lab p, float& bd, float* second) const {
        const Entry* e = sorted_.data();
        size_t n = sorted_.size();
        size_t lo = 0, hi = n;  // first entry with l >= p.l
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (e[mid].c.l < p.l) lo = mid + 1;
            else hi = mid;
        }
        int best = -1;
        float b = INFINITY, sd = INFINITY;
        size_t up = lo, down = lo;  // next up is e[up], next down is e[down - 1]
        bool goingUp = up < n, goingDown = down > 0;
        while (goingUp || goingDown) {
            for (int side = 0; side < 2; side++) {
                const Entry* c;
                if (side == 0) {
                    if (!goingUp) continue;
                    c = &e[up];
                } else {
                    if (!goingDown) continue;
                    c = &e[down - 1];
                }
                float dl = p.l - c->c.l;
                if (dl * dl > (kSecond ? sd : b)) {
                    // dist2 >= dl^2, and dl only grows further out this side.
                    if (side == 0) goingUp = false;
                    else goingDown = false;
                    continue;
                }
                float da = p.a - c->c.a, db = p.b - c->c.b;
                float d = dl * dl + da * da + db * db;
                if (d < b || (d == b && c->index < best)) {
                    if (b < sd) sd = b;
                    b = d, best = c->index;
                } else if (d < sd) {
                    sd = d;
                }
                if (side == 0) goingUp = ++up < n;
                else goingDown = --down > 0;
            }
        }
        bd = b;
        if (kSecond) *second = sd;
        return best;
    }

    struct Entry {
        Lab c;
        int index;
    };
    std::vector<Entry> sorted_;
};

struct Quantized {
    std::vector<int> indices;  // [y * w + x], y bottom-up; -1 = transparent
    std::vector<uint16_t> palette;
};

// Palette for the texels in `use`, at most k entries, then every used texel
// mapped onto it.
//
// When the source already fits in k colours once each is rounded to 15 bits,
// the palette is those colours and every texel gets its own, no dithering.
// Otherwise a k-means in OKLab, weighted by the square root of how many
// texels carry each colour, picks the palette (deterministic farthest-point seeding, iterated
// until the assignment is stable), the result is rounded to 15 bits, and the
// texels are error-diffused onto the rounded palette in serpentine order.
Quantized quantize(const Image& img, const std::vector<bool>& use, int k) {
    int w = img.width, h = img.height;
    size_t n = size_t(w) * h;
    Quantized q;
    q.indices.assign(n, -1);

    // Exact case: distinct 15-bit colours in first-appearance order.
    std::vector<int> exact(0x10000, -1);  // word -> palette slot
    std::vector<uint16_t> exactOrder;
    std::vector<uint16_t> words(n);
    for (size_t i = 0; i < n; i++) {
        if (!use[i]) continue;
        const auto& p = img.pixels[i];
        uint16_t word = words[i] = opaqueWord(p.r, p.g, p.b);
        if (exact[word] < 0) exact[word] = int(exactOrder.size()), exactOrder.push_back(word);
    }
    if (exactOrder.empty()) return q;
    if (int(exactOrder.size()) <= k) {
        q.palette = exactOrder;
        for (size_t i = 0; i < n; i++)
            if (use[i]) q.indices[i] = exact[words[i]];
        return q;
    }

    // Weighted distinct source colours, in OKLab.
    struct Point {
        Lab c;
        float weight;
    };
    std::vector<Point> pts;
    {
        // Colours are equal when their channels compare equal, so -0 and +0
        // are one colour (adding +0 makes -0 +0). Open addressing on the bits.
        struct Slot {
            uint32_t r, g, b;
            int point = -1;
        };
        size_t cap = 16;
        while (cap < 2 * n) cap *= 2;
        std::vector<Slot> table(cap);
        Slot* T = table.data();
        auto bits = [](float f) {
            f += 0.f;
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        };
        for (size_t i = 0; i < n; i++) {
            if (!use[i]) continue;
            const auto& p = img.pixels[i];
            uint32_t r = bits(p.r), g = bits(p.g), b = bits(p.b);
            size_t at = (r * 0x9E3779B1u ^ g * 0x85EBCA77u ^ b * 0xC2B2AE3Du) & (cap - 1);
            while (T[at].point >= 0 && !(T[at].r == r && T[at].g == g && T[at].b == b)) at = (at + 1) & (cap - 1);
            if (T[at].point < 0) {
                T[at] = {r, g, b, int(pts.size())};
                pts.push_back({oklab(p.r, p.g, p.b), 1.f});
            } else {
                pts[size_t(T[at].point)].weight += 1.f;
            }
        }
    }
    // Square-root weights: by raw texel count a large flat background takes
    // most of a 16-entry palette and a small saturated detail (a face, a logo)
    // goes grey. Corpus mean deltaE is the same within 0.01 at 1.0 and 0.5.
    for (auto& p : pts) p.weight = std::sqrt(p.weight);

    // The hot loops below use raw pointers and spell dist2 out (the same
    // expression, so the same rounding): unoptimised builds pay a call for
    // every vector index and every dist2.
    const size_t np = pts.size();
    const Point* P = pts.data();
    std::vector<Lab> centres;
    std::vector<float> nearest(np, INFINITY);
    float* nearD = nearest.data();
    // Farthest-point seeding: the next centre is the colour with the largest
    // weight x distance to the centres so far (first such in order). Each pass
    // folds the newest centre into the distances and finds the next pick.
    size_t next = 0;
    {
        size_t heaviest = 0;
        for (size_t i = 1; i < np; i++)
            if (P[i].weight > P[heaviest].weight) heaviest = i;
        centres.push_back(P[heaviest].c);
        float bestScore = -1;
        for (size_t i = 0; i < np; i++) {
            nearD[i] = dist2(P[i].c, centres[0]);
            float s = P[i].weight * nearD[i];
            if (s > bestScore) bestScore = s, next = i;
        }
    }
    while (int(centres.size()) < k) {
        const Lab c = P[next].c;
        centres.push_back(c);
        if (int(centres.size()) == k) break;
        float bestScore = -1;
        next = 0;
        for (size_t i = 0; i < np; i++) {
            float dl = P[i].c.l - c.l;
            if (dl * dl < nearD[i]) {  // else dist2 >= dl^2 >= nearD: the min stays
                float da = P[i].c.a - c.a, db = P[i].c.b - c.b;
                float d = dl * dl + da * da + db * db;
                if (d < nearD[i]) nearD[i] = d;
            }
            float s = P[i].weight * nearD[i];
            if (s > bestScore) bestScore = s, next = i;
        }
    }

    // Lloyd iterations. Each point is assigned the centre a full scan would
    // pick (nearest, ties to the lowest index), but most points skip the scan:
    // Hamerly bounds, an upper bound on the distance to the assigned centre
    // and a lower bound on the distance to every other one, kept in double
    // and moved by how far the centres moved. A point whose bounds are apart
    // by far more than float dist2's rounding cannot change centre.
    constexpr double kSlack = 1e-3;
    std::vector<int> assignV(np, -1);
    std::vector<double> upperV(np, 0), lowerV(np, 0);
    int* assign = assignV.data();
    double* upper = upperV.data();
    double* lower = lowerV.data();
    const size_t kk = size_t(k);
    std::vector<Lab> before(kk), sumV(kk);
    std::vector<float> weightV(kk);
    std::vector<double> movedV(kk), halfV(kk);
    for (int iter = 0; iter < 100; iter++) {
        bool changed = false;
        NearestSearch search(centres);
        const Lab* C = centres.data();
        // Half the distance from each centre to its nearest other centre: a
        // point closer than that to its centre is closer to it than to any
        // other (triangle inequality).
        double* half = halfV.data();
        for (size_t j = 0; j < kk; j++) {
            float m = INFINITY;
            for (size_t o = 0; o < kk; o++)
                if (o != j) m = std::min(m, dist2(C[j], C[o]));
            half[j] = std::sqrt(double(m)) * (1 - 1e-5) / 2;
        }
        for (size_t i = 0; i < np; i++) {
            if (assign[i] >= 0) {
                double bound = std::max(lower[i], half[assign[i]]);
                if (upper[i] * (1 + kSlack) < bound) continue;
                const Lab& c = C[assign[i]];
                float dl = P[i].c.l - c.l, da = P[i].c.a - c.a, db = P[i].c.b - c.b;
                upper[i] = std::sqrt(double(dl * dl + da * da + db * db)) * (1 + 1e-5);
                if (upper[i] * (1 + kSlack) < bound) continue;
            }
            float bd, sd;
            int best = search.find(P[i].c, bd, sd);
            upper[i] = std::sqrt(double(bd)) * (1 + 1e-5);
            lower[i] = std::sqrt(double(sd)) * (1 - 1e-5);
            if (assign[i] != best) assign[i] = best, changed = true;
        }
        if (!changed) break;
        before = centres;
        Lab* sum = sumV.data();
        float* weight = weightV.data();
        for (size_t j = 0; j < kk; j++) sum[j] = Lab{}, weight[j] = 0.f;
        for (size_t i = 0; i < np; i++) {
            Lab& s = sum[assign[i]];
            float w = P[i].weight;
            s.l = s.l + P[i].c.l * w, s.a = s.a + P[i].c.a * w, s.b = s.b + P[i].c.b * w;
            weight[assign[i]] += w;
        }
        for (size_t j = 0; j < kk; j++)
            if (weight[j] > 0) centres[j] = sum[j] * (1.f / weight[j]);
        // An empty cluster moves to the worst-served colour, by the distances
        // to the centres the points were just assigned.
        if (std::find(weightV.begin(), weightV.end(), 0.f) != weightV.end())
            for (size_t i = 0; i < np; i++) nearD[i] = dist2(P[i].c, before[size_t(assign[i])]);
        for (size_t j = 0; j < kk; j++) {
            if (weight[j] > 0) continue;
            size_t worst = 0;
            for (size_t i = 1; i < np; i++)
                if (P[i].weight * nearD[i] > P[worst].weight * nearD[worst]) worst = i;
            centres[j] = P[worst].c;
            nearD[worst] = 0;
        }

        // Hamerly: a point's lower bound drops by the most any other centre
        // moved.
        double* moved = movedV.data();
        size_t most = 0;
        double maxMove = 0, nextMove = 0;
        for (size_t j = 0; j < kk; j++) {
            moved[j] = std::sqrt(double(dist2(before[j], centres[j]))) * (1 + 1e-5) + 1e-12;
            if (moved[j] > maxMove) nextMove = maxMove, maxMove = moved[j], most = j;
            else if (moved[j] > nextMove) nextMove = moved[j];
        }
        for (size_t i = 0; i < np; i++) {
            upper[i] += moved[assign[i]];
            lower[i] -= size_t(assign[i]) == most ? nextMove : maxMove;
        }
    }

    // Round to 15 bits. Two centres can land on one word; the freed slot
    // goes to the colour the rounded palette serves worst.
    std::vector<Lab> palLab;
    auto addWord = [&](uint16_t word) {
        if (std::find(q.palette.begin(), q.palette.end(), word) != q.palette.end()) return false;
        q.palette.push_back(word);
        palLab.push_back(wordLab(word));
        return true;
    };
    for (const Lab& c : centres) {
        float r, g, b;
        oklabToSrgb(c, r, g, b);
        addWord(opaqueWord(r, g, b));
    }
    // Each point's distance to the rounded palette so far, kept up to date as
    // words are added (a min, so the same whatever order it is taken in).
    std::vector<float> palDist;
    size_t palDone = 0;
    while (int(q.palette.size()) < k) {
        if (palDist.empty()) {
            NearestSearch search(palLab);
            palDist.resize(pts.size());
            for (size_t i = 0; i < pts.size(); i++) search.find(pts[i].c, palDist[i]);
        } else {
            for (size_t j = palDone; j < palLab.size(); j++)
                for (size_t i = 0; i < pts.size(); i++) {
                    float dl = pts[i].c.l - palLab[j].l;
                    if (dl * dl < palDist[i]) palDist[i] = std::min(palDist[i], dist2(pts[i].c, palLab[j]));
                }
        }
        palDone = palLab.size();
        size_t worst = 0;
        float worstScore = -1;
        for (size_t i = 0; i < pts.size(); i++) {
            float d = palDist[i];
            if (pts[i].weight * d > worstScore) worstScore = pts[i].weight * d, worst = i;
        }
        float r, g, b;
        oklabToSrgb(pts[worst].c, r, g, b);
        if (!addWord(opaqueWord(r, g, b))) break;  // nothing left that rounds to a new word
    }

    // Error diffusion onto the rounded palette.
    NearestSearch palSearch(palLab);
    std::vector<Lab> lab(n);
    for (size_t i = 0; i < n; i++)
        if (use[i]) lab[i] = oklab(img.pixels[i].r, img.pixels[i].g, img.pixels[i].b);
    for (int y = 0; y < h; y++) {
        bool rtl = y & 1;
        for (int s = 0; s < w; s++) {
            int x = rtl ? w - 1 - s : s;
            size_t i = size_t(y) * w + x;
            if (!use[i]) continue;
            float bd;
            int best = palSearch.find(lab[i], bd);
            q.indices[i] = best;
            Lab err = (lab[i] - palLab[size_t(best)]) * kDitherStrength;
            int fwd = rtl ? -1 : 1;
            auto spread = [&](int dx, int dy, float f) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= w || ny >= h) return;
                size_t j = size_t(ny) * w + nx;
                if (use[j]) lab[j] = lab[j] + err * f;
            };
            spread(fwd, 0, 7.f / 16.f);
            spread(-fwd, 1, 3.f / 16.f);
            spread(0, 1, 5.f / 16.f);
            spread(fwd, 1, 1.f / 16.f);
        }
    }
    return q;
}

}  // namespace

Image loadImage(const std::filesystem::path& file) {
    int w, h, n;
    unsigned char* data = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!data) throw std::runtime_error("cannot load image " + file.string() + ": " + stbi_failure_reason());
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.resize(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        const unsigned char* row = data + size_t(h - 1 - y) * w * 4;
        for (int x = 0; x < w; x++) {
            const unsigned char* p = row + x * 4;
            img.pixels[size_t(y) * w + x] = {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
        }
    }
    stbi_image_free(data);
    return img;
}

void savePng(const Image& img, const std::filesystem::path& file) {
    std::vector<unsigned char> rgba(size_t(img.width) * img.height * 4);
    for (int y = 0; y < img.height; y++)
        for (int x = 0; x < img.width; x++) {
            const auto& p = img.pixels[size_t(img.height - 1 - y) * img.width + x];
            unsigned char* o = &rgba[(size_t(y) * img.width + x) * 4];
            const float c[4] = {p.r, p.g, p.b, p.a};
            for (int k = 0; k < 4; k++) o[k] = (unsigned char)std::lround(std::clamp(c[k], 0.f, 1.f) * 255.f);
        }
    if (!stbi_write_png(file.string().c_str(), img.width, img.height, 4, rgba.data(), img.width * 4))
        throw std::runtime_error("cannot write " + file.string());
}

PsxTexture convertTexture(const Image& img, BitDepth depth, bool cutout) {
    PsxTexture t;
    t.bitDepth = depth;
    t.cutout = cutout;
    t.width = img.width;
    t.height = img.height;
    t.quantizedWidth = depth == BitDepth::Bpp4 ? img.width / 4 : depth == BitDepth::Bpp8 ? img.width / 2 : img.width;
    auto opaque = [&](const Image::Px& p) { return !(cutout && p.a < kCutoutAlpha); };

    if (depth == BitDepth::Bpp16) {
        t.imageData.resize(size_t(t.quantizedWidth) * t.height);
        for (int y = 0; y < t.height; y++)
            for (int x = 0; x < t.width; x++) {
                const auto& p = img.pixels[size_t(t.height - y - 1) * t.width + x];
                t.imageData[size_t(y) * t.quantizedWidth + x] = opaque(p) ? opaqueWord(p.r, p.g, p.b) : 0;
            }
        t.hasPalette = false;
        return t;
    }

    // With cutout, palette entry 0 is the transparent word and the opaque
    // texels share the rest.
    int maxColors = depth == BitDepth::Bpp4 ? 16 : 256;
    std::vector<bool> use(img.pixels.size());
    for (size_t i = 0; i < use.size(); i++) use[i] = opaque(img.pixels[i]);
    Quantized q = quantize(img, use, cutout ? maxColors - 1 : maxColors);
    int base = cutout ? 1 : 0;
    t.hasPalette = true;
    if (cutout) t.palette.push_back(0);
    t.palette.insert(t.palette.end(), q.palette.begin(), q.palette.end());
    // Keep the CLUT a whole number of 4-word units, as the 2.4 writer did
    // (it appended 1..4 entries, even to an already aligned palette).
    t.palette.resize((t.palette.size() + 3) / 4 * 4, 0);

    t.imageData.assign(size_t(t.quantizedWidth) * t.height, 0);
    for (int y = 0; y < t.height; y++) {
        int row = t.height - y - 1;
        const int* idx = &q.indices[size_t(y) * t.width];
        auto at = [&](int x) { return idx[x] < 0 ? 0 : idx[x] + base; };
        for (int g = 0; g < t.quantizedWidth; g++) {
            uint16_t packed;
            if (depth == BitDepth::Bpp8) {
                int b = g * 2;
                packed = uint16_t(((at(b + 1) & 0xFF) << 8) | (at(b) & 0xFF));
            } else {
                int b = g * 4;
                packed = uint16_t(((at(b + 3) & 0xF) << 12) | ((at(b + 2) & 0xF) << 8) | ((at(b + 1) & 0xF) << 4) |
                                  (at(b) & 0xF));
            }
            t.imageData[size_t(row) * t.quantizedWidth + g] = packed;
        }
    }
    return t;
}

Image decodeTexture(const PsxTexture& t) {
    Image img;
    img.width = t.width;
    img.height = t.height;
    img.pixels.resize(size_t(t.width) * t.height);
    for (int row = 0; row < t.height; row++) {
        int y = t.height - row - 1;
        for (int x = 0; x < t.width; x++) {
            uint16_t w;
            if (t.bitDepth == BitDepth::Bpp16) {
                w = t.word(x, row);
            } else if (t.bitDepth == BitDepth::Bpp8) {
                w = t.palette[(t.word(x / 2, row) >> ((x & 1) * 8)) & 0xff];
            } else {
                w = t.palette[(t.word(x / 4, row) >> ((x & 3) * 4)) & 0xf];
            }
            float a = w == 0 ? 0.f : 1.f;
            img.pixels[size_t(y) * t.width + x] = {expand5(w & 31), expand5((w >> 5) & 31), expand5((w >> 10) & 31), a};
        }
    }
    return img;
}

TextureError measureTexture(const Image& source, const PsxTexture& t) {
    Image out = decodeTexture(t);
    int w = source.width, h = source.height;
    auto counted = [&](int i) { return !(t.cutout && source.pixels[size_t(i)].a < kCutoutAlpha); };
    auto blur = [&](const Image& im, int x, int y) {
        float r = 0, g = 0, b = 0;
        int n = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || !counted(ny * w + nx)) continue;
                const auto& p = im.pixels[size_t(ny) * w + nx];
                r += p.r, g += p.g, b += p.b, n++;
            }
        return oklab(r / n, g / n, b / n);
    };
    auto dist = [](Lab p, Lab q) {
        return std::sqrt(double(p.l - q.l) * (p.l - q.l) + double(p.a - q.a) * (p.a - q.a) + double(p.b - q.b) * (p.b - q.b));
    };
    TextureError e;
    double se = 0, de = 0, deb = 0;
    long n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (!counted(i)) continue;
            const auto& s = source.pixels[size_t(i)];
            const auto& o = out.pixels[size_t(i)];
            for (float d : {s.r - o.r, s.g - o.g, s.b - o.b}) se += double(d) * d * 255.0 * 255.0;
            de += dist(oklab(s.r, s.g, s.b), oklab(o.r, o.g, o.b));
            deb += dist(blur(source, x, y), blur(out, x, y));
            n++;
        }
    if (n) {
        double mse = se / (3.0 * n);
        e.psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        e.deltaE = 100.0 * de / n;
        e.deltaEBlur = 100.0 * deb / n;
    }
    if (t.bitDepth != BitDepth::Bpp16) {
        std::vector<bool> used(t.palette.size());
        int per = t.bitDepth == BitDepth::Bpp8 ? 2 : 4, bits = 16 / per;
        for (uint16_t word : t.imageData)
            for (int k = 0; k < per; k++) used[(word >> (k * bits)) & ((1 << bits) - 1)] = true;
        for (bool u : used) e.colorsUsed += u;
    }
    return e;
}

}  // namespace splash
