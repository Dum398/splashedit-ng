// The inspector's Source vs PS1 texture preview, shared by the mesh inspector
// and the UI canvas inspector's Image element. The conversion itself is
// texture_preview.hh (off the UI thread); this draws it and owns the two GL
// textures it is shown with.
#pragma once

#include <imgui.h>

#include <string>

#include "editor/texture_preview.hh"
#include "scene.hh"

namespace editor {
class Document;
}

namespace ui {

// One preview's state. Each inspector that shows one owns one, so two on
// screen never share a conversion or a Compare toggle.
struct TexturePreviewView {
    TexturePreviewView() = default;
    ~TexturePreviewView();
    TexturePreviewView(const TexturePreviewView&) = delete;
    TexturePreviewView& operator=(const TexturePreviewView&) = delete;

    editor::TexturePreviewer previewer;
    unsigned srcTex = 0, ps1Tex = 0;  // GL textures for the two panes
    unsigned shownGen = 0;            // previewer generation on the GPU
    bool compare = false;             // one big pane to flick instead of two
    // What the last frame drew, for scripted checks: empty until the panes
    // show, else "<w>x<h> <bpp> cutout <0|1> ps1 <colours> dE <mean> source
    // <x>,<y> ps1 <x> size <w>x<h>" (the panes' screen rectangles).
    std::string shown;
};

// Source next to what the console will show, at the same zoom, from `y` down
// between x0 and x1. A click on the strip (or the Compare button) flicks the
// whole strip to one pane; holding the mouse there shows the source, to A/B
// the dithering. Returns the height used.
float texturePreview(TexturePreviewView& tp, ImDrawList* dl, float x0, float x1, float y, editor::Document& doc,
                     const std::string& texPath, splash::BitDepth depth, bool cutout);

}  // namespace ui
