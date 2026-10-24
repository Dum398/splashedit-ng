#include "editor/tv_preview.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace editor::tv {

Rect displayRect(Rect p, int fw, int fh, bool tvAspect) {
    if (fw <= 0 || fh <= 0 || p.w() <= 0 || p.h() <= 0) return {p.x0, p.y0, p.x0, p.y0};
    const float aspect = tvAspect ? 4.0f / 3.0f : float(fw) / float(fh);
    const float w = std::floor(std::min(p.w(), p.h() * aspect) + 1e-3f);
    const float h = std::floor(w / aspect + 1e-3f);
    const float x0 = std::floor(p.x0 + (p.w() - w) * 0.5f), y0 = std::floor(p.y0 + (p.h() - h) * 0.5f);
    return {x0, y0, x0 + w, y0 + h};
}

Rect safeRect(Rect f, float fraction) {
    const float ix = std::round(f.w() * (1 - fraction) * 0.5f), iy = std::round(f.h() * (1 - fraction) * 0.5f);
    return {f.x0 + ix, f.y0 + iy, f.x1 - ix, f.y1 - iy};
}

namespace {

// A box blur of radius r along one row, edges clamped, in place.
void boxRow(float* v, int w, int r, std::vector<float>& tmp) {
    if (r <= 0 || w <= 1) return;
    tmp.assign(v, v + w);
    auto at = [&](int i) { return tmp[size_t(std::clamp(i, 0, w - 1))]; };
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += at(i);
    const float n = float(2 * r + 1);
    for (int x = 0; x < w; ++x) {
        v[x] = sum / n;
        sum += at(x + r + 1) - at(x - r);
    }
}

uint8_t byte(float v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); }

}  // namespace

std::vector<uint8_t> applyLook(const uint8_t* rgba, int w, int h, Look look, bool scanlines, int* outH) {
    const size_t px = size_t(std::max(w, 0)) * size_t(std::max(h, 0));
    if (look == Look::RGB || px == 0) {
        if (outH) *outH = h;
        return std::vector<uint8_t>(rgba, rgba + px * 4);
    }
    // Widths for a 320-wide line, scaled to this one.
    const float k = std::max(1.0f, float(w) / 320.0f);
    const bool composite = look == Look::Composite;
    const int chromaR = int(std::lround((composite ? 4 : 1) * k));
    const bool scan = scanlines && h <= 256;
    const int oh = scan ? h * 2 : h;
    if (outH) *outH = oh;
    std::vector<uint8_t> out(size_t(w) * size_t(oh) * 4);
    std::vector<float> Y(static_cast<size_t>(w)), I(static_cast<size_t>(w)), Q(static_cast<size_t>(w)), tmp;
    for (int y = 0; y < h; ++y) {
        const uint8_t* in = rgba + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            const float r = in[x * 4], g = in[x * 4 + 1], b = in[x * 4 + 2];
            Y[size_t(x)] = 0.299f * r + 0.587f * g + 0.114f * b;
            I[size_t(x)] = 0.596f * r - 0.274f * g - 0.322f * b;
            Q[size_t(x)] = 0.211f * r - 0.523f * g + 0.312f * b;
        }
        // Two box passes make a triangle, softer than a box's hard edges.
        for (int pass = 0; pass < 2; ++pass) {
            boxRow(I.data(), w, chromaR, tmp);
            boxRow(Q.data(), w, chromaR, tmp);
        }
        if (composite) {
            // Luma loses a little to the chroma notch: [1 2 1] / 4 per lumaR.
            const int lumaR = int(std::lround(k));
            for (int pass = 0; pass < lumaR; ++pass) {
                tmp.assign(Y.begin(), Y.end());
                for (int x = 0; x < w; ++x)
                    Y[size_t(x)] = (tmp[size_t(std::max(x - 1, 0))] + 2 * tmp[size_t(x)] + tmp[size_t(std::min(x + 1, w - 1))]) * 0.25f;
            }
        }
        uint8_t* o = out.data() + size_t(scan ? y * 2 : y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            const float yy = Y[size_t(x)], ii = I[size_t(x)], qq = Q[size_t(x)];
            o[x * 4] = byte(yy + 0.956f * ii + 0.621f * qq);
            o[x * 4 + 1] = byte(yy - 0.272f * ii - 0.647f * qq);
            o[x * 4 + 2] = byte(yy - 1.106f * ii + 1.703f * qq);
            o[x * 4 + 3] = in[x * 4 + 3];
        }
        if (scan) {
            // The gap between a 240-line mode's lines, mildly darker.
            uint8_t* d = o + size_t(w) * 4;
            for (int x = 0; x < w * 4; ++x) d[x] = (x & 3) == 3 ? o[x] : uint8_t(o[x] * 3 / 4);
        }
    }
    return out;
}

const char* lookName(Look look) {
    switch (look) {
        case Look::SVideo: return "svideo";
        case Look::Composite: return "composite";
        default: return "rgb";
    }
}

Look lookFromName(const char* name) {
    if (!std::strcmp(name, "svideo")) return Look::SVideo;
    if (!std::strcmp(name, "composite")) return Look::Composite;
    return Look::RGB;
}

}  // namespace editor::tv
