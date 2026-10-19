#include "editor/frame_view.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "gl.h"
#include "ui/file_dialog.h"
#include "ui/icons.h"
#include "ui/texture_image.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
using gpudump::Command;
using gpudump::Kind;

namespace {

constexpr float kHeaderH = 46, kPad = 10, kListW = 280, kDetailsW = 320, kRowH = 18, kTransportH = 34, kToolbarH = 30;

std::string utf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Scripted runs print `line`; with a `tag`, only when it differs from the last line printed under it.
void say(const char* tag, const std::string& line) {
    if (!namedRectsOn()) return;
    if (tag) {
        static std::map<std::string, std::string> printed;
        std::string& last = printed[tag];
        if (last == line) return;
        last = line;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void upload(unsigned& tex, int w, int h, const std::vector<uint8_t>& rgba) {
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

const char* blendName(int mode) {
    static const char* names[4] = {"B/2 + F/2", "B + F", "B - F", "B + F/4"};
    return names[mode & 3];
}

const char* depthName(int depth) { return depth == 0 ? "4-bit" : depth == 1 ? "8-bit" : "15-bit"; }

// Texture page width in VRAM halfwords for a depth.
int pageWidth(int depth) { return depth == 0 ? 64 : depth == 1 ? 128 : 256; }

ImU32 vertexColour(const gpudump::Vertex& v) { return IM_COL32(v.r, v.g, v.b, 255); }

// Transport glyphs: 0 first, 1 previous, 2 next, 3 last.
bool transportButton(const char* id, ImRect r, int glyph, const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Hit h = interact(id, r);
    nameRect(id, r);
    tooltip(tip);
    dl->AddRectFilled(r.Min, r.Max, lerpColor(color::raised, color::hover, h.hover), radius::button);
    dl->AddRect(r.Min, r.Max, color::borderStrong, radius::button);
    const ImVec2 c = r.GetCenter();
    const ImU32 col = lerpColor(color::textDim, color::text, h.hover);
    const float s = 5;
    const bool left = glyph < 2;
    const float dir = left ? -1.f : 1.f;
    dl->AddTriangleFilled(ImVec2(c.x + dir * s, c.y), ImVec2(c.x - dir * s * 0.7f, c.y - s), ImVec2(c.x - dir * s * 0.7f, c.y + s), col);
    if (glyph == 0 || glyph == 3) {
        const float bx = c.x + dir * (s + 2);
        dl->AddRectFilled(ImVec2(bx - 1, c.y - s), ImVec2(bx + 1, c.y + s), col);
    }
    return h.clicked;
}

}  // namespace

FramePanel::FramePanel() = default;

FramePanel::~FramePanel() {
    // At exit the GL context may already be gone; the driver frees the textures with it.
}

bool FramePanel::load(const std::filesystem::path& file, std::string* error) {
    gpudump::Dump dump;
    std::string err;
    if (!gpudump::load(file, dump, &err)) {
        if (error) *error = err;
        return false;
    }
    m_dump = std::move(dump);
    m_list = gpudump::decode(m_dump);
    m_file = file;
    m_loaded = true;
    m_gpu.clear();
    m_uploaded = size_t(-1);
    m_selected = -1;
    m_pickX = m_pickY = -1;
    m_pan = ImVec2(0, 0);
    m_listScroll = 0;
    // The frame image: the drawing area of the frame's last primitive (a game
    // draws into the buffer it shows next), else the display area.
    const gpudump::DrawState* st = nullptr;
    if (!m_list.drawn.empty()) st = &m_list.commands[size_t(m_list.drawn.back())].state;
    else if (!m_list.commands.empty()) st = &m_list.commands.back().state;
    if (st && !m_list.drawn.empty() && st->areaX2 >= st->areaX1 && st->areaY2 >= st->areaY1) {
        m_areaX = st->areaX1;
        m_areaY = st->areaY1;
        m_areaW = st->areaX2 - st->areaX1 + 1;
        m_areaH = st->areaY2 - st->areaY1 + 1;
    } else if (st) {
        m_areaX = st->displayX();
        m_areaY = st->displayY();
        m_areaW = st->displayWidth();
        m_areaH = st->displayHeight();
    } else {
        m_areaX = m_areaY = 0;
        m_areaW = 320;
        m_areaH = 240;
    }
    m_areaW = std::clamp(m_areaW, 1, gpuraster::kVramWidth);
    m_areaH = std::clamp(m_areaH, 1, gpuraster::kVramHeight);
    m_step = int(m_list.drawn.size());
    open = true;
    ++shows;
    say(nullptr, "frame: loaded " + utf8(file.filename()) + " prims " + std::to_string(m_list.drawn.size()) + " commands " +
                     std::to_string(m_list.commands.size()) + " frames " + std::to_string(m_list.frames) + " image " +
                     std::to_string(m_areaW) + "x" + std::to_string(m_areaH) + " at " + std::to_string(m_areaX) + "," +
                     std::to_string(m_areaY));
    say(nullptr, "frame: prims " + std::to_string(m_list.drawn.size()));
    return true;
}

ImRect FramePanel::dock(ImRect area) {
    if (!open) return area;
    const float g = size::gutter;
    const float hi = std::max(220.0f, area.GetHeight() - 160);
    m_height = std::clamp(m_height, 220.0f, hi);
    ImRect bar(ImVec2(area.Min.x, area.Max.y - m_height - g), ImVec2(area.Max.x, area.Max.y - m_height));
    ImGui::SetCursorScreenPos(bar.Min);
    ImGui::InvisibleButton("##framesplit", ImMax(bar.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) m_height = std::clamp(m_height - ImGui::GetIO().MouseDelta.y, 220.0f, hi);
    m_rect = ImRect(ImVec2(area.Min.x, area.Max.y - m_height), area.Max);
    return ImRect(area.Min, ImVec2(area.Max.x, bar.Min.y));
}

size_t FramePanel::replayTarget() const {
    if (m_step <= 0) {
        // Everything before the first live command: the capture's restore.
        for (size_t i = 0; i < m_list.commands.size(); ++i)
            if (m_list.commands[i].live) return i;
        return m_list.commands.size();
    }
    return size_t(m_list.drawn[size_t(m_step - 1)]) + 1;
}

int FramePanel::primitiveOf(int command) const {
    auto it = std::lower_bound(m_list.drawn.begin(), m_list.drawn.end(), command);
    return it != m_list.drawn.end() && *it == command ? int(it - m_list.drawn.begin()) : -1;
}

void FramePanel::sync() {
    const size_t target = replayTarget();
    if (m_gpu.done != target) gpuraster::replay(m_list, target, m_gpu);
    if (m_uploaded == target && m_tex && m_vramTex) return;
    upload(m_tex, m_areaW, m_areaH, gpuraster::rgba(m_gpu, m_areaX, m_areaY, m_areaW, m_areaH));
    upload(m_vramTex, gpuraster::kVramWidth, gpuraster::kVramHeight,
           gpuraster::rgba(m_gpu, 0, 0, gpuraster::kVramWidth, gpuraster::kVramHeight));
    m_uploaded = target;
}

void FramePanel::setStep(int step) {
    const int n = int(m_list.drawn.size());
    step = std::clamp(step, 0, n);
    if (step == m_step) return;
    m_step = step;
    if (m_step > 0) select(m_step - 1, true);
}

void FramePanel::select(int prim, bool scrollTo) {
    m_selected = prim;
    if (scrollTo) m_scrollToSelection = true;
}

void FramePanel::pick(int x, int y) {
    sync();
    m_pickX = x;
    m_pickY = y;
    const int32_t owner = m_gpu.owner[size_t(y & 511) * gpuraster::kVramWidth + size_t(x & 1023)];
    std::string what;
    if (owner < 0) {
        what = "none";
    } else {
        const int prim = primitiveOf(owner);
        if (prim < 0) {
            what = "setup";
        } else {
            select(prim, true);
            what = "#" + std::to_string(prim) + " " + gpudump::typeName(m_list.commands[size_t(owner)]);
        }
    }
    say(nullptr, "frame: pick " + std::to_string(x) + "," + std::to_string(y) + " -> " + what);
}

void FramePanel::onOpenPicked(void* self, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* p = static_cast<FramePanel*>(self);
    std::lock_guard<std::mutex> lock(p->m_pickMutex);
    p->m_openPicked = files[0];
}

void FramePanel::onSavePicked(void* self, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* p = static_cast<FramePanel*>(self);
    std::lock_guard<std::mutex> lock(p->m_pickMutex);
    p->m_savePicked = files[0];
}

void FramePanel::takePicked() {
    std::string openFile, saveFile;
    {
        std::lock_guard<std::mutex> lock(m_pickMutex);
        std::swap(openFile, m_openPicked);
        std::swap(saveFile, m_savePicked);
    }
    if (!openFile.empty()) {
        const std::filesystem::path f(std::u8string(openFile.begin(), openFile.end()));
        std::string err;
        if (load(f, &err)) {
            notice = "Opened " + utf8(f.filename());
            noticeBad = false;
        } else {
            notice = "Cannot open " + utf8(f.filename()) + ": " + err;
            noticeBad = true;
        }
    }
    if (!saveFile.empty() && m_loaded) {
        std::filesystem::path f(std::u8string(saveFile.begin(), saveFile.end()));
        if (f.extension().empty()) f += ".gpd";
        std::string err;
        if (gpudump::save(f, m_dump, &err)) {
            notice = "Saved the capture as " + utf8(f.filename());
            noticeBad = false;
        } else {
            notice = "Cannot save " + utf8(f.filename()) + ": " + err;
            noticeBad = true;
        }
    }
}

void FramePanel::draw(ImDrawList* dl) {
    if (!open) return;
    takePicked();
    const ImRect r = m_rect;
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window);
    dl->AddRect(r.Min, r.Max, color::border, radius::window);
    ImGui::PushID("framepanel");
    ImGui::PushClipRect(r.Min, r.Max, true);
    header(dl, ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + kHeaderH)));
    const ImRect body(ImVec2(r.Min.x + kPad, r.Min.y + kHeaderH + kPad), ImVec2(r.Max.x - kPad, r.Max.y - kPad));
    if (!m_loaded) {
        textCentered(dl, body, fonts().regular, type::body, color::textDim,
                     "No frame yet. While the game plays, Capture frame on the Game view (or Build > Capture Frame), or Open a .gpd file.");
    } else {
        sync();
        // Image | details | list; the image takes what the other two leave.
        const float listW = std::min(kListW, body.GetWidth() * 0.27f);
        const float detW = std::min(kDetailsW, body.GetWidth() * 0.32f);
        const ImRect list(ImVec2(body.Max.x - listW, body.Min.y), body.Max);
        const ImRect det(ImVec2(list.Min.x - kPad - detW, body.Min.y), ImVec2(list.Min.x - kPad, body.Max.y));
        const ImRect img(body.Min, ImVec2(det.Min.x - kPad, body.Max.y));
        imageColumn(dl, img);
        details(dl, det);
        primitiveList(dl, list);
        say("frame.step", "frame: step " + std::to_string(m_step) + "/" + std::to_string(m_list.drawn.size()));
        if (m_selected < 0) {
            say("frame.sel", "frame: selected none");
        } else {
            const Command& c = m_list.commands[size_t(m_list.drawn[size_t(m_selected)])];
            char b[200];
            int n = std::snprintf(b, sizeof b, "frame: selected #%d %s", m_selected, gpudump::typeName(c).c_str());
            if (c.textured())
                n += std::snprintf(b + n, sizeof b - size_t(n), " page %d,%d depth %d", c.state.pageX(), c.state.pageY(), c.state.depth());
            if (c.textured() && c.state.depth() < 2) n += std::snprintf(b + n, sizeof b - size_t(n), " clut %d,%d", c.clutX(), c.clutY());
            if (c.semiTransparent()) std::snprintf(b + n, sizeof b - size_t(n), " blend %d", c.state.blendMode());
            say("frame.sel", b);
        }
    }
    ImGui::PopClipRect();
    ImGui::PopID();
}

void FramePanel::header(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    dl->AddLine(ImVec2(r.Min.x, r.Max.y - 0.5f), ImVec2(r.Max.x, r.Max.y - 0.5f), color::border);
    const float cy = r.GetCenter().y;
    float x = r.Min.x + space::md;
    if (tabs) {
        tabs(dl, ImRect(r.Min, ImVec2(r.Min.x + tabsWidth, r.Max.y)));
        x = r.Min.x + tabsWidth + space::md;
    }
    if (m_loaded) {
        std::string info = utf8(m_file.filename()) + "   " + std::to_string(m_list.drawn.size()) + " primitives   " +
                           std::to_string(m_list.commands.size()) + " commands";
        if (m_list.frames > 1) info += "   " + std::to_string(m_list.frames) + " vsyncs";
        if (m_list.incomplete || m_dump.truncated) info += "   (cut short)";
        text(dl, ImVec2(x, cy - 8), f.medium, type::label, color::textDim, info.c_str());
    }
    // Right: Open, Save as, close.
    float rx = r.Max.x - space::md - 26;
    if (iconButton("frame.close", ImRect(ImVec2(rx, cy - 13), ImVec2(rx + 26, cy + 13)), icon::x, false, "Close the Frame tab")) open = false;
    nameRect("frame.close", ImRect(ImVec2(rx, cy - 13), ImVec2(rx + 26, cy + 13)));
    static const SDL_DialogFileFilter filter[] = {{"GPU dump", "gpd"}};
    const float saveW = buttonWidth(icon::hardDrive, "Save as");
    rx -= space::sm + saveW;
    if (m_loaded && button("frame.save", ImVec2(rx, cy - (size::field + 6) / 2), icon::hardDrive, "Save as", ButtonKind::Ghost, nullptr,
                           "Save this capture as a .gpd file"))
        showSaveFileDialog("frame-save", onSavePicked, this, filter, 1, nullptr);
    const float openW = buttonWidth(icon::folderOpen, "Open");
    rx -= space::sm + openW;
    if (button("frame.open", ImVec2(rx, cy - (size::field + 6) / 2), icon::folderOpen, "Open", ButtonKind::Ghost, nullptr,
               "Open a GPU dump (.gpd)"))
        showOpenFileDialog("frame-open", onOpenPicked, this, filter, 1, nullptr, false);
}

void FramePanel::imageColumn(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    // Toolbar: zoom, and what the image is.
    const ImVec2 zp(r.Min.x, r.Min.y);
    float zw = 0;
    const int sel = segmented("frame.zoom", zp, {"Fit", "1x", "2x", "3x", "4x"}, m_zoom, &zw);
    if (sel != m_zoom) {
        m_zoom = sel;
        m_pan = ImVec2(0, 0);
    }
    char what[96];
    std::snprintf(what, sizeof what, "Drawing area %dx%d at %d,%d", m_areaW, m_areaH, m_areaX, m_areaY);
    text(dl, ImVec2(zp.x + zw + space::md, r.Min.y + 5), f.regular, type::caption, color::textFaint, what);

    const ImRect view(ImVec2(r.Min.x, r.Min.y + kToolbarH), ImVec2(r.Max.x, r.Max.y - kTransportH));
    dl->AddRectFilled(view.Min, view.Max, rgb(0x0e1014), radius::field);
    const float fit = std::max(0.05f, std::min(view.GetWidth() / float(m_areaW), view.GetHeight() / float(m_areaH)));
    const float scale = m_zoom == 0 ? fit : float(m_zoom);
    const ImVec2 size(m_areaW * scale, m_areaH * scale);
    // Centred when it fits; panned (right or middle drag) when it does not.
    ImVec2 room(std::max(0.f, (size.x - view.GetWidth()) * 0.5f), std::max(0.f, (size.y - view.GetHeight()) * 0.5f));
    m_pan.x = std::clamp(m_pan.x, -room.x, room.x);
    m_pan.y = std::clamp(m_pan.y, -room.y, room.y);
    const ImVec2 origin(std::floor(view.GetCenter().x - size.x * 0.5f + m_pan.x), std::floor(view.GetCenter().y - size.y * 0.5f + m_pan.y));
    const ImRect image(origin, origin + size);
    ImGui::PushClipRect(view.Min, view.Max, true);
    addTextureImage(dl, (ImTextureID)(intptr_t)m_tex, image.Min, image.Max);
    auto toScreen = [&](float vx, float vy) { return ImVec2(origin.x + (vx - m_areaX) * scale, origin.y + (vy - m_areaY) * scale); };
    // The selected primitive's outline (dark under light, so it reads on any picture).
    if (m_selected >= 0) {
        const Command& c = m_list.commands[size_t(m_list.drawn[size_t(m_selected)])];
        std::vector<std::pair<int, int>> pts = gpuraster::outline(c);
        std::vector<ImVec2> sp;
        for (auto [x, y] : pts) sp.push_back(toScreen(float(x), float(y)));
        const bool closed = c.kind != Kind::Line;
        if (sp.size() >= 2) {
            dl->AddPolyline(sp.data(), int(sp.size()), rgb(0x000000, 200), closed ? ImDrawFlags_Closed : 0, 3.5f);
            dl->AddPolyline(sp.data(), int(sp.size()), rgb(0xffd166), closed ? ImDrawFlags_Closed : 0, 1.5f);
        }
    }
    if (m_pickX >= 0) {
        const ImVec2 a = toScreen(float(m_pickX), float(m_pickY)), b = toScreen(float(m_pickX + 1), float(m_pickY + 1));
        const ImVec2 pad(std::max(2.f, scale * 0.5f), std::max(2.f, scale * 0.5f));
        dl->AddRect(a - pad - ImVec2(1, 1), b + pad + ImVec2(1, 1), rgb(0x000000, 220), 0.0f, 0, 3.0f);
        dl->AddRect(a - pad, b + pad, rgb(0x5ccfe0), 0.0f, 0, 1.5f);
    }
    ImGui::PopClipRect();
    // The image's own rect is named (not the view), so a check can click pixel (px, py) at
    // @frame.image,dx,dy with dx = (px + 0.5 - w/2) * zoom.
    nameRect("frame.image", image);
    ImRect hitR(ImMax(view.Min, image.Min), ImMin(view.Max, image.Max));
    if (hitR.GetWidth() > 0 && hitR.GetHeight() > 0) {
        Hit h = interact("frame.image", hitR);
        const ImGuiIO& io = ImGui::GetIO();
        if (h.clicked) {
            const int px = int(std::floor((io.MousePos.x - origin.x) / scale)), py = int(std::floor((io.MousePos.y - origin.y) / scale));
            if (px >= 0 && py >= 0 && px < m_areaW && py < m_areaH) pick(m_areaX + px, m_areaY + py);
        }
        if (h.hovered) {
            const int px = int(std::floor((io.MousePos.x - origin.x) / scale)), py = int(std::floor((io.MousePos.y - origin.y) / scale));
            if (px >= 0 && py >= 0 && px < m_areaW && py < m_areaH) {
                char b[64];
                std::snprintf(b, sizeof b, "%d, %d", m_areaX + px, m_areaY + py);
                const ImVec2 ts = measure(f.regular, type::caption, b);
                const ImRect chip(ImVec2(view.Max.x - ts.x - 18, view.Max.y - 24), ImVec2(view.Max.x - 4, view.Max.y - 4));
                dl->AddRectFilled(chip.Min, chip.Max, rgb(0x0e1014, 210), radius::pill);
                textCentered(dl, chip, f.regular, type::caption, color::textDim, b);
            }
            if (io.MouseWheel != 0) {
                const int z = m_zoom == 0 ? std::max(1, int(fit)) : m_zoom;
                m_zoom = std::clamp(z + (io.MouseWheel > 0 ? 1 : -1), 1, 4);
            }
        }
        if (h.hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
            m_pan = m_pan + io.MouseDelta;
    }
    transport(dl, ImRect(ImVec2(r.Min.x, r.Max.y - kTransportH + 6), r.Max));
}

void FramePanel::transport(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    const int n = int(m_list.drawn.size());
    const float bh = r.GetHeight(), bw = 30;
    float x = r.Min.x;
    auto at = [&](float w) {
        ImRect b(ImVec2(x, r.Min.y), ImVec2(x + w, r.Min.y + bh));
        x += w + space::xs;
        return b;
    };
    if (transportButton("frame.first", at(bw), 0, "First primitive")) setStep(n > 0 ? 1 : 0);
    if (transportButton("frame.prev", at(bw), 1, "Previous primitive")) setStep(m_step - 1);
    if (transportButton("frame.next", at(bw), 2, "Next primitive")) setStep(m_step + 1);
    if (transportButton("frame.last", at(bw), 3, "Whole frame")) setStep(n);
    char label[48];
    std::snprintf(label, sizeof label, "%d / %d", m_step, n);
    const float lw = std::max(measure(f.medium, type::label, "0000 / 0000").x, measure(f.medium, type::label, label).x) + space::md;
    const ImRect slider(ImVec2(x + space::sm, r.Min.y), ImVec2(r.Max.x - lw, r.Max.y));
    text(dl, ImVec2(slider.Max.x + space::md, r.GetCenter().y - 8), f.medium, type::label, color::text, label);
    if (slider.GetWidth() < 20) return;
    Hit h = interact("frame.slider", slider);
    nameRect("frame.slider", slider);
    const float ty = slider.GetCenter().y;
    dl->AddRectFilled(ImVec2(slider.Min.x, ty - 2), ImVec2(slider.Max.x, ty + 2), color::field, 2);
    const float t = n > 0 ? float(m_step) / float(n) : 1.f;
    const float hx = slider.Min.x + t * slider.GetWidth();
    dl->AddRectFilled(ImVec2(slider.Min.x, ty - 2), ImVec2(hx, ty + 2), color::accent, 2);
    dl->AddCircleFilled(ImVec2(hx, ty), h.hover > 0 || h.held ? 7.f : 6.f, color::text);
    if (h.held && n > 0) {
        const float u = std::clamp((ImGui::GetIO().MousePos.x - slider.Min.x) / slider.GetWidth(), 0.f, 1.f);
        setStep(int(std::lround(u * n)));
    }
}

void FramePanel::details(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    dl->AddRectFilled(r.Min, r.Max, rgb(0x171a1f), radius::field);
    const ImRect text_(ImVec2(r.Min.x + kPad, r.Min.y + 8), ImVec2(r.Max.x - kPad, r.Max.y - 6));
    const Command* c = m_selected >= 0 ? &m_list.commands[size_t(m_list.drawn[size_t(m_selected)])] : nullptr;
    ImGui::PushClipRect(r.Min, ImVec2(r.Max.x, text_.Max.y), true);
    float y = text_.Min.y;
    const float lineH = 17, labelW = 74;
    auto row = [&](const char* label, const std::string& value, ImU32 col = color::text) {
        text(dl, ImVec2(text_.Min.x, y), f.regular, type::caption, color::textFaint, label);
        text(dl, ImVec2(text_.Min.x + labelW, y), f.regular, type::caption, col, value.c_str());
        y += lineH;
    };
    if (!c) {
        text(dl, ImVec2(text_.Min.x, y), f.semibold, type::label, color::textDim, "No primitive selected");
        y += 22;
        text(dl, ImVec2(text_.Min.x, y), f.regular, type::caption, color::textFaint, "Click the image to find what drew a pixel,");
        y += lineH;
        text(dl, ImVec2(text_.Min.x, y), f.regular, type::caption, color::textFaint, "or step through the draw order below it.");
    } else {
        char b[160];
        std::snprintf(b, sizeof b, "#%d  %s", m_selected, gpudump::typeName(*c).c_str());
        text(dl, ImVec2(text_.Min.x, y), f.semibold, type::label, color::text, b);
        y += 22;
        std::snprintf(b, sizeof b, "#%d, %zu words (file words %u..%u)", m_list.drawn[size_t(m_selected)], c->words.size(), c->firstWord,
                      c->lastWord);
        row("Command", b, color::textDim);
        const gpudump::DrawState& st = c->state;
        row("Blend", c->semiTransparent() ? std::string(blendName(st.blendMode())) + " (mode " + std::to_string(st.blendMode()) + ")"
                                          : std::string("opaque"));
        if (c->textured()) {
            std::snprintf(b, sizeof b, "page %d,%d  %s%s", st.pageX(), st.pageY(), depthName(st.depth()), c->raw() ? "  raw" : "  modulated");
            row("Texture", b);
            if (st.depth() < 2) {
                std::snprintf(b, sizeof b, "%d,%d  (%d entries)", c->clutX(), c->clutY(), st.depth() == 0 ? 16 : 256);
                row("CLUT", b, rgb(0xf0a43a));
            }
            const int mx = int(st.texWindow & 31) * 8, my = int((st.texWindow >> 5) & 31) * 8;
            const int ox = int((st.texWindow >> 10) & 31) * 8, oy = int((st.texWindow >> 15) & 31) * 8;
            if (mx || my) std::snprintf(b, sizeof b, "mask %d,%d  offset %d,%d", mx, my, ox, oy);
            else std::snprintf(b, sizeof b, "none");
            row("Tex window", b);
        } else if (c->kind == Kind::Fill || c->kind == Kind::CopyIn || c->kind == Kind::CopyVram) {
            if (c->kind == Kind::CopyVram) std::snprintf(b, sizeof b, "%d,%d -> %d,%d  %dx%d", c->srcX, c->srcY, c->dstX, c->dstY, c->width, c->height);
            else std::snprintf(b, sizeof b, "%d,%d  %dx%d", c->dstX, c->dstY, c->width, c->height);
            row("Rect", b);
        }
        if (c->kind != Kind::Fill && c->kind != Kind::CopyIn && c->kind != Kind::CopyVram) {
            std::snprintf(b, sizeof b, "%d,%d - %d,%d   offset %d,%d", st.areaX1, st.areaY1, st.areaX2, st.areaY2, st.offsetX, st.offsetY);
            row("Area", b);
            std::snprintf(b, sizeof b, "dither %s   mask set %s, check %s", st.dither() ? "on" : "off", st.setMask ? "on" : "off",
                          st.checkMask ? "on" : "off");
            row("Mode", b, color::textDim);
        }
        if (c->kind == Kind::Rect) {
            std::snprintf(b, sizeof b, "%dx%d", c->width, c->height);
            row("Size", b);
        }
        // Vertices: position (VRAM, offset applied), colour, texture coordinate.
        y += 3;
        const bool showColour = c->kind != Kind::CopyIn && c->kind != Kind::CopyVram;
        for (size_t i = 0; i < c->vertices.size() && y < text_.Max.y; ++i) {
            const gpudump::Vertex& v = c->vertices[i];
            const bool vtx = c->kind == Kind::Polygon || c->kind == Kind::Line || c->kind == Kind::Rect;
            std::snprintf(b, sizeof b, "v%zu", i);
            text(dl, ImVec2(text_.Min.x, y), f.regular, type::caption, color::textFaint, b);
            float vx = text_.Min.x + 26;
            if (vtx) {
                std::snprintf(b, sizeof b, "%d,%d", v.x + st.offsetX, v.y + st.offsetY);
                text(dl, ImVec2(vx, y), f.regular, type::caption, color::text, b);
            }
            vx += 72;
            if (showColour && (i == 0 || c->gouraud())) {
                dl->AddRectFilled(ImVec2(vx, y + 2), ImVec2(vx + 11, y + 13), vertexColour(v), 2);
                dl->AddRect(ImVec2(vx, y + 2), ImVec2(vx + 11, y + 13), color::borderStrong, 2);
                std::snprintf(b, sizeof b, "%02x%02x%02x", v.r, v.g, v.b);
                text(dl, ImVec2(vx + 16, y), f.regular, type::caption, color::textDim, b);
            }
            vx += 78;
            if (c->textured()) {
                std::snprintf(b, sizeof b, "uv %d,%d", v.u, v.v);
                text(dl, ImVec2(vx, y), f.regular, type::caption, color::text, b);
            }
            y += lineH;
        }
    }
    ImGui::PopClipRect();
}

void FramePanel::vramInset(ImDrawList* dl, ImRect r, const Command* c) {
    Fonts& f = fonts();
    text(dl, ImVec2(r.Min.x, r.Min.y), f.medium, type::caption, color::textFaint, "VRAM");
    if (c && c->textured()) {
        // Legend at the right: a swatch and a word per marked box.
        float lx = r.Max.x;
        auto key = [&](const char* word, ImU32 col) {
            lx -= measure(f.regular, type::caption, word).x;
            text(dl, ImVec2(lx, r.Min.y), f.regular, type::caption, color::textFaint, word);
            dl->AddRectFilled(ImVec2(lx - 13, r.Min.y + 4), ImVec2(lx - 5, r.Min.y + 12), col);
            lx -= 13 + space::md;
        };
        if (c->state.depth() < 2) key("CLUT", rgb(0xf0a43a));
        key("page", rgb(0x5ccfe0));
    }
    const float top = r.Min.y + 17;
    const float w = std::min(r.GetWidth(), (r.Max.y - top) * 2);
    const ImRect v(ImVec2(r.Min.x, top), ImVec2(r.Min.x + w, top + w / 2));
    nameRect("frame.vram", v);
    addTextureImage(dl, (ImTextureID)(intptr_t)m_vramTex, v.Min, v.Max);
    dl->AddRect(v.Min - ImVec2(1, 1), v.Max + ImVec2(1, 1), color::borderStrong);
    const float s = w / gpuraster::kVramWidth;
    auto box = [&](int x, int y, int bw, int bh, ImU32 col) {
        const ImVec2 a(v.Min.x + x * s, v.Min.y + y * s), b(v.Min.x + (x + bw) * s, v.Min.y + (y + bh) * s);
        dl->AddRect(a - ImVec2(1, 1), ImMax(b, a + ImVec2(2, 2)) + ImVec2(1, 1), rgb(0x000000, 200), 0.0f, 0, 2.5f);
        dl->AddRect(a, ImMax(b, a + ImVec2(2, 2)), col, 0.0f, 0, 1.5f);
    };
    box(m_areaX, m_areaY, m_areaW, m_areaH, rgb(0xffffff, 90));
    if (c && c->textured()) {
        box(c->state.pageX(), c->state.pageY(), pageWidth(c->state.depth()), 256, rgb(0x5ccfe0));
        if (c->state.depth() < 2) box(c->clutX(), c->clutY(), c->state.depth() == 0 ? 16 : 256, 1, rgb(0xf0a43a));
    } else if (c && (c->kind == Kind::Fill || c->kind == Kind::CopyIn || c->kind == Kind::CopyVram)) {
        box(c->dstX, c->dstY, c->width, c->height, rgb(0x5ccfe0));
    }
}

void FramePanel::primitiveList(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    // The VRAM inset on top (2:1, the column's width), the list under it.
    const float vw = r.GetWidth() - 2 * kPad;
    const float vramH = std::min(vw / 2 + 24, r.GetHeight() * 0.45f);
    const Command* sel = m_selected >= 0 ? &m_list.commands[size_t(m_list.drawn[size_t(m_selected)])] : nullptr;
    dl->AddRectFilled(r.Min, ImVec2(r.Max.x, r.Min.y + vramH + 8), rgb(0x171a1f), radius::field);
    vramInset(dl, ImRect(ImVec2(r.Min.x + kPad, r.Min.y + 6), ImVec2(r.Max.x - kPad, r.Min.y + vramH + 4)), sel);
    r.Min.y += vramH + 8 + kPad;
    dl->AddRectFilled(r.Min, r.Max, rgb(0x171a1f), radius::field);
    char b[96];
    std::snprintf(b, sizeof b, "Primitives  %zu", m_list.drawn.size());
    text(dl, ImVec2(r.Min.x + kPad, r.Min.y + 6), f.medium, type::caption, color::textFaint, b);
    const ImRect rows(ImVec2(r.Min.x + 2, r.Min.y + 26), ImVec2(r.Max.x - 2, r.Max.y - 2));
    nameRect("frame.list", rows);
    const int n = int(m_list.drawn.size());
    const float total = n * kRowH, maxScroll = std::max(0.f, total - rows.GetHeight());
    if (m_scrollToSelection && m_selected >= 0) {
        const float top = m_selected * kRowH;
        if (top < m_listScroll) m_listScroll = top;
        if (top + kRowH > m_listScroll + rows.GetHeight()) m_listScroll = top + kRowH - rows.GetHeight();
        m_scrollToSelection = false;
    }
    if (rows.Contains(ImGui::GetIO().MousePos) && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        m_listScroll -= ImGui::GetIO().MouseWheel * kRowH * 3;
    m_listScroll = std::clamp(m_listScroll, 0.f, maxScroll);
    ImGui::PushClipRect(rows.Min, rows.Max, true);
    const int first = int(m_listScroll / kRowH);
    for (int i = first; i < n; ++i) {
        const float y = rows.Min.y + i * kRowH - m_listScroll;
        if (y > rows.Max.y) break;
        const ImRect rr(ImVec2(rows.Min.x, y), ImVec2(rows.Max.x - (maxScroll > 0 ? 6 : 0), y + kRowH));
        const std::string id = "frame.row/" + std::to_string(i);
        Hit h = interact(id.c_str(), ImRect(ImMax(rr.Min, rows.Min), ImMin(rr.Max, rows.Max)));
        nameRect(id, rr);
        if (i == m_selected) dl->AddRectFilled(rr.Min, rr.Max, color::accentSoft, radius::field);
        else if (h.hover > 0) dl->AddRectFilled(rr.Min, rr.Max, color::hover, radius::field);
        const Command& c = m_list.commands[size_t(m_list.drawn[size_t(i)])];
        const bool drawn = i < m_step;
        std::snprintf(b, sizeof b, "%d", i);
        text(dl, ImVec2(rr.Min.x + 8, y + 1), f.regular, type::caption, color::textFaint, b);
        text(dl, ImVec2(rr.Min.x + 50, y + 1), f.regular, type::caption, drawn ? color::text : color::textFaint, gpudump::typeName(c).c_str());
        if (h.clicked) {
            select(i, false);
            m_step = i + 1;
        }
    }
    ImGui::PopClipRect();
    if (maxScroll > 0) {
        const float th = std::max(20.f, rows.GetHeight() * rows.GetHeight() / total);
        const float ty = rows.Min.y + (rows.GetHeight() - th) * (m_listScroll / maxScroll);
        dl->AddRectFilled(ImVec2(rows.Max.x - 5, ty), ImVec2(rows.Max.x - 1, ty + th), color::borderStrong, 2);
    }
}

}  // namespace ui
