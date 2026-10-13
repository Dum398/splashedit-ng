// The timeline panel: a cutscene's or an animation's tracks as lanes under a
// frame ruler, docked under the middle panel. Keys are diamonds: click to
// select (Ctrl or Shift adds), drag to move (whole frames), double-click a
// lane to key it at the scrub head, Delete to remove. The row at the bottom
// edits the selected key's frame, value and curve. Every edit goes through
// editor/timeline.hh, one undo step each.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

#include "editor/timeline.hh"

namespace editor {
class Document;
}

namespace ui {

class TimelinePanel {
public:
    bool open = false;

    // Opens the panel on `ref`, scrub head at frame 0.
    void show(const editor::Document& doc, editor::SeqRef ref);
    // Before the main screen's shortcuts: while the panel has focus, Delete
    // removes the selected keys and Space plays or pauses, and both are
    // locked for the frame so the scene's own Delete does not also run.
    void takeKeys(editor::Document& doc);
    // Takes the bottom of `area` (with a drag bar to resize) when open;
    // returns what is left above it.
    ImRect dock(ImRect area);
    // Draws in the rect dock() took.
    void draw(ImDrawList* dl, editor::Document& doc);
    // Inspector rows: each cutscene and animation with an Edit in timeline
    // button, then New cutscene and New animation. Returns the y under them.
    float sequenceList(ImDrawList* dl, float x0, float x1, float y, editor::Document& doc);

    // The Scene view's preview: the sequence posed at the scrub head while
    // the panel is open on one, else empty (the scene draws as it is). Read
    // only; the document is never touched.
    editor::Preview preview(const editor::Document& doc) const;
    // The scrub head and what each track samples there, for the lane labels.
    struct Sampled {
        int track;
        splash::TrackType type;
        std::string target;
        editor::KeyValue value;
    };
    float frame() const { return m_frame; }
    editor::SeqRef sequence() const { return m_ref; }
    std::vector<Sampled> sampled(const editor::Document& doc) const;

private:
    void header(ImDrawList* dl, ImRect r, editor::Document& doc);
    void ruler(ImDrawList* dl, ImRect r, int length);
    void lanes(ImDrawList* dl, ImRect labels, ImRect body, editor::Document& doc);
    void keyRow(ImDrawList* dl, ImRect r, editor::Document& doc);
    void addTrackPopup(editor::Document& doc);
    void sequencePopup(editor::Document& doc);
    float xOf(float frame) const;
    float frameAt(float x) const;
    void fit(int length);
    void validate(const editor::Document& doc);

    editor::SeqRef m_ref;
    unsigned m_loadId = ~0u;
    std::vector<editor::KeyRef> m_sel;
    float m_frame = 0;
    bool m_playing = false;
    bool m_loop = true;
    bool m_focused = false;
    // View: pixels per frame and the frame at the left edge of the lanes.
    float m_ppf = 6;
    float m_view0 = 0;
    bool m_fitPending = true;
    float m_scrollY = 0;
    float m_height = 320;
    ImRect m_rect;
    float m_bodyX0 = 0, m_bodyX1 = 0;
    // A key drag: the frame under the mouse when it started and the whole
    // frames applied so far.
    bool m_dragging = false;
    float m_dragFrom = 0;
    int m_dragApplied = 0;
};

}  // namespace ui
