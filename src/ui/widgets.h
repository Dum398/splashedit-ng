#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "ui/theme.h"

// The editor's own widget layer. Everything is drawn with ImDrawList from
// theme tokens; ImGui is used for input, ids and clipping only.
namespace ui {

// Linear blend of two colours, alpha included; t is clamped to 0..1.
ImU32 lerpColor(ImU32 a, ImU32 b, float t);

// Eased 0..1 value that follows `target` over time, keyed by id.
float anim(ImGuiID id, bool target, float speed = 14.0f);

// Text drawing.
ImVec2 measure(ImFont* font, float size, const char* text);
void text(ImDrawList* dl, ImVec2 pos, ImFont* font, float size, ImU32 col, const char* text);
void textCentered(ImDrawList* dl, ImRect r, ImFont* font, float size, ImU32 col, const char* text);

struct Hit {
    bool hovered = false;
    bool held = false;
    bool clicked = false;
    float hover = 0;  // animated
};

// Registers an interactive rectangle at an absolute screen position.
Hit interact(const char* id, ImRect r);

// Every rectangle interact() registered since the last clear. The window hit test
// uses it to keep clicks on title bar controls away from window dragging.
std::vector<ImRect>& interactiveRects();

// Controls by name, as the last frame drew them, for scripted checks
// (main.cpp's --click @name). Recorded only while namedRectsOn() is set (in
// screenshot runs); nameRect() is a no-op otherwise.
bool& namedRectsOn();
std::map<std::string, ImRect>& namedRects();
void nameRect(const std::string& name, ImRect r);

void tooltip(const char* text);

enum class ButtonKind { Primary, Secondary, Ghost };
bool button(const char* id, ImVec2 pos, const char* icon, const char* label, ButtonKind kind, float* outWidth = nullptr,
            const char* tip = nullptr);
// A disabled button keeps its tooltip but does not hover, press or click.
bool iconButton(const char* id, ImRect r, const char* icon, bool toggled = false, const char* tip = nullptr,
                ImU32 iconColor = theme::color::textDim, bool enabled = true);

// Segmented control; returns new selection.
int segmented(const char* id, ImVec2 pos, std::initializer_list<const char*> items, int selected, float* outWidth = nullptr);

// A text field with a search icon. With `query` it is editable: click to
// type, Escape clears (a second one leaves), the x clears; `focus` puts the caret in it
// this frame. Returns true while it has the keyboard. Without `searchIcon`
// it is a plain field (the script editor's Replace).
bool searchField(const char* id, ImRect r, const char* placeholder, const char* shortcut = nullptr, std::string* query = nullptr,
                 bool focus = false, bool searchIcon = true);

struct TreeRow {
    int depth = 0;
    const char* icon = nullptr;
    ImU32 iconColor = theme::color::textDim;
    const char* label = "";
    const char* meta = nullptr;  // right-aligned faint text
    bool hasChildren = false;
    bool expanded = false;
    bool selected = false;
    bool warning = false;
    bool hidden = false;
};
// Returns the row's hit state so the caller can tell a chevron click from a row click.
// With `visibilityClicked` the row draws an eye that sets it when clicked; the eye wins over the row.
Hit treeRow(const char* id, ImRect r, const TreeRow& row, bool* visibilityClicked = nullptr);

// Collapsible inspector section: clicking the header opens or closes it. The
// state is kept per id; `open` is how a section starts. Returns whether it is open.
// A removable section's ellipsis opens a menu; `removed` is set when its
// Remove component item is picked.
bool section(const char* id, ImRect r, const char* icon, ImU32 iconColor, const char* title, bool open, bool enabled = true,
             bool removable = true, bool* removed = nullptr, bool* toggled = nullptr);

// Inspector property row: draws the label (with a tooltip) and returns the value rect.
ImRect property(const char* id, ImRect row, float labelWidth, const char* label, const char* tip);

// What an editable field did this frame.
struct FieldEdit {
    bool changed = false;  // the value was written this frame
    bool done = false;     // the interaction ended (drag released, text committed or cancelled): close the undo step
    int index = 0;         // component that changed, for vec3Field
};

// A number in a well. With `edit`, it is editable: drag horizontally to
// scrub (faster for larger values, Shift for fine steps), double-click to
// type (Enter or clicking away commits, Esc cancels). `value` is the text shown.
FieldEdit numberField(const char* id, ImRect r, const char* value, const char* unit = nullptr, ImU32 axis = 0,
                      const char* axisLabel = nullptr, float* edit = nullptr);
// Three numberFields; `edit` points at three floats.
FieldEdit vec3Field(const char* id, ImRect r, const char* x, const char* y, const char* z, const char* unit = nullptr,
                    float* edit = nullptr);

// In-place text editing, styled as a focused field well. beginTextEdit()
// arms the editor for `id` (in the current ID scope); textEdit() draws it over
// `field` with the text starting at `textX` and ending before `textRight`.
enum class TextEdit { Inactive, Editing, Commit, Cancel };
void beginTextEdit(const char* id, const std::string& initial);
bool textEditing(const char* id);
// `maxLen` caps what can be typed, in bytes (at most 255). `charFilter`, when
// given, sees each typed character (ImGuiInputTextFlags_CallbackCharFilter):
// it may change EventChar, or return 1 to drop it.
TextEdit textEdit(const char* id, ImRect field, ImFont* font, float size, float textX, float textRight, std::string* out,
                  size_t maxLen = 255, ImGuiInputTextCallback charFilter = nullptr);
// A one-line text value in a field well: a click starts typing, Enter or
// clicking away commits, Esc cancels. Returns true on the frame a changed
// value is committed, with it in `out`. `placeholder` shows faint while the
// value is empty; with `maxLen`, typing stops there and a "12/32" count shows.
// `charFilter` is textEdit's.
bool textField(const char* id, ImRect r, const std::string& value, std::string* out, const char* placeholder = nullptr,
               size_t maxLen = 0, ImGuiInputTextCallback charFilter = nullptr);

void dropdown(const char* id, ImRect r, const char* icon, const char* value);
// A dropdown that opens a themed list under it. Returns the picked index, or -1.
int dropdownMenu(const char* id, ImRect r, const char* icon, const char* value, std::initializer_list<const char*> items, int current);
int dropdownMenu(const char* id, ImRect r, const char* icon, const char* value, const std::vector<const char*>& items, int current);

// Searchable list in a popup, opened with ImGui::OpenPopup(id) from the same
// ID stack. The caller keeps the search text in `query` and passes `items`
// filtered and ordered by it; the list follows the next frame. Up/Down move,
// Enter picks, Escape closes. Opens at `pos`, kept on screen. Returns the
// picked index into `items`, or -1.
struct PickerItem {
    const char* icon;
    ImU32 iconColor;
    const char* label;
    const char* blurb;        // second line
    const char* unavailable;  // why it cannot be picked, shown instead of the blurb; nullptr = available
};
int picker(const char* id, ImVec2 pos, float width, const char* placeholder, std::string* query, const std::vector<PickerItem>& items);

// Returns true when clicked.
bool assetField(const char* id, ImRect r, const char* icon, ImU32 iconColor, const char* name, const char* meta);
// Returns true on the frame the switch is clicked.
bool toggle(const char* id, ImRect r, bool on);
// Returns true when clicked.
bool colorField(const char* id, ImRect r, ImU32 col, const char* hex);
void slider(const char* id, ImRect r, float t, const char* value);

// Problem card with an inline fix action (none when fixLabel is null).
// Returns its height.
// `fixed`, when given, is set on the frame the fix button is clicked;
// `fixRect`, when given, gets the fix button's rect.
float problemCard(const char* id, ImVec2 pos, float width, const char* title, const char* body, const char* fixLabel,
                  bool* fixed = nullptr, ImRect* fixRect = nullptr);

// Budget meter for the status bar. Returns its width.
// fraction < 0 draws no bar (a plain count).
float meter(const char* id, ImVec2 pos, const char* icon, const char* label, float fraction, const char* value, const char* tip);

}  // namespace ui
