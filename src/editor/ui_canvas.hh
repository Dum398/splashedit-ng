// The UI canvas editor's model: where elements land on the PS1 screen, hit
// testing, snapping, z-order, the edits (as Document commands, one undo step
// each) and a software render of the canvases the way psxsplash's
// uisystem.cpp draws them. No ImGui; the view is ui_canvas_view.hh.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "editor/document.hh"
#include "scene.hh"

namespace splash {
struct FontSheet;
}

namespace editor {

namespace uic {

// The screen the UI is laid out on (the project's VRAM resolution).
struct Screen {
    int w = 320, h = 240;
};
Screen screenOf(const splash::VramSettings& vram);

// A rectangle in screen pixels: x..x+w-1, y..y+h-1.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool operator==(const Rect&) const = default;
};

// Where the element sits on screen as authored: the anchors resolved at the
// screen size, the way the export bakes them (anchor rounding corrected), but
// before the runtime clamps it to the screen. Lines: the box around both
// endpoints.
Rect layout(const splash::UIElement& e, Screen s);
// What the runtime draws: layout() clipped the way UISystem::resolveLayout
// does (a left or top overhang shrinks the element, at least 1 px, cut at the
// right and bottom edges).
Rect drawn(const splash::UIElement& e, Screen s);
// Sets the element's rect (or, for a line, moves its endpoints) so that
// layout() gives `r`, keeping its anchors. A line keeps its size.
void place(splash::UIElement& e, Rect r, Screen s);

// The front-most visible element of `cv` at screen point (x, y), or -1. Lines
// count within `slop` pixels.
int hitTest(const splash::UICanvas& cv, Screen s, float x, float y, float slop = 2);

// The box an element occupies on screen: layout(), or for a line the box
// around its endpoints.
Rect boundsOf(const splash::UIElement& e, Screen s);
// The visible elements of `cv` that touch the box x0,y0 .. x1,y1 (screen
// pixel coordinates, either corner first): a rect overlaps it, a line runs
// through it. Ascending indices; for a rubber band.
std::vector<int> elementsInBox(const splash::UICanvas& cv, Screen s, float x0, float y0, float x1, float y1);

enum class Handle { None, Move, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight, LineFrom, LineTo };
// The resize handle of `r` at (x, y), within `slop` pixels of a corner or an
// edge (corners first); Move inside it; None elsewhere.
Handle hitHandle(Rect r, float x, float y, float slop);
// A line's endpoint handle at (x, y) within `slop`; Move near the line; None elsewhere.
Handle hitLineHandle(const splash::UIElement& line, float x, float y, float slop);

// `start` with handle `h` dragged by (dx, dy) pixels. The opposite edges stay
// put and the dragged ones stop 1 px short of them, so the size never drops
// below 1x1 or flips; sizes stay within the format's 16-bit range.
Rect resize(Rect start, Handle h, int dx, int dy);

// A snap guide: vertical lines at x = pos from y = from to y = to, horizontal
// at y = pos from x = from to x = to.
struct Guide {
    bool vertical = true;
    int pos = 0, from = 0, to = 0;
    bool operator==(const Guide&) const = default;
};
struct Snapped {
    Rect rect;
    std::vector<Guide> guides;
};
// Snaps a rect being moved: its left edge, centre and right edge (and top,
// middle, bottom) to the screen's edges and centre and to the edges and
// centres of the other visible elements of `cv` (not `self`), each axis on
// its own, when within `threshold` pixels. Guides show what it snapped to.
Snapped snapMove(const splash::UICanvas& cv, int self, Rect r, Screen s, int threshold);
// The same for a resize: only the edges handle `h` moves snap.
Snapped snapResize(const splash::UICanvas& cv, int self, Rect r, Handle h, Screen s, int threshold);
// A line endpoint at (x, y) snapped to the same targets; `x`, `y` updated.
std::vector<Guide> snapPoint(const splash::UICanvas& cv, int self, int* x, int* y, Screen s, int threshold);

// ---- Multi-selection. Document::uiSelection() holds the primary element and
// the others; these read and build it.
// The selected elements of the selected canvas that exist, sorted.
std::vector<int> selectedElements(const Document& doc);
// A selection of the elements `idx` of canvas `c`, `primary` (one of them; -1 =
// the last) being the primary one.
Document::UiSelection selectionOf(int c, std::vector<int> idx, int primary = -1);

// ---- Edits: each one undo step through the Document. A `mergeKey` folds
// consecutive edits with the same key into one step until
// Document::endMerge() (a drag). Every edit keeps the UI selection, and
// undo puts back the selection from before it.

// Runs `fn` on element `e` of canvas `c`.
bool editElement(Document& doc, int c, int e, const std::function<void(splash::UIElement&)>& fn,
                 const std::string& mergeKey = {});
// Runs `fn` on each element of `idx` (and its place in `idx`) as ONE edit.
bool editElements(Document& doc, int c, const std::vector<int>& idx,
                  const std::function<void(splash::UIElement&, size_t)>& fn, const std::string& mergeKey = {});
// Renaming the scene's loading canvas (SceneSettings::loadingCanvas) renames
// that setting too, in the same undo step; deleting it leaves the name.
bool editCanvas(Document& doc, int c, const std::function<void(splash::UICanvas&)>& fn, const std::string& mergeKey = {});
// A new element of `type` on top of canvas `c`, centred, named after its type
// and numbered when taken. Selects it and returns its index (-1: no canvas).
int addElement(Document& doc, int c, splash::UIElementType type, Screen s);
// A copy of element `e` right in front of it, 8 px down and right, renamed. Selects it.
int duplicateElement(Document& doc, int c, int e);
bool deleteElement(Document& doc, int c, int e);
// The same for a whole selection, as ONE undo step each. Duplicating puts each
// copy right in front of its original and selects the copies; it returns how
// many, or -2 (no change) when the canvas would pass kMaxCanvasElements.
bool deleteElements(Document& doc, int c, const std::vector<int>& idx);
int duplicateElements(Document& doc, int c, const std::vector<int>& idx);
std::string copyElementsText(const Document& doc, int c, const std::vector<int>& idx);
// The box around the elements in clipboard `text` as laid out on screen `s`;
// nothing for text that is not UI elements.
std::optional<Rect> clipboardBounds(const std::string& text, Screen s);
// Clipboard text for element `e` of canvas `c` (splash::writeUIElementsText);
// empty when there is no such element. Cut is this, then deleteElement.
std::string copyElementText(const Document& doc, int c, int e);
// Inserts the elements in clipboard `text` into canvas `c` right in front of
// element `after` (on top of all when -1 or gone), as one undo step. Each one
// moves 8 px down and right per copy already sitting where it would land, so
// a paste next to its original never covers it (a cut one comes back in
// place), and takes a name no element of the canvas has. Selects the last
// one and returns its index; -1, with no change, for text that is not UI
// elements or no such canvas. All pasted elements end up selected. With `at`
// the top-left of the pasted group's box lands there instead (no offset
// stepping). -2, with no change: the canvas would hold more than
// kMaxCanvasElements.
int pasteElements(Document& doc, int c, int after, const std::string& text, Screen s,
                  const std::array<int, 2>* at = nullptr);
// Moves element `from` to index `to` (draw order: higher is in front). The
// selection follows the element.
bool moveElement(Document& doc, int c, int from, int to);
// A new empty canvas, in front of the others. Selects it and returns its index.
int addCanvas(Document& doc);
bool deleteCanvas(Document& doc, int c);

// ---- Scene fonts (Scene::fonts), each edit one undo step that carries the
// fonts and the canvases together.
// A new font named "font" (numbered when taken) from `source`: a .png is a
// bitmap font, anything else a TTF/OTF; empty leaves it to pick. Returns its
// index, or -1 (no change) when the scene has kMaxFonts already.
int addFont(Document& doc, const std::string& source);
// Runs `fn` on font `i`; its name stays (renameFont renames).
bool editFont(Document& doc, int i, const std::function<void(splash::UIFont&)>& fn, const std::string& mergeKey = {});
// Renames font `i` and every text element of every canvas that uses it.
// Refuses, with the reason in `why`, an empty name or one another font has.
bool renameFont(Document& doc, int i, const std::string& newName, std::string* why = nullptr);
// Removes font `i`; text elements that used it go back to the system font.
bool deleteFont(Document& doc, int i);
// Sets `path` as the font's file: a .png as its bitmap (no source), else as its source (no bitmap).
void setFontFile(splash::UIFont& f, const std::string& path);
bool isBitmapFontFile(const std::string& path);
// `base`, or `base (2)`... whichever no font of `fonts` has.
std::string uniqueFontName(const std::vector<splash::UIFont>& fonts, const std::string& base);
// How many text elements of the scene use font `name`, their own or their canvas's default.
int fontUses(const splash::Scene& scene, const std::string& name);

// `base`, or `base (2)`, `base (3)`... whichever no element of `cv` has.
std::string uniqueElementName(const splash::UICanvas& cv, const std::string& base);
const char* typeName(splash::UIElementType t);

// ---- Problems, each saying how to fix it, some with a one-click fix.
enum class Fix { None, MoveOnScreen, TrimText, AsciiText, SystemFont, HalveColour, NeutralTint, ClampValue };
struct Problem {
    std::string title, body;
    Fix fix = Fix::None;
};
// The button text for a fix.
const char* fixLabel(Fix f);
// Applies `fix` to element `e` of canvas `c` as one undo step.
bool applyFix(Document& doc, int c, int e, Fix fix, Screen s);
// What is wrong with element `e` of canvas `c`: off screen or cut by the
// screen edge, over the runtime's element budget, text the export cuts or
// cannot draw, a missing font or texture, colours the GPU saturates.
std::vector<Problem> elementProblems(const Document& doc, int c, int e, Screen s);
std::vector<Problem> canvasProblems(const Document& doc, int c);

constexpr int kMaxCanvases = 24, kMaxElements = 256, kMaxCanvasElements = 255, kTextMax = 63;
// The export's limit on Scene::fonts (splashpack.cpp kUiMaxFonts).
constexpr int kMaxFonts = 3;

// ---- Rendering.

// Files the render reads, cached by path and settings until refresh().
class Assets {
public:
    Assets();
    ~Assets();
    // Drops everything read from disk so it is read again (the project changed
    // or Document::assetsId() moved on).
    void refresh(const std::filesystem::path& root, unsigned assetsId);

    struct Texture {
        int width = 0, height = 0;
        std::vector<uint16_t> words;  // 15-bit colour per texel, row 0 on top; 0 = transparent
        std::string error;
    };
    const Texture& texture(const std::string& path, splash::BitDepth depth, bool cutout);
    // The sheet a font name stands for; nullptr when the scene has no such
    // font or it cannot be built (the reason in *error).
    const splash::FontSheet* font(const splash::Scene& scene, const std::string& name, std::string* error = nullptr);

private:
    std::filesystem::path m_root;
    unsigned m_assetsId = ~0u;
    std::map<std::tuple<std::string, int, bool>, Texture> m_textures;
    struct Font;
    std::map<std::string, std::unique_ptr<Font>> m_fonts;
};

// Width in pixels of `text` as the runtime draws it in font `font` (empty =
// the system font).
int textWidth(const splash::Scene& scene, Assets& assets, const std::string& font, const std::string& text);

// `text` in font `font` of `scene`, white, drawn by render() on an image just
// big enough for it (its size in *size): a font's sample line.
std::vector<uint32_t> renderSample(const splash::Scene& scene, Assets& assets, const std::string& font, const std::string& text,
                                   uint32_t background, Screen* size);

// The canvases with `show[c]` set drawn on a w x h RGBA image (0xAABBGGRR,
// row 0 on top) as psxsplash draws them: canvases by sortOrder, elements in
// order, hidden elements skipped, system-font text last, 15-bit colour, PS1
// texture tinting. Pixels nothing drew keep `background`; `covered`, when
// given, gets 1 where something drew.
std::vector<uint32_t> render(const splash::Scene& scene, const std::vector<bool>& show, Screen s, Assets& assets,
                             uint32_t background, std::vector<uint8_t>* covered = nullptr);

}  // namespace uic
}  // namespace editor
