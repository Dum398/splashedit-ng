// Block scripts: Scratch-style stacks of blocks saved as <stem>.blocks (JSON)
// and turned into Lua for the psxsplash runtime when a scene is exported. A
// .blocks file goes wherever a .lua file does (Script component, scene script,
// trigger script); the extension decides which it is.
//
// Encoding:
//   op      block id from blockCatalogue()
//   field   the block's one typed-in or chosen value: a variable name, an
//           object ("self" or an object name), a button (CROSS ... R3), a
//           number/text/bool literal, a sound clip or a cutscene name.
//           position_x/_y/_z take the object here; the axis is in the op.
//   args    inputs in order; a block with an empty op is an empty slot
//   bodies  the statement lists of a C block (if_else: then, else)
// A stack's first block is its hat; a stack without a hat is loose and makes
// no code. Stacks with the same hat merge into one Lua function, in stack
// order; functions come out in catalogue order.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace splash {

struct Block {
    std::string op;
    std::string field;
    std::vector<Block> args;
    std::vector<std::vector<Block>> bodies;
    bool operator==(const Block&) const = default;
};

struct BlockStack {
    float x = 0, y = 0;
    std::vector<Block> blocks;  // blocks[0] is the hat
    bool operator==(const BlockStack&) const = default;
};

// value is a literal: integer, decimal, "true"/"false", or anything else as text.
struct BlockVariable {
    std::string name;
    std::string value;
    bool operator==(const BlockVariable&) const = default;
};

struct BlockScript {
    std::vector<BlockVariable> variables;
    std::vector<BlockStack> stacks;
    bool operator==(const BlockScript&) const = default;
};

// Throws std::runtime_error("<path>: ...") on malformed JSON or structure.
// Unknown ops are accepted here and reported by blocksToLua.
BlockScript parseBlocks(const std::string& json, const std::string& path);
// Stable key order, 2-space indent, trailing newline: save(parse(save(x)))
// is byte-identical to save(x).
std::string saveBlocks(const BlockScript& script);

enum class BlockKind { Hat, Statement, CBlock, Reporter, Boolean };
enum class BlockField { None, Variable, Object, Button, Number, Text, Bool, Sound, Cutscene };

// Label templates (English, for the block editor): %f is the field, %1..%9
// the inputs; '|' separates the label shown before each body after the
// first (if_else: "if %1 then|else").
struct BlockSpec {
    std::string_view op;
    BlockKind kind;
    std::string_view label;
    int args;
    int bodies;
    BlockField field;
    std::string_view category;  // one of blockCategories()
};

const std::vector<BlockSpec>& blockCatalogue();
const BlockSpec* findBlockSpec(std::string_view op);
// Button names a Button field takes, in the order Input numbers them.
const std::vector<std::string_view>& blockButtons();
// The palette's sections, in order: Events (every hat), Control, Objects,
// Sound and scenes, Variables, Operators, Input, Debug.
const std::vector<std::string_view>& blockCategories();

// Why name can't be a variable: not an identifier, a Lua keyword, an event
// parameter, an API table or an event function. Empty when it can.
std::string blockNameError(std::string_view name);

struct LuaGen {
    std::string lua;
    std::vector<std::string> errors;  // "stack 2, block 3 (move_by): missing input 2"
};
LuaGen blocksToLua(const BlockScript& script);
// parseBlocks + blocksToLua; throws std::runtime_error("<path>: <first error>").
std::string blocksFileToLua(const std::string& json, const std::string& path);

}  // namespace splash
