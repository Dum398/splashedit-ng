// Sprite sheet editing for the scene inspector: every change is one undo step
// through Document::editSpriteSheets. No UI here, so it is tested headless.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

class Document;

// `base`, or "base (N)" with the next free N, unused among the sheet names.
std::string uniqueSpriteSheetName(const std::vector<splash::SpriteSheet>& sheets, const std::string& base);
// The same over every animation of every sheet: psxsplash's Sprite.AnimIndex
// searches all sheets and takes the first match, so names are scene-wide.
std::string uniqueSpriteAnimName(const std::vector<splash::SpriteSheet>& sheets, const std::string& base);

// Adds a sheet for the project-relative image `texture`, named after its file
// stem (made unique), with one "idle" animation. Returns its index, or -1 at
// psxsplash's 16 sheets.
int addSpriteSheet(Document& doc, const std::string& texture);
bool deleteSpriteSheet(Document& doc, int sheet);
// Runs `fn` on sheet `sheet` (its animations included) as one undo step;
// consecutive edits with one non-empty `mergeKey` merge (a drag).
bool editSpriteSheet(Document& doc, int sheet, const std::function<void(splash::SpriteSheet&)>& fn, const std::string& mergeKey = {});
// Adds an animation to `sheet` named "anim" (made unique across the scene)
// starting on the cell after the sheet's last animation. Returns its index or -1.
int addSpriteAnim(Document& doc, int sheet, int cells);
bool deleteSpriteAnim(Document& doc, int sheet, int anim);

// A sheet's grid for a texture of w x h texels: whole when both divide.
struct SpriteGrid {
    int cols = 0, rows = 0;
    bool whole = false;
    int cells() const { return whole ? cols * rows : 0; }
};
SpriteGrid spriteGrid(const splash::SpriteSheet& s, int width, int height);

// A direction set: animations base_n, base_e, ... (splash::spriteDirectionSuffixes)
// one after another, which Sprite.SetFacingFromYaw(sprite, "base_n", count, yaw)
// picks from by yaw.
struct DirectionSet {
    std::string base;
    int count = 0;  // 4 or 8
    int first = 0;  // index of base_n in the sheet's animations
};
// The sets in `s`, in list order: `count` animations in a row named base_<suffix>
// with exactly the suffixes in their order.
std::vector<DirectionSet> directionSets(const splash::SpriteSheet& s);
// The set animation `anim` of `s` belongs to, or nullptr.
const DirectionSet* directionSetOf(const std::vector<DirectionSet>& sets, int anim);
// The Lua line that turns a sprite of the set.
std::string facingLuaLine(const DirectionSet& set);

struct DirectionSetSpec {
    std::string base = "walk";
    int directions = 8;  // 4 or 8
    int framesPerDirection = 1;
    int firstCell = 0;  // direction d starts at firstCell + d * framesPerDirection
    int frameDuration = 6;
    bool loop = true;
};
// What the form starts with for sheet `s` whose grid has `cols` x `rows` cells:
// a row of cells per direction (frames = cols), from the first row after the
// sheet's animations, or row 0 when the set would not fit there.
DirectionSetSpec defaultDirectionSet(const splash::SpriteSheet& s, int directions, int cols, int rows);
// Appends the set to sheet `sheet` (whose grid has `cells` cells, 0 = unknown) as
// one undo step. Returns the index of its first animation, or -1 with `error`
// saying why nothing was added: a name already used in any sheet, cells past
// the sheet, psxsplash's 64 animations, or a bad field.
int addDirectionSet(Document& doc, int sheet, const DirectionSetSpec& spec, int cells, std::string* error);

// The frame (index within the animation) psxsplash shows `vsyncs` vsyncs after
// PlayAnim, stepping as spritemath::advanceAnim does: each frame shows
// frameDuration vsyncs; a loop wraps, otherwise the last frame holds.
int spriteAnimFrameAt(const splash::SpriteAnim& a, long long vsyncs);

}  // namespace editor
