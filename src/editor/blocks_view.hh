// The block editor's ImGui side: what a .blocks tab shows. A palette of
// every catalogue block by category on the left, the script's stacks on a
// canvas on the right. Drag a block out of the palette to add it; drag one on
// the canvas to move it and the blocks under it (snapping under a block, into
// a C block's mouth, over a loose stack or into an input); drop it on the
// palette, or select it and press Delete, to remove it. Click a field or a
// literal to type its value. Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) undo and redo,
// one step per edit.
//
// Controls by name (ui::nameRect, for scripted checks), with paths as
// editor::blk::Path::str() spells them ("s0.b1", "s0.b1.a0", "s0.b2.d0.b1"):
//   blocks.canvas              the canvas
//   blocks.category.<name>     a palette category chip ("Sound and scenes")
//   blocks.palette.<op>        a palette block, while it is in view
//   blocks.block.<path>        a block on the canvas
//   blocks.slot.<path>.<i>     input i of that block (the empty well, or the reporter in it)
//   blocks.field.<path>        its field (a literal: the whole block)
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

#include "blocks.hh"
#include "editor/blocks_edit.hh"

namespace ui {

class BlocksView {
public:
    // Shows `s` with nothing to undo.
    void reset(splash::BlockScript s);
    // `s` as one undoable step (the file reloaded or rewritten); false when
    // it is what is shown already.
    bool commit(splash::BlockScript s);
    const splash::BlockScript& script() const { return m_history.current(); }

    // What a frame did, for the tab to mirror in its file buffer.
    struct Result {
        struct Op {
            enum Kind { Edit, Undo, Redo } kind;
            std::string text;  // Edit: the script as saveBlocks() writes it
        };
        std::vector<Op> ops;  // in the order they happened
        bool save = false;    // Ctrl+S
    };
    // The tab's content in `r`. `focused` is the tab's keyboard: a click in
    // `r` takes it, one elsewhere lets it go; the shortcuts need it.
    Result draw(ImDrawList* dl, ImRect r, bool* focused);
    // While set, the text is not a block script: the tab says why instead.
    std::string error;

private:
    using Path = editor::blk::Path;
    struct Drag {
        bool on = false;
        bool fromCanvas = false;
        splash::BlockStack fragment;
        splash::BlockScript rest;  // the script without it
        ImVec2 grab;               // the pointer from the fragment's top left
    };
    struct Press {
        bool on = false;
        bool palette = false;
        std::string op;         // palette: the block
        ImVec2 blockMin;        // palette: the block's top left on screen
        editor::blk::Hit hit;   // canvas: what it was on
        ImVec2 at;
        bool pan = false;
        ImVec2 scrollAt;
    };
    bool edit(splash::BlockScript s, Result* out);
    void select(const Path& p) { m_selected = p; }

    editor::blk::History m_history;
    Drag m_drag;
    Press m_press;
    Path m_selected;
    Path m_editing;   // a field being typed into
    Path m_picking;   // a button field with its list open
    std::string m_pickQuery;
    ImVec2 m_scroll{0, 0};
    float m_paletteScroll = 0;
    float m_paletteHeight = 0;
    // The Lua check, kept until the script changes.
    splash::BlockScript m_checked;
    bool m_checkedAny = false;
    splash::LuaGen m_gen;
};

}  // namespace ui
