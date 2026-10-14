#include "editor/texture_preview_view.hh"

#include <imgui_internal.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <SDL3/SDL_video.h>

#include "editor/document.hh"
#include "gl.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;

namespace {

const char* bppLabel(splash::BitDepth d) {
    switch (d) {
        case splash::BitDepth::Bpp4: return "4 bpp";
        case splash::BitDepth::Bpp8: return "8 bpp";
        case splash::BitDepth::Bpp16: return "16 bpp";
    }
    return "";
}

// Uploads an RGBA8 top-down image into `tex` (created on first use), nearest
// filtering so a magnified PS1 texel stays a hard square.
void uploadPreview(unsigned& tex, int w, int h, const std::vector<uint8_t>& rgba) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace

// Only while a GL context is current: at exit the app state outlives the
// context, and the textures go with it.
TexturePreviewView::~TexturePreviewView() {
    if ((srcTex || ps1Tex) && SDL_GL_GetCurrentContext()) {
        if (srcTex) glDeleteTextures(1, &srcTex);
        if (ps1Tex) glDeleteTextures(1, &ps1Tex);
    }
}

float texturePreview(TexturePreviewView& tp, ImDrawList* dl, float x0, float x1, float y, editor::Document& doc,
                     const std::string& texPath, splash::BitDepth depth, bool cutout) {
    Fonts& f = fonts();
    const float top = y;
    editor::TexturePreviewKey key{doc.resolve(texPath), depth, cutout};
    tp.previewer.request(key);
    std::shared_ptr<const editor::TexturePreview> p = tp.previewer.latest();
    tp.shown.clear();

    // Upload the newest result once; keep the old one on the GPU meanwhile.
    if (p && p->error.empty() && tp.previewer.generation() != tp.shownGen) {
        uploadPreview(tp.srcTex, p->width, p->height, p->source);
        uploadPreview(tp.ps1Tex, p->width, p->height, p->ps1);
        tp.shownGen = tp.previewer.generation();
    }
    const bool haveShown = p && p->error.empty() && p->key == key && tp.srcTex && tp.ps1Tex;

    const float gap = space::sm;
    const float stripY = y + 18;
    const float maxH = 150;
    // Fit to the panel width at a whole-pixel zoom, same for both panes.
    float paneW = tp.compare ? (x1 - x0) : (x1 - x0 - gap) / 2;
    float imgW = p && p->width > 0 ? float(p->width) : 64;
    float imgH = p && p->height > 0 ? float(p->height) : 64;
    float zoom = std::min(paneW / imgW, maxH / imgH);
    if (zoom < 1) zoom = std::max(zoom, 0.01f);
    float drawW = imgW * zoom, drawH = imgH * zoom;

    if (p && !p->error.empty()) {
        text(dl, ImVec2(x0, stripY), f.regular, type::caption, color::warn, p->error.c_str());
        return stripY + 18 - top;
    }
    if (!haveShown) {
        text(dl, ImVec2(x0, stripY), f.regular, type::caption, color::textFaint,
             tp.previewer.busy() ? "Converting preview..." : "Preview...");
        return stripY + 18 - top;
    }

    // Ids are this preview's own, should two inspectors show one at once.
    ImGui::PushID(&tp);

    // Labels.
    text(dl, ImVec2(x0, y), f.regular, type::caption, color::textFaint, "Source");
    if (!tp.compare) text(dl, ImVec2(x0 + paneW + gap, y), f.regular, type::caption, color::accent, "PS1");

    auto pane = [&](float px, unsigned tex, const char* badge, ImU32 badgeCol) {
        ImRect box(ImVec2(px, stripY), ImVec2(px + drawW, stripY + drawH));
        dl->AddRectFilled(box.Min - ImVec2(1, 1), box.Max + ImVec2(1, 1), color::field, radius::field);
        dl->AddImage((ImTextureID)(intptr_t)tex, box.Min, box.Max);
        dl->AddRect(box.Min, box.Max, color::border, 0);
        if (badge) text(dl, ImVec2(box.Min.x + 3, box.Min.y + 2), f.semibold, type::caption, badgeCol, badge);
        return box;
    };

    float panesBottom;
    if (tp.compare) {
        // One pane: PS1 by default, Source while the mouse is held on it.
        ImRect box(ImVec2(x0, stripY), ImVec2(x0 + drawW, stripY + drawH));
        Hit h = interact("texcompare", box);
        bool showSrc = h.held;
        pane(x0, showSrc ? tp.srcTex : tp.ps1Tex, showSrc ? "SOURCE" : "PS1", showSrc ? color::textFaint : color::accent);
        tooltip("Hold to show the source; release for the PS1 image");
        panesBottom = box.Max.y;
    } else {
        ImRect a = pane(x0, tp.srcTex, nullptr, 0);
        ImRect b = pane(x0 + paneW + gap, tp.ps1Tex, nullptr, 0);
        // Holding the mouse on the PS1 pane swaps in the source, in place.
        Hit h = interact("texflick", b);
        if (h.held) pane(x0 + paneW + gap, tp.srcTex, "SOURCE", color::textFaint);
        tooltip("Hold to show the source here");
        panesBottom = std::max(a.Max.y, b.Max.y);
    }
    y = panesBottom + space::sm;

    // Numbers: bit depth, palette size and used, colours, mean OKLab error.
    char line[160];
    if (depth == splash::BitDepth::Bpp16)
        std::snprintf(line, sizeof line, "%s  \xc2\xb7  %d colours  \xc2\xb7  \xce\x94" "E %.2f", bppLabel(depth), p->ps1Colors,
                      p->deltaE);
    else
        std::snprintf(line, sizeof line, "%s  \xc2\xb7  palette %d (%d used)  \xc2\xb7  %d colours  \xc2\xb7  \xce\x94" "E %.2f",
                      bppLabel(depth), p->paletteSize, p->paletteUsed, p->ps1Colors, p->deltaE);
    text(dl, ImVec2(x0, y), f.regular, type::caption, color::textDim, line);
    y += 16;
    char src[96];
    std::snprintf(src, sizeof src, "source %d colours  \xc2\xb7  %.1f KB VRAM", p->sourceColors, p->vramBytes / 1024.0);
    text(dl, ImVec2(x0, y), f.regular, type::caption, color::textFaint, src);

    // Compare toggle, right-aligned on the shorter source row (on the numbers
    // row it covered the deltaE figure at the inspector's default width).
    const char* cmp = tp.compare ? "Split" : "Compare";
    float cw = measure(f.medium, type::caption, cmp).x + 16;
    ImRect cb(ImVec2(x1 - cw, y - 1), ImVec2(x1, y + 17));
    Hit hc = interact("texcomparebtn", cb);
    dl->AddRectFilled(cb.Min, cb.Max, hc.hovered ? color::hover : color::raised, radius::button);
    textCentered(dl, cb, f.medium, type::caption, color::textDim, cmp);
    if (hc.clicked) tp.compare = !tp.compare;
    tooltip(tp.compare ? "Show Source and PS1 side by side" : "Flick between Source and PS1 in one pane");

    if (tp.previewer.busy()) text(dl, ImVec2(x0, y + 16), f.regular, type::caption, color::textFaint, "re-quantizing...");
    y += 16;
    ImGui::PopID();

    char shown[160];
    std::snprintf(shown, sizeof shown, "%dx%d %s cutout %d ps1 %d dE %.2f source %.0f,%.0f ps1 %.0f size %.0fx%.0f", p->width,
                  p->height, bppLabel(depth), cutout ? 1 : 0, p->ps1Colors, p->deltaE, x0, stripY, tp.compare ? x0 : x0 + paneW + gap,
                  drawW, drawH);
    tp.shown = shown;
    return y - top;
}

}  // namespace ui
