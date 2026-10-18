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

// The frame (index within the animation) psxsplash shows `vsyncs` vsyncs after
// PlayAnim, stepping as spritemath::advanceAnim does: each frame shows
// frameDuration vsyncs; a loop wraps, otherwise the last frame holds.
int spriteAnimFrameAt(const splash::SpriteAnim& a, long long vsyncs);

}  // namespace editor
