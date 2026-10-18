#pragma once

#include <imgui.h>

// Draw a GL texture with the filter set on the texture itself.
//
// The OpenGL3 backend binds a linear sampler object to unit 0 on GL 3.3+,
// which overrides every texture's own GL_TEXTURE_MIN/MAG_FILTER, so PS1 art
// uploaded GL_NEAREST was still drawn smeared. These helpers unbind that
// sampler around the image and put the backend's linear sampler back after.
namespace ui {

void addTextureImage(ImDrawList* dl, ImTextureID tex, const ImVec2& pMin, const ImVec2& pMax,
                     const ImVec2& uvMin = ImVec2(0, 0), const ImVec2& uvMax = ImVec2(1, 1),
                     ImU32 col = IM_COL32_WHITE);

void addTextureImageRounded(ImDrawList* dl, ImTextureID tex, const ImVec2& pMin, const ImVec2& pMax,
                            const ImVec2& uvMin, const ImVec2& uvMax, ImU32 col, float rounding,
                            ImDrawFlags flags = 0);

}  // namespace ui
