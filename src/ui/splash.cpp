#include "ui/splash.h"

#include <SDL3/SDL.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gl.h"
#include "ui/bootlogo_data.hh"

using namespace psxsplash::bootlogo;

namespace splash {
namespace {

// Timeline in milliseconds, as psxsplash's src/bootlogo.cpp plays it.
constexpr int kLetterDelay = 130;
constexpr int kDrop = 730;
constexpr int kPsxStart = 1130;
constexpr int kPsxDrop = 530;
constexpr int kTip = 200;
constexpr int kEnd = kJingleMs > 2700 ? kJingleMs : 2700;
constexpr double kFade = 350;     // the editor fades in under the last kFade ms
constexpr double kSkipFade = 200;  // and this fast after a click or key

constexpr int kPieceCount = 2 * kLetterCount + 1;
const ImU32 kBackground = IM_COL32(0x16, 0x12, 0x2b, 255);

GLuint g_tex[kPieceCount] = {};
bool g_loaded = false;
SDL_AudioStream* g_stream = nullptr;
bool g_started = false;
double g_t0 = 0;  // clock at the first frame
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

// One piece of the sheet through its CLUT, as RGBA. CLUT entry 0x0000 is
// transparent on the PS1 and here.
GLuint uploadPiece(const Piece& p, int clut) {
    std::vector<uint32_t> px(size_t(p.w) * p.h);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
            const int u = p.u + x, v = p.v + y;
            const int index = (kPage[v * 64 + u / 4] >> ((u & 3) * 4)) & 15;
            const uint16_t c = kCluts[clut][index];
            auto expand = [](int five) { return uint32_t((five << 3) | (five >> 2)); };
            px[size_t(y) * p.w + x] = c == 0 ? 0
                                             : expand(c & 31) | expand((c >> 5) & 31) << 8 | expand((c >> 10) & 31) << 16 | 0xff000000u;
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p.w, p.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return t;
}

ImTextureID id(int piece) { return (ImTextureID)(intptr_t)g_tex[piece]; }

}  // namespace

bool load(const char* assetDir, bool sound) {
    for (int i = 0; i < kLetterCount; ++i) {
        g_tex[2 * i] = uploadPiece(kBack[i], 2 * i);
        g_tex[2 * i + 1] = uploadPiece(kFront[i], 2 * i + 1);
    }
    g_tex[kPieceCount - 1] = uploadPiece(kPsx, kPieceCount - 1);
    g_loaded = true;
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
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_stream) SDL_PutAudioStreamData(g_stream, data, int(len));
    else std::fprintf(stderr, "splash: no audio device: %s\n", SDL_GetError());
    SDL_free(data);
    return true;
}

bool covering() { return g_loaded && !g_done && g_fadeFrom < 0; }

bool draw(ImVec2 display, double clockMs) {
    if (!g_loaded || g_done) return false;
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
    if (g_fadeFrom < 0 && (skip || ms >= kEnd - kFade)) {
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

    // The PS1's 320x240 screen, scaled by a whole number where it fits so the
    // pixels stay square and even.
    float s = std::min(display.x / 320.f, display.y / 240.f);
    if (s >= 1) s = std::floor(s);
    const ImVec2 o(std::floor((display.x - 320 * s) / 2), std::floor((display.y - 240 * s) / 2));
    const ImU32 tint = fade(IM_COL32_WHITE);
    auto piece = [&](const Piece& p, int tex, double dy) {
        const ImVec2 a(o.x + p.x * s, o.y + float(std::round(p.y + dy)) * s);
        dl->AddImage(id(tex), a, ImVec2(a.x + p.w * s, a.y + p.h * s), ImVec2(0, 0), ImVec2(1, 1), tint);
    };
    double dy[kLetterCount];
    for (int i = 0; i < kLetterCount; ++i) dy[i] = -bounce(ms - i * kLetterDelay, kDrop, 220);
    for (int i = 0; i < kLetterCount; ++i) piece(kBack[i], 2 * i, dy[i]);
    for (int i = 0; i < kLetterCount; ++i) piece(kFront[i], 2 * i + 1, dy[i]);

    const double t = ms - kPsxStart;
    if (t > 0) {
        const double lift = bounce(t, kPsxDrop, 160);
        const double tip = std::clamp((t - kPsxDrop) / kTip, 0.0, 1.0);
        const double turn = kPsxAngle * tip * tip * (3 - 2 * tip) / 4096.0;  // eased, in turns
        const double sn = std::sin(turn * 2 * M_PI), cs = std::cos(turn * 2 * M_PI);
        const double cx = kPsxCentreX, cy = kPsxCentreY - lift;
        auto rot = [&](double x, double y) {
            const double rx = x - kPsxCentreX, ry = y - kPsxCentreY;
            return ImVec2(o.x + float(cx + rx * cs - ry * sn) * s, o.y + float(cy + rx * sn + ry * cs) * s);
        };
        const Piece& p = kPsx;
        dl->AddImageQuad(id(kPieceCount - 1), rot(p.x, p.y), rot(p.x + p.w, p.y), rot(p.x + p.w, p.y + p.h),
                         rot(p.x, p.y + p.h), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
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
    if (g_loaded) {
        glDeleteTextures(kPieceCount, g_tex);
        g_loaded = false;
    }
}

}  // namespace splash
