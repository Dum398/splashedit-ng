#include "gpu_raster.hh"

#include <algorithm>
#include <cstdlib>
#include <numeric>

namespace gpuraster {

using gpudump::Command;
using gpudump::DrawState;
using gpudump::Kind;

namespace {

// psx-spx "24bit RGB to 15bit RGB Dithering": offsets added to 8-bit
// channels by position in a 4x4 block.
const int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

struct Point {
    int x, y;
    int r, g, b;
    int u, v;
};

int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// One primitive's pixel pipeline: texture fetch, modulation, dither,
// semi-transparency, mask; with the clip rect of the drawing area.
struct Painter {
    Gpu& gpu;
    const Command& cmd;
    const DrawState& st;
    int32_t id;
    bool textured, raw, semi, dither;
    int clutX, clutY, pageX, pageY, depth;
    int maskX, maskY, offX, offY;  // texture window, in texels

    Painter(Gpu& g, const Command& c, int32_t owner, bool ditherable)
        : gpu(g), cmd(c), st(c.state), id(owner), textured(c.textured()), raw(c.raw()), semi(c.semiTransparent()) {
        dither = ditherable && st.dither() && g.options.dither;
        clutX = c.clutX();
        clutY = c.clutY();
        pageX = st.pageX();
        pageY = st.pageY();
        depth = st.depth();
        maskX = int(st.texWindow & 31) * 8;
        maskY = int((st.texWindow >> 5) & 31) * 8;
        offX = int((st.texWindow >> 10) & 31) * 8;
        offY = int((st.texWindow >> 15) & 31) * 8;
    }

    bool inArea(int x, int y) const { return x >= st.areaX1 && x <= st.areaX2 && y >= st.areaY1 && y <= st.areaY2; }

    // psx-spx texture window: Texcoord = (Texcoord AND NOT (Mask*8)) OR ((Offset AND Mask)*8).
    uint16_t texel(int u, int v) const {
        u &= 0xff;
        v &= 0xff;
        u = (u & ~maskX) | (offX & maskX);
        v = (v & ~maskY) | (offY & maskY);
        const int y = pageY + v;
        switch (depth) {
            case 0: {
                const uint16_t word = gpu.at(pageX + u / 4, y);
                const int index = (word >> ((u & 3) * 4)) & 0xf;
                return gpu.at(clutX + index, clutY);
            }
            case 1: {
                const uint16_t word = gpu.at(pageX + u / 2, y);
                const int index = (word >> ((u & 1) * 8)) & 0xff;
                return gpu.at(clutX + index, clutY);
            }
            default: return gpu.at(pageX + u, y);
        }
    }

    // 8-bit channel to 5 bits, dithered by position when this primitive dithers.
    int to5(int c8, int x, int y) const {
        if (dither) c8 = clamp8(c8 + kDither[y & 3][x & 3]);
        else c8 = std::min(c8, 255);
        return c8 >> 3;
    }

    static uint16_t blend(uint16_t back, uint16_t front, int mode) {
        uint16_t out = 0;
        for (int shift = 0; shift <= 10; shift += 5) {
            const int b = (back >> shift) & 31, f = (front >> shift) & 31;
            int c = 0;
            switch (mode) {
                case 0: c = (b + f) >> 1; break;
                case 1: c = std::min(31, b + f); break;
                case 2: c = std::max(0, b - f); break;
                default: c = std::min(31, b + (f >> 2)); break;
            }
            out |= uint16_t(c << shift);
        }
        return out;
    }

    // Plots pixel (x, y) (VRAM coordinates, already offset) with the
    // interpolated colour r,g,b (8-bit) and texture coordinate u,v.
    void plot(int x, int y, int r, int g, int b, int u, int v) {
        if (!inArea(x, y)) return;
        x &= 1023;
        y &= 511;
        uint16_t colour;
        bool blendThis = semi;
        uint16_t maskBit = 0;
        if (textured) {
            const uint16_t t = texel(u, v);
            if (t == 0) return;  // fully transparent
            blendThis = semi && (t & 0x8000);
            maskBit = t & 0x8000;
            if (raw) {
                colour = t & 0x7fff;
            } else {
                // Modulation: texel (as 8 bits) * colour / 128, i.e. texel5 * colour / 16.
                const int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
                colour = uint16_t(to5((tr * r) >> 4, x, y) | to5((tg * g) >> 4, x, y) << 5 | to5((tb * b) >> 4, x, y) << 10);
            }
        } else {
            colour = uint16_t(to5(r, x, y) | to5(g, x, y) << 5 | to5(b, x, y) << 10);
        }
        const size_t i = size_t(y) * kVramWidth + size_t(x);
        const uint16_t back = gpu.vram[i];
        if (st.checkMask && (back & 0x8000)) return;
        if (blendThis) colour = blend(back, colour, st.blendMode());
        if (st.setMask) maskBit = 0x8000;
        gpu.vram[i] = colour | maskBit;
        gpu.owner[i] = id;
    }
};

// Edges are dropped past 1023 horizontally or 511 vertically (psx-spx Vertex attribute).
bool edgeTooLong(const Point& a, const Point& b) { return std::abs(a.x - b.x) > 1023 || std::abs(a.y - b.y) > 511; }

// Pixel (x, y) is inside when every edge function is positive there, or
// zero on a top or left edge: polygons stop short of their lower-right
// edges (psx-spx Render Polygon Commands notes). Colours and texture
// coordinates are the barycentric blend at the pixel, rounded to nearest
// (psx-spx gives no precision; the rounding, exact halves included, is what
// pcsx-redux does, measured on hand-built dumps).
void triangle(Painter& p, Point a, Point b, Point c, bool gouraud) {
    if (edgeTooLong(a, b) || edgeTooLong(b, c) || edgeTooLong(c, a)) return;
    a.x += p.st.offsetX, a.y += p.st.offsetY;
    b.x += p.st.offsetX, b.y += p.st.offsetY;
    c.x += p.st.offsetX, c.y += p.st.offsetY;
    long long area = (long long)(b.x - a.x) * (c.y - a.y) - (long long)(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    const int x0 = std::max({std::min({a.x, b.x, c.x}), p.st.areaX1}), x1 = std::min({std::max({a.x, b.x, c.x}), p.st.areaX2});
    const int y0 = std::max({std::min({a.y, b.y, c.y}), p.st.areaY1}), y1 = std::min({std::max({a.y, b.y, c.y}), p.st.areaY2});
    // Edge p->q: E(x,y) = (q.x-p.x)(y-p.y) - (q.y-p.y)(x-p.x). With this winding the inside is positive;
    // a left edge runs upwards (dy < 0) and a top edge rightwards (dy == 0, dx > 0).
    struct Edge {
        long long dx, dy, base;  // E = base + dx*y - dy*x
        int bias;                // 0 on a top-left edge, else -1 (a zero is outside)
        long long at(int x, int y) const { return base + dx * y - dy * x; }
    };
    auto edge = [](const Point& s, const Point& e) {
        Edge out;
        out.dx = e.x - s.x;
        out.dy = e.y - s.y;
        out.base = -out.dx * s.y + out.dy * s.x;
        out.bias = (out.dy < 0 || (out.dy == 0 && out.dx > 0)) ? 0 : -1;
        return out;
    };
    const Edge bc = edge(b, c), ca = edge(c, a), ab = edge(a, b);
    // Nearest. An exact half goes up, except for a colour that rises from
    // left to right by a step not exact in binary (its slope is not k / 2^n):
    // that one goes down, as stepping a truncated slope across the span falls
    // just short of the half.
    auto lerp = [area](int va, int vb, int vc, long long w0, long long w1, long long w2, bool halfUp) {
        return int(((va * w0 + vb * w1 + vc * w2) * 2 + area - (halfUp ? 0 : 1)) / (2 * area));
    };
    auto colourHalfUp = [&](int va, int vb, int vc) {
        const long long nx = (long long)(vb - va) * (c.y - a.y) - (long long)(vc - va) * (b.y - a.y);  // area * dA/dx
        if (nx <= 0) return true;
        const long long den = area / std::gcd(nx, area);
        return (den & (den - 1)) == 0;
    };
    const bool upR = colourHalfUp(a.r, b.r, c.r), upG = colourHalfUp(a.g, b.g, c.g), upB = colourHalfUp(a.b, b.b, c.b);
    const bool pairs = gouraud && p.textured && p.gpu.options.reduxPairs;
    for (int y = y0; y <= y1; ++y) {
        int spanStart = -1;
        for (int x = x0; x <= x1; ++x) {
            const long long w0 = bc.at(x, y), w1 = ca.at(x, y), w2 = ab.at(x, y);
            if (w0 + bc.bias < 0 || w1 + ca.bias < 0 || w2 + ab.bias < 0) continue;
            if (spanStart < 0) spanStart = x;
            int r = a.r, g = a.g, bl = a.b;
            if (gouraud) {
                // reduxPairs: the colour of the even pixel of each pair from the span's start.
                const int cx = pairs ? x - ((x - spanStart) & 1) : x;
                const long long c0 = bc.at(cx, y), c1 = ca.at(cx, y), c2 = ab.at(cx, y);
                r = lerp(a.r, b.r, c.r, c0, c1, c2, upR);
                g = lerp(a.g, b.g, c.g, c0, c1, c2, upG);
                bl = lerp(a.b, b.b, c.b, c0, c1, c2, upB);
            }
            int u = 0, v = 0;
            if (p.textured) {
                u = lerp(a.u, b.u, c.u, w0, w1, w2, true);
                v = lerp(a.v, b.v, c.v, w0, w1, w2, true);
            }
            p.plot(x, y, r, g, bl, u, v);
        }
    }
}

Point point(const gpudump::Vertex& v) { return Point{v.x, v.y, v.r, v.g, v.b, v.u, v.v}; }

void polygon(Gpu& gpu, const Command& c, int32_t id) {
    // Dithered only when gouraud shaded or modulated (psx-spx dithering notes).
    const bool ditherable = c.gouraud() || (c.textured() && !c.raw());
    Painter p(gpu, c, id, ditherable);
    const auto& v = c.vertices;
    triangle(p, point(v[0]), point(v[1]), point(v[2]), c.gouraud());
    if (v.size() == 4) triangle(p, point(v[1]), point(v[2]), point(v[3]), c.gouraud());
}

void rect(Gpu& gpu, const Command& c, int32_t id) {
    Painter p(gpu, c, id, false);  // rectangles are not dithered
    const gpudump::Vertex& v = c.vertices[0];
    const int x0 = v.x + c.state.offsetX, y0 = v.y + c.state.offsetY;
    const bool flipX = c.state.texpage >> 12 & 1, flipY = c.state.texpage >> 13 & 1;
    for (int j = 0; j < c.height; ++j) {
        for (int i = 0; i < c.width; ++i) {
            const int u = flipX ? v.u - i : v.u + i, tv = flipY ? v.v - j : v.v + j;
            p.plot(x0 + i, y0 + j, v.r, v.g, v.b, u, tv);
        }
    }
}

// Both end points included (psx-spx Render Line Commands); colours
// interpolated along the longer axis; a zero-length line is one pixel in
// the first colour.
void segment(Painter& p, Point a, Point b, bool gouraud) {
    if (edgeTooLong(a, b)) return;
    a.x += p.st.offsetX, a.y += p.st.offsetY;
    b.x += p.st.offsetX, b.y += p.st.offsetY;
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int n = std::max(std::abs(dx), std::abs(dy));
    if (n == 0) {
        p.plot(a.x, a.y, a.r, a.g, a.b, 0, 0);
        return;
    }
    // Rounded to nearest, halves away from the start.
    auto step = [n](int d, int i) {
        const long long num = (long long)d * i * 2;
        return int(num >= 0 ? (num + n) / (2LL * n) : -((-num + n) / (2LL * n)));
    };
    for (int i = 0; i <= n; ++i) {
        const int x = a.x + step(dx, i), y = a.y + step(dy, i);
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud) {
            r = a.r + (b.r - a.r) * i / n;
            g = a.g + (b.g - a.g) * i / n;
            bl = a.b + (b.b - a.b) * i / n;
        }
        p.plot(x, y, r, g, bl, 0, 0);
    }
}

void line(Gpu& gpu, const Command& c, int32_t id) {
    Painter p(gpu, c, id, true);  // lines dither, mono or shaded
    for (size_t i = 0; i + 1 < c.vertices.size(); ++i) segment(p, point(c.vertices[i]), point(c.vertices[i + 1]), c.gouraud());
}

// GP0(02h): 24-bit colour to 15 bits by dropping the low bits, mask bit 0;
// not clipped, not offset, not masked; wraps at the VRAM edges.
void fill(Gpu& gpu, const Command& c, int32_t id) {
    const gpudump::Vertex& col = c.vertices[0];
    const uint16_t value = uint16_t((col.r >> 3) | (col.g >> 3) << 5 | (col.b >> 3) << 10);
    for (int j = 0; j < c.height; ++j)
        for (int i = 0; i < c.width; ++i) {
            const size_t at = size_t((c.dstY + j) & 511) * kVramWidth + size_t((c.dstX + i) & 1023);
            gpu.vram[at] = value;
            gpu.owner[at] = id;
        }
}

// Copies honour the mask settings, per halfword.
void store(Gpu& gpu, const DrawState& st, int x, int y, uint16_t value, int32_t id) {
    const size_t at = size_t(y & 511) * kVramWidth + size_t(x & 1023);
    if (st.checkMask && (gpu.vram[at] & 0x8000)) return;
    gpu.vram[at] = value | (st.setMask ? 0x8000 : 0);
    gpu.owner[at] = id;
}

void copyIn(Gpu& gpu, const Command& c, int32_t id) {
    size_t n = 0;
    const size_t total = size_t(c.width) * size_t(c.height);
    for (int j = 0; j < c.height; ++j)
        for (int i = 0; i < c.width; ++i, ++n) {
            if (n >= total) return;
            const size_t word = 3 + n / 2;
            if (word >= c.words.size()) return;
            const uint16_t value = uint16_t(n & 1 ? c.words[word] >> 16 : c.words[word]);
            store(gpu, c.state, c.dstX + i, c.dstY + j, value, id);
        }
}

void copyVram(Gpu& gpu, const Command& c, int32_t id) {
    for (int j = 0; j < c.height; ++j)
        for (int i = 0; i < c.width; ++i) store(gpu, c.state, c.dstX + i, c.dstY + j, gpu.at(c.srcX + i, c.srcY + j), id);
}

}  // namespace

void Gpu::clear() {
    std::fill(vram.begin(), vram.end(), 0);
    std::fill(owner.begin(), owner.end(), -1);
    done = 0;
}

void Gpu::execute(const Command& c, int32_t id) {
    switch (c.kind) {
        case Kind::Polygon: polygon(*this, c, id); break;
        case Kind::Rect: rect(*this, c, id); break;
        case Kind::Line: line(*this, c, id); break;
        case Kind::Fill: fill(*this, c, id); break;
        case Kind::CopyIn: copyIn(*this, c, id); break;
        case Kind::CopyVram: copyVram(*this, c, id); break;
        default: break;  // state changes travel in Command::state
    }
}

void replay(const gpudump::CommandList& list, size_t k, Gpu& gpu) {
    k = std::min(k, list.commands.size());
    if (gpu.done > k) gpu.clear();
    for (size_t i = gpu.done; i < k; ++i) gpu.execute(list.commands[i], int32_t(i));
    gpu.done = k;
}

std::vector<uint8_t> rgba(const Gpu& gpu, int x, int y, int w, int h) {
    std::vector<uint8_t> out(size_t(std::max(w, 0)) * size_t(std::max(h, 0)) * 4);
    size_t o = 0;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i, o += 4) {
            toRgb(gpu.at(x + i, y + j), out[o], out[o + 1], out[o + 2]);
            out[o + 3] = 255;
        }
    return out;
}

std::vector<std::pair<int, int>> outline(const Command& c) {
    std::vector<std::pair<int, int>> pts;
    const int ox = c.state.offsetX, oy = c.state.offsetY;
    switch (c.kind) {
        case Kind::Polygon: {
            const auto& v = c.vertices;
            static const int quadOrder[4] = {0, 1, 3, 2};
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& p = v[v.size() == 4 ? quadOrder[i] : i];
                pts.push_back({p.x + ox, p.y + oy});
            }
            break;
        }
        case Kind::Line:
            for (const auto& p : c.vertices) pts.push_back({p.x + ox, p.y + oy});
            break;
        case Kind::Rect: {
            const int x = c.vertices[0].x + ox, y = c.vertices[0].y + oy;
            pts = {{x, y}, {x + c.width, y}, {x + c.width, y + c.height}, {x, y + c.height}};
            break;
        }
        case Kind::Fill:
        case Kind::CopyIn:
        case Kind::CopyVram:
            pts = {{c.dstX, c.dstY}, {c.dstX + c.width, c.dstY}, {c.dstX + c.width, c.dstY + c.height}, {c.dstX, c.dstY + c.height}};
            break;
        default: break;
    }
    return pts;
}

}  // namespace gpuraster
