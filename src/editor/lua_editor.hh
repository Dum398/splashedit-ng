// The script editor's model: a text buffer with cursor, selection and undo,
// the Lua tokenizer behind the highlighting, completion, definitions and the
// live compile check. No ImGui; lua_editor_view draws it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace editor::lua {

// --- psxsplash API --------------------------------------------------------

enum class ApiKind { Table, Function, Constant, Callback };

struct ApiSymbol {
    const char* table;  // "Entity"; empty for globals and callbacks
    const char* name;   // "Find"; empty for the table itself
    ApiKind kind;
    const char* signature;
    const char* doc;
};

// Generated from psxsplash (tools/gen_lua_api.py), plus the Lua libraries
// psxlua opens.
const std::vector<ApiSymbol>& api();
// `Entity.Find` is ("Entity", "Find"); the table itself and globals like
// `print` have an empty table or name.
const ApiSymbol* findApi(std::string_view table, std::string_view name);
bool isApiTable(std::string_view name);

// --- Text -------------------------------------------------------------------

// Line and byte column, both from 0.
struct Pos {
    int line = 0, col = 0;
    friend bool operator==(Pos a, Pos b) { return a.line == b.line && a.col == b.col; }
    friend bool operator!=(Pos a, Pos b) { return !(a == b); }
    friend bool operator<(Pos a, Pos b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }
    friend bool operator<=(Pos a, Pos b) { return !(b < a); }
};

class Buffer {
public:
    Buffer() : m_lines(1) {}

    // Replaces everything and forgets the undo history.
    void setText(std::string_view text);
    std::string text() const;
    const std::vector<std::string>& lines() const { return m_lines; }
    int lineCount() const { return (int)m_lines.size(); }
    const std::string& line(int i) const { return m_lines[i]; }

    Pos cursor() const { return m_cursor; }
    Pos anchor() const { return m_anchor; }
    bool hasSelection() const { return m_cursor != m_anchor; }
    Pos selStart() const { return m_cursor < m_anchor ? m_cursor : m_anchor; }
    Pos selEnd() const { return m_cursor < m_anchor ? m_anchor : m_cursor; }
    std::string selectedText() const;
    std::string textRange(Pos a, Pos b) const;
    // Moves the cursor; with `select` the anchor stays put.
    void moveTo(Pos p, bool select = false);
    void select(Pos anchor, Pos cursor);
    void selectAll();
    Pos clamp(Pos p) const;

    // Cursor motions.
    enum class Move { Left, Right, Up, Down, Home, End, WordLeft, WordRight, Top, Bottom };
    void move(Move m, bool select, int count = 1);
    // Word under or just before p (identifier characters).
    void wordAt(Pos p, Pos* start, Pos* end) const;

    // Edits. Each replaces the selection when there is one and records undo.
    void insert(std::string_view s);
    // Typed text. ( [ { " and ' insert their closer too (quotes not inside a
    // word, a string or a comment; brackets only before a blank, the line's
    // end or punctuation), and wrap a selection on one line. Typing a closer
    // over one inserted that way steps over it. Anything else is insert().
    void type(std::string_view s);
    // Enter: a newline that keeps the indentation, one level deeper after an
    // opening line (`then`, `do`, `function (...)`, `{`, ...).
    void newline();
    // Between an opener and the closer type() put after it, deletes both.
    void backspace();
    void del();
    // Tab and Shift+Tab: indent or outdent the selected lines, or insert
    // spaces to the next tab stop.
    void indent();
    void outdent();
    // Adds or removes `-- ` on the selected lines.
    void toggleComment();
    // Replaces [a, b) with s; the cursor ends after s.
    void replace(Pos a, Pos b, std::string_view s);

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    // Ends the current run of typing, so the next keystroke is its own undo step.
    void breakUndoMerge() { m_mergeOpen = false; }

    // Changes with every edit, undo and redo; equal revisions mean equal text.
    uint64_t revision() const { return m_revision; }

    // Next match of `needle` from `from` (wrapping around), or nothing.
    std::optional<std::pair<Pos, Pos>> find(std::string_view needle, Pos from, bool forward, bool matchCase) const;
    int countMatches(std::string_view needle, bool matchCase) const;
    // Replace: with a match of `needle` selected, puts `with` there; then
    // selects the next match. True when it replaced one.
    bool replaceNext(std::string_view needle, std::string_view with, bool matchCase);
    // Every match (left to right, not overlapping) as one undo step; the
    // cursor stays where it was. Returns how many.
    int replaceAll(std::string_view needle, std::string_view with, bool matchCase);
    // Closers type() inserted that typing over still steps over, for tests.
    int autoClosers() const;

    static constexpr int kTab = 4;

private:
    struct Edit {
        Pos at;
        std::string removed, inserted;
        Pos cursorBefore, anchorBefore, cursorAfter;
        uint64_t revBefore = 0, revAfter = 0;
        int merge = 0;  // typing (1) or backspacing (2) that later keys may extend
    };
    Pos rawInsert(Pos at, std::string_view s);
    void rawErase(Pos a, Pos b);
    Pos endOf(Pos at, std::string_view s) const;
    void record(Edit e);
    void deleteSelection();
    bool selectionMatches(std::string_view needle, bool matchCase) const;
    void dropStaleClosers();

    std::vector<std::string> m_lines;
    Pos m_cursor, m_anchor;
    int m_wantCol = -1;  // column Up/Down aims for, in characters
    std::vector<Edit> m_undo, m_redo;
    bool m_mergeOpen = false;
    uint64_t m_revision = 1, m_nextRevision = 2;
    // Closers type() inserted, innermost last, valid while the text is at
    // m_closerRevision (any other edit forgets them).
    std::vector<Pos> m_closers;
    uint64_t m_closerRevision = 0;
};

// --- Highlighting -----------------------------------------------------------

enum class Tok : uint8_t {
    Text,       // whitespace
    Keyword,    // local, function, if, ...
    Constant,   // nil, true, false, self
    Identifier,
    Function,   // a name being called or defined
    Api,        // psxsplash or library table or global: Entity, print
    ApiMember,  // a known member of one: Entity.Find, Input.CROSS
    Number,
    String,
    Comment,
    Operator,
    Error,      // an unfinished string
};

struct Token {
    int col, len;
    Tok kind;
};

// Tokens for every line. Long strings and comments carry across lines.
std::vector<std::vector<Token>> tokenize(const std::vector<std::string>& lines);
std::vector<Token> tokenizeLine(const std::string& line, int* longLevel, bool* longComment);

// Display column of byte `col` (tabs to the next stop, one per character),
// and back.
int columnOf(const std::string& line, int col);
int byteAt(const std::string& line, int column);

// --- Live check -------------------------------------------------------------

// The first compile error, as psxlua (the PS1 compiler) reports it.
struct Diagnostic {
    int line = -1;  // from 0; -1 when the script compiles
    std::string message;
    bool ok() const { return line < 0; }
};
Diagnostic check(const std::string& source);

// --- Definitions and completion ------------------------------------------

enum class DefKind { Function, LocalFunction, Local, Parameter, Global };

struct Definition {
    std::string name;  // "foo", or "M.foo" for function M.foo()
    Pos pos;
    DefKind kind;
    std::string signature;  // "function foo(a, b)"; empty for variables
};

std::vector<Definition> definitions(const std::vector<std::string>& lines);

// What the name at `p` means: a definition in the file (the last one
// before p, else the first one after), or an API symbol.
struct Lookup {
    std::optional<Definition> def;
    const ApiSymbol* api = nullptr;
    Pos start, end;  // the name's span
    int arg = -1;    // callAt: the argument p is in, from 0
};
Lookup lookup(const std::vector<std::string>& lines, Pos p);

// The function whose argument list contains `p`, for the signature hint.
Lookup callAt(const std::vector<std::string>& lines, Pos p);

enum class ItemKind { Keyword, Local, Function, ApiTable, ApiFunction, ApiConstant, Callback, Field };

struct Completion {
    std::string label;
    ItemKind kind;
    std::string detail;  // signature
    const ApiSymbol* api = nullptr;
};

struct Completions {
    std::vector<Completion> items;
    Pos from;  // the typed prefix starts here; accepting replaces [from, cursor)
    std::string prefix;
};

// Candidates at `p`: after `Table.` the table's members, otherwise keywords,
// globals, the API and the file's own names. Best match first; empty
// prefixes only complete members (or everything when `force`).
Completions complete(const std::vector<std::string>& lines, Pos p, bool force = false);

// --- Script files -----------------------------------------------------------

class Script {
public:
    // A file that does not exist yet opens empty; saving creates it.
    bool open(const std::filesystem::path& file, std::string* error);
    bool save(std::string* error);
    bool dirty() const { return buffer.revision() != m_savedRevision; }
    const std::filesystem::path& path() const { return m_path; }
    std::string title() const;
    // Re-runs the compile check when the text changed since the last call.
    const Diagnostic& diagnose();
    const Diagnostic& diagnostic() const { return m_diag; }
    // Highlight tokens, cached per revision.
    const std::vector<std::vector<Token>>& tokens();

    // Whether the file on disk changed since it was opened, saved or
    // reloaded: its time or size moved and its text is not what was last
    // read or written. A file that went missing is not a change (a save
    // elsewhere may be replacing it; deletedOnDisk() tells); one that
    // appears is.
    bool changedOnDisk();
    // The file was on disk when this was opened, saved or reloaded and is
    // gone now: deleted or moved away. save() writes it again. A script
    // opened for a file not yet written is not deleted.
    bool deletedOnDisk() const;
    // Reads the file again. The new text replaces the old as one undo step,
    // so Ctrl+Z gets the old text back; the cursor stays on its line (moved
    // with the lines when the change was above it). Not dirty afterwards.
    bool reload(std::string* error);
    // Keep mine: this change on disk is no longer reported; saving
    // overwrites it.
    void keepMine();

    Buffer buffer;

private:
    struct Stamp {
        bool exists = false;
        std::filesystem::file_time_type time{};
        uintmax_t size = 0;
        friend bool operator==(const Stamp& a, const Stamp& b) { return a.exists == b.exists && a.time == b.time && a.size == b.size; }
    };
    Stamp stamp() const;
    bool read(std::string* text, std::string* error) const;

    std::filesystem::path m_path;
    Stamp m_stamp;
    std::string m_diskText;  // as last read or written, line endings and all
    bool m_crlf = false;  // written back with the line endings it had
    uint64_t m_savedRevision = 0, m_checkedRevision = 0, m_tokenRevision = 0;
    Diagnostic m_diag;
    std::vector<std::vector<Token>> m_tokens;
};

}  // namespace editor::lua
