#include "editor/asset_browser_view.hh"

#include <SDL3/SDL.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include "editor/document.hh"
#include "editor/project.hh"
#include "gl.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
using editor::AssetEntry;
using editor::AssetType;
namespace fs = std::filesystem;

namespace {

constexpr float kHeaderH = 46, kTreeW = 196, kFooterH = 26, kTileW = 92, kTileH = 108, kThumb = 60, kGap = 6, kRowH = 24;

// Lucide glyphs the shared icon list does not carry.
constexpr const char* kImage = "\xee\x83\xb6";        // e0f6 image
constexpr const char* kFilm = "\xee\x83\x90";         // e0d0 film
constexpr const char* kShapes = "\xee\x92\xb3";       // e4b3 shapes
constexpr const char* kFile = "\xee\x83\x80";         // e0c0 file
constexpr const char* kType = "\xee\x86\x98";         // e198 type
constexpr const char* kList = "\xee\x84\x86";         // e106 list
constexpr const char* kLayoutGrid = "\xee\x83\xbf";   // e0ff layout-grid
constexpr const char* kClapper = "\xee\x8a\x9b";      // e29b clapperboard

const char* typeIcon(AssetType t) {
    switch (t) {
        case AssetType::Folder: return icon::folder;
        case AssetType::Model: return kShapes;
        case AssetType::Mesh: return icon::box;
        case AssetType::Texture: return kImage;
        case AssetType::Script: return icon::fileCode;
        case AssetType::Audio: return icon::music;
        case AssetType::Animation: return kFilm;
        case AssetType::Scene: return kClapper;
        case AssetType::Font: return kType;
        case AssetType::Other: return kFile;
    }
    return kFile;
}

ImU32 typeColor(AssetType t) {
    switch (t) {
        case AssetType::Folder: return rgb(0xd8b46a);
        case AssetType::Model: return rgb(0x7aa7ff);
        case AssetType::Mesh: return rgb(0x7aa7ff);
        case AssetType::Texture: return rgb(0xf0a46a);
        case AssetType::Script: return rgb(0x5ccfe0);
        case AssetType::Audio: return rgb(0x46c98a);
        case AssetType::Animation: return rgb(0xd98ad2);
        case AssetType::Scene: return color::accent;
        case AssetType::Font: return color::text;
        case AssetType::Other: return color::textFaint;
    }
    return color::textFaint;
}

ImU32 withAlpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

float centerY(ImFont* font, float size, float y0, float y1) { return y0 + (y1 - y0 - measure(font, size, "Ag").y) * 0.5f; }

// `s` cut to fit `width` with an ellipsis.
std::string fit(ImFont* font, float size, const std::string& s, float width) {
    if (measure(font, size, s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && measure(font, size, (t + "...").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xc0) == 0x80) t.pop_back();  // whole UTF-8 characters
    }
    return t + "...";
}

std::string stemOf(const AssetEntry& e) {
    if (e.type == AssetType::Folder) return e.name;
    const size_t dot = e.name.rfind('.');
    return dot == std::string::npos || dot == 0 ? e.name : e.name.substr(0, dot);
}

std::string baseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentOfPath(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// "; 3 references updated in 2 scenes" after a rename or move, or nothing.
std::string updatedNote(const editor::AssetOp& op) {
    if (!op.error.empty() || op.references == 0) return {};
    std::string s = "; " + std::to_string(op.references) + (op.references == 1 ? " reference" : " references") + " updated";
    if (!op.scenes.empty())
        s += ", " + std::to_string(op.scenes.size()) + (op.scenes.size() == 1 ? " saved scene" : " saved scenes") + " rewritten";
    return s;
}

// The project folder's name, as UTF-8.
std::string projectName(const fs::path& project) {
    fs::path p = project.has_filename() ? project.filename() : project.parent_path().filename();
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path absPath(const fs::path& project, const std::string& rel) { return project / fs::path(std::u8string(rel.begin(), rel.end())); }

struct MenuItem {
    std::string label;
    std::string shortcut;
    bool enabled = true;
    std::function<void()> run;  // empty label: a separator
};

// A themed popup menu of `items` (open, sized and placed by the caller).
// With a `name`, scripted checks find each row as <name>/<label>.
std::function<void()> menuRows(const std::vector<MenuItem>& items, float w, float h, const char* name = nullptr) {
    Fonts& f = fonts();
    const float pad = space::xs, rowH = size::field + 4, sepH = 9;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect box(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImVec2(w, h));
    dl->AddRectFilled(box.Min + ImVec2(0, 2), box.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
    dl->AddRectFilled(box.Min, box.Max, color::raised, radius::card);
    dl->AddRect(box.Min, box.Max, color::borderStrong, radius::card);
    float y = box.Min.y + pad;
    std::function<void()> run;
    int i = 0;
    for (const MenuItem& e : items) {
        ImGui::PushID(i++);
        if (e.label.empty()) {
            dl->AddLine(ImVec2(box.Min.x + space::sm, y + sepH * 0.5f), ImVec2(box.Max.x - space::sm, y + sepH * 0.5f), color::border);
            y += sepH;
        } else {
            ImRect ir(ImVec2(box.Min.x + pad, y), ImVec2(box.Max.x - pad, y + rowH));
            Hit hit = interact("item", ir);
            if (name) nameRect(std::string(name) + "/" + e.label, ir);
            const bool on = e.enabled && hit.hover > 0;
            if (on) dl->AddRectFilled(ir.Min, ir.Max, lerpColor(rgb(0x2a2f39, 0), color::hover, hit.hover), radius::field);
            const ImU32 tc = !e.enabled ? color::textFaint : on && hit.hover > 0.5f ? color::text : color::textDim;
            text(dl, ImVec2(ir.Min.x + space::md, centerY(f.regular, type::body, ir.Min.y, ir.Max.y)), f.regular, type::body, tc,
                 e.label.c_str());
            if (!e.shortcut.empty()) {
                ImVec2 ss = measure(f.regular, type::label, e.shortcut.c_str());
                text(dl, ImVec2(ir.Max.x - space::sm - ss.x, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular,
                     type::label, color::textFaint, e.shortcut.c_str());
            }
            if (hit.clicked && e.enabled && e.run) run = e.run;
            y += rowH;
        }
        ImGui::PopID();
    }
    return run;
}

void menuSize(const std::vector<MenuItem>& items, float* w, float* h) {
    Fonts& f = fonts();
    *w = 200;
    *h = space::xs * 2;
    for (const MenuItem& e : items) {
        if (e.label.empty()) {
            *h += 9;
            continue;
        }
        *h += size::field + 4;
        *w = std::max(*w, space::md + measure(f.regular, type::body, e.label.c_str()).x + space::xl +
                              measure(f.regular, type::label, e.shortcut.c_str()).x + space::md);
    }
}

}  // namespace

// --- layout ---

ImRect AssetBrowser::dock(ImRect area) {
    const float g = size::gutter;
    if (!open) {
        m_rect = ImRect(ImVec2(area.Min.x, area.Max.y - kHeaderH), area.Max);
        return ImRect(area.Min, ImVec2(area.Max.x, m_rect.Min.y - g));
    }
    const float hi = std::max(140.0f, area.GetHeight() - 200);
    m_height = std::clamp(m_height, 140.0f, hi);
    ImRect bar(ImVec2(area.Min.x, area.Max.y - m_height - g), ImVec2(area.Max.x, area.Max.y - m_height));
    ImGui::SetCursorScreenPos(bar.Min);
    ImGui::InvisibleButton("##assetsplit", ImMax(bar.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) m_height = std::clamp(m_height - ImGui::GetIO().MouseDelta.y, 140.0f, hi);
    m_rect = ImRect(ImVec2(area.Min.x, area.Max.y - m_height), area.Max);
    return ImRect(area.Min, ImVec2(area.Max.x, bar.Min.y));
}

std::vector<AssetBrowser::Request> AssetBrowser::takeRequests() {
    std::vector<Request> r;
    r.swap(m_requests);
    return r;
}

const AssetEntry* AssetBrowser::dragging() const {
    // Folders only move inside the panel; outside it, files are what drag.
    const AssetEntry* e = m_dragging ? m_index.find(m_pressed) : nullptr;
    return e && e->type != AssetType::Folder ? e : nullptr;
}

bool AssetBrowser::hasKeyboard() const { return m_searching || !m_renaming.empty(); }

std::string AssetBrowser::shownList() const {
    std::string s;
    for (const std::string& p : m_shown) s += (s.empty() ? "" : "|") + baseName(p);
    return s;
}

// --- drawing ---

void AssetBrowser::draw(ImDrawList* dl, editor::Document& doc) {
    ImGuiIO& io = ImGui::GetIO();
    m_thumbLoads = 0;
    const double now = ImGui::GetTime();
    const bool otherProject = doc.projectRoot() != m_index.project();
    if (otherProject || m_scannedAt < 0 || now - m_scannedAt >= 1.0) {
        m_scannedAt = now;
        if (otherProject) {
            m_folder.clear();
            m_selected.clear();
            m_query.clear();
            m_collapsed.clear();
            m_scroll = m_treeScroll = 0;
            dropThumbnails();
            // A project just opened has no undo step that could bring old
            // deletes back: what has lain in its trash for a month goes.
            if (int n = editor::emptyOldTrash(doc.projectRoot(), fs::file_time_type::clock::now()))
                notice("Emptied " + std::to_string(n) + (n == 1 ? " delete" : " deletes") + " older than 30 days from the project's .trash", false);
        }
        if (m_index.scan(doc.projectRoot())) {
            if (!m_folder.empty() && !m_index.find(m_folder)) m_folder.clear();
            if (!m_selected.empty() && !m_index.find(m_selected)) m_selected.clear();
        }
    }
    {
        fs::path rel = doc.scenePath().lexically_relative(doc.projectRoot());
        std::u8string u = rel.generic_u8string();
        m_openScene = doc.projectRoot().empty() ? std::string() : std::string(u.begin(), u.end());
    }
    m_refs.update(m_index, doc.scene(), m_openScene, doc.revision());
    m_folderTargets.clear();

    const ImRect r = m_rect;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (r.Contains(io.MousePos))
            m_focused = true;
        else if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            m_focused = false;
    }
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window);
    dl->AddRect(r.Min, r.Max, m_focused && open ? withAlpha(color::accent, 90) : rgb(0xffffff, 8), radius::window);
    ImGui::PushID("assetbrowser");
    header(dl, ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + kHeaderH)));
    if (open) {
        const ImRect body(ImVec2(r.Min.x, r.Min.y + kHeaderH), r.Max);
        dl->AddLine(ImVec2(body.Min.x, body.Min.y), ImVec2(body.Max.x, body.Min.y), color::border);
        if (doc.projectRoot().empty()) {
            Fonts& f = fonts();
            textCentered(dl, body, f.regular, type::body, color::textFaint, "Open or create a project to see its files here.");
            m_shown.clear();
        } else {
            const ImRect tree(body.Min + ImVec2(0, 1), ImVec2(body.Min.x + kTreeW, body.Max.y));
            folderTree(dl, tree);
            dl->AddLine(ImVec2(tree.Max.x, tree.Min.y), ImVec2(tree.Max.x, tree.Max.y), color::border);
            const ImRect area(ImVec2(tree.Max.x + 1, body.Min.y + 1), ImVec2(body.Max.x, body.Max.y - kFooterH));
            files(dl, area, doc);
            footer(dl, ImRect(ImVec2(area.Min.x, area.Max.y), body.Max));
        }
        // Keys while the panel has the keyboard.
        const AssetEntry* sel = m_index.find(m_selected);
        if (m_focused && sel && !hasKeyboard() && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                m_menuFor = m_selected;
                if (!sel->madeBy.empty() || holdsOpenScene(*sel)) {
                    m_ask = Ask::CannotRename;
                    m_askFor = m_selected;
                    m_openAsk = true;
                } else {
                    m_renaming = m_selected;
                    m_renameStart = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                m_askFor = m_selected;
                m_ask = m_refs.to(m_index, m_selected).empty() && !holdsOpenScene(*sel) ? Ask::Delete : Ask::CannotDelete;
                m_openAsk = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) openEntry(*sel);
        }
    }
    menu(doc);
    prompt(doc);
    ImGui::PopID();

    // Drag out: past the threshold the asset follows the pointer; released
    // outside the panel it goes to the main screen.
    // Released on a folder in the panel (a tree row or a tile), a file or
    // folder moves there.
    std::string dropFolder;
    if (!m_pressed.empty() && m_dragging && r.Contains(io.MousePos))
        for (const auto& [fr, folder] : m_folderTargets)
            if (fr.Contains(io.MousePos)) {
                const bool self = folder == m_pressed || folder.rfind(m_pressed + "/", 0) == 0;
                if (!self && folder != parentOfPath(m_pressed)) {
                    dropFolder = folder;
                    ImGui::GetForegroundDrawList()->AddRect(fr.Min, fr.Max, color::accent, radius::field, 0, 2.0f);
                }
                break;
            }
    if (!m_pressed.empty()) {
        const AssetEntry* e = m_index.find(m_pressed);
        if (!e) {
            m_pressed.clear();
            m_dragging = false;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!m_dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6)) m_dragging = true;
        } else {
            if (m_dragging && !dropFolder.empty()) {
                const std::string moving = m_pressed;
                m_pressed.clear();
                m_dragging = false;
                moveInto(doc, moving, dropFolder);
            } else if (m_dragging && !r.Contains(io.MousePos) && e->type != AssetType::Folder) {
                m_requests.push_back({Request::Drop, e->path, e->type, io.MousePos, {}, {}, false});
                m_focused = false;  // the keyboard goes with the file
            }
            m_pressed.clear();
            m_dragging = false;
        }
    }
    if (const AssetEntry* e = m_dragging ? m_index.find(m_pressed) : nullptr) {
        Fonts& f = fonts();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 at = io.MousePos + ImVec2(14, 10);
        const ImVec2 ts = measure(f.medium, type::body, e->name.c_str());
        ImRect tag(at, at + ImVec2(ts.x + 40, 28));
        fg->AddRectFilled(tag.Min, tag.Max, color::raised, radius::field);
        fg->AddRect(tag.Min, tag.Max, color::borderStrong, radius::field);
        text(fg, ImVec2(tag.Min.x + space::sm, centerY(f.medium, type::icon, tag.Min.y, tag.Max.y)), f.medium, type::icon - 1,
             typeColor(e->type), typeIcon(e->type));
        text(fg, ImVec2(tag.Min.x + 30, centerY(f.medium, type::body, tag.Min.y, tag.Max.y)), f.medium, type::body, color::text,
             e->name.c_str());
    }
}

void AssetBrowser::header(ImDrawList* dl, ImRect h) {
    Fonts& f = fonts();
    // The dock's tab strip: the main screen's (Project and Timeline) when
    // set, else Project alone.
    ImRect tr;
    if (tabs) {
        tr = ImRect(h.Min, ImVec2(h.Min.x + tabsWidth, h.Max.y));
        tabs(dl, tr);
    } else {
        const char* tab = "Project";
        const float tw = measure(f.semibold, type::body, tab).x + 30 + space::md * 2;
        tr = ImRect(h.Min, ImVec2(h.Min.x + tw, h.Max.y));
        if (interact("tab", tr).clicked) open = !open;
        text(dl, ImVec2(tr.Min.x + space::md, centerY(f.medium, type::icon, tr.Min.y, tr.Max.y)), f.medium, type::icon - 1,
             typeColor(AssetType::Folder), icon::folderOpen);
        text(dl, ImVec2(tr.Min.x + space::md + 22, centerY(f.semibold, type::body, tr.Min.y, tr.Max.y)), f.semibold, type::body, color::text, tab);
        if (open) dl->AddRectFilled(ImVec2(tr.Min.x + space::sm, tr.Max.y - 2), ImVec2(tr.Max.x - space::sm, tr.Max.y), color::accent, 1);
    }
    // The tabs take the strip's full height (the Timeline's header's, so the
    // strip does not jump between tabs); the controls keep a 34 px band
    // across its middle.
    h = ImRect(ImVec2(h.Min.x, h.GetCenter().y - 17), ImVec2(h.Max.x, h.GetCenter().y + 17));

    // Right: collapse, view mode, type filter, search.
    float x = h.Max.x - space::xs;
    ImRect cb(ImVec2(x - 26, h.Min.y + 4), ImVec2(x, h.Max.y - 4));
    if (iconButton("collapse", cb, open ? icon::chevronDown : icon::chevronRight, false, open ? "Hide the Project panel" : "Show the Project panel"))
        open = !open;
    x = cb.Min.x - space::xs;
    if (!open) return;
    ImRect lb(ImVec2(x - 26, h.Min.y + 4), ImVec2(x, h.Max.y - 4));
    if (iconButton("listview", lb, kList, m_list, "List")) m_list = true;
    ImRect gb(ImVec2(lb.Min.x - 28, h.Min.y + 4), ImVec2(lb.Min.x - 2, h.Max.y - 4));
    if (iconButton("gridview", gb, kLayoutGrid, !m_list, "Grid")) m_list = false;
    x = gb.Min.x - space::sm;
    static const char* typeNames[] = {"All types", "Models", "Meshes", "Textures", "Scripts", "Audio", "Animations", "Scenes", "Fonts", "Other files"};
    static const AssetType typeOf[] = {AssetType::Folder, AssetType::Model, AssetType::Mesh, AssetType::Texture, AssetType::Script,
                                       AssetType::Audio, AssetType::Animation, AssetType::Scene, AssetType::Font, AssetType::Other};
    int current = 0;
    for (int i = 1; i < 10; ++i)
        if (m_types == (1u << static_cast<unsigned>(typeOf[i]))) current = i;
    ImRect tf(ImVec2(x - 132, h.Min.y + 5), ImVec2(x, h.Max.y - 5));
    // Its list opens above: the panel sits at the bottom of the window.
    dropdown("typefilter", tf, nullptr, typeNames[current]);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("##typefilter");
    if (ImGui::IsPopupOpen("##typefilter")) {
        std::vector<MenuItem> items;
        for (int i = 0; i < 10; ++i)
            items.push_back({std::string(i == current ? "\xe2\x80\xa2 " : "") + typeNames[i], "", true, [this, i] {
                                 m_types = i == 0 ? 0 : 1u << static_cast<unsigned>(typeOf[i]);
                                 m_scroll = 0;
                             }});
        float mw, mh;
        menuSize(items, &mw, &mh);
        mw = std::max(mw, tf.GetWidth());
        ImGui::SetNextWindowPos(ImVec2(tf.Max.x - mw, std::max(space::xs, tf.Min.y - space::xs - mh)));
        ImGui::SetNextWindowSize(ImVec2(mw, mh));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
        const bool shown = ImGui::BeginPopup("##typefilter", ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);
        if (shown) {
            std::function<void()> run = menuRows(items, mw, mh);
            if (run || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (run) run();
        }
    }
    x = tf.Min.x - space::sm;
    const float sw = std::clamp(x - tr.Max.x - 220, 120.0f, 240.0f);
    ImRect sr(ImVec2(x - sw, h.Min.y + 5), ImVec2(x, h.Max.y - 5));
    const std::string before = m_query;
    m_searching = searchField("search", sr, "Search files", nullptr, &m_query, false);
    if (m_query != before) m_scroll = 0;

    // Breadcrumb: the project, then each folder down to the one shown.
    float bx = tr.Max.x + space::sm;
    const float bmax = sr.Min.x - space::sm;
    std::vector<std::pair<std::string, std::string>> crumbs{{"", projectName(m_index.project())}};
    if (!m_query.empty() || m_types) {
        crumbs.push_back({m_folder, m_query.empty() ? "Filtered" : "Search"});
    } else {
        std::string acc;
        size_t start = 0;
        while (start < m_folder.size()) {
            size_t slash = m_folder.find('/', start);
            if (slash == std::string::npos) slash = m_folder.size();
            acc = m_folder.substr(0, slash);
            crumbs.push_back({acc, m_folder.substr(start, slash - start)});
            start = slash + 1;
        }
    }
    for (size_t i = 0; i < crumbs.size() && bx < bmax; ++i) {
        if (i) {
            text(dl, ImVec2(bx, centerY(f.medium, type::label, h.Min.y, h.Max.y)), f.medium, type::label, color::textFaint, icon::chevronRight);
            bx += 16;
        }
        const std::string label = fit(f.regular, type::body, crumbs[i].second, std::max(20.0f, bmax - bx - space::sm));
        const float w = measure(f.regular, type::body, label.c_str()).x + space::sm;
        ImRect cr(ImVec2(bx - 2, h.Min.y + 6), ImVec2(bx + w, h.Max.y - 6));
        ImGui::PushID(static_cast<int>(i));
        Hit hit = interact("crumb", cr);
        ImGui::PopID();
        const bool last = i + 1 == crumbs.size();
        if (hit.hover > 0 && !last) dl->AddRectFilled(cr.Min, cr.Max, rgb(0x2a2f39, static_cast<int>(160 * hit.hover)), radius::field);
        text(dl, ImVec2(bx + 2, centerY(f.regular, type::body, h.Min.y, h.Max.y)), last ? f.medium : f.regular, type::body,
             last ? color::text : color::textDim, label.c_str());
        if (hit.clicked && !last) {
            m_folder = crumbs[i].first;
            m_query.clear();
            m_types = 0;
            m_scroll = 0;
        }
        bx += w + space::xs;
    }
}

void AssetBrowser::folderTree(ImDrawList*, ImRect r) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushClipRect(r.Min, r.Max, true);
    ImGui::PushID("tree");
    const std::vector<std::string> folders = m_index.folders();
    std::vector<std::pair<std::string, int>> rows;  // shown folders and depth
    for (const std::string& p : folders) {
        int depth = p.empty() ? 0 : 1 + static_cast<int>(std::count(p.begin(), p.end(), '/'));
        // Hidden while an ancestor is closed.
        bool shown = true;
        for (std::string a = p; shown && !a.empty();) {
            const size_t s = a.rfind('/');
            a = s == std::string::npos ? std::string() : a.substr(0, s);
            shown = !m_collapsed.count(a) || !m_collapsed.at(a);
        }
        if (shown) rows.push_back({p, depth});
    }
    const float contentH = static_cast<float>(rows.size()) * size::row + space::sm * 2;
    if (r.Contains(io.MousePos) && io.MouseWheel != 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        m_treeScroll -= io.MouseWheel * size::row * 3;
    m_treeScroll = std::clamp(m_treeScroll, 0.0f, std::max(0.0f, contentH - r.GetHeight()));
    float y = r.Min.y + space::sm - m_treeScroll;
    const std::string rootName = projectName(m_index.project());
    for (const auto& [p, depth] : rows) {
        ImRect rr(ImVec2(r.Min.x + space::xs, y), ImVec2(r.Max.x - space::xs, y + size::row));
        y += size::row;
        if (rr.Max.y < r.Min.y || rr.Min.y > r.Max.y) continue;
        bool hasChildren = false;
        for (const std::string& q : folders)
            if (!q.empty() && q != p && (p.empty() ? q.find('/') == std::string::npos : q.rfind(p + "/", 0) == 0)) {
                hasChildren = true;
                break;
            }
        const bool expanded = !m_collapsed.count(p) || !m_collapsed.at(p);
        const std::string label = p.empty() ? rootName : baseName(p);
        TreeRow row;
        row.depth = depth;
        row.icon = expanded && hasChildren ? icon::folderOpen : icon::folder;
        row.iconColor = typeColor(AssetType::Folder);
        row.label = label.c_str();
        row.hasChildren = hasChildren;
        row.expanded = expanded;
        row.selected = p == m_folder && m_query.empty() && !m_types;
        Hit h = treeRow(("f:" + p).c_str(), rr, row);
        m_folderTargets.push_back({ImRect(ImMax(rr.Min, r.Min), ImMin(rr.Max, r.Max)), p});
        if (h.clicked) {
            if (hasChildren && io.MousePos.x < rr.Min.x + space::xs + depth * 16.0f + 16)
                m_collapsed[p] = expanded;
            else {
                m_folder = p;
                m_query.clear();
                m_types = 0;
                m_scroll = 0;
            }
        }
    }
    ImGui::PopID();
    ImGui::PopClipRect();
}

void AssetBrowser::itemInput(const AssetEntry& e, ImRect r, bool hovered) {
    if (e.type == AssetType::Folder) m_folderTargets.push_back({r, e.path});
    if (!hovered) return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_selected = e.path;
        m_pressed = e.path;
        m_dragging = false;
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_pressed.clear();
        openEntry(e);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_selected = e.path;
        m_menuFor = e.path;
        m_openMenu = true;
        m_menuPos = ImGui::GetIO().MousePos;
    }
}

void AssetBrowser::files(ImDrawList* dl, ImRect r, editor::Document& doc) {
    ImGuiIO& io = ImGui::GetIO();
    Fonts& f = fonts();
    editor::AssetFilter filter{m_folder, m_query, m_types};
    const std::vector<const AssetEntry*> items = editor::filterAssets(m_index, filter);
    m_shown.clear();
    for (const AssetEntry* e : items) m_shown.push_back(e->path);
    // A rename whose file has gone or scrolled out of the list ends.
    if (!m_renaming.empty() && std::find(m_shown.begin(), m_shown.end(), m_renaming) == m_shown.end()) m_renaming.clear();
    const ImRect inner(r.Min + ImVec2(space::sm, space::sm), r.Max - ImVec2(space::sm, 0));
    const int cols = m_list ? 1 : std::max(1, static_cast<int>((inner.GetWidth() + kGap) / (kTileW + kGap)));
    const float itemH = m_list ? kRowH : kTileH + kGap;
    const int rowsN = (static_cast<int>(items.size()) + cols - 1) / cols;
    const float listTop = m_list ? kRowH : 0;  // the list's column header
    const float contentH = listTop + rowsN * itemH + space::sm;
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (r.Contains(io.MousePos) && io.MouseWheel != 0 && !popup) m_scroll -= io.MouseWheel * itemH * (m_list ? 3 : 1);
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, contentH - inner.GetHeight()));

    ImGui::PushClipRect(r.Min, r.Max, true);
    ImGui::PushID("files");
    if (items.empty()) {
        const char* msg = !m_query.empty() ? "No files match the search." : m_types ? "No files of this type." : "This folder is empty.";
        textCentered(dl, inner, f.regular, type::body, color::textFaint, msg);
    }
    if (m_list && !items.empty()) {
        const float y = inner.Min.y - m_scroll;
        const float cName = inner.Min.x + space::sm, cType = inner.Max.x - 300, cSize = inner.Max.x - 190, cUsed = inner.Max.x - 110;
        for (auto [x, label] : {std::pair<float, const char*>{cName + 26, "Name"}, {cType, "Type"}, {cSize, "Size"}, {cUsed, "Used by"}})
            text(dl, ImVec2(x, centerY(f.medium, type::label, y, y + kRowH)), f.medium, type::label, color::textFaint, label);
        dl->AddLine(ImVec2(inner.Min.x, y + kRowH - 1), ImVec2(inner.Max.x, y + kRowH - 1), color::border);
    }
    const bool hoverArea = r.Contains(io.MousePos) && !popup;
    bool overItem = false;
    for (size_t i = 0; i < items.size(); ++i) {
        const AssetEntry& e = *items[i];
        const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
        ImRect ir;
        if (m_list)
            ir = ImRect(ImVec2(inner.Min.x, inner.Min.y + listTop + row * itemH - m_scroll),
                        ImVec2(inner.Max.x, inner.Min.y + listTop + (row + 1) * itemH - m_scroll));
        else
            ir = ImRect(ImVec2(inner.Min.x + col * (kTileW + kGap), inner.Min.y + row * itemH - m_scroll),
                        ImVec2(inner.Min.x + col * (kTileW + kGap) + kTileW, inner.Min.y + row * itemH - m_scroll + kTileH));
        if (ir.Max.y < r.Min.y || ir.Min.y > r.Max.y) continue;
        ImGui::PushID(e.path.c_str());
        ImRect hitR = ir;
        hitR.ClipWith(r);
        Hit h = interact("item", hitR);
        const bool hovered = ImGui::IsItemHovered() && hoverArea;
        overItem = overItem || hovered;
        itemInput(e, ir, hovered);
        const bool selected = e.path == m_selected;
        const bool renaming = e.path == m_renaming;
        if (selected) {
            dl->AddRectFilled(ir.Min, ir.Max, color::accentSoft, radius::card);
            dl->AddRect(ir.Min, ir.Max, m_focused ? color::accent : withAlpha(color::accent, 120), radius::card);
        } else if (h.hover > 0) {
            dl->AddRectFilled(ir.Min, ir.Max, rgb(0x2a2f39, static_cast<int>(170 * h.hover)), radius::card);
        }
        const size_t used = e.type == AssetType::Folder ? 0 : m_refs.to(m_index, e.path).size();
        int tw = 0, th = 0;
        const unsigned tex = e.type == AssetType::Texture ? thumbnail(e, &tw, &th) : 0;
        const std::string nameShown = e.type == AssetType::Folder ? e.name : e.name;
        ImRect nameField;
        float nameX = 0;
        if (m_list) {
            const float cType = ir.Max.x - 300, cSize = ir.Max.x - 190, cUsed = ir.Max.x - 110;
            float x = ir.Min.x + space::sm;
            if (tex) {
                ImRect t(ImVec2(x, ir.Min.y + 3), ImVec2(x + 18, ir.Max.y - 3));
                dl->AddImage((ImTextureID)(intptr_t)tex, t.Min, t.Max);
            } else {
                text(dl, ImVec2(x, centerY(f.medium, type::icon, ir.Min.y, ir.Max.y)), f.medium, type::icon - 1, typeColor(e.type), typeIcon(e.type));
            }
            nameX = x + 26;
            nameField = ImRect(ImVec2(nameX - 4, ir.Min.y + 1), ImVec2(cType - space::sm, ir.Max.y - 1));
            if (!renaming)
                text(dl, ImVec2(nameX, centerY(f.regular, type::body, ir.Min.y, ir.Max.y)), f.regular, type::body, color::text,
                     fit(f.regular, type::body, e.name, cType - nameX - space::md).c_str());
            text(dl, ImVec2(cType, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular, type::label, color::textDim,
                 editor::assetTypeName(e.type));
            if (e.type != AssetType::Folder) {
                text(dl, ImVec2(cSize, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular, type::label, color::textDim,
                     editor::sizeLabel(e.size).c_str());
                const std::string u = used ? std::to_string(used) + (used == 1 ? " use" : " uses") : e.madeBy.empty() ? "Not used" : "Import output";
                text(dl, ImVec2(cUsed, centerY(f.regular, type::label, ir.Min.y, ir.Max.y)), f.regular, type::label,
                     used ? color::textDim : color::textFaint, u.c_str());
            }
        } else {
            ImRect t(ImVec2(ir.GetCenter().x - kThumb * 0.5f, ir.Min.y + 8), ImVec2(ir.GetCenter().x + kThumb * 0.5f, ir.Min.y + 8 + kThumb));
            if (tex && tw > 0 && th > 0) {
                const float s = std::min(kThumb / tw, kThumb / th);
                const ImVec2 sz(tw * s, th * s);
                const ImVec2 o = t.GetCenter() - sz * 0.5f;
                dl->AddRectFilled(o - ImVec2(1, 1), o + sz + ImVec2(1, 1), color::field, 2);
                dl->AddImage((ImTextureID)(intptr_t)tex, o, o + sz);
            } else {
                dl->AddRectFilled(t.Min, t.Max, e.type == AssetType::Folder ? 0 : color::raised, radius::card);
                ImGui::PushFont(f.medium, 30.0f);
                const ImVec2 is = ImGui::CalcTextSize(typeIcon(e.type));
                ImGui::PopFont();
                dl->AddText(f.medium, 30.0f, t.GetCenter() - is * 0.5f + ImVec2(0, 2), typeColor(e.type), typeIcon(e.type));
            }
            if (used && e.type != AssetType::Folder) {
                // A dot on the thumbnail: something in a scene points at it.
                dl->AddCircleFilled(ImVec2(t.Max.x - 2, t.Min.y + 2), 4.0f, color::good, 12);
                dl->AddCircle(ImVec2(t.Max.x - 2, t.Min.y + 2), 4.5f, color::panel, 12, 1.5f);
            }
            const std::string label = fit(f.regular, type::label, nameShown, kTileW - 8);
            const float lw = measure(f.regular, type::label, label.c_str()).x;
            const float ly = t.Max.y + 6;
            nameX = ir.Min.x + 6;
            nameField = ImRect(ImVec2(ir.Min.x + 2, ly - 3), ImVec2(ir.Max.x - 2, ly + 19));
            if (!renaming) {
                text(dl, ImVec2(ir.GetCenter().x - lw * 0.5f, ly), f.regular, type::label, color::text, label.c_str());
                std::string meta = e.type == AssetType::Texture && tw ? std::to_string(tw) + " x " + std::to_string(th) : editor::assetTypeName(e.type);
                meta = fit(f.regular, type::caption, meta, kTileW - 8);
                const float mw = measure(f.regular, type::caption, meta.c_str()).x;
                text(dl, ImVec2(ir.GetCenter().x - mw * 0.5f, ly + 16), f.regular, type::caption, color::textFaint, meta.c_str());
            }
        }
        if (renaming) {
            if (m_renameStart) {
                beginTextEdit("rename", stemOf(e));
                m_renameStart = false;
            }
            std::string typed;
            TextEdit res = textEdit("rename", nameField, f.regular, m_list ? type::body : type::label, nameX, nameField.Max.x - 4, &typed);
            if (res == TextEdit::Commit) {
                commitRename(doc, typed);
                m_renaming.clear();
            } else if (res == TextEdit::Cancel || res == TextEdit::Inactive) {
                m_renaming.clear();
            }
        }
        if (h.hovered && !m_dragging) {
            std::string tip = e.path;
            if (e.type != AssetType::Folder) tip += "\n" + std::string(editor::assetTypeName(e.type)) + ", " + editor::sizeLabel(e.size);
            if (!e.madeBy.empty()) tip += "\nWritten by the import of " + e.madeBy;
            if (used) tip += "\nUsed by " + std::to_string(used) + (used == 1 ? " field" : " fields");
            tooltip(tip.c_str());
        }
        ImGui::PopID();
    }
    // A click on empty space clears the selection; a right-click opens the folder's menu.
    if (hoverArea && !overItem) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_selected.clear();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            m_selected.clear();
            m_menuFor.clear();
            m_openMenu = true;
            m_menuPos = io.MousePos;
        }
    }
    ImGui::PopID();
    ImGui::PopClipRect();
}

void AssetBrowser::footer(ImDrawList* dl, ImRect r) {
    Fonts& f = fonts();
    dl->AddLine(ImVec2(r.Min.x, r.Min.y), ImVec2(r.Max.x, r.Min.y), color::border);
    const float y = centerY(f.regular, type::label, r.Min.y, r.Max.y);
    std::string s;
    ImU32 col = color::textFaint;
    if (const AssetEntry* e = m_index.find(m_selected)) {
        s = e->path;
        if (e->type != AssetType::Folder) {
            s += "  \xc2\xb7  " + editor::sizeLabel(e->size);
            const std::vector<editor::AssetRef> refs = m_refs.to(m_index, e->path);
            if (!refs.empty()) {
                const editor::AssetRef& a = refs.front();
                s += "  \xc2\xb7  Used by " + (a.object.empty() ? a.field : a.object) + (refs.size() > 1 ? " and " + std::to_string(refs.size() - 1) + " more" : "");
            } else if (!e->madeBy.empty()) {
                s += "  \xc2\xb7  Written by the import of " + e->madeBy;
            } else {
                s += "  \xc2\xb7  Not used by any scene";
            }
        }
        col = color::textDim;
    } else {
        s = std::to_string(m_shown.size()) + (m_shown.size() == 1 ? " item" : " items");
        if (!m_query.empty()) s += " matching \"" + m_query + "\" in " + (m_folder.empty() ? std::string("the project") : m_folder);
        s += "  \xc2\xb7  Drag a file onto the scene or an inspector field";
    }
    s = fit(f.regular, type::label, s, r.GetWidth() - space::md * 2);
    text(dl, ImVec2(r.Min.x + space::md, y), f.regular, type::label, col, s.c_str());
}

// --- actions ---

void AssetBrowser::openEntry(const AssetEntry& e) {
    if (e.type == AssetType::Folder) {
        m_folder = e.path;
        m_query.clear();
        m_types = 0;
        m_scroll = 0;
        for (std::string a = e.path; !a.empty();) {
            const size_t s = a.rfind('/');
            a = s == std::string::npos ? std::string() : a.substr(0, s);
            m_collapsed[a] = false;
        }
        return;
    }
    m_requests.push_back({Request::Open, e.path, e.type, {}, {}, {}, false});
}

void AssetBrowser::after(editor::Document& doc, const editor::AssetOp& op, const std::string& done) {
    if (!op.error.empty()) {
        notice(op.error, true);
        return;
    }
    m_index.scan(m_index.project());
    m_scannedAt = ImGui::GetTime();
    m_refs.update(m_index, doc.scene(), m_openScene, doc.revision());
    // Files the document may have looked for, under either name.
    std::vector<std::string> recheck;
    for (const std::string& p : op.touched) {
        const AssetType t = editor::classifyAsset(fs::path(p));
        if (t == AssetType::Mesh || t == AssetType::Texture || t == AssetType::Script || t == AssetType::Audio || t == AssetType::Animation)
            recheck.push_back(p);
        m_thumbs.erase(p);
    }
    if (!recheck.empty()) doc.refreshAssets(recheck);
    notice(done, false);
}

void AssetBrowser::moveInto(editor::Document& doc, const std::string& asset, const std::string& folder) {
    const std::string name = baseName(asset);
    editor::AssetOp op = editor::applyAssetPlan(doc, m_index, m_openScene, editor::planMove(m_index, m_refs, asset, folder, m_openScene));
    if (!op.error.empty()) op.error = "Could not move " + name + ": " + op.error;
    if (op.error.empty() && op.touched.empty()) return;  // already there
    after(doc, op, "Moved " + name + " to " + (folder.empty() ? projectName(m_index.project()) : folder) + updatedNote(op));
    if (op.error.empty()) {
        if (m_folder == asset || m_folder.rfind(asset + "/", 0) == 0) m_folder = op.path + m_folder.substr(asset.size());
        m_selected = op.path;
    }
}

bool AssetBrowser::holdsOpenScene(const AssetEntry& e) const {
    return !m_openScene.empty() && (e.path == m_openScene || (e.type == AssetType::Folder && m_openScene.rfind(e.path + "/", 0) == 0));
}

void AssetBrowser::commitRename(editor::Document& doc, const std::string& typed) {
    const AssetEntry* e = m_index.find(m_renaming);
    if (!e) return;
    const std::string old = e->name;
    if (typed == stemOf(*e) || typed == e->name) return;
    editor::AssetOp op = editor::applyAssetPlan(doc, m_index, m_openScene, editor::planRename(m_index, m_refs, m_renaming, typed, m_openScene));
    if (!op.error.empty()) op.error = "Could not rename " + old + ": " + op.error;
    after(doc, op, "Renamed " + old + " to " + baseName(op.path) + updatedNote(op));
    if (op.error.empty()) {
        if (m_folder == m_renaming) m_folder = op.path;
        m_selected = op.path;
    }
}

void AssetBrowser::menu(editor::Document& doc) {
    static const char* id = "##assetmenu";
    if (m_openMenu) {
        m_openMenu = false;
        ImGui::OpenPopup(id);
    }
    if (!ImGui::IsPopupOpen(id)) return;
    const AssetEntry* e = m_menuFor.empty() ? nullptr : m_index.find(m_menuFor);
    const fs::path project = m_index.project();
    std::vector<MenuItem> items;
    if (e) {
        const std::string path = e->path;
        const AssetType t = e->type;
        const std::vector<editor::AssetRef> refs = m_refs.to(m_index, path);
        const char* openLabel = t == AssetType::Folder   ? "Open"
                                : t == AssetType::Script ? "Edit script"
                                : t == AssetType::Scene  ? "Open scene"
                                : t == AssetType::Model || t == AssetType::Mesh ? "Add to scene"
                                                                                : "Open in default app";
        items.push_back({openLabel, "Enter", t != AssetType::Scene || path != m_openScene, [this, path] {
                             if (const AssetEntry* x = m_index.find(path)) openEntry(*x);
                         }});
        const editor::AssetRef* inScene = nullptr;
        for (const editor::AssetRef& r : refs)
            if (r.scene == m_openScene && !r.path.empty()) {
                inScene = &r;
                break;
            }
        if (t != AssetType::Folder) {
            std::vector<int> obj = inScene ? inScene->path : std::vector<int>{};
            items.push_back({"Find in scene", "", inScene != nullptr,
                             [this, obj] { m_requests.push_back({Request::Select, {}, {}, {}, obj, {}, false}); }});
        }
        std::error_code ec;
        if (t == AssetType::Model && e->folder == "models" && fs::exists(absPath(project, path + ".import.json"), ec))
            items.push_back({"Reimport", "", true, [this, path] { m_requests.push_back({Request::Reimport, path, AssetType::Model, {}, {}, {}, false}); }});
        items.push_back({});
        const bool generated = !e->madeBy.empty();
        const bool holdsOpen = holdsOpenScene(*e);
        items.push_back({"Rename", "F2", true, [this, path, generated, holdsOpen] {
                             if (generated || holdsOpen) {
                                 m_ask = Ask::CannotRename;
                                 m_askFor = path;
                                 m_openAsk = true;
                             } else {
                                 m_renaming = path;
                                 m_renameStart = true;
                             }
                         }});
        items.push_back({"Delete", "Del", true, [this, path, refs, holdsOpen] {
                             m_askFor = path;
                             m_ask = refs.empty() && !holdsOpen ? Ask::Delete : Ask::CannotDelete;
                             m_openAsk = true;
                         }});
        items.push_back({});
        const std::string folder = e->folder;
        items.push_back({"Reveal in file manager", "", true, [project, folder] {
                             SDL_OpenURL(editor::fileUrl(absPath(project, folder)).c_str());
                         }});
        items.push_back({"Copy path", "", true, [path] { SDL_SetClipboardText(path.c_str()); }});
    } else {
        const std::string folder = m_folder;
        items.push_back({"New folder", "", m_query.empty() && !m_types, [this, &doc, project, folder] {
                             std::string name = "New Folder";
                             std::error_code ec;
                             for (int n = 2; fs::exists(absPath(project, folder.empty() ? name : folder + "/" + name), ec); ++n)
                                 name = "New Folder " + std::to_string(n);
                             const std::string rel = folder.empty() ? name : folder + "/" + name;
                             fs::create_directory(absPath(project, rel), ec);
                             if (ec) {
                                 notice("Could not create " + rel + ": " + ec.message(), true);
                                 return;
                             }
                             m_index.scan(project);
                             m_refs.update(m_index, doc.scene(), m_openScene, doc.revision());
                             m_selected = rel;
                             m_renaming = rel;
                             m_renameStart = true;
                         }});
        // A script here, with the starter an object's script gets, renamed straight away.
        auto newScript = [this, &doc, project, folder](editor::ScriptLanguage lang) {
            const std::string text = lang == editor::ScriptLanguage::Blocks
                                         ? editor::blocksTemplate(editor::ScriptKind::Object)
                                         : editor::scriptTemplate("New Script", editor::ScriptKind::Object);
            const editor::NewScript ns = editor::writeNewScript(project, folder, "new_script", lang, text);
            if (!ns.error.empty()) {
                notice("Could not create a script: " + ns.error, true);
                return;
            }
            m_index.scan(project);
            m_refs.update(m_index, doc.scene(), m_openScene, doc.revision());
            m_selected = ns.path;
            m_renaming = ns.path;
            m_renameStart = true;
        };
        items.push_back({"New blocks script", "", m_query.empty() && !m_types,
                         [newScript] { newScript(editor::ScriptLanguage::Blocks); }});
        items.push_back({"New Lua script", "", m_query.empty() && !m_types,
                         [newScript] { newScript(editor::ScriptLanguage::Lua); }});
        items.push_back({"Reveal in file manager", "", true, [project, folder] {
                             SDL_OpenURL(editor::fileUrl(absPath(project, folder)).c_str());
                         }});
        items.push_back({"Refresh", "", true, [this] { m_scannedAt = -1; }});
    }
    float w, h;
    menuSize(items, &w, &h);
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    ImVec2 pos = m_menuPos;
    if (pos.x + w > d.x - space::xs) pos.x -= w;
    if (pos.y + h > d.y - space::xs) pos.y -= h;
    pos.x = std::max(pos.x, space::xs);
    pos.y = std::max(pos.y, space::xs);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    const bool shown = ImGui::BeginPopup(id, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!shown) return;
    std::function<void()> run = menuRows(items, w, h, "assets.menu");
    if (run || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (run) run();
}

// Delete's confirmation, or why a rename or delete is refused, with what uses the file.
void AssetBrowser::prompt(editor::Document& doc) {
    static const char* id = "##assetprompt";
    if (m_openAsk) {
        m_openAsk = false;
        ImGui::OpenPopup(id);
    }
    if (m_ask == Ask::None) return;
    const AssetEntry* e = m_index.find(m_askFor);
    const float w = 460;
    ImVec2 d = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::lg, space::lg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool shown = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!shown) {
        m_ask = Ask::None;
        return;
    }
    if (!e) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        m_ask = Ask::None;
        return;
    }
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float inner = w - space::lg * 2;
    float y = o.y;
    const std::vector<editor::AssetRef> refs = m_refs.to(m_index, e->path);
    std::vector<std::string> lines;
    std::string title, body;
    if (m_ask == Ask::Delete) {
        title = "Delete " + e->name + "?";
        const std::vector<std::string> set = editor::deletionSet(m_index, e->path);
        body = std::string(set.size() > 1 ? "These files go" : "The file goes") +
               " to the project's .trash folder. Undo (Ctrl+Z) brings " + (set.size() > 1 ? "them" : "it") + " back.";
        if (set.size() > 1)
            for (const std::string& p : set) lines.push_back(p);
    } else {
        title = "Cannot " + std::string(m_ask == Ask::CannotRename ? "rename " : "delete ") + e->name;
        if (holdsOpenScene(*e)) {
            body = std::string(e->type == AssetType::Folder ? "It holds the open scene" : "It is the open scene") +
                   (m_ask == Ask::CannotRename ? ", whose file the editor keeps open. Use Save As to give the scene another name."
                                               : ", whose file the editor keeps open. Open another scene first.");
        } else if (!e->madeBy.empty() && m_ask == Ask::CannotRename) {
            body = "The import of " + e->madeBy + " wrote it and writes it again on every import. Rename the model instead.";
        } else {
            body = std::string("Deleting it would break ") +
                   (refs.size() == 1 ? "this reference" : "these references") + ". Point " + (refs.size() == 1 ? "it" : "them") +
                   " at another file first.";
            for (const editor::AssetRef& r : refs)
                lines.push_back((r.object.empty() ? std::string("Scene settings") : r.object) + "  \xc2\xb7  " + r.field + "  \xc2\xb7  " + r.scene);
        }
    }
    text(dl, ImVec2(o.x, y), f.semibold, type::title, color::text, fit(f.semibold, type::title, title, inner).c_str());
    y += 26;
    // The body wraps at the prompt's width.
    {
        std::string line, word;
        auto flush = [&]() {
            text(dl, ImVec2(o.x, y), f.regular, type::label, color::textDim, line.c_str());
            y += 18;
            line.clear();
        };
        for (size_t i = 0; i <= body.size(); ++i) {
            if (i == body.size() || body[i] == ' ') {
                const std::string next = line.empty() ? word : line + " " + word;
                if (!line.empty() && measure(f.regular, type::label, next.c_str()).x > inner) {
                    flush();
                    line = word;
                } else {
                    line = next;
                }
                word.clear();
            } else {
                word += body[i];
            }
        }
        if (!line.empty()) flush();
    }
    if (!lines.empty()) {
        y += space::xs;
        const size_t maxLines = 6;
        const float boxH = static_cast<float>(std::min(lines.size(), maxLines + (lines.size() > maxLines ? 1 : 0))) * 20.0f + space::sm * 2;
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + inner, y + boxH), color::field, radius::field);
        float ly = y + space::sm;
        for (size_t i = 0; i < lines.size() && i < maxLines; ++i, ly += 20)
            text(dl, ImVec2(o.x + space::sm, ly), f.regular, type::label, color::text, fit(f.regular, type::label, lines[i], inner - space::md).c_str());
        if (lines.size() > maxLines)
            text(dl, ImVec2(o.x + space::sm, ly), f.regular, type::label, color::textFaint,
                 ("and " + std::to_string(lines.size() - maxLines) + " more").c_str());
        y += boxH;
    }
    y += space::lg;
    auto width = [&](const char* l) { return space::md * 2 + measure(f.medium, type::body, l).x; };
    int done = 0;  // 1 confirm, 2 cancel, 3 show in scene
    float bx = o.x + inner;
    if (m_ask == Ask::Delete) {
        bx -= width("Delete");
        if (button("delete", ImVec2(bx, y), nullptr, "Delete", ButtonKind::Primary) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) done = 1;
        if (button("cancel", ImVec2(o.x, y), nullptr, "Cancel", ButtonKind::Ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = 2;
    } else {
        bx -= width("OK");
        if (button("ok", ImVec2(bx, y), nullptr, "OK", ButtonKind::Primary) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            done = 2;
        const editor::AssetRef* inScene = nullptr;
        for (const editor::AssetRef& r : refs)
            if (r.scene == m_openScene && !r.path.empty()) {
                inScene = &r;
                break;
            }
        if (inScene && button("show", ImVec2(o.x, y), nullptr, "Show in scene", ButtonKind::Secondary)) {
            m_requests.push_back({Request::Select, {}, {}, {}, inScene->path, {}, false});
            done = 3;
        }
    }
    y += size::field + 6;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner, y - o.y));
    if (done) {
        ImGui::CloseCurrentPopup();
        if (done == 1) {
            const std::string name = e->name;
            editor::AssetOp op = editor::applyAssetPlan(doc, m_index, m_openScene, editor::planDelete(m_index, m_refs, e->path, m_openScene));
            if (!op.error.empty()) op.error = "Could not delete " + name + ": " + op.error;
            after(doc, op, "Moved " + name + " to the project's .trash folder (Ctrl+Z brings it back)");
            if (op.error.empty()) m_selected.clear();
        }
        m_ask = Ask::None;
    }
    ImGui::EndPopup();
}

// --- thumbnails ---

unsigned AssetBrowser::thumbnail(const AssetEntry& e, int* w, int* h) {
    Thumb& t = m_thumbs[e.path];
    if ((t.tex || t.failed) && t.mtime != e.mtime) {
        if (t.tex) glDeleteTextures(1, &t.tex);
        t = Thumb{};
    }
    if (!t.tex && !t.failed && m_thumbLoads < 4) {
        ++m_thumbLoads;
        t.mtime = e.mtime;
        int n = 0;
        const fs::path file = absPath(m_index.project(), e.path);
        std::u8string u = file.u8string();
        // Top row first, the way the file stores it: ImGui draws with v down.
        stbi_set_flip_vertically_on_load_thread(0);
        unsigned char* px = stbi_load(std::string(u.begin(), u.end()).c_str(), &t.w, &t.h, &n, 4);
        if (!px) {
            t.failed = true;
        } else {
            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            // PS1 art: nearest, so a 64 px texture stays crisp at thumbnail size.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            stbi_image_free(px);
            t.tex = id;
        }
    }
    *w = t.w;
    *h = t.h;
    return t.tex;
}

void AssetBrowser::dropThumbnails() {
    for (auto& [p, t] : m_thumbs)
        if (t.tex) glDeleteTextures(1, &t.tex);
    m_thumbs.clear();
}

}  // namespace ui
