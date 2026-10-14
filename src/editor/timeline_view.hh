// The timeline panel: a cutscene's or an animation's tracks as lanes under a
// frame ruler, docked under the middle panel, then a lane of audio events
// (cutscenes) and one of skin clip events. Keys are diamonds, events flags:
// click to select (Ctrl or Shift adds), drag on empty lane space to box-select
// across lanes, drag a selected item to move the whole selection (whole
// frames), double-click a lane to key it at the scrub head, Delete to remove,
// Ctrl+C / Ctrl+V to copy and paste at the scrub head. The row at the bottom
// edits the selected key or event. Every edit goes through
// editor/timeline.hh, one undo step each.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/timeline.hh"

namespace editor {
class Document;
}

namespace ui {

class TimelinePanel {
public:
    bool open = false;
    // The bottom dock's tab strip, drawn at the left of the header in place
    // of the panel's own title when set (`tabsWidth` wide).
    std::function<void(ImDrawList*, ImRect)> tabs;
    float tabsWidth = 0;
    // Counts show() calls, so a dock can raise the panel each time.
    unsigned shows = 0;

    // Opens the panel on `ref`, scrub head at frame 0.
    void show(const editor::Document& doc, editor::SeqRef ref);
    // Before the main screen's shortcuts: while the panel has focus, Delete
    // removes the selection, Ctrl+C / Ctrl+V copy it and paste at the scrub
    // head, Space plays or pauses; those keys are locked for the frame so the
    // scene's own Delete, copy and paste do not also run.
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
    // The last rename since the last call, for the script tabs: a tab with
    // unsaved changes gets the same rewrite in its buffer (the file on disk
    // already has it), and the status bar the summary.
    struct Renamed {
        editor::SeqKind kind;
        std::string from, to;
        std::string summary;
    };
    std::optional<Renamed> takeRenamed() { return std::exchange(m_renamed, std::nullopt); }

private:
    void header(ImDrawList* dl, ImRect r, editor::Document& doc);
    void ruler(ImDrawList* dl, ImRect r, int length);
    void lanes(ImDrawList* dl, ImRect labels, ImRect body, editor::Document& doc);
    void keyRow(ImDrawList* dl, ImRect r, editor::Document& doc);
    void addTrackPopup(editor::Document& doc);
    // The audio and skin lanes' add menus (at m_addFrame) and the selected event's clip pickers.
    void addEventPopups(editor::Document& doc);
    void eventRow(ImDrawList* dl, ImRect r, float x, editor::Document& doc);
    void paste(editor::Document& doc);
    void sequencePopup(editor::Document& doc);
    void trackMenu(editor::Document& doc);
    // Renames, or says why not in the note.
    void rename(editor::Document& doc, editor::SeqRef ref, const std::string& name);
    void remove(editor::Document& doc, editor::SeqRef ref);
    void note(std::string msg);
    // The note: a refused rename, a deleted sequence; shown a few seconds.
    std::string m_note;
    double m_noteUntil = 0;
    std::optional<Renamed> m_renamed;
    // The sequence picker's Rename arms the name field next frame.
    bool m_renameArm = false;
    // The lane whose menu is open.
    int m_menuTrack = -1;
    float xOf(float frame) const;
    float frameAt(float x) const;
    void fit(int length);
    void validate(const editor::Document& doc);

    editor::SeqRef m_ref;
    unsigned m_loadId = ~0u;
    editor::Selection m_sel;
    // Ctrl+C's keys and events, for Ctrl+V in this sequence or another.
    editor::TimelineClipboard m_clip;
    // A box select: where it started (frame, and y in the lanes' scrolled
    // content) and the selection it adds to.
    bool m_boxing = false;
    float m_boxFrame = 0, m_boxY = 0;
    editor::Selection m_boxBase;
    // Where an event lane's add menu puts the event.
    int m_addFrame = 0;
    // Where the bottom row's clip dropdown opens its menu.
    ImVec2 m_evClipAt, m_addAt;
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
    // A drag of the selection: the frame under the mouse when it started and
    // the whole frames applied so far.
    bool m_dragging = false;
    float m_dragFrom = 0;
    int m_dragApplied = 0;
};

}  // namespace ui
