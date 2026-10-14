#include "editor/timeline_view.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <set>

#include "editor/document.hh"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
using editor::KeyRef;
using editor::KeyValue;
using editor::SeqKind;
using editor::SeqRef;
using splash::TrackType;

namespace {

constexpr float kHeaderH = 46, kRulerH = 26, kLaneH = 40, kKeyRowH = 46, kLabelW = 240, kDiamond = 6, kPad = 12;

ImU32 trackColor(TrackType t) {
    const int v = int(t);
    if (t == TrackType::CameraPosition || t == TrackType::CameraRotation || t == TrackType::CameraH) return rgb(0x5ccfe0);
    if (v >= 5 && v <= 9) return rgb(0xd98ad2);
    if (v >= 14) return color::warn;
    if (t == TrackType::RumbleSmall || t == TrackType::RumbleLarge) return color::textDim;
    return color::good;
}

ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00ffffffu) | (ImU32(a) << IM_COL32_A_SHIFT); }

std::string fmtValue(TrackType t, const KeyValue& v) {
    if (editor::isDiscrete(t)) return v[0] > 0.5f ? "on" : "off";
    char buf[96];
    const int n = editor::valueCount(t);
    if (n == 1)
        std::snprintf(buf, sizeof buf, "%.2f", v[0]);
    else if (n == 2)
        std::snprintf(buf, sizeof buf, "%.2f, %.2f", v[0], v[1]);
    else
        std::snprintf(buf, sizeof buf, "%.2f, %.2f, %.2f", v[0], v[1], v[2]);
    return buf;
}

std::string fmtNum(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

const char* kInterpNames[] = {"Linear", "Step", "Ease in", "Ease out", "Ease in-out"};

// Frames between labelled ruler ticks: the first step that leaves room for a label.
int tickStep(float ppf) {
    static const int steps[] = {1, 2, 5, 10, 15, 30, 60, 90, 150, 300, 600, 900, 1800, 3600};
    for (int s : steps)
        if (s * ppf >= 56) return s;
    return 3600;
}

void pushPopupStyle() {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color::hover);
    ImGui::PushStyleColor(ImGuiCol_Header, color::active);
    ImGui::PushStyleColor(ImGuiCol_Text, color::text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, color::textFaint);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::sm, space::sm));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(space::sm, space::xs + 2));
    ImGui::PushFont(fonts().medium, type::label);
}

void popPopupStyle() {
    ImGui::PopFont();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(6);
}

// Transport buttons drawn as shapes: 0 play, 1 pause, 2 back to start.
bool transport(const char* id, ImRect r, int shape, const char* tip, bool on = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Hit h = interact(id, r);
    if (on || h.hover > 0)
        dl->AddRectFilled(r.Min, r.Max, on ? color::active : withAlpha(color::hover, int(255 * h.hover)), radius::button);
    if (tip) tooltip(tip);
    const ImVec2 c = r.GetCenter();
    const ImU32 col = lerpColor(color::textDim, color::text, std::max(h.hover, on ? 1.0f : 0.0f));
    if (shape == 0) {
        dl->AddTriangleFilled(c + ImVec2(-4, -6), c + ImVec2(-4, 6), c + ImVec2(6, 0), col);
    } else if (shape == 1) {
        dl->AddRectFilled(c + ImVec2(-5, -6), c + ImVec2(-1, 6), col, 1);
        dl->AddRectFilled(c + ImVec2(1, -6), c + ImVec2(5, 6), col, 1);
    } else {
        dl->AddRectFilled(c + ImVec2(-6, -6), c + ImVec2(-4, 6), col);
        dl->AddTriangleFilled(c + ImVec2(5, -6), c + ImVec2(5, 6), c + ImVec2(-3, 0), col);
    }
    return h.clicked;
}

void diamond(ImDrawList* dl, ImVec2 c, float s, ImU32 fill, ImU32 edge, float thick) {
    const ImVec2 p[4] = {c + ImVec2(0, -s), c + ImVec2(s, 0), c + ImVec2(0, s), c + ImVec2(-s, 0)};
    dl->AddConvexPolyFilled(p, 4, fill);
    dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, thick);
}

}  // namespace

void TimelinePanel::show(const editor::Document& doc, SeqRef ref) {
    open = true;
    ++shows;
    m_ref = ref;
    m_loadId = doc.loadId();
    m_frame = 0;
    m_playing = false;
    m_sel.clear();
    m_scrollY = 0;
    m_fitPending = true;
}

void TimelinePanel::validate(const editor::Document& doc) {
    if (doc.loadId() != m_loadId) {
        // Another scene: start on its first sequence.
        m_loadId = doc.loadId();
        m_ref = {};
        m_sel.clear();
        m_frame = 0;
        m_playing = false;
        m_fitPending = true;
    }
    if (!editor::valid(doc, m_ref)) {
        const std::vector<SeqRef> all = editor::allSequences(doc);
        m_ref = all.empty() ? SeqRef{} : all.front();
        m_sel.clear();
        m_fitPending = true;
    }
    std::erase_if(m_sel, [&](const KeyRef& k) { return !editor::key(doc, m_ref, k); });
}

std::vector<TimelinePanel::Sampled> TimelinePanel::sampled(const editor::Document& doc) const {
    std::vector<Sampled> out;
    const std::vector<splash::CutsceneTrack>* tracks = editor::seqTracks(doc, m_ref);
    if (!tracks) return out;
    for (int i = 0; i < int(tracks->size()); ++i) {
        const splash::CutsceneTrack& t = (*tracks)[size_t(i)];
        out.push_back({i, t.type, t.target, editor::sampleAt(doc, m_ref, i, m_frame)});
    }
    return out;
}

editor::Preview TimelinePanel::preview(const editor::Document& doc) const {
    // Closed, or not yet validated against a newly loaded scene: no preview.
    if (!open || doc.loadId() != m_loadId || !editor::valid(doc, m_ref)) return {};
    return editor::samplePreview(doc, m_ref, m_frame);
}

void TimelinePanel::takeKeys(editor::Document& doc) {
    if (!open || !m_focused) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    validate(doc);
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !m_sel.empty()) {
        editor::deleteKeys(doc, m_ref, m_sel);
        m_sel.clear();
    }
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) && editor::valid(doc, m_ref)) m_playing = !m_playing;
    // The keys belong to the timeline this frame: the scene's Delete stays out.
    const ImGuiID owner = ImHashStr("timeline-keys");
    ImGui::SetKeyOwner(ImGuiKey_Delete, owner, ImGuiInputFlags_LockThisFrame);
    ImGui::SetKeyOwner(ImGuiKey_Space, owner, ImGuiInputFlags_LockThisFrame);
}

ImRect TimelinePanel::dock(ImRect area) {
    if (!open) return area;
    const float g = size::gutter;
    const float hi = std::max(180.0f, area.GetHeight() - 200);
    m_height = std::clamp(m_height, 180.0f, hi);
    ImRect bar(ImVec2(area.Min.x, area.Max.y - m_height - g), ImVec2(area.Max.x, area.Max.y - m_height));
    ImGui::SetCursorScreenPos(bar.Min);
    ImGui::InvisibleButton("##timelinesplit", ImMax(bar.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) m_height = std::clamp(m_height - ImGui::GetIO().MouseDelta.y, 180.0f, hi);
    m_rect = ImRect(ImVec2(area.Min.x, area.Max.y - m_height), area.Max);
    return ImRect(area.Min, ImVec2(area.Max.x, bar.Min.y));
}

float TimelinePanel::xOf(float frame) const { return m_bodyX0 + (frame - m_view0) * m_ppf; }
float TimelinePanel::frameAt(float x) const { return m_view0 + (x - m_bodyX0) / m_ppf; }

void TimelinePanel::fit(int length) {
    // Room after the end for its ruler label.
    m_ppf = std::max(0.2f, (m_bodyX1 - m_bodyX0 - 28) / float(std::max(length, 1)));
    m_view0 = 0;
    m_fitPending = false;
}

void TimelinePanel::draw(ImDrawList* dl, editor::Document& doc) {
    if (!open) return;
    validate(doc);
    const ImRect r = m_rect;
    ImGuiIO& io = ImGui::GetIO();
    // The panel has the keyboard from a click in it until a click elsewhere.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (r.Contains(io.MousePos))
            m_focused = true;
        else if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            m_focused = false;
    }
    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window);
    dl->AddRect(r.Min, r.Max, m_focused ? withAlpha(color::accent, 90) : color::border, radius::window);
    ImGui::PushID("timeline");
    ImGui::PushClipRect(r.Min, r.Max, true);

    const int len = editor::seqLength(doc, m_ref);
    if (m_playing && len > 0) {
        m_frame += io.DeltaTime * editor::kTimelineFps;
        if (m_frame > float(len)) {
            if (m_loop) {
                m_frame = 0;
            } else {
                m_frame = float(len);
                m_playing = false;
            }
        }
    }

    const ImRect head(r.Min, ImVec2(r.Max.x, r.Min.y + kHeaderH));
    header(dl, head, doc);
    if (editor::valid(doc, m_ref)) {
        const float top = head.Max.y;
        const ImRect keyRowR(ImVec2(r.Min.x, r.Max.y - kKeyRowH), r.Max);
        const ImRect rulerR(ImVec2(r.Min.x + kLabelW, top), ImVec2(r.Max.x - space::xs, top + kRulerH));
        const ImRect labels(ImVec2(r.Min.x + 1, top + kRulerH), ImVec2(r.Min.x + kLabelW, keyRowR.Min.y));
        const ImRect body(ImVec2(r.Min.x + kLabelW, top + kRulerH), ImVec2(r.Max.x - space::xs, keyRowR.Min.y));
        m_bodyX0 = body.Min.x + kPad;
        m_bodyX1 = body.Max.x - kPad;
        if (m_fitPending) fit(len);
        // Corner over the labels: how many tracks of the runtime's eight.
        const ImRect corner(ImVec2(r.Min.x + 1, top), ImVec2(r.Min.x + kLabelW, top + kRulerH));
        dl->AddRectFilled(corner.Min, corner.Max, rgb(0x1f2229));
        char tc[48];
        std::snprintf(tc, sizeof tc, "TRACKS  %d / %d", int(editor::seqTracks(doc, m_ref)->size()), editor::kMaxTimelineTracks);
        text(dl, ImVec2(corner.Min.x + space::md, corner.Min.y + 6), fonts().medium, type::caption, color::textFaint, tc);
        ruler(dl, rulerR, len);
        lanes(dl, labels, body, doc);
        keyRow(dl, keyRowR, doc);
    }
    // The note (a refused rename, a delete), over the lanes' bottom right.
    if (ImGui::GetTime() < m_noteUntil) {
        const ImVec2 ts = measure(fonts().medium, type::label, m_note.c_str());
        const ImVec2 mx(r.Max.x - space::md, r.Max.y - kKeyRowH - space::sm);
        const ImRect pill(mx - ts - ImVec2(space::md * 2, space::sm * 2), mx);
        dl->AddRectFilled(pill.Min, pill.Max, color::raised, radius::field);
        dl->AddRect(pill.Min, pill.Max, color::warn, radius::field);
        text(dl, pill.Min + ImVec2(space::md, space::sm), fonts().medium, type::label, color::text, m_note.c_str());
    }
    ImGui::PopClipRect();
    ImGui::PopID();
}

void TimelinePanel::header(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    dl->AddLine(ImVec2(r.Min.x, r.Max.y - 0.5f), ImVec2(r.Max.x, r.Max.y - 0.5f), color::border);
    const float cy = r.GetCenter().y;
    float x = r.Min.x + space::md;
    if (tabs) {
        tabs(dl, ImRect(r.Min, ImVec2(r.Min.x + tabsWidth, r.Max.y)));
        x = r.Min.x + tabsWidth + space::md;
    } else {
        textCentered(dl, ImRect(ImVec2(x, r.Min.y), ImVec2(x + 18, r.Max.y)), f.medium, type::icon, color::accentHover, icon::layers);
        x += 18 + space::sm;
        text(dl, ImVec2(x, cy - 9), f.semibold, type::title, color::text, "Timeline");
        x += measure(f.semibold, type::title, "Timeline").x + space::lg;
    }

    // The sequence: a dropdown of every cutscene and animation.
    const bool have = editor::valid(doc, m_ref);
    {
        const ImRect dd(ImVec2(x, cy - 13), ImVec2(x + (tabs ? 176 : 210), cy + 13));
        std::string label = have ? *editor::seqName(doc, m_ref) : std::string("No cutscenes");
        const char* kindLabel = !have ? "" : m_ref.kind == SeqKind::Cutscene ? "CUTSCENE" : "ANIM";
        const float kw = measure(f.medium, type::caption, kindLabel).x;
        const float tx = dd.Min.x + space::sm + kw + (have ? space::sm : 0);
        // Renaming: the name field takes the dropdown's place (Enter commits, Esc cancels).
        std::string typed;
        const TextEdit te = have ? textEdit("seqname", ImRect(ImVec2(tx - space::xs, dd.Min.y), dd.Max), f.medium, type::label, tx,
                                            dd.Max.x - space::sm, &typed, 96)
                                 : TextEdit::Inactive;
        if (te == TextEdit::Commit) rename(doc, m_ref, typed);
        if (te == TextEdit::Inactive) {
            Hit h = interact("seq", dd);
            dl->AddRectFilled(dd.Min, dd.Max, h.hover > 0 ? color::hover : color::field, radius::field);
            dl->AddRect(dd.Min, dd.Max, color::borderStrong, radius::field);
            ImGui::PushClipRect(dd.Min, ImVec2(dd.Max.x - 22, dd.Max.y), true);
            text(dl, ImVec2(tx, cy - 9), f.medium, type::label, have ? color::text : color::textFaint, label.c_str());
            ImGui::PopClipRect();
            textCentered(dl, ImRect(ImVec2(dd.Max.x - 22, dd.Min.y), dd.Max), f.medium, type::caption + 1, color::textDim,
                         icon::chevronDown);
            if (h.clicked) ImGui::OpenPopup("##seqpick");
        }
        text(dl, ImVec2(dd.Min.x + space::sm, cy - 8), f.medium, type::caption, m_ref.kind == SeqKind::Cutscene ? rgb(0x5ccfe0) : color::good,
             kindLabel);
        // The panel sits at the bottom of the window: its popups open upwards.
        ImGui::SetNextWindowPos(ImVec2(dd.Min.x, dd.Min.y - 4), ImGuiCond_Always, ImVec2(0, 1));
        sequencePopup(doc);
        if (m_renameArm && have) beginTextEdit("seqname", label);
        m_renameArm = false;
        x = dd.Max.x + space::md;
    }

    // Close, at the right.
    if (iconButton("close", ImRect(ImVec2(r.Max.x - 34, cy - 13), ImVec2(r.Max.x - 8, cy + 13)), icon::x, false, "Close the timeline"))
        open = false;
    if (!have) {
        float w = 0;
        if (button("newcs", ImVec2(x, cy - 15), icon::plus, "New cutscene", ButtonKind::Secondary, &w))
            if (auto ref = editor::addSequence(doc, SeqKind::Cutscene, "cutscene")) show(doc, *ref);
        if (button("newanim", ImVec2(x + w + space::sm, cy - 15), icon::plus, "New animation", ButtonKind::Secondary))
            if (auto ref = editor::addSequence(doc, SeqKind::Animation, "animation")) show(doc, *ref);
        return;
    }
    const int len = editor::seqLength(doc, m_ref);

    // Transport.
    dl->AddLine(ImVec2(x - space::sm + 2, cy - 12), ImVec2(x - space::sm + 2, cy + 12), color::border);
    if (transport("start", ImRect(ImVec2(x, cy - 13), ImVec2(x + 28, cy + 13)), 2, "Back to frame 0")) {
        m_frame = 0;
        m_playing = false;
    }
    x += 30;
    if (transport("play", ImRect(ImVec2(x, cy - 13), ImVec2(x + 28, cy + 13)), m_playing ? 1 : 0, m_playing ? "Pause (Space)" : "Play (Space)",
                  m_playing)) {
        if (!m_playing && m_frame >= float(len)) m_frame = 0;
        m_playing = !m_playing;
    }
    x += 30;
    if (iconButton("loop", ImRect(ImVec2(x, cy - 13), ImVec2(x + 28, cy + 13)), icon::redo, m_loop,
                   m_loop ? "Loop: on (playback starts over at the end)" : "Loop: off"))
        m_loop = !m_loop;
    x += 28 + space::md;

    // Scrub frame, length and time.
    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "FRAME");
    x += measure(f.medium, type::caption, "FRAME").x + space::sm;
    {
        float v = std::round(m_frame);
        char b[16];
        std::snprintf(b, sizeof b, "%d", int(v));
        FieldEdit fe = numberField("frame", ImRect(ImVec2(x, cy - 12), ImVec2(x + 58, cy + 12)), b, nullptr, 0, nullptr, &v);
        if (fe.changed) {
            m_frame = std::clamp(std::round(v), 0.0f, float(len));
            m_playing = false;
        }
        x += 58 + space::md;
    }
    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "LENGTH");
    x += measure(f.medium, type::caption, "LENGTH").x + space::sm;
    {
        float v = float(len);
        char b[16];
        std::snprintf(b, sizeof b, "%d", len);
        FieldEdit fe = numberField("length", ImRect(ImVec2(x, cy - 12), ImVec2(x + 58, cy + 12)), b, nullptr, 0, nullptr, &v);
        if (fe.changed) editor::setLength(doc, m_ref, int(std::lround(v)), "timeline.length");
        if (fe.done) {
            doc.endMerge();
            m_fitPending = true;
        }
        x += 58 + space::md;
    }
    char tm[64];
    std::snprintf(tm, sizeof tm, "%.2f s / %.2f s", m_frame / editor::kTimelineFps, float(len) / editor::kTimelineFps);
    const float tmX = x;  // drawn once the right side is placed, where it fits

    // Right side: fit and add track.
    float rx = r.Max.x - 34 - space::sm;
    if (iconButton("fit", ImRect(ImVec2(rx - 28, cy - 13), ImVec2(rx, cy + 13)), icon::maximize, false,
                   "Fit the whole length in the panel (Ctrl+wheel zooms, wheel scrolls)"))
        m_fitPending = true;
    rx -= 28 + space::sm;
    const char* addLabel = "Add track";
    const float aw = space::md * 2 + measure(f.medium, type::icon, icon::plus).x + space::sm - 2 + measure(f.medium, type::body, addLabel).x;
    const bool full = int(editor::seqTracks(doc, m_ref)->size()) >= editor::kMaxTimelineTracks;
    if (button("addtrack", ImVec2(rx - aw, cy - 15), icon::plus, addLabel, ButtonKind::Secondary, nullptr,
               full ? "This sequence has the runtime's 8 tracks" : "A track drives one property of one target") &&
        !full)
        ImGui::OpenPopup("##addtrack");
    // The time in seconds where it fits left of Add track (the dock's tabs
    // take room on a narrow window): scrub and length, else the scrub alone.
    if (tmX + measure(f.regular, type::label, tm).x > rx - aw - space::md)
        std::snprintf(tm, sizeof tm, "%.2f s", m_frame / editor::kTimelineFps);  // the scrub's time alone
    if (tmX + measure(f.regular, type::label, tm).x <= rx - aw - space::md) text(dl, ImVec2(tmX, cy - 9), f.regular, type::label, color::textDim, tm);
    ImGui::SetNextWindowPos(ImVec2(rx - aw, cy - 19), ImGuiCond_Always, ImVec2(0, 1));
    addTrackPopup(doc);
}

void TimelinePanel::sequencePopup(editor::Document& doc) {
    pushPopupStyle();
    if (ImGui::BeginPopup("##seqpick")) {
        auto list = [&](SeqKind kind, const char* heading) {
            ImGui::TextDisabled("%s", heading);
            bool any = false;
            for (SeqRef ref : editor::allSequences(doc)) {
                if (ref.kind != kind) continue;
                any = true;
                char row[160];
                std::snprintf(row, sizeof row, "%s##%d", editor::seqName(doc, ref)->c_str(), ref.index);
                if (ImGui::Selectable(row, ref == m_ref, 0, ImVec2(220, 0))) show(doc, ref);
            }
            if (!any) ImGui::TextDisabled("  none");
        };
        list(SeqKind::Cutscene, "Cutscenes");
        list(SeqKind::Animation, "Animations");
        ImGui::Separator();
        if (ImGui::Selectable("New cutscene"))
            if (auto ref = editor::addSequence(doc, SeqKind::Cutscene, "cutscene")) show(doc, *ref);
        if (ImGui::Selectable("New animation"))
            if (auto ref = editor::addSequence(doc, SeqKind::Animation, "animation")) show(doc, *ref);
        if (editor::valid(doc, m_ref)) {
            ImGui::Separator();
            if (ImGui::Selectable("Rename...")) m_renameArm = true;
            const std::string del = "Delete '" + *editor::seqName(doc, m_ref) + "'";
            if (ImGui::Selectable(del.c_str())) remove(doc, m_ref);
        }
        ImGui::EndPopup();
    }
    popPopupStyle();
}

void TimelinePanel::note(std::string msg) {
    m_note = std::move(msg);
    m_noteUntil = ImGui::GetTime() + 4;
}

void TimelinePanel::rename(editor::Document& doc, SeqRef ref, const std::string& name) {
    if (!editor::valid(doc, ref) || name == *editor::seqName(doc, ref)) return;
    const std::string why = editor::seqNameProblem(doc, ref, name);
    if (!why.empty())
        note("Not renamed: " + why);
    else
        editor::renameSequence(doc, ref, name);
}

void TimelinePanel::remove(editor::Document& doc, SeqRef ref) {
    if (!editor::valid(doc, ref)) return;
    const std::string name = *editor::seqName(doc, ref);
    if (!editor::removeSequence(doc, ref)) return;
    note("Deleted '" + name + "' (Ctrl+Z brings it back)");
    if (ref == m_ref) {
        // On to whatever sits there now, or the first one.
        m_sel.clear();
        m_frame = 0;
        m_playing = false;
        m_fitPending = true;
    } else if (ref.kind == m_ref.kind && ref.index < m_ref.index) {
        --m_ref.index;  // the open one moved down
    }
}

void TimelinePanel::trackMenu(editor::Document& doc) {
    pushPopupStyle();
    if (ImGui::BeginPopup("##trackmenu")) {
        const std::vector<splash::CutsceneTrack>& tracks = *editor::seqTracks(doc, m_ref);
        const int ti = m_menuTrack;
        if (ti < 0 || ti >= int(tracks.size())) {
            ImGui::CloseCurrentPopup();
        } else {
            const splash::CutsceneTrack t = tracks[size_t(ti)];
            ImGui::TextDisabled("%s", editor::trackLabel(t).c_str());
            ImGui::Separator();
            if (editor::needsTarget(t.type)) {
                const std::vector<std::string> targets = editor::targetsFor(doc, t.type);
                if (ImGui::BeginMenu("Drive instead", !targets.empty())) {
                    for (const std::string& name : targets) {
                        const bool taken = std::any_of(tracks.begin(), tracks.end(),
                                                       [&](const auto& o) { return o.type == t.type && o.target == name; });
                        if (ImGui::MenuItem(name.c_str(), nullptr, name == t.target, !taken)) editor::setTrackTarget(doc, m_ref, ti, name);
                    }
                    ImGui::EndMenu();
                }
            } else {
                ImGui::MenuItem("Drive instead", nullptr, false, false);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Camera and rumble tracks have no target");
            }
            if (ImGui::MenuItem("Remove track")) {
                editor::removeTrack(doc, m_ref, ti);
                m_sel.clear();
            }
        }
        ImGui::EndPopup();
    } else {
        m_menuTrack = -1;
    }
    popPopupStyle();
}

void TimelinePanel::addTrackPopup(editor::Document& doc) {
    pushPopupStyle();
    if (ImGui::BeginPopup("##addtrack")) {
        const std::vector<splash::CutsceneTrack>& tracks = *editor::seqTracks(doc, m_ref);
        auto has = [&](TrackType t, const std::string& target) {
            return std::any_of(tracks.begin(), tracks.end(), [&](const auto& tr) { return tr.type == t && tr.target == target; });
        };
        auto add = [&](TrackType t, const std::string& target) {
            const int i = editor::addTrack(doc, m_ref, t, target);
            if (i >= 0) m_scrollY = std::max(0.0f, (i + 1) * kLaneH - 4 * kLaneH);
        };
        int lastGroup = -1;
        for (int ti = 0; ti <= int(TrackType::LightEnabled); ++ti) {
            const TrackType t = TrackType(ti);
            if (!editor::allowedIn(m_ref.kind, t)) continue;
            const int group = !editor::needsTarget(t) ? (ti >= 11 ? 4 : 0) : ti >= 14 ? 3 : (ti >= 5 && ti <= 9) ? 2 : 1;
            if (lastGroup >= 0 && group != lastGroup) ImGui::Separator();
            lastGroup = group;
            if (!editor::needsTarget(t)) {
                if (ImGui::MenuItem(editor::trackTypeName(t), nullptr, false, !has(t, ""))) add(t, "");
                continue;
            }
            // What the export can address: objects with a mesh, runtime point lights, canvases, elements.
            const std::vector<std::string> targets = editor::targetsFor(doc, t);
            if (ImGui::BeginMenu(editor::trackTypeName(t), !targets.empty())) {
                for (const std::string& name : targets)
                    if (ImGui::MenuItem(name.c_str(), nullptr, false, !has(t, name))) add(t, name);
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    popPopupStyle();
}

void TimelinePanel::ruler(ImDrawList* dl, ImRect r, int length) {
    Fonts& f = fonts();
    ImGuiIO& io = ImGui::GetIO();
    dl->AddRectFilled(r.Min, r.Max, rgb(0x1f2229));
    dl->AddLine(ImVec2(r.Min.x, r.Max.y - 0.5f), ImVec2(r.Max.x, r.Max.y - 0.5f), color::border);
    ImGui::PushClipRect(r.Min, r.Max, true);
    const float endX = xOf(float(length));
    if (endX < r.Max.x) dl->AddRectFilled(ImVec2(std::max(endX, r.Min.x), r.Min.y), r.Max, rgb(0x000000, 70));
    const int step = tickStep(m_ppf);
    const int minor = step % 5 == 0 ? step / 5 : step % 2 == 0 ? step / 2 : step;
    const int first = std::max(0, int(std::floor(frameAt(r.Min.x))) / minor * minor);
    for (int fr = first; xOf(float(fr)) <= r.Max.x; fr += minor) {
        const float x = std::floor(xOf(float(fr))) + 0.5f;
        if (fr % step == 0) {
            dl->AddLine(ImVec2(x, r.Min.y + 12), ImVec2(x, r.Max.y), color::textFaint);
            char b[16];
            std::snprintf(b, sizeof b, "%d", fr);
            text(dl, ImVec2(x + 3, r.Min.y + 2), f.regular, type::caption, fr > length ? color::textFaint : color::textDim, b);
        } else {
            dl->AddLine(ImVec2(x, r.Max.y - 6), ImVec2(x, r.Max.y), color::borderStrong);
        }
    }
    // The end of the sequence.
    if (endX >= r.Min.x && endX <= r.Max.x) dl->AddLine(ImVec2(endX, r.Min.y), ImVec2(endX, r.Max.y), color::bad, 1.5f);

    ImGui::SetCursorScreenPos(r.Min);
    ImGui::InvisibleButton("##ruler", ImMax(r.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemActive()) {
        m_frame = std::clamp(std::round(frameAt(io.MousePos.x)), 0.0f, float(length));
        m_playing = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // The scrub head: a pill with the frame number.
    const float hx = std::floor(xOf(m_frame)) + 0.5f;
    char b[16];
    std::snprintf(b, sizeof b, "%d", int(std::floor(m_frame)));
    const float w = measure(f.semibold, type::caption, b).x + space::sm * 2;
    const ImRect pill(ImVec2(hx - w / 2, r.Min.y + 3), ImVec2(hx + w / 2, r.Max.y - 5));
    dl->AddRectFilled(pill.Min, pill.Max, color::accent, radius::field);
    dl->AddTriangleFilled(ImVec2(hx - 4, pill.Max.y), ImVec2(hx + 4, pill.Max.y), ImVec2(hx, r.Max.y), color::accent);
    textCentered(dl, pill, f.semibold, type::caption, rgb(0xffffff), b);
    ImGui::PopClipRect();
}

void TimelinePanel::lanes(ImDrawList* dl, ImRect labels, ImRect body, editor::Document& doc) {
    Fonts& f = fonts();
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<splash::CutsceneTrack>& tracks = *editor::seqTracks(doc, m_ref);
    const int n = int(tracks.size());
    const int length = editor::seqLength(doc, m_ref);
    const float maxScroll = std::max(0.0f, n * kLaneH - body.GetHeight());

    // Wheel: Ctrl zooms about the mouse, Shift (or no room to scroll) pans, else scrolls the lanes.
    const ImRect area(labels.Min, ImVec2(body.Max.x, body.Max.y));
    const ImRect rulerArea(ImVec2(body.Min.x, body.Min.y - kRulerH), body.Max);
    if ((area.Contains(io.MousePos) || rulerArea.Contains(io.MousePos)) && io.MouseWheel != 0 &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (io.KeyCtrl) {
            const float at = frameAt(io.MousePos.x);
            m_ppf = std::clamp(m_ppf * std::pow(1.2f, io.MouseWheel), 0.2f, 40.0f);
            m_view0 = at - (io.MousePos.x - m_bodyX0) / m_ppf;
        } else if (io.KeyShift || maxScroll <= 0) {
            m_view0 -= io.MouseWheel * 40 / m_ppf;
        } else {
            m_scrollY -= io.MouseWheel * kLaneH;
        }
        m_view0 = std::clamp(m_view0, -kPad / m_ppf, float(editor::kMaxTimelineFrame));
    }
    if (io.MouseWheelH != 0 && area.Contains(io.MousePos)) m_view0 = std::max(0.0f, m_view0 - io.MouseWheelH * 40 / m_ppf);
    m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);

    std::set<int> selTracks;
    for (const KeyRef& k : m_sel) selTracks.insert(k.track);

    // Labels.
    dl->AddRectFilled(labels.Min, labels.Max, rgb(0x1d2026));
    dl->AddLine(ImVec2(labels.Max.x - 0.5f, labels.Min.y - kRulerH), ImVec2(labels.Max.x - 0.5f, labels.Max.y), color::border);
    ImGui::PushClipRect(labels.Min, labels.Max, true);
    int removeTrack = -1;
    for (int i = 0; i < n; ++i) {
        const splash::CutsceneTrack& t = tracks[size_t(i)];
        const float y = labels.Min.y + i * kLaneH - m_scrollY;
        if (y + kLaneH < labels.Min.y || y > labels.Max.y) continue;
        const ImRect row(ImVec2(labels.Min.x, y), ImVec2(labels.Max.x - 1, y + kLaneH));
        if (selTracks.count(i)) dl->AddRectFilled(row.Min, row.Max, rgb(0x8b7bff, 22));
        dl->AddLine(ImVec2(row.Min.x, row.Max.y - 0.5f), ImVec2(row.Max.x, row.Max.y - 0.5f), color::border);
        const ImU32 tc = trackColor(t.type);
        dl->AddRectFilled(ImVec2(row.Min.x + 6, y + 8), ImVec2(row.Min.x + 9, y + kLaneH - 8), tc, 2);
        const std::string who = t.target.empty() ? (t.type == TrackType::RumbleSmall || t.type == TrackType::RumbleLarge ? "Rumble" : "Camera")
                                                 : t.target;
        const float tx = row.Min.x + 18;
        ImGui::PushClipRect(row.Min, ImVec2(row.Max.x - 54, row.Max.y), true);
        text(dl, ImVec2(tx, y + 4), f.medium, type::label, color::text, who.c_str());
        ImGui::PopClipRect();
        const std::string full = editor::trackLabel(t);
        const char* prop = full.c_str() + full.find(": ") + 2;
        text(dl, ImVec2(tx, y + 21), f.regular, type::caption, tc, prop);
        // What the track samples at the scrub head, as the console rounds it.
        // UI and rumble tracks have nothing in the Scene view: this is their preview.
        const std::string val = t.keyframes.empty() ? std::string("no keys") : fmtValue(t.type, editor::sampleAt(doc, m_ref, i, m_frame));
        const float vw = measure(f.regular, type::caption, val.c_str()).x;
        const float px = tx + measure(f.regular, type::caption, prop).x + space::sm;
        const float vx = std::max(px, row.Max.x - space::sm - vw);
        ImGui::PushClipRect(ImVec2(px, row.Min.y), row.Max, true);
        text(dl, ImVec2(vx, y + 21), f.regular, type::caption, color::textFaint, val.c_str());
        ImGui::PopClipRect();
        ImGui::PushID(i);
        const bool hovered = row.Contains(io.MousePos);
        const ImU32 btn = hovered || m_menuTrack == i ? color::textDim : withAlpha(color::textFaint, 110);
        if (iconButton("rm", ImRect(ImVec2(row.Max.x - 28, y + 4), ImVec2(row.Max.x - 6, y + 20)), icon::x, false, "Remove this track", btn))
            removeTrack = i;
        bool menu = iconButton("menu", ImRect(ImVec2(row.Max.x - 52, y + 4), ImVec2(row.Max.x - 30, y + 20)), icon::ellipsis, m_menuTrack == i,
                               "Retarget or remove this track (or right-click the lane name)", btn);
        ImGui::PopID();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) menu = true;
        if (menu) {
            m_menuTrack = i;
            ImGui::OpenPopup("##trackmenu");
            ImGui::SetNextWindowPos(ImVec2(row.Min.x + 18, y + kLaneH), ImGuiCond_Always);
        }
    }
    ImGui::PopClipRect();
    trackMenu(doc);

    // Lanes.
    dl->AddRectFilled(body.Min, body.Max, rgb(0x16181d));
    ImGui::PushClipRect(body.Min, body.Max, true);
    const float endX = xOf(float(length));
    const int step = tickStep(m_ppf);
    for (int fr = std::max(0, int(std::floor(frameAt(body.Min.x))) / step * step); xOf(float(fr)) <= body.Max.x; fr += step) {
        const float x = std::floor(xOf(float(fr))) + 0.5f;
        dl->AddLine(ImVec2(x, body.Min.y), ImVec2(x, body.Max.y), rgb(0xffffff, 9));
    }
    if (endX < body.Max.x) dl->AddRectFilled(ImVec2(std::max(endX, body.Min.x), body.Min.y), body.Max, rgb(0x000000, 80));

    // What the mouse is over: a lane and the nearest key in reach.
    const int laneAt = body.Contains(io.MousePos) ? int(std::floor((io.MousePos.y - body.Min.y + m_scrollY) / kLaneH)) : -1;
    std::optional<KeyRef> hit;
    if (laneAt >= 0 && laneAt < n) {
        float best = kDiamond + 4;
        const auto& ks = tracks[size_t(laneAt)].keyframes;
        for (int k = 0; k < int(ks.size()); ++k) {
            const float d = std::fabs(xOf(float(ks[size_t(k)].frame)) - io.MousePos.x);
            if (d <= best) {
                best = d;
                hit = KeyRef{laneAt, k};
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        const splash::CutsceneTrack& t = tracks[size_t(i)];
        const float y = body.Min.y + i * kLaneH - m_scrollY;
        if (y + kLaneH < body.Min.y || y > body.Max.y) continue;
        const float cy = std::floor(y + kLaneH / 2) + 0.5f;
        if (i == laneAt) dl->AddRectFilled(ImVec2(body.Min.x, y), ImVec2(body.Max.x, y + kLaneH), rgb(0xffffff, 6));
        dl->AddLine(ImVec2(body.Min.x, y + kLaneH - 0.5f), ImVec2(body.Max.x, y + kLaneH - 0.5f), rgb(0xffffff, 10));
        const ImU32 tc = trackColor(t.type);
        // Keys in frame order for the connecting lines (the file may hold them unsorted).
        std::vector<int> order(t.keyframes.size());
        for (int k = 0; k < int(order.size()); ++k) order[size_t(k)] = k;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return t.keyframes[size_t(a)].frame < t.keyframes[size_t(b)].frame; });
        if (editor::isDiscrete(t.type)) {
            // On spans as bars, to the next key or the end.
            for (size_t k = 0; k < order.size(); ++k) {
                const splash::Keyframe& kf = t.keyframes[size_t(order[k])];
                if (kf.value[0] <= 0.5f) continue;
                const float x0 = xOf(float(kf.frame));
                const float x1 = k + 1 < order.size() ? xOf(float(t.keyframes[size_t(order[k + 1])].frame)) : std::max(endX, x0);
                dl->AddRectFilled(ImVec2(x0, y + 10), ImVec2(x1, y + kLaneH - 10), withAlpha(tc, 50), 3);
            }
        } else {
            for (size_t k = 0; k + 1 < order.size(); ++k) {
                const splash::Keyframe& a = t.keyframes[size_t(order[k])];
                const splash::Keyframe& b = t.keyframes[size_t(order[k + 1])];
                const float x0 = xOf(float(a.frame)), x1 = xOf(float(b.frame));
                if (b.interp == splash::Interp::Step) {
                    // Holds, then jumps at the later key.
                    dl->AddLine(ImVec2(x0, cy + 5), ImVec2(x1, cy + 5), withAlpha(tc, 120), 1.5f);
                    dl->AddLine(ImVec2(x1, cy + 5), ImVec2(x1, cy - 5), withAlpha(tc, 120), 1.5f);
                } else {
                    dl->AddRectFilled(ImVec2(x0, cy - 3), ImVec2(x1, cy + 3), withAlpha(tc, 28));
                    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), withAlpha(tc, 150), 1.5f);
                }
            }
        }
        for (int k : order) {
            const splash::Keyframe& kf = t.keyframes[size_t(k)];
            const ImVec2 c(std::floor(xOf(float(kf.frame))) + 0.5f, cy);
            const bool sel = std::find(m_sel.begin(), m_sel.end(), KeyRef{i, k}) != m_sel.end();
            const bool over = hit && *hit == KeyRef{i, k};
            const bool past = kf.frame > length;
            const ImU32 fill = sel ? rgb(0xffffff) : past ? color::textFaint : over ? lerpColor(tc, rgb(0xffffff), 0.35f) : tc;
            diamond(dl, c, sel ? kDiamond + 1 : kDiamond, fill, sel ? color::accent : rgb(0x0e1014), sel ? 2.5f : 1.5f);
        }
    }
    if (n == 0) {
        textCentered(dl, body, f.regular, type::label, color::textFaint, "No tracks yet: Add track picks a property and a target.");
    }
    // The scrub head across the lanes.
    const float hx = std::floor(xOf(m_frame)) + 0.5f;
    dl->AddLine(ImVec2(hx, body.Min.y), ImVec2(hx, body.Max.y), color::accent, 1.5f);

    ImGui::SetCursorScreenPos(body.Min);
    ImGui::InvisibleButton("##lanes", ImMax(body.GetSize(), ImVec2(1, 1)));
    const bool addMod = io.KeyCtrl || io.KeyShift;
    if (ImGui::IsItemActivated()) {
        m_dragging = false;
        if (hit) {
            auto it = std::find(m_sel.begin(), m_sel.end(), *hit);
            if (addMod && it != m_sel.end()) {
                m_sel.erase(it);
            } else {
                if (!addMod && it == m_sel.end()) m_sel.clear();
                if (it == m_sel.end()) m_sel.push_back(*hit);
                m_dragging = true;
                m_dragFrom = frameAt(io.MousePos.x);
                m_dragApplied = 0;
            }
        } else if (!addMod) {
            m_sel.clear();
        }
    }
    if (ImGui::IsItemActive() && m_dragging && !m_sel.empty()) {
        const int want = int(std::lround(frameAt(io.MousePos.x) - m_dragFrom));
        if (want != m_dragApplied) {
            if (auto moved = editor::moveKeys(doc, m_ref, m_sel, want - m_dragApplied, "timeline.drag")) {
                m_sel = *moved;
                m_dragApplied = want;
            }
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemDeactivated() && m_dragging) {
        m_dragging = false;
        doc.endMerge();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && laneAt >= 0 && laneAt < n && !hit) {
        // A key at the scrub head, or the one already there selected.
        const int fr = int(std::lround(m_frame));
        if (auto k = editor::addKey(doc, m_ref, laneAt, fr)) {
            m_sel = {*k};
        } else {
            const auto& ks = tracks[size_t(laneAt)].keyframes;
            for (int k2 = 0; k2 < int(ks.size()); ++k2)
                if (ks[size_t(k2)].frame == fr) m_sel = {KeyRef{laneAt, k2}};
        }
    }
    if (hit && ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (const splash::Keyframe* kf = editor::key(doc, m_ref, *hit)) {
            const TrackType ty = tracks[size_t(hit->track)].type;
            char tip[160];
            std::snprintf(tip, sizeof tip, "Frame %d: %s%s%s", kf->frame, fmtValue(ty, kf->value).c_str(), editor::isDiscrete(ty) ? "" : ", ",
                          editor::isDiscrete(ty) ? "" : kInterpNames[int(kf->interp)]);
            tooltip(tip);
        }
    }
    ImGui::PopClipRect();
    if (removeTrack >= 0 && editor::removeTrack(doc, m_ref, removeTrack)) m_sel.clear();
}

void TimelinePanel::keyRow(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    dl->AddRectFilled(r.Min + ImVec2(1, 0), r.Max - ImVec2(1, 1), rgb(0x1d2026), radius::window, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(r.Min.x, r.Min.y + 0.5f), ImVec2(r.Max.x, r.Min.y + 0.5f), color::border);
    const float cy = r.GetCenter().y;
    float x = r.Min.x + space::md;
    if (m_sel.empty()) {
        text(dl, ImVec2(x, cy - 9), f.regular, type::label, color::textFaint,
             "Double-click a lane to key it at the scrub head. Drag keys to move them; Delete removes them; Space plays.");
        return;
    }
    if (m_sel.size() > 1) {
        char b[64];
        std::snprintf(b, sizeof b, "%d keys selected", int(m_sel.size()));
        text(dl, ImVec2(x, cy - 9), f.medium, type::label, color::text, b);
        x += measure(f.medium, type::label, b).x + space::lg;
        text(dl, ImVec2(x, cy - 9), f.regular, type::label, color::textFaint, "Drag one to move them all; Delete removes them.");
        if (iconButton("delkeys", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)), icon::x, false, "Delete the selected keys (Del)")) {
            editor::deleteKeys(doc, m_ref, m_sel);
            m_sel.clear();
        }
        return;
    }
    const KeyRef k = m_sel.front();
    const splash::Keyframe kf = *editor::key(doc, m_ref, k);
    const splash::CutsceneTrack& t = (*editor::seqTracks(doc, m_ref))[size_t(k.track)];
    const std::string label = editor::trackLabel(t);
    ImGui::PushClipRect(ImVec2(x, r.Min.y), ImVec2(x + 150, r.Max.y), true);
    text(dl, ImVec2(x, cy - 9), f.medium, type::label, trackColor(t.type), label.c_str());
    ImGui::PopClipRect();
    x += 150 + space::sm;

    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "FRAME");
    x += measure(f.medium, type::caption, "FRAME").x + space::sm;
    {
        float v = float(kf.frame);
        char b[16];
        std::snprintf(b, sizeof b, "%d", kf.frame);
        FieldEdit fe = numberField("kframe", ImRect(ImVec2(x, cy - 12), ImVec2(x + 54, cy + 12)), b, nullptr, 0, nullptr, &v);
        if (fe.changed) {
            const int to = std::clamp(int(std::lround(v)), 0, editor::kMaxTimelineFrame);
            if (auto moved = editor::moveKeys(doc, m_ref, {k}, to - kf.frame, "timeline.kframe")) m_sel = *moved;
        }
        if (fe.done) doc.endMerge();
        x += 54 + space::md;
    }

    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "VALUE");
    x += measure(f.medium, type::caption, "VALUE").x + space::sm;
    if (editor::isDiscrete(t.type)) {
        const bool on = kf.value[0] > 0.5f;
        if (toggle("kon", ImRect(ImVec2(x, cy - 10), ImVec2(x + 36, cy + 10)), on))
            editor::setKeyValue(doc, m_ref, k, {on ? 0.0f : 1.0f, 0, 0});
        text(dl, ImVec2(x + 44, cy - 9), f.regular, type::label, color::textDim, on ? "on" : "off");
        x += 80;
        text(dl, ImVec2(x, cy - 9), f.regular, type::label, color::textFaint, "On/off tracks hold each key until the next.");
    } else {
        const int nv = editor::valueCount(t.type);
        const bool rgbTrack = t.type == TrackType::UIColor || t.type == TrackType::LightColor;
        static const char* xyz[3] = {"X", "Y", "Z"};
        static const char* rgbl[3] = {"R", "G", "B"};
        const ImU32 axes[3] = {color::axisX, color::axisY, color::axisZ};
        const float fw = nv == 1 ? 84 : 76;
        for (int c = 0; c < nv; ++c) {
            float v = kf.value[size_t(c)];
            char id[16];
            std::snprintf(id, sizeof id, "kv%d", c);
            const std::string shown = fmtNum(v);
            FieldEdit fe = numberField(id, ImRect(ImVec2(x, cy - 12), ImVec2(x + fw, cy + 12)), shown.c_str(), nullptr,
                                       nv == 1 ? 0 : axes[c], nv == 1 ? nullptr : (rgbTrack ? rgbl[c] : xyz[c]), &v);
            if (fe.changed) {
                KeyValue nvv = kf.value;
                nvv[size_t(c)] = v;
                editor::setKeyValue(doc, m_ref, k, nvv, "timeline.value." + std::to_string(c));
            }
            if (fe.done) doc.endMerge();
            x += fw + space::xs;
        }
        x += space::md;
        text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "CURVE");
        x += measure(f.medium, type::caption, "CURVE").x + space::sm;
        const int cur = int(kf.interp);
        const int pick = segmented("kinterp", ImVec2(x, cy - 13), {"Linear", "Step", "In", "Out", "In-out"}, cur);
        if (ImGui::IsItemHovered()) tooltip("The curve from the previous key into this one, as the runtime plays it.");
        if (pick != cur) editor::setKeyInterp(doc, m_ref, k, splash::Interp(pick));
    }
    if (iconButton("delkey", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)), icon::x, false, "Delete this key (Del)")) {
        editor::deleteKeys(doc, m_ref, m_sel);
        m_sel.clear();
    }
}

float TimelinePanel::sequenceList(ImDrawList* dl, float x0, float x1, float y, editor::Document& doc) {
    Fonts& f = fonts();
    const std::vector<SeqRef> all = editor::allSequences(doc);
    if (all.empty()) {
        text(dl, ImVec2(x0, y + 2), f.regular, type::label, color::textFaint, "None in this scene yet.");
        y += 24;
    }
    const char* editLabel = "Edit in timeline";
    const float bw = space::md * 2 + measure(f.medium, type::body, editLabel).x;
    std::optional<SeqRef> removing;
    for (SeqRef ref : all) {
        ImGui::PushID(ref.kind == SeqKind::Cutscene ? ref.index : 1000 + ref.index);
        const bool cs = ref.kind == SeqKind::Cutscene;
        textCentered(dl, ImRect(ImVec2(x0, y), ImVec2(x0 + 18, y + 34)), f.medium, type::icon, cs ? rgb(0x5ccfe0) : color::good,
                     cs ? icon::camera : icon::play);
        // Delete, left of Edit in timeline.
        const float delX = x1 - bw - space::xs - 26;
        if (iconButton("del", ImRect(ImVec2(delX, y + 4), ImVec2(delX + 24, y + 28)), icon::x, false,
                       cs ? "Delete this cutscene (Ctrl+Z brings it back)" : "Delete this animation (Ctrl+Z brings it back)"))
            removing = ref;
        // The name: double-click to rename (Enter commits, Esc cancels).
        const std::string& name = *editor::seqName(doc, ref);
        const ImVec2 np(x0 + 26, y);
        std::string typed;
        const TextEdit te = textEdit("seqname", ImRect(np - ImVec2(space::xs, 2), ImVec2(delX - space::xs, np.y + 17)), f.medium,
                                     type::label, np.x, delX - space::sm, &typed, 96);
        if (te == TextEdit::Commit) rename(doc, ref, typed);
        ImGui::PushClipRect(ImVec2(x0, y), ImVec2(delX - space::xs, y + 34), true);
        if (te == TextEdit::Inactive) {
            const ImVec2 ns = measure(f.medium, type::label, name.c_str());
            interact("seqname", ImRect(np, np + ImVec2(std::max(ns.x, 40.0f), ns.y)));
            tooltip("Double-click to rename; Lua plays it by this name");
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) beginTextEdit("seqname", name);
            text(dl, np, f.medium, type::label, color::text, name.c_str());
        }
        char meta[64];
        const int nt = int(editor::seqTracks(doc, ref)->size());
        std::snprintf(meta, sizeof meta, "%s, %d frames, %d track%s", cs ? "cutscene" : "animation", editor::seqLength(doc, ref), nt,
                      nt == 1 ? "" : "s");
        text(dl, ImVec2(x0 + 26, y + 17), f.regular, type::caption, color::textFaint, meta);
        ImGui::PopClipRect();
        if (button("edit", ImVec2(x1 - bw, y + 2), nullptr, editLabel, ButtonKind::Secondary, nullptr,
                   "Opens it in the Timeline panel under the Scene."))
            show(doc, ref);
        ImGui::PopID();
        y += 34 + space::xs;
    }
    if (removing) remove(doc, *removing);
    if (ImGui::GetTime() < m_noteUntil) {
        text(dl, ImVec2(x0, y + 2), f.medium, type::label, color::warn, m_note.c_str());
        y += 24;
    }
    float w = 0;
    if (button("newcs", ImVec2(x0, y + space::xs), icon::plus, "New cutscene", ButtonKind::Ghost, &w))
        if (auto ref = editor::addSequence(doc, SeqKind::Cutscene, "cutscene")) show(doc, *ref);
    if (button("newanim", ImVec2(x0 + w + space::xs, y + space::xs), icon::plus, "New animation", ButtonKind::Ghost))
        if (auto ref = editor::addSequence(doc, SeqKind::Animation, "animation")) show(doc, *ref);
    return y + space::xs + size::field + 6;
}

}  // namespace ui
