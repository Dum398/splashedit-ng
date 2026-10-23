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
// A drag past the lanes' edge scrolls this many pixels a second, plus the gain per pixel out, up to the max.
constexpr float kAutoScrollBase = 240, kAutoScrollGain = 8, kAutoScrollMax = 2400;

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

const ImU32 kAudioColor = rgb(0xf0b45a), kSkinColor = rgb(0x7fb2ff);

// One selectable thing in the lanes: a key, or an audio or skin event by index.
struct Item {
    enum Kind { Key, Audio, Skin } kind = Key;
    KeyRef key;
    int index = -1;
    bool operator==(const Item&) const = default;
};

std::vector<int>& eventsOf(editor::Selection& s, Item::Kind k) { return k == Item::Audio ? s.audio : s.skin; }

bool selHas(const editor::Selection& s, const Item& it) {
    if (it.kind == Item::Key) return std::find(s.keys.begin(), s.keys.end(), it.key) != s.keys.end();
    const std::vector<int>& v = it.kind == Item::Audio ? s.audio : s.skin;
    return std::find(v.begin(), v.end(), it.index) != v.end();
}

void selAdd(editor::Selection& s, const Item& it) {
    if (selHas(s, it)) return;
    if (it.kind == Item::Key)
        s.keys.push_back(it.key);
    else
        eventsOf(s, it.kind).push_back(it.index);
}

void selRemove(editor::Selection& s, const Item& it) {
    if (it.kind == Item::Key)
        std::erase(s.keys, it.key);
    else
        std::erase(eventsOf(s, it.kind), it.index);
}

std::string skinLabel(const splash::SkinAnimEvent& e) { return e.object + ": " + e.clip + (e.loop ? " (loop)" : ""); }

}  // namespace

void TimelinePanel::show(const editor::Document& doc, SeqRef ref) {
    open = true;
    ++shows;
    m_ref = ref;
    m_loadId = doc.loadId();
    m_frame = 0;
    m_playing = false;
    m_sel = {};
    m_scrollY = 0;
    m_fitPending = true;
}

void TimelinePanel::validate(const editor::Document& doc) {
    if (doc.loadId() != m_loadId) {
        // Another scene: start on its first sequence.
        m_loadId = doc.loadId();
        m_ref = {};
        m_sel = {};
        m_frame = 0;
        m_playing = false;
        m_fitPending = true;
    }
    if (!editor::valid(doc, m_ref)) {
        const std::vector<SeqRef> all = editor::allSequences(doc);
        m_ref = all.empty() ? SeqRef{} : all.front();
        m_sel = {};
        m_fitPending = true;
    }
    std::erase_if(m_sel.keys, [&](const KeyRef& k) { return !editor::key(doc, m_ref, k); });
    const auto* audio = editor::seqAudio(doc, m_ref);
    const auto* skin = editor::seqSkin(doc, m_ref);
    std::erase_if(m_sel.audio, [&](int i) { return !audio || i < 0 || i >= int(audio->size()); });
    std::erase_if(m_sel.skin, [&](int i) { return !skin || i < 0 || i >= int(skin->size()); });
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
        editor::deleteSelection(doc, m_ref, m_sel);
        m_sel = {};
    }
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) && editor::valid(doc, m_ref)) m_playing = !m_playing;
    const bool ctrlOnly = io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_C, false) && !m_sel.empty()) {
        m_clip = editor::copySelection(doc, m_ref, m_sel);
        const size_t n = m_clip.keys.size() + m_clip.audio.size() + m_clip.skin.size();
        note("Copied " + std::to_string(n) + (n == 1 ? " item" : " items") + ": Ctrl+V pastes at the scrub head");
    }
    if (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste(doc);
    // The keys belong to the timeline this frame: the scene's Delete, copy and paste stay out.
    const ImGuiID owner = ImHashStr("timeline-keys");
    for (ImGuiKey k : {ImGuiKey_Delete, ImGuiKey_Space, ImGuiKey_C, ImGuiKey_V})
        ImGui::SetKeyOwner(k, owner, ImGuiInputFlags_LockThisFrame);
}

void TimelinePanel::paste(editor::Document& doc) {
    if (m_clip.empty()) {
        note("Nothing copied: select keys or events and press Ctrl+C");
        return;
    }
    int skipped = 0;
    const int at = int(std::lround(m_frame));
    const size_t before = editor::seqTracks(doc, m_ref) ? editor::seqTracks(doc, m_ref)->size() : 0;
    if (auto sel = editor::pasteClipboard(doc, m_ref, m_clip, at, &skipped)) {
        m_sel = *sel;
        const int added = int(editor::seqTracks(doc, m_ref)->size() - before);
        std::string msg;
        if (added) msg = "Pasted; added " + std::to_string(added) + (added == 1 ? " track" : " tracks") + " for the keys";
        if (skipped) msg += (msg.empty() ? "Pasted; " : "; ") + std::to_string(skipped) + " skipped (an animation has no camera or audio)";
        if (!msg.empty()) note(msg);
    } else if (skipped) {
        note("Not pasted: an animation has no camera tracks or audio events");
    } else {
        note("Not pasted: it would pass frame " + std::to_string(editor::kMaxTimelineFrame) + " or the runtime's limits");
    }
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
        FieldEdit fe = numberField("frame", ImRect(ImVec2(x, cy - 12), ImVec2(x + 58, cy + 12)), b, nullptr, 0, nullptr, &v, true);
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
        FieldEdit fe = numberField("length", ImRect(ImVec2(x, cy - 12), ImVec2(x + 58, cy + 12)), b, nullptr, 0, nullptr, &v, true);
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
    if (!why.empty()) {
        note("Not renamed: " + why);
        return;
    }
    const std::string old = *editor::seqName(doc, ref);
    editor::ScriptRenames sr;
    if (!editor::renameSequence(doc, ref, name, &sr)) return;
    // Scripts play it by name: say what changed with it, and what could not.
    std::string msg = "Renamed '" + old + "' to '" + name + "'";
    if (sr.refs)
        msg += ", " + std::to_string(sr.refs) + " script reference" + (sr.refs == 1 ? "" : "s") + " updated in " +
               std::to_string(sr.files.size()) + " file" + (sr.files.size() == 1 ? "" : "s") + " (Ctrl+Z undoes both)";
    else
        msg += ", no script references to update";
    for (const std::string& f : sr.failed) msg += "; could not update " + f;
    // Literals the rename could not prove name it: say where, so a stale one is found.
    if (const size_t left = sr.leftovers.size()) {
        msg += "; " + std::to_string(left) + " other \"" + old + "\" string" + (left == 1 ? "" : "s") + " left as " +
               (left == 1 ? "it was: " : "they were: ");
        for (size_t i = 0; i < left && i < 3; ++i) msg += (i ? ", " : "") + sr.leftovers[i];
        if (left > 3) msg += " and " + std::to_string(left - 3) + " more";
    }
    note(msg);
    m_renamed = Renamed{ref.kind, old, name, msg};
}

void TimelinePanel::remove(editor::Document& doc, SeqRef ref) {
    if (!editor::valid(doc, ref)) return;
    const std::string name = *editor::seqName(doc, ref);
    if (!editor::removeSequence(doc, ref)) return;
    note("Deleted '" + name + "' (Ctrl+Z brings it back)");
    if (ref == m_ref) {
        // On to whatever sits there now, or the first one.
        m_sel = {};
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
            } else if (t.type == TrackType::FollowPath) {
                const std::vector<std::string> paths = editor::pathNames(doc);
                if (ImGui::BeginMenu("Follow instead", !paths.empty())) {
                    for (const std::string& pn : paths)
                        if (ImGui::MenuItem(pn.c_str(), nullptr, pn == t.path)) editor::setTrackPath(doc, m_ref, ti, pn);
                    ImGui::EndMenu();
                }
            } else {
                ImGui::MenuItem("Drive instead", nullptr, false, false);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Camera and rumble tracks have no target");
            }
            if (ImGui::MenuItem("Remove track")) {
                editor::removeTrack(doc, m_ref, ti);
                m_sel = {};
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
        // Follow Path (editor only: the export bakes it into a position track): who rides, then which path.
        ImGui::Separator();
        const std::vector<std::string> paths = editor::pathNames(doc);
        if (ImGui::BeginMenu(editor::trackTypeName(TrackType::FollowPath), !paths.empty())) {
            std::vector<std::string> riders;
            if (m_ref.kind == editor::SeqKind::Cutscene) riders.push_back("");
            for (const std::string& n : editor::targetsFor(doc, TrackType::ObjectPosition)) riders.push_back(n);
            for (const std::string& who : riders)
                if (ImGui::BeginMenu(who.empty() ? "Camera" : who.c_str(), !has(TrackType::FollowPath, who))) {
                    for (const std::string& pn : paths)
                        if (ImGui::MenuItem(pn.c_str())) {
                            const int i = editor::addFollowPathTrack(doc, m_ref, who, pn);
                            if (i >= 0) m_scrollY = std::max(0.0f, (i + 1) * kLaneH - 4 * kLaneH);
                        }
                    ImGui::EndMenu();
                }
            ImGui::EndMenu();
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
    const std::vector<splash::CutsceneAudioEvent>* audio = editor::seqAudio(doc, m_ref);
    const std::vector<splash::SkinAnimEvent>& skin = *editor::seqSkin(doc, m_ref);
    const int n = int(tracks.size());
    const int length = editor::seqLength(doc, m_ref);
    // Under the tracks: the audio lane (cutscenes only), then the skin clip
    // lane when the scene has a skinned object or the sequence skin events.
    bool anySkinned = !skin.empty();
    if (!anySkinned)
        for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
            if (fo.object->skin) {
                anySkinned = true;
                break;
            }
    const int audioRow = audio ? n : -1;
    const int skinRow = anySkinned ? n + (audio ? 1 : 0) : -1;
    const int rows = n + (audio ? 1 : 0) + (anySkinned ? 1 : 0);
    const float maxScroll = std::max(0.0f, rows * kLaneH - body.GetHeight());
    const std::vector<std::string> clips = audio ? editor::audioClips(doc) : std::vector<std::string>();
    // What the skin events can play (read from the .anim files, so only again after an edit).
    if (!skin.empty() && (m_skinRev != doc.revision() || m_skinAssets != doc.assetsId() || m_skinLoad != doc.loadId())) {
        m_skinTargets = editor::skinTargets(doc);
        m_skinRev = doc.revision();
        m_skinAssets = doc.assetsId();
        m_skinLoad = doc.loadId();
    }
    auto skinProblem = [&](int i) { return editor::skinEventProblem(m_skinTargets, skin[size_t(i)]); };

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
    // Dragging keys or a box past the left or right of the lanes scrolls them,
    // faster the further out the mouse is.
    if ((m_dragging || m_boxing) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float out = io.MousePos.x < body.Min.x ? io.MousePos.x - body.Min.x : io.MousePos.x > body.Max.x ? io.MousePos.x - body.Max.x : 0;
        if (out != 0) {
            const float pxPerSec = std::min(kAutoScrollMax, kAutoScrollBase + kAutoScrollGain * std::fabs(out));
            m_view0 += std::copysign(pxPerSec, out) * io.DeltaTime / m_ppf;
            m_view0 = std::clamp(m_view0, -kPad / m_ppf, float(editor::kMaxTimelineFrame));
        }
    }
    m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);

    std::set<int> selTracks;
    for (const KeyRef& k : m_sel.keys) selTracks.insert(k.track);

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
    // The event lanes' labels: what they hold, how many of the runtime's limit, and + to add one at the scrub head.
    const char* openAdd = nullptr;
    ImVec2 addAt;
    auto eventLabel = [&](int rowIdx, const char* name, const char* id, ImU32 col, int count, int limit, const char* addTip,
                          const char* fullTip, const char* popup, bool canAdd) {
        const float y = labels.Min.y + rowIdx * kLaneH - m_scrollY;
        if (y + kLaneH < labels.Min.y || y > labels.Max.y) return;
        const ImRect row(ImVec2(labels.Min.x, y), ImVec2(labels.Max.x - 1, y + kLaneH));
        dl->AddRectFilled(row.Min, row.Max, rgb(0x191b20));
        dl->AddLine(ImVec2(row.Min.x, row.Max.y - 0.5f), ImVec2(row.Max.x, row.Max.y - 0.5f), color::border);
        dl->AddRectFilled(ImVec2(row.Min.x + 6, y + 8), ImVec2(row.Min.x + 9, y + kLaneH - 8), col, 2);
        text(dl, ImVec2(row.Min.x + 18, y + 4), f.medium, type::label, color::text, name);
        char c[48];
        std::snprintf(c, sizeof c, "EVENTS  %d / %d", count, limit);
        text(dl, ImVec2(row.Min.x + 18, y + 21), f.medium, type::caption, count >= limit ? color::warn : color::textFaint, c);
        const bool full = count >= limit;
        const ImRect plus(ImVec2(row.Max.x - 28, y + 4), ImVec2(row.Max.x - 6, y + 20));
        const bool hovered = row.Contains(io.MousePos);
        nameRect(std::string("timeline.") + id, plus);
        if (iconButton(id, plus, icon::plus, false, full ? fullTip : addTip, hovered ? color::textDim : withAlpha(color::textFaint, 110)) &&
            !full && canAdd) {
            openAdd = popup;
            addAt = ImVec2(plus.Min.x, plus.Min.y);
            m_addFrame = int(std::lround(m_frame));
        }
    };
    char fullAudio[96], fullSkin[96];
    std::snprintf(fullAudio, sizeof fullAudio, "This cutscene has the runtime's %d audio events", editor::kMaxAudioEvents);
    std::snprintf(fullSkin, sizeof fullSkin, "This sequence has the runtime's %d skin clip events", editor::kMaxSkinEvents);
    if (audio)
        eventLabel(audioRow, "Audio", "addaudio", kAudioColor, int(audio->size()), editor::kMaxAudioEvents,
                   "Add an audio event at the scrub head (or double-click the lane)", fullAudio, "##addaudio", true);
    if (anySkinned)
        eventLabel(skinRow, "Skin clips", "addskin", kSkinColor, int(skin.size()), editor::kMaxSkinEvents,
                   "Play a skinned mesh's clip from the scrub head (or double-click the lane)", fullSkin, "##addskin", true);
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

    auto rowY = [&](int r) { return body.Min.y + r * kLaneH - m_scrollY; };
    // An event's flag: a stem at its frame and a tab with its name.
    auto flagRect = [&](int r, int frame, const std::string& label) {
        const float x = std::floor(xOf(float(frame))) + 0.5f;
        const float w = std::min(150.0f, measure(f.medium, type::caption, label.c_str()).x + 12);
        return ImRect(ImVec2(x, rowY(r) + 7), ImVec2(x + w, rowY(r) + 23));
    };
    auto audioName = [&](int i) { return (*audio)[size_t(i)].clip; };
    auto skinName = [&](int i) { return skinLabel(skin[size_t(i)]); };

    // What the mouse is over: a lane and the nearest key or event in reach.
    const int laneAt = body.Contains(io.MousePos) ? int(std::floor((io.MousePos.y - body.Min.y + m_scrollY) / kLaneH)) : -1;
    std::optional<Item> hit;
    if (laneAt >= 0 && laneAt < n) {
        float best = kDiamond + 4;
        const auto& ks = tracks[size_t(laneAt)].keyframes;
        for (int k = 0; k < int(ks.size()); ++k) {
            const float d = std::fabs(xOf(float(ks[size_t(k)].frame)) - io.MousePos.x);
            if (d <= best) {
                best = d;
                hit = Item{Item::Key, KeyRef{laneAt, k}, -1};
            }
        }
    } else if (laneAt >= 0 && (laneAt == audioRow || laneAt == skinRow)) {
        // The stem nearest the mouse, else the topmost tab under it (later events draw over earlier).
        const bool isAudio = laneAt == audioRow;
        const int count = isAudio ? int(audio->size()) : int(skin.size());
        float best = kDiamond;
        for (int i = 0; i < count; ++i) {
            const int fr = isAudio ? (*audio)[size_t(i)].frame : skin[size_t(i)].frame;
            const float d = std::fabs(xOf(float(fr)) - io.MousePos.x);
            if (d <= best) {
                best = d;
                hit = Item{isAudio ? Item::Audio : Item::Skin, {}, i};
            }
        }
        if (!hit)
            for (int i = count - 1; i >= 0; --i) {
                const int fr = isAudio ? (*audio)[size_t(i)].frame : skin[size_t(i)].frame;
                if (flagRect(laneAt, fr, isAudio ? audioName(i) : skinName(i)).Contains(io.MousePos)) {
                    hit = Item{isAudio ? Item::Audio : Item::Skin, {}, i};
                    break;
                }
            }
    }

    for (int i = 0; i < n; ++i) {
        const splash::CutsceneTrack& t = tracks[size_t(i)];
        const float y = rowY(i);
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
            const Item me{Item::Key, KeyRef{i, k}, -1};
            const bool sel = selHas(m_sel, me);
            const bool over = hit && *hit == me;
            const bool past = kf.frame > length;
            const ImU32 fill = sel ? rgb(0xffffff) : past ? color::textFaint : over ? lerpColor(tc, rgb(0xffffff), 0.35f) : tc;
            diamond(dl, c, sel ? kDiamond + 1 : kDiamond, fill, sel ? color::accent : rgb(0x0e1014), sel ? 2.5f : 1.5f);
            if (namedRectsOn())
                nameRect("timeline.key." + std::to_string(i) + "." + std::to_string(k), ImRect(c - ImVec2(kDiamond, kDiamond), c + ImVec2(kDiamond, kDiamond)));
        }
    }

    // The event lanes: a flag per event, its clip named on the tab.
    auto eventLane = [&](int r, Item::Kind kind, ImU32 col, int count, auto frameOf, auto nameOf, auto problemOf) {
        const float y = rowY(r);
        if (y + kLaneH < body.Min.y || y > body.Max.y) return;
        dl->AddRectFilled(ImVec2(body.Min.x, y), ImVec2(body.Max.x, y + kLaneH), r == laneAt ? rgb(0xffffff, 8) : rgb(0x000000, 40));
        dl->AddLine(ImVec2(body.Min.x, y - 0.5f), ImVec2(body.Max.x, y - 0.5f), rgb(0xffffff, 16));
        dl->AddLine(ImVec2(body.Min.x, y + kLaneH - 0.5f), ImVec2(body.Max.x, y + kLaneH - 0.5f), rgb(0xffffff, 10));
        if (namedRectsOn()) nameRect(std::string("timeline.lane.") + (kind == Item::Audio ? "audio" : "skin"), ImRect(ImVec2(body.Min.x, y), ImVec2(body.Max.x, y + kLaneH)));
        for (int i = 0; i < count; ++i) {
            const int fr = frameOf(i);
            const std::string name = nameOf(i);
            const Item me{kind, {}, i};
            const bool sel = selHas(m_sel, me), over = hit && *hit == me;
            const bool bad = !problemOf(i).empty();
            const ImU32 c = bad ? color::bad : fr > length ? color::textFaint : col;
            const ImRect tab = flagRect(r, fr, name);
            const float x = tab.Min.x;
            if (namedRectsOn())
                nameRect(std::string("timeline.") + (kind == Item::Audio ? "audio." : "skin.") + std::to_string(i), ImRect(ImVec2(x - 2, y + 5), ImVec2(x + 2, y + kLaneH - 5)));
            dl->AddLine(ImVec2(x, y + 5), ImVec2(x, y + kLaneH - 5), sel ? color::accent : c, sel ? 2.5f : 1.5f);
            dl->AddRectFilled(tab.Min, tab.Max, sel ? rgb(0xffffff) : withAlpha(c, over ? 120 : 70), 3, ImDrawFlags_RoundCornersRight);
            if (sel) dl->AddRect(tab.Min, tab.Max, color::accent, 3, ImDrawFlags_RoundCornersRight, 1.5f);
            ImGui::PushClipRect(tab.Min, tab.Max, true);
            text(dl, ImVec2(tab.Min.x + 5, tab.Min.y + 1), f.medium, type::caption, sel ? rgb(0x101216) : color::text, name.c_str());
            ImGui::PopClipRect();
            dl->AddTriangleFilled(ImVec2(x - 4, y + kLaneH - 5), ImVec2(x + 4, y + kLaneH - 5), ImVec2(x, y + kLaneH - 11),
                                  sel ? color::accent : c);
        }
    };
    if (audio) {
        eventLane(
            audioRow, Item::Audio, kAudioColor, int(audio->size()), [&](int i) { return (*audio)[size_t(i)].frame; }, audioName,
            [&](int i) {
                const std::string& c = (*audio)[size_t(i)].clip;
                return std::find(clips.begin(), clips.end(), c) == clips.end()
                           ? "No active object's audio component is named '" + c + "': the export refuses it"
                           : std::string();
            });
    }
    if (anySkinned)
        eventLane(skinRow, Item::Skin, kSkinColor, int(skin.size()), [&](int i) { return skin[size_t(i)].frame; }, skinName, skinProblem);

    if (rows == 0) {
        textCentered(dl, body, f.regular, type::label, color::textFaint, "No tracks yet: Add track picks a property and a target.");
    } else if (n == 0) {
        const float y = rowY(rows);
        if (y + 24 < body.Max.y)
            text(dl, ImVec2(body.Min.x + kPad, y + 8), f.regular, type::label, color::textFaint, "No tracks yet: Add track picks a property and a target.");
    }
    // The scrub head across the lanes.
    const float hx = std::floor(xOf(m_frame)) + 0.5f;
    dl->AddLine(ImVec2(hx, body.Min.y), ImVec2(hx, body.Max.y), color::accent, 1.5f);

    ImGui::SetCursorScreenPos(body.Min);
    ImGui::InvisibleButton("##lanes", ImMax(body.GetSize(), ImVec2(1, 1)));
    const bool addMod = io.KeyCtrl || io.KeyShift;
    if (ImGui::IsItemActivated()) {
        m_dragging = false;
        m_boxing = false;
        if (hit) {
            const bool had = selHas(m_sel, *hit);
            if (addMod && had) {
                selRemove(m_sel, *hit);
            } else {
                // A click on an unselected item without Ctrl or Shift selects it alone;
                // on a selected one it keeps the selection, to drag it all.
                if (!addMod && !had) m_sel = {};
                selAdd(m_sel, *hit);
                m_dragging = true;
                m_dragFrom = frameAt(io.MousePos.x);
                m_dragApplied = 0;
            }
        } else {
            // Empty lane space: a box select, adding to the selection with Ctrl or Shift.
            m_boxing = true;
            m_boxBase = addMod ? m_sel : editor::Selection{};
            m_sel = m_boxBase;
            m_boxFrame = frameAt(io.MousePos.x);
            m_boxY = io.MousePos.y - body.Min.y + m_scrollY;
        }
    }
    if (ImGui::IsItemActive() && m_boxing) {
        const ImVec2 a(xOf(m_boxFrame), body.Min.y + m_boxY - m_scrollY);
        const ImRect box(ImMin(a, io.MousePos), ImMax(a, io.MousePos));
        editor::Selection s = m_boxBase;
        if (box.GetWidth() >= 3 || box.GetHeight() >= 3) {
            // Whatever has its key or stem inside the box (within a key's reach of its edge).
            const ImRect reach(box.Min - ImVec2(kDiamond, kDiamond), box.Max + ImVec2(kDiamond, kDiamond));
            auto inside = [&](int r, int frame) {
                const ImVec2 p(xOf(float(frame)), rowY(r) + kLaneH / 2);
                return reach.Contains(p) || (p.x >= reach.Min.x && p.x <= reach.Max.x && box.Min.y <= rowY(r) + kLaneH - 8 &&
                                             box.Max.y >= rowY(r) + 8);
            };
            for (int i = 0; i < n; ++i)
                for (int k = 0; k < int(tracks[size_t(i)].keyframes.size()); ++k)
                    if (inside(i, tracks[size_t(i)].keyframes[size_t(k)].frame)) selAdd(s, Item{Item::Key, KeyRef{i, k}, -1});
            if (audio)
                for (int i = 0; i < int(audio->size()); ++i)
                    if (inside(audioRow, (*audio)[size_t(i)].frame)) selAdd(s, Item{Item::Audio, {}, i});
            if (anySkinned)
                for (int i = 0; i < int(skin.size()); ++i)
                    if (inside(skinRow, skin[size_t(i)].frame)) selAdd(s, Item{Item::Skin, {}, i});
            dl->AddRectFilled(box.Min, box.Max, withAlpha(color::accent, 34));
            dl->AddRect(box.Min, box.Max, withAlpha(color::accent, 200), 0, 0, 1.0f);
        }
        m_sel = s;
    }
    if (ImGui::IsItemActive() && m_dragging && !m_sel.empty()) {
        const int want = int(std::lround(frameAt(io.MousePos.x) - m_dragFrom));
        if (want != m_dragApplied) {
            if (auto moved = editor::moveSelection(doc, m_ref, m_sel, want - m_dragApplied, "timeline.drag")) {
                m_sel = *moved;
                m_dragApplied = want;
            }
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemDeactivated()) {
        if (m_dragging) doc.endMerge();
        m_dragging = false;
        m_boxing = false;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && laneAt >= 0 && laneAt < n && !hit) {
        // A key at the scrub head, or the one already there selected.
        const int fr = int(std::lround(m_frame));
        if (auto k = editor::addKey(doc, m_ref, laneAt, fr)) {
            m_sel = {{*k}, {}, {}};
        } else {
            const auto& ks = tracks[size_t(laneAt)].keyframes;
            for (int k2 = 0; k2 < int(ks.size()); ++k2)
                if (ks[size_t(k2)].frame == fr) m_sel = {{KeyRef{laneAt, k2}}, {}, {}};
        }
    }
    // Double- or right-click an event lane: its add menu, for the scrub head.
    const bool eventLaneAt = laneAt >= 0 && (laneAt == audioRow || laneAt == skinRow);
    if (eventLaneAt && !hit && ImGui::IsItemHovered() &&
        (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const bool isAudio = laneAt == audioRow;
        const int count = isAudio ? int(audio->size()) : int(skin.size());
        if (count >= (isAudio ? editor::kMaxAudioEvents : editor::kMaxSkinEvents)) {
            note(isAudio ? fullAudio : fullSkin);
        } else {
            openAdd = isAudio ? "##addaudio" : "##addskin";
            addAt = io.MousePos;
            m_addFrame = int(std::lround(m_frame));
        }
    }
    if (hit && ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        char tip[320];
        if (hit->kind == Item::Key) {
            if (const splash::Keyframe* kf = editor::key(doc, m_ref, hit->key)) {
                const TrackType ty = tracks[size_t(hit->key.track)].type;
                std::snprintf(tip, sizeof tip, "Frame %d: %s%s%s", kf->frame, fmtValue(ty, kf->value).c_str(), editor::isDiscrete(ty) ? "" : ", ",
                              editor::isDiscrete(ty) ? "" : kInterpNames[int(kf->interp)]);
                tooltip(tip);
            }
        } else if (hit->kind == Item::Audio) {
            const splash::CutsceneAudioEvent& a = (*audio)[size_t(hit->index)];
            const bool missing = std::find(clips.begin(), clips.end(), a.clip) == clips.end();
            std::snprintf(tip, sizeof tip, "Frame %d: play '%s', volume %d, pan %d%s", a.frame, a.clip.c_str(), a.volume, a.pan,
                          missing ? "\nNo active object's audio component has this clip name: the export refuses it" : "");
            tooltip(tip);
        } else {
            const splash::SkinAnimEvent& e = skin[size_t(hit->index)];
            const std::string problem = skinProblem(hit->index);
            std::snprintf(tip, sizeof tip, "Frame %d: %s plays '%s'%s%s%s", e.frame, e.object.c_str(), e.clip.c_str(), e.loop ? ", looping" : "",
                          problem.empty() ? "" : "\n", problem.c_str());
            tooltip(tip);
        }
    }
    ImGui::PopClipRect();
    if (removeTrack >= 0 && editor::removeTrack(doc, m_ref, removeTrack)) m_sel = {};
    if (openAdd) {
        ImGui::OpenPopup(openAdd);
        m_addAt = addAt;
    }
    addEventPopups(doc);
}

void TimelinePanel::addEventPopups(editor::Document& doc) {
    pushPopupStyle();
    // The panel sits at the bottom of the window: these open upwards.
    ImGui::SetNextWindowPos(m_addAt, ImGuiCond_Always, ImVec2(0, 1));
    if (ImGui::BeginPopup("##addaudio")) {
        ImGui::TextDisabled("Audio event at frame %d", m_addFrame);
        ImGui::Separator();
        const std::vector<std::string> clips = editor::audioClips(doc);
        if (clips.empty()) ImGui::TextDisabled("No audio clips: give an object an Audio component");
        for (const std::string& c : clips) {
            const bool picked = ImGui::MenuItem(c.c_str());
            nameRect("timeline.addaudio/" + c, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            if (picked) {
                const int i = editor::addAudioEvent(doc, m_ref, m_addFrame, c);
                if (i >= 0) m_sel = {{}, {i}, {}};
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(m_addAt, ImGuiCond_Always, ImVec2(0, 1));
    if (ImGui::BeginPopup("##addskin")) {
        ImGui::TextDisabled("Skin clip from frame %d", m_addFrame);
        ImGui::Separator();
        const std::vector<editor::SkinTarget> targets = editor::skinTargets(doc);
        if (targets.empty()) ImGui::TextDisabled("No skinned objects");
        for (const editor::SkinTarget& t : targets) {
            // (An open menu's Begin takes over the last item: its row is named while closed.)
            const bool open = ImGui::BeginMenu(t.object.c_str(), !t.clips.empty());
            if (!open) nameRect("timeline.addskin/" + t.object, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            if (open) {
                for (const std::string& c : t.clips) {
                    const bool picked = ImGui::MenuItem(c.c_str());
                    nameRect("timeline.addskin/" + t.object + "/" + c, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                    if (picked) {
                        const int i = editor::addSkinEvent(doc, m_ref, m_addFrame, t.object, c);
                        if (i >= 0) m_sel = {{}, {}, {i}};
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    // The selected event's clip, from the bottom row.
    ImGui::SetNextWindowPos(m_evClipAt, ImGuiCond_Always, ImVec2(0, 1));
    if (ImGui::BeginPopup("##evclip")) {
        const auto* audio = editor::seqAudio(doc, m_ref);
        const auto* skin = editor::seqSkin(doc, m_ref);
        if (m_sel.size() == 1 && m_sel.audio.size() == 1 && audio) {
            splash::CutsceneAudioEvent a = (*audio)[size_t(m_sel.audio[0])];
            for (const std::string& c : editor::audioClips(doc)) {
                const bool picked = ImGui::MenuItem(c.c_str(), nullptr, c == a.clip);
                nameRect("timeline.evclip/" + c, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                if (picked) {
                    a.clip = c;
                    editor::setAudioEvent(doc, m_ref, m_sel.audio[0], a);
                }
            }
        } else if (m_sel.size() == 1 && m_sel.skin.size() == 1 && skin) {
            splash::SkinAnimEvent e = (*skin)[size_t(m_sel.skin[0])];
            for (const editor::SkinTarget& t : editor::skinTargets(doc)) {
                const bool open = ImGui::BeginMenu(t.object.c_str(), !t.clips.empty());
                if (!open) nameRect("timeline.evclip/" + t.object, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                if (open) {
                    for (const std::string& c : t.clips) {
                        const bool picked = ImGui::MenuItem(c.c_str(), nullptr, t.object == e.object && c == e.clip);
                        nameRect("timeline.evclip/" + t.object + "/" + c, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
                        if (picked) {
                            e.object = t.object;
                            e.clip = c;
                            editor::setSkinEvent(doc, m_ref, m_sel.skin[0], e);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    popPopupStyle();
}

void TimelinePanel::keyRow(ImDrawList* dl, ImRect r, editor::Document& doc) {
    Fonts& f = fonts();
    dl->AddRectFilled(r.Min + ImVec2(1, 0), r.Max - ImVec2(1, 1), rgb(0x1d2026), radius::window, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(r.Min.x, r.Min.y + 0.5f), ImVec2(r.Max.x, r.Min.y + 0.5f), color::border);
    const float cy = r.GetCenter().y;
    float x = r.Min.x + space::md;
    if (m_sel.empty()) {
        text(dl, ImVec2(x, cy - 9), f.regular, type::label, color::textFaint,
             "Double-click a lane to key it at the scrub head. Drag across lanes to box-select, drag a key to move the "
             "selection; Del deletes, Ctrl+C / Ctrl+V copy and paste at the scrub head.");
        return;
    }
    if (m_sel.size() > 1) {
        char b[96];
        const int ev = int(m_sel.audio.size() + m_sel.skin.size());
        if (ev == 0)
            std::snprintf(b, sizeof b, "%d keys selected", int(m_sel.keys.size()));
        else if (m_sel.keys.empty())
            std::snprintf(b, sizeof b, "%d events selected", ev);
        else
            std::snprintf(b, sizeof b, "%d keys and %d event%s selected", int(m_sel.keys.size()), ev, ev == 1 ? "" : "s");
        text(dl, ImVec2(x, cy - 9), f.medium, type::label, color::text, b);
        x += measure(f.medium, type::label, b).x + space::lg;
        text(dl, ImVec2(x, cy - 9), f.regular, type::label, color::textFaint,
             "Drag one to move them all; Del deletes them; Ctrl+C copies them.");
        nameRect("timeline.delkeys", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)));
        if (iconButton("delkeys", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)), icon::x, false,
                       "Delete the selection (Del)")) {
            editor::deleteSelection(doc, m_ref, m_sel);
            m_sel = {};
        }
        return;
    }
    if (m_sel.keys.empty()) {
        eventRow(dl, r, x, doc);
        return;
    }
    const KeyRef k = m_sel.keys.front();
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
        const ImRect fr(ImVec2(x, cy - 12), ImVec2(x + 54, cy + 12));
        FieldEdit fe = numberField("kframe", fr, b, nullptr, 0, nullptr, &v, true);
        nameRect("timeline.kframe", fr);
        if (fe.changed) {
            const int to = std::clamp(int(std::lround(v)), 0, editor::kMaxTimelineFrame);
            if (auto moved = editor::moveKeys(doc, m_ref, {k}, to - kf.frame, "timeline.kframe")) m_sel = {*moved, {}, {}};
        }
        if (fe.done) doc.endMerge();
        x += 54 + space::md;
    }

    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "VALUE");
    x += measure(f.medium, type::caption, "VALUE").x + space::sm;
    if (editor::isDiscrete(t.type)) {
        const bool on = kf.value[0] > 0.5f;
        const ImRect tr(ImVec2(x, cy - 10), ImVec2(x + 36, cy + 10));
        nameRect("timeline.kon", tr);
        if (toggle("kon", tr, on))
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
            const ImRect vr(ImVec2(x, cy - 12), ImVec2(x + fw, cy + 12));
            FieldEdit fe = numberField(id, vr, shown.c_str(), nullptr, nv == 1 ? 0 : axes[c], nv == 1 ? nullptr : (rgbTrack ? rgbl[c] : xyz[c]), &v);
            nameRect(std::string("timeline.") + id, vr);
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
    nameRect("timeline.delkey", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)));
    if (iconButton("delkey", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)), icon::x, false, "Delete this key (Del)")) {
        editor::deleteSelection(doc, m_ref, m_sel);
        m_sel = {};
    }
}

void TimelinePanel::eventRow(ImDrawList* dl, ImRect r, float x, editor::Document& doc) {
    Fonts& f = fonts();
    const float cy = r.GetCenter().y;
    const bool isAudio = !m_sel.audio.empty();
    const int index = isAudio ? m_sel.audio[0] : m_sel.skin[0];
    const auto* audio = editor::seqAudio(doc, m_ref);
    const auto& skin = *editor::seqSkin(doc, m_ref);
    splash::CutsceneAudioEvent a;
    splash::SkinAnimEvent e;
    if (isAudio)
        a = (*audio)[size_t(index)];
    else
        e = skin[size_t(index)];
    text(dl, ImVec2(x, cy - 9), f.medium, type::label, isAudio ? kAudioColor : kSkinColor, isAudio ? "Audio event" : "Skin clip event");
    x += 150 + space::sm;

    auto number = [&](const char* label, const char* id, int value, float w, auto apply, const char* tip = nullptr) {
        text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, label);
        x += measure(f.medium, type::caption, label).x + space::sm;
        float v = float(value);
        char b[16];
        std::snprintf(b, sizeof b, "%d", value);
        const ImRect nr(ImVec2(x, cy - 12), ImVec2(x + w, cy + 12));
        FieldEdit fe = numberField(id, nr, b, nullptr, 0, nullptr, &v);
        nameRect(std::string("timeline.") + id, nr);
        if (tip && ImGui::IsItemHovered()) tooltip(tip);
        if (fe.changed) apply(int(std::lround(v)));
        if (fe.done) doc.endMerge();
        x += w + space::md;
    };
    number("FRAME", "evframe", isAudio ? a.frame : e.frame, 54, [&](int v) {
        if (isAudio) {
            a.frame = v;
            editor::setAudioEvent(doc, m_ref, index, a, "timeline.evframe");
        } else {
            e.frame = v;
            editor::setSkinEvent(doc, m_ref, index, e, "timeline.evframe");
        }
    });

    // The clip: a dropdown of the project's audio clips, or of each skinned object's clips.
    text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "CLIP");
    x += measure(f.medium, type::caption, "CLIP").x + space::sm;
    {
        const ImRect dd(ImVec2(x, cy - 12), ImVec2(x + 190, cy + 12));
        const std::string label = isAudio ? a.clip : e.object + " / " + e.clip;
        Hit h = interact("evclip", dd);
        nameRect("timeline.evclip", dd);
        dl->AddRectFilled(dd.Min, dd.Max, h.hover > 0 ? color::hover : color::field, radius::field);
        dl->AddRect(dd.Min, dd.Max, color::borderStrong, radius::field);
        ImGui::PushClipRect(dd.Min, ImVec2(dd.Max.x - 22, dd.Max.y), true);
        text(dl, ImVec2(dd.Min.x + space::sm, cy - 9), f.medium, type::label, color::text, label.c_str());
        ImGui::PopClipRect();
        textCentered(dl, ImRect(ImVec2(dd.Max.x - 22, dd.Min.y), dd.Max), f.medium, type::caption + 1, color::textDim, icon::chevronDown);
        if (h.clicked) {
            ImGui::OpenPopup("##evclip");
            m_evClipAt = ImVec2(dd.Min.x, dd.Min.y - 4);
        }
        x += 190 + space::md;
    }
    if (isAudio) {
        char tip[96];
        std::snprintf(tip, sizeof tip, "0..%d, as the runtime reads it", editor::kMaxEventVolume);
        number("VOLUME", "evvol", a.volume, 54, [&](int v) {
            a.volume = v;
            editor::setAudioEvent(doc, m_ref, index, a, "timeline.evvol");
        }, tip);
        number("PAN", "evpan", a.pan, 54, [&](int v) {
            a.pan = v;
            editor::setAudioEvent(doc, m_ref, index, a, "timeline.evpan");
        }, "0 left, 64 centre, 127 right");
    } else {
        text(dl, ImVec2(x, cy - 8), f.medium, type::caption, color::textFaint, "LOOP");
        x += measure(f.medium, type::caption, "LOOP").x + space::sm;
        const ImRect lr(ImVec2(x, cy - 10), ImVec2(x + 36, cy + 10));
        nameRect("timeline.evloop", lr);
        if (toggle("evloop", lr, e.loop)) {
            e.loop = !e.loop;
            editor::setSkinEvent(doc, m_ref, index, e);
        }
    }
    nameRect("timeline.delev", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)));
    if (iconButton("delev", ImRect(ImVec2(r.Max.x - 36, cy - 13), ImVec2(r.Max.x - 10, cy + 13)), icon::x, false, "Delete this event (Del)")) {
        editor::deleteSelection(doc, m_ref, m_sel);
        m_sel = {};
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
        // Wrapped to the section: a rename note naming its scripts runs long.
        const float wrap = x1 - x0;
        const ImVec2 ns = f.medium->CalcTextSizeA(type::label, FLT_MAX, wrap, m_note.c_str());
        dl->AddText(f.medium, type::label, ImVec2(x0, y + 2), color::warn, m_note.c_str(), nullptr, wrap);
        y += std::max(24.0f, ns.y + 8);
    }
    float w = 0;
    if (button("newcs", ImVec2(x0, y + space::xs), icon::plus, "New cutscene", ButtonKind::Ghost, &w))
        if (auto ref = editor::addSequence(doc, SeqKind::Cutscene, "cutscene")) show(doc, *ref);
    if (button("newanim", ImVec2(x0 + w + space::xs, y + space::xs), icon::plus, "New animation", ButtonKind::Ghost))
        if (auto ref = editor::addSequence(doc, SeqKind::Animation, "animation")) show(doc, *ref);
    return y + space::xs + size::field + 6;
}

}  // namespace ui
