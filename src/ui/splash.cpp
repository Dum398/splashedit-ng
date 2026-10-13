#include "ui/splash.h"

#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "gl.h"
#include "ui/bootlogo_hires.hh"

using namespace gl;
using namespace psxsplash::bootlogo::hires;

namespace splash {
namespace {

// Timeline in milliseconds, as psxsplash's src/bootlogo.cpp plays it.
constexpr int kLetterDelay = 130;
constexpr int kDrop = 730;
constexpr int kPsxStart = 1130;
constexpr int kPsxDrop = 530;
constexpr int kTip = 200;
constexpr double kFade = 350;      // the editor fades in under the last kFade ms
constexpr double kSkipFade = 200;  // and this fast after a click or key

constexpr int kPieceCount = 2 * kLetterCount + 1;
const ImU32 kBackground = IM_COL32(0x16, 0x12, 0x2b, 255);

GLuint g_atlas = 0;
SDL_AudioStream* g_stream = nullptr;
double g_end = 4200;  // the jingle's length when it loads; never under 2700
bool g_started = false;
double g_t0 = 0;                               // clock at the first frame
double g_fadeFrom = -1, g_fadeLength = kFade;  // ms; -1 until the fade starts
bool g_done = false;

// Pixels above the resting position, t ms into a drop of length d from
// height h: a fall, then two parabolic bounces (bootlogo.cpp bounce()).
double bounce(double t, double d, double h) {
    if (t <= 0) return h;
    if (t >= d) return 0;
    const double p1 = d * 0.45, p2 = d * 0.75;
    if (t < p1) {
        const double u = t / p1;
        return h - h * u * u;
    }
    const double a = t < p2 ? p1 : p2, b = t < p2 ? p2 : d, top = h * (t < p2 ? 0.18 : 0.05);
    const double u = (t - a) / (b - a);
    return top * 4 * u * (1 - u);
}

}  // namespace

bool load(const char* assetDir, bool sound) {
    const std::string png = std::string(assetDir) + "/brand/bootlogo-hires.png";
    int w, h, n;
    unsigned char* px = stbi_load(png.c_str(), &w, &h, &n, 4);
    if (!px) {
        std::fprintf(stderr, "splash: %s: %s\n", png.c_str(), stbi_failure_reason());
        g_done = true;
        return false;
    }
    if (w != kAtlasWidth || h != kAtlasHeight) std::fprintf(stderr, "splash: %s is %dx%d, expected %dx%d\n", png.c_str(), w, h, kAtlasWidth, kAtlasHeight);
    glGenTextures(1, &g_atlas);
    glBindTexture(GL_TEXTURE_2D, g_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(px);
    if (!sound) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "splash: no audio: %s\n", SDL_GetError());
        return true;
    }
    const std::string file = std::string(assetDir) + "/brand/boot-jingle.wav";
    SDL_AudioSpec spec;
    Uint8* data = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(file.c_str(), &spec, &data, &len)) {
        std::fprintf(stderr, "splash: %s: %s\n", file.c_str(), SDL_GetError());
        return true;
    }
    const int frameBytes = SDL_AUDIO_FRAMESIZE(spec);
    if (frameBytes > 0 && spec.freq > 0) g_end = std::max(2700.0, 1000.0 * len / frameBytes / spec.freq);
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_stream) SDL_PutAudioStreamData(g_stream, data, int(len));
    else std::fprintf(stderr, "splash: no audio device: %s\n", SDL_GetError());
    SDL_free(data);
    return true;
}

bool covering() { return g_atlas && !g_done && g_fadeFrom < 0; }

bool draw(ImVec2 display, double clockMs) {
    if (!g_atlas || g_done) return false;
    if (!g_started) {
        g_started = true;
        g_t0 = clockMs;
        if (g_stream) SDL_ResumeAudioStreamDevice(g_stream);
    }
    const double ms = clockMs - g_t0;

    // Any click or key skips to a short fade.
    bool skip = false;
    for (int b = 0; b < ImGuiMouseButton_COUNT; ++b) skip |= ImGui::IsMouseClicked(b);
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) skip |= ImGui::IsKeyPressed(ImGuiKey(k), false);
    if (g_fadeFrom < 0 && (skip || ms >= g_end - kFade)) {
        g_fadeFrom = ms;
        g_fadeLength = skip ? kSkipFade : kFade;
        if (skip && g_stream) SDL_ClearAudioStream(g_stream);
    }
    const double alpha = g_fadeFrom < 0 ? 1.0 : 1.0 - (ms - g_fadeFrom) / g_fadeLength;
    if (alpha <= 0) {
        shutdown();
        return false;
    }
    auto fade = [&](ImU32 c) {
        const int a = int(((c >> IM_COL32_A_SHIFT) & 0xff) * alpha);
        return (c & ~IM_COL32_A_MASK) | ImU32(a) << IM_COL32_A_SHIFT;
    };

    // A window over everything, so the editor underneath gets no input.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), display, fade(kBackground));

    // The layout is the PS1's 320x240 screen, fitted to the window.
    const float s = std::min(display.x / 320.f, display.y / 240.f);
    const ImVec2 o((display.x - 320 * s) / 2, (display.y - 240 * s) / 2);
    const ImU32 tint = fade(IM_COL32_WHITE);
    auto uv = [](float u, float v) { return ImVec2(u / kAtlasWidth, v / kAtlasHeight); };
    const ImTextureID tex = (ImTextureID)(intptr_t)g_atlas;
    auto piece = [&](const Piece& p, double dy) {
        const ImVec2 a(o.x + p.x * s, o.y + float(p.y + dy) * s);
        dl->AddImage(tex, a, ImVec2(a.x + p.w * s, a.y + p.h * s), uv(p.u, p.v), uv(p.u + p.pw, p.v + p.ph), tint);
    };
    double dy[kLetterCount];
    for (int i = 0; i < kLetterCount; ++i) dy[i] = -bounce(ms - i * kLetterDelay, kDrop, 220);
    for (int i = 0; i < kLetterCount; ++i) piece(kPieces[2 * i], dy[i]);
    for (int i = 0; i < kLetterCount; ++i) piece(kPieces[2 * i + 1], dy[i]);

    const double t = ms - kPsxStart;
    if (t > 0) {
        const double lift = bounce(t, kPsxDrop, 160);
        const double tip = std::clamp((t - kPsxDrop) / kTip, 0.0, 1.0);
        const double rad = kPsxTiltDegrees * tip * tip * (3 - 2 * tip) * (3.141592653589793 / 180);  // eased
        const double sn = std::sin(rad), cs = std::cos(rad);
        const double cx = kPsxCentreX, cy = kPsxCentreY - lift;
        auto rot = [&](double x, double y) {
            const double rx = x - kPsxCentreX, ry = y - kPsxCentreY;
            return ImVec2(o.x + float(cx + rx * cs - ry * sn) * s, o.y + float(cy + rx * sn + ry * cs) * s);
        };
        const Piece& p = kPieces[kPieceCount - 1];
        dl->AddImageQuad(tex, rot(p.x, p.y), rot(p.x + p.w, p.y), rot(p.x + p.w, p.y + p.h), rot(p.x, p.y + p.h),
                         uv(p.u, p.v), uv(p.u + p.pw, p.v), uv(p.u + p.pw, p.v + p.ph), uv(p.u, p.v + p.ph), tint);
    }
    ImGui::End();
    return true;
}

void shutdown() {
    g_done = true;
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
    if (g_atlas) {
        glDeleteTextures(1, &g_atlas);
        g_atlas = 0;
    }
}

}  // namespace splash
