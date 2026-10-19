#include "ui/texture_image.h"

#include <imgui_impl_opengl3.h>

#include "gl.h"

namespace ui {
namespace {

// Unbind the backend's sampler object so the texture's own filter applies.
// Without a bound sampler (GL < 3.3) the backend leaves texture parameters
// alone unless asked, so there is nothing to undo.
void useTextureFilter(const ImDrawList*, const ImDrawCmd*) {
    ImGui_ImplOpenGL3_RenderState* rs = ImGui_ImplOpenGL3_GetRenderState();
    if (!rs || !rs->UseBindSampler) return;
    gl::glBindSampler(0, 0);
    rs->CurrentSampler = 0;
    rs->UseTexParameterFilter = false;
}

// Put the backend's default linear sampler back. Only on the bound-sampler
// path: elsewhere SetSamplerLinear would start rewriting texture parameters.
void restoreSampler(const ImDrawList* dl, const ImDrawCmd* cmd) {
    ImGui_ImplOpenGL3_RenderState* rs = ImGui_ImplOpenGL3_GetRenderState();
    if (!rs || !rs->UseBindSampler) return;
    if (ImDrawCallback linear = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear) linear(dl, cmd);
}

}  // namespace

void addTextureImage(ImDrawList* dl, ImTextureID tex, const ImVec2& pMin, const ImVec2& pMax,
                     const ImVec2& uvMin, const ImVec2& uvMax, ImU32 col) {
    dl->AddCallback(useTextureFilter, nullptr);
    dl->AddImage(tex, pMin, pMax, uvMin, uvMax, col);
    dl->AddCallback(restoreSampler, nullptr);
}

void addTextureImageRounded(ImDrawList* dl, ImTextureID tex, const ImVec2& pMin, const ImVec2& pMax,
                            const ImVec2& uvMin, const ImVec2& uvMax, ImU32 col, float rounding,
                            ImDrawFlags flags) {
    dl->AddCallback(useTextureFilter, nullptr);
    dl->AddImageRounded(tex, pMin, pMax, uvMin, uvMax, col, rounding, flags);
    dl->AddCallback(restoreSampler, nullptr);
}

void addTextureImages(ImDrawList* dl, const std::function<void()>& draw) {
    dl->AddCallback(useTextureFilter, nullptr);
    draw();
    dl->AddCallback(restoreSampler, nullptr);
}

}  // namespace ui
