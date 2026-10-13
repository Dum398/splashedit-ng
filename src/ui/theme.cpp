#include "ui/theme.h"

#include <misc/freetype/imgui_freetype.h>

#include <string>

namespace theme {

Fonts& fonts() {
    static Fonts f;
    return f;
}

static ImFont* addFace(const std::string& dir, const char* file) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    // Full hinting at whole-pixel sizes: light hinting only snaps vertically and
    // reads as soft next to native Windows text.
    cfg.FontLoaderFlags = 0;
    // Inter carries its own private-use glyphs, which would shadow the icons.
    static const ImWchar privateUse[] = {0xe000, 0xf8ff, 0};
    cfg.GlyphExcludeRanges = privateUse;
    ImFont* font = io.Fonts->AddFontFromFileTTF((dir + "/fonts/" + file).c_str(), type::body, &cfg);

    // Lucide icons merged into every face, so text and icons share a line.
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.FontLoaderFlags = 0;
    icons.GlyphOffset = ImVec2(0, 2);
    static const ImWchar range[] = {0xe000, 0xe6ff, 0};
    icons.GlyphRanges = range;
    io.Fonts->AddFontFromFileTTF((dir + "/fonts/lucide.ttf").c_str(), type::body, &icons);
    return font;
}

void load(const char* assetDir) {
    std::string dir = assetDir;
    Fonts& f = fonts();
    f.regular = addFace(dir, "Inter-Regular.ttf");
    f.medium = addFace(dir, "Inter-Medium.ttf");
    f.semibold = addFace(dir, "Inter-SemiBold.ttf");
    // ImGui's own scalable ProggyForever: monospaced, no file to ship.
    ImFontConfig mono;
    mono.SizePixels = type::code;
    f.mono = ImGui::GetIO().Fonts->AddFontDefaultVector(&mono);

    ImGuiStyle& s = ImGui::GetStyle();
    s.FontSizeBase = type::body;
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.WindowRounding = 0;
    s.ItemSpacing = ImVec2(space::sm, space::xs);
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = radius::pill;
    s.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(color::base);
    s.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(color::text);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(color::active);
}

}  // namespace theme
