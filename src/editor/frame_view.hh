// The Frame tab of the bottom dock: one GPU frame captured from Play (or
// opened from a .gpd file), replayed in the frame debugger's rasterizer
// (core/gpu_raster.hh). It shows the frame's drawing area after any number of
// its primitives, steps through them in draw order, and tells which
// primitive last wrote a clicked pixel: its type, vertices, colours, texture
// coordinates, blend mode, texture page, depth, CLUT, texture window and
// drawing area, outlined on the image, its page and CLUT marked on a VRAM
// inset.
//
// Scripted runs print `frame: loaded <file> ...`, `frame: prims N`,
// `frame: step s/N`, `frame: selected #i <type> ...` and, for a click on
// the image, `frame: pick x,y -> #i <type>` (x,y in VRAM; `-> none` for a
// pixel nothing drew, `-> setup` for one the capture's VRAM restore wrote).
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "gpu_raster.hh"
#include "gpudump.hh"

namespace ui {

class FramePanel {
public:
    FramePanel();
    ~FramePanel();
    FramePanel(const FramePanel&) = delete;
    FramePanel& operator=(const FramePanel&) = delete;

    bool open = false;
    // The bottom dock's tab strip, drawn at the left of the header (`tabsWidth` wide).
    std::function<void(ImDrawList*, ImRect)> tabs;
    float tabsWidth = 0;
    // Counts load() calls, so the dock can raise the tab each time.
    unsigned shows = 0;
    // What Open, Save or a failed load have to say, for the status bar (the caller clears it).
    std::string notice;
    bool noticeBad = false;

    // Opens a capture: shows the whole frame, nothing selected. False, with `error` set, when it cannot.
    bool load(const std::filesystem::path& file, std::string* error);
    bool loaded() const { return m_loaded; }
    // Takes the bottom of `area` (with a drag bar to resize) when open; returns what is left above.
    ImRect dock(ImRect area);
    // Draws in the rect dock() took.
    void draw(ImDrawList* dl);

private:
    void header(ImDrawList* dl, ImRect r);
    void imageColumn(ImDrawList* dl, ImRect r);
    void transport(ImDrawList* dl, ImRect r);
    void details(ImDrawList* dl, ImRect r);
    void vramInset(ImDrawList* dl, ImRect r, const gpudump::Command* c);
    void primitiveList(ImDrawList* dl, ImRect r);
    void setStep(int step);
    void select(int prim, bool scrollTo);
    void pick(int x, int y);
    void sync();  // replays to the step and uploads the textures when stale
    void takePicked();
    size_t replayTarget() const;
    int primitiveOf(int command) const;

    bool m_loaded = false;
    std::filesystem::path m_file;
    gpudump::Dump m_dump;
    gpudump::CommandList m_list;
    gpuraster::Gpu m_gpu;
    int m_step = 0;        // primitives drawn: 0 .. m_list.drawn.size()
    int m_selected = -1;   // primitive index, -1 none
    int m_pickX = -1, m_pickY = -1;
    int m_areaX = 0, m_areaY = 0, m_areaW = 0, m_areaH = 0;  // the frame image, in VRAM
    int m_zoom = 0;        // 0 fit, else the integer scale
    ImVec2 m_pan{0, 0};
    float m_listScroll = 0;
    bool m_scrollToSelection = false;
    float m_height = 430;
    ImRect m_rect;
    unsigned m_tex = 0, m_vramTex = 0;
    size_t m_uploaded = size_t(-1);  // replay target the textures show
    // File dialogs answer on their own thread.
    std::mutex m_pickMutex;
    std::string m_openPicked, m_savePicked;
    static void onOpenPicked(void* self, const char* const* files, int filter);
    static void onSavePicked(void* self, const char* const* files, int filter);
};

}  // namespace ui
