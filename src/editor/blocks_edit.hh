// The block editor's model side: where every block of a BlockScript sits on
// the canvas, what a point on it hits, and the edits the editor makes (take a
// stack, drop it somewhere, set a field), each a pure function of the script.
// No ImGui: the view (blocks_view.hh) measures text and draws.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "blocks.hh"

namespace editor::blk {

using splash::Block;
using splash::BlockKind;
using splash::BlockScript;
using splash::BlockStack;

// A block's address: the stack, then steps down from its block list. 'b' n
// is block n of the list at hand, 'a' n input n of the block at hand, 'd' n
// body n of the block at hand (a list again). A block path ends in 'b' or 'a'.
// As a string (control names, tests): "s0.b1", "s0.b1.a0" (block 1's first
// input), "s0.b2.d0.b1" (the second block in block 2's first body).
struct Step {
    char kind = 'b';  // 'b', 'a' or 'd'
    int index = 0;
    bool operator==(const Step&) const = default;
};
struct Path {
    int stack = -1;
    std::vector<Step> steps;
    bool operator==(const Path&) const = default;
    std::string str() const;
    bool empty() const { return stack < 0; }
    // The path one step down.
    Path then(char kind, int index) const;
    // The block holding this one (an input's owner, or a body block's C
    // block); empty for a stack's own blocks.
    Path parent() const;
    // In a block list ('b' last), not in an input.
    bool inList() const { return !steps.empty() && steps.back().kind == 'b'; }
};
// "s0.b2.d0.b1" back to a path; empty when malformed.
Path parsePath(std::string_view s);

const Block* blockAt(const BlockScript& s, const Path& p);
Block* blockAt(BlockScript& s, const Path& p);
// Reporters with a typed-in value: they sit in a slot as its value.
bool isLiteral(const Block& b);
// Input `arg` of `op` takes only a Boolean or a variable (if, while, and, or, not).
bool booleanSlot(std::string_view op, int arg);
// The block a palette drag makes: its inputs filled with literals where a
// number or a text goes ("0", "1" for change, "10" for repeat, "" for log),
// empty where a condition goes; an object field starts as "self", a button
// as CROSS, a bool as true.
Block newBlock(std::string_view op);

// --- Layout ---------------------------------------------------------------

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    bool operator==(const Rect&) const = default;
};

struct Metrics {
    float text = 13;        // label text height
    float row = 32;         // a statement's minimum row height
    float padX = 10;        // a statement's padding left and right
    float padY = 6;         // around the tallest thing in a row
    float gap = 6;          // between the pieces of a label
    float slotH = 20;       // an empty slot, a field, a literal
    float slotW = 28;       // an empty slot's width, the least a field takes
    float wellPad = 7;      // text inside a field or literal, left and right
    float reporterPadX = 8;
    float hatTop = 12;      // the hat's cap, over its row
    float mouthIndent = 16; // a C block's arm
    float mouthMin = 20;    // an empty mouth's height
    float arm = 14;         // a C block's bottom arm
    float minW = 60;        // a statement's least width
};
// Width of `text` in the label font.
using TextWidth = std::function<float(std::string_view)>;

// A piece of a block's label, for drawing.
struct Piece {
    enum Kind { Text, Field, Slot } kind = Text;
    Rect rect;
    std::string text;  // Text: the words
    int arg = -1;      // Slot: the input
    bool filled = false;       // Slot: a reporter sits in it (laid out as its own box)
    bool placeholder = false;  // Field: empty, `text` says what goes there
};

struct BlockBox {
    Path path;
    std::string op;
    BlockKind kind = BlockKind::Statement;
    std::string_view category;
    Rect rect;                // the whole block, bodies included
    std::vector<Rect> parts;  // what the block itself covers: its rows and arm (a C block's mouths are not its own)
    std::vector<Rect> mouths; // one per body: where its blocks go
    std::vector<Rect> slots;  // one per input: the empty well, or the reporter in it
    std::vector<Piece> pieces;
    bool hasField = false;
    Rect field;               // the field's well (a literal's: the whole block)
    int depth = 0;            // how many blocks hold it
    // What touches it, for drawing square corners there: a block above it
    // (or the C block's row over a body), one under it, the arm left of a body.
    bool above = false, below = false, inBody = false;
};

struct StackBox {
    int stack = 0;
    Rect rect;
    float bottomX = 0, bottomY = 0;  // where a block attached under the last one goes
    bool hat = false;
};

// Boxes come parent first, in stack order, so drawing them in order puts
// every block over what holds it and later stacks over earlier ones.
struct Layout {
    std::vector<BlockBox> blocks;
    std::vector<StackBox> stacks;
    const BlockBox* find(const Path& p) const;
};
Layout layout(const BlockScript& s, const TextWidth& tw, const Metrics& m = {});
// Width and height of a stack laid out on its own.
std::pair<float, float> stackSize(const BlockStack& st, const TextWidth& tw, const Metrics& m = {});

// What a canvas point is on, innermost first: a field, an empty input slot,
// else the block. A C block's mouth is not the C block.
struct Hit {
    enum Kind { None, Block, Slot, Field } kind = None;
    Path path;    // the block (Slot: the block with the slot)
    int slot = -1;
};
Hit hitTest(const Layout& l, float x, float y);

// --- Edits ------------------------------------------------------------------

// The block at `p` and every block under it in its list, out of the script
// as a loose fragment (at 0,0; the caller places it). A whole stack goes
// when its first block is taken. A reporter taken from an input leaves the
// input empty (a block with no op).
struct Taken {
    BlockScript rest;
    BlockStack fragment;
};
std::optional<Taken> takeStack(const BlockScript& s, const Path& p);
// takeStack's rest: the fragment is thrown away.
std::optional<BlockScript> deleteFragment(const BlockScript& s, const Path& p);

struct Target {
    enum Kind {
        None,
        Canvas,    // a new stack at x, y
        After,     // under the block at path, in its list
        BodyTop,   // first in body `index` of the C block at path
        StackTop,  // over stack path.stack, which moves to x, y
        Slot       // input `index` of the block at path; a reporter there pops out to x, y
    } kind = None;
    Path path;
    int index = 0;
    float x = 0, y = 0;
    bool operator==(const Target&) const = default;
};
// The kind rules: a hat only first in a stack (a new stack, or over a loose
// one); statements and C blocks in lists, never under a stop nor with a stop
// above other blocks; reporters only in inputs (a Boolean input takes a
// Boolean or a variable, which may hold true or false) or loose.
bool canDrop(const BlockScript& s, const BlockStack& fragment, const Target& t);
// The script with `fragment` at `t`, or nothing when canDrop() refuses.
// `placed` gets the path of the fragment's first block.
std::optional<BlockScript> dropStack(const BlockScript& s, const BlockStack& fragment, const Target& t, Path* placed = nullptr);
// A reporter into input `arg` of the block at `p`. An input holding a
// literal takes the new reporter in its place; one holding any other
// reporter pops it out as a loose stack at x, y.
std::optional<BlockScript> dropReporter(const BlockScript& s, const Block& reporter, const Path& p, int arg, float x, float y);
std::optional<BlockScript> setField(const BlockScript& s, const Path& p, const std::string& value);

// The nearest place `fragment`, its top left at fx, fy on the canvas, snaps
// to within `radius` that canDrop() allows; None when there is none. `l` is
// layout(s).
Target findTarget(const BlockScript& s, const Layout& l, const BlockStack& fragment, float fx, float fy, const TextWidth& tw,
                  const Metrics& m = {}, float radius = 28);

// --- Variables ----------------------------------------------------------------

// How many blocks (in stacks, inputs and bodies) have a Variable field naming `name`.
int variableUses(const BlockScript& s, std::string_view name);
// "used by 1 block", "used by 3 blocks".
std::string usedBy(int uses);
// The script with a new variable "my_variable" (then "my_variable2", ...: the
// first name no variable has), value "0". `name` gets its name.
BlockScript addVariable(const BlockScript& s, std::string* name = nullptr);
// Variable `from` called `to`, and every Variable field naming `from` with it.
// Nothing, with `why` set, when `to` is not a name blockNameError() takes, is
// another variable's, or there is no `from`.
std::optional<BlockScript> renameVariable(const BlockScript& s, std::string_view from, std::string_view to,
                                          std::string* why = nullptr);
// Variable i's initial value; nothing when there is no variable i.
std::optional<BlockScript> setVariableValue(const BlockScript& s, int i, const std::string& value);
// Variable i out of the script; nothing, with `why` set ("used by 2 blocks"),
// while a block names it or when there is no variable i.
std::optional<BlockScript> deleteVariable(const BlockScript& s, int i, std::string* why = nullptr);

// --- Undo -----------------------------------------------------------------

// A tab's undo: the whole script before and after each committed edit.
class History {
public:
    explicit History(BlockScript initial = {}) : m_now(std::move(initial)) {}
    const BlockScript& current() const { return m_now; }
    // A step, unless `next` is what there is already. True when it took one.
    bool commit(BlockScript next);
    bool undo();
    bool redo();
    bool canUndo() const { return !m_past.empty(); }
    bool canRedo() const { return !m_future.empty(); }
    // Starts over at `s`, with nothing to undo.
    void reset(BlockScript s);
    static constexpr size_t kMax = 1000;

private:
    std::vector<BlockScript> m_past, m_future;
    BlockScript m_now;
};

}  // namespace editor::blk
