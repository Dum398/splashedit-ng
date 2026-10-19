// Tilemap editing for the scene inspector and the Tilemap tab: every change is
// one undo step through Document::editTilemap. No UI here, so it is tested headless.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

class Document;

// Adds the scene's tilemap drawing from sprite sheet `tileset` (whose grid has
// `cells` cells, 0 = unknown): 20 x 15 tiles (a 320 x 240 screen of 16 px
// tiles), all empty, with a walkable "floor" brush on cell 0 and a solid
// "wall" on cell 1. False when the scene already has one.
bool addTilemap(Document& doc, const std::string& tileset, int cells);
bool removeTilemap(Document& doc);
// Runs `fn` on the tilemap as one undo step; consecutive edits with one
// non-empty `mergeKey` merge (a drag, a paint stroke). False when there is no
// tilemap or nothing changed.
bool editTilemap(Document& doc, const std::function<void(splash::Tilemap&)>& fn, const std::string& mergeKey = {});
// A new size (each side 1..256), keeping the overlap.
bool resizeTilemap(Document& doc, int width, int height, const std::string& mergeKey = {});

// `base`, or "base (N)" with the next free N, unused among the brush names.
std::string uniqueTileBrushName(const splash::Tilemap& t, const std::string& base);
// Adds a brush after the last, drawing the cell after the last brush's (wrapping
// at `cells`, 0 = unknown) with the next tint of a fixed set. Returns its index or -1.
int addTileBrush(Document& doc, int cells);
// Deletes a brush: the cells painted with it become empty and the later
// brushes' cells follow their brush down one index.
bool deleteTileBrush(Document& doc, int brush);
// The tint brush i starts with (cycles through a fixed set).
std::array<float, 3> tileBrushTint(int i);

// The cells from x0,y0 to x1,y1 inclusive on a line (Bresenham), in order.
std::vector<std::array<int, 2>> tileLine(int x0, int y0, int x1, int y1);
// Paints `cells` (off-map ones skipped) with `brush` (-1 erases), merging
// under `strokeKey` so one stroke is one undo step. False when nothing changed.
bool paintTiles(Document& doc, const std::vector<std::array<int, 2>>& cells, int brush, const std::string& strokeKey);
// Fills the rectangle between two corners (inclusive, either order, clipped to
// the map) with `brush` as one undo step. Returns how many cells changed.
int fillTiles(Document& doc, int x0, int y0, int x1, int y1, int brush);
// Bucket fill: the 4-connected region of cells equal to cell x,y (empty, -1,
// included) becomes `brush` (-1 erases), as one undo step. Returns how many
// cells changed: 0, with no undo step, off the map or when the region already
// is `brush`.
int floodTiles(Document& doc, int x, int y, int brush);

}  // namespace editor
