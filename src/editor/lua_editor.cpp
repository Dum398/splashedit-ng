#include "editor/lua_editor.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "luacompile.hh"

namespace editor::lua {

// --- psxsplash API --------------------------------------------------------

const std::vector<ApiSymbol>& api() {
    static const std::vector<ApiSymbol> v = {
#include "editor/lua_api.inc"
    };
    return v;
}

static const std::unordered_map<std::string, const ApiSymbol*>& apiIndex() {
    static const std::unordered_map<std::string, const ApiSymbol*> m = [] {
        std::unordered_map<std::string, const ApiSymbol*> out;
        for (const ApiSymbol& s : api()) {
            // Library globals have no table: "print". Tables have no name: "Entity".
            std::string key = !*s.table ? s.name : !*s.name ? s.table : std::string(s.table) + "." + s.name;
            out.emplace(key, &s);
        }
        return out;
    }();
    return m;
}

const ApiSymbol* findApi(std::string_view table, std::string_view name) {
    std::string key = table.empty() ? std::string(name) : name.empty() ? std::string(table) : std::string(table) + "." + std::string(name);
    auto it = apiIndex().find(key);
    return it == apiIndex().end() ? nullptr : it->second;
}

bool isApiTable(std::string_view name) {
    const ApiSymbol* s = findApi(name, "");
    return s && s->kind == ApiKind::Table;
}

// --- Helpers ----------------------------------------------------------------

static bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool isIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
static bool isCont(char c) { return ((unsigned char)c & 0xc0) == 0x80; }
static bool isSpace(char c) { return c == ' ' || c == '\t'; }

static const char* const kKeywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
                                        "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};

static bool isKeyword(std::string_view w) {
    for (const char* k : kKeywords)
        if (w == k) return true;
    return false;
}

static std::vector<std::string> splitLines(std::string_view s) {
    std::vector<std::string> out(1);
    for (char c : s) {
        if (c == '\n') out.emplace_back();
        else if (c != '\r') out.back() += c;
    }
    return out;
}

int columnOf(const std::string& line, int col) {
    int c = 0;
    for (int i = 0; i < col && i < (int)line.size(); ++i) {
        if (line[i] == '\t') c = (c / Buffer::kTab + 1) * Buffer::kTab;
        else if (!isCont(line[i])) ++c;
    }
    return c;
}

int byteAt(const std::string& line, int column) {
    int c = 0, i = 0;
    while (i < (int)line.size()) {
        int w = line[i] == '\t' ? (c / Buffer::kTab + 1) * Buffer::kTab - c : 1;
        // Past the middle of a character, the column is after it.
        if (c + w > column) return (column - c) * 2 >= w && w > 1 ? i + 1 : i;
        c += w;
        ++i;
        while (i < (int)line.size() && isCont(line[i])) ++i;
    }
    return (int)line.size();
}

static std::string indentOf(const std::string& line) {
    size_t n = 0;
    while (n < line.size() && isSpace(line[n])) ++n;
    return line.substr(0, n);
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// --- Buffer -----------------------------------------------------------------

void Buffer::setText(std::string_view text) {
    m_lines = splitLines(text);
    m_cursor = m_anchor = Pos();
    m_undo.clear();
    m_redo.clear();
    m_mergeOpen = false;
    m_wantCol = -1;
    m_revision = m_nextRevision++;
}

std::string Buffer::text() const {
    std::string out;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (i) out += '\n';
        out += m_lines[i];
    }
    return out;
}

Pos Buffer::clamp(Pos p) const {
    p.line = std::clamp(p.line, 0, (int)m_lines.size() - 1);
    p.col = std::clamp(p.col, 0, (int)m_lines[p.line].size());
    while (p.col > 0 && p.col < (int)m_lines[p.line].size() && isCont(m_lines[p.line][p.col])) --p.col;
    return p;
}

std::string Buffer::textRange(Pos a, Pos b) const {
    a = clamp(a), b = clamp(b);
    if (b < a) std::swap(a, b);
    if (a.line == b.line) return m_lines[a.line].substr(a.col, b.col - a.col);
    std::string out = m_lines[a.line].substr(a.col);
    for (int l = a.line + 1; l < b.line; ++l) out += "\n" + m_lines[l];
    return out + "\n" + m_lines[b.line].substr(0, b.col);
}

std::string Buffer::selectedText() const { return textRange(selStart(), selEnd()); }

void Buffer::moveTo(Pos p, bool select) {
    m_cursor = clamp(p);
    if (!select) m_anchor = m_cursor;
    m_wantCol = -1;
    m_mergeOpen = false;
}

void Buffer::select(Pos anchor, Pos cursor) {
    m_anchor = clamp(anchor);
    m_cursor = clamp(cursor);
    m_wantCol = -1;
    m_mergeOpen = false;
}

void Buffer::selectAll() { select(Pos(), Pos{lineCount() - 1, (int)m_lines.back().size()}); }

static int charClass(char c) { return isIdent(c) || ((unsigned char)c & 0x80) ? 2 : isSpace(c) ? 0 : 1; }

void Buffer::move(Move m, bool select, int count) {
    Pos p = m_cursor;
    int wantCol = m_wantCol;
    if (!select && hasSelection() && (m == Move::Left || m == Move::Right)) {
        p = m == Move::Left ? selStart() : selEnd();
        moveTo(p, false);
        return;
    }
    for (int n = 0; n < count; ++n) {
        const std::string& l = m_lines[p.line];
        switch (m) {
            case Move::Left:
                if (p.col > 0) {
                    --p.col;
                    while (p.col > 0 && isCont(l[p.col])) --p.col;
                } else if (p.line > 0) {
                    --p.line;
                    p.col = (int)m_lines[p.line].size();
                }
                break;
            case Move::Right:
                if (p.col < (int)l.size()) {
                    ++p.col;
                    while (p.col < (int)l.size() && isCont(l[p.col])) ++p.col;
                } else if (p.line + 1 < lineCount()) {
                    ++p.line;
                    p.col = 0;
                }
                break;
            case Move::Up:
            case Move::Down: {
                if (wantCol < 0) wantCol = columnOf(l, p.col);
                int to = p.line + (m == Move::Up ? -1 : 1);
                if (to < 0) p.col = 0;
                else if (to >= lineCount()) p.col = (int)l.size();
                else p = Pos{to, byteAt(m_lines[to], wantCol)};
                break;
            }
            case Move::Home: {
                // First press: the first non-blank; again: column 0.
                int first = (int)indentOf(l).size();
                p.col = p.col == first ? 0 : first;
                break;
            }
            case Move::End: p.col = (int)l.size(); break;
            case Move::WordLeft:
                if (p.col == 0) {
                    if (p.line > 0) p = Pos{p.line - 1, (int)m_lines[p.line - 1].size()};
                    break;
                }
                while (p.col > 0 && isSpace(l[p.col - 1])) --p.col;
                if (p.col > 0) {
                    int k = charClass(l[p.col - 1]);
                    while (p.col > 0 && charClass(l[p.col - 1]) == k) --p.col;
                }
                break;
            case Move::WordRight:
                if (p.col == (int)l.size()) {
                    if (p.line + 1 < lineCount()) p = Pos{p.line + 1, 0};
                    break;
                }
                {
                    int k = charClass(l[p.col]);
                    while (p.col < (int)l.size() && charClass(l[p.col]) == k) ++p.col;
                    while (p.col < (int)l.size() && isSpace(l[p.col])) ++p.col;
                }
                break;
            case Move::Top: p = Pos(); break;
            case Move::Bottom: p = Pos{lineCount() - 1, (int)m_lines.back().size()}; break;
        }
    }
    moveTo(p, select);
    if (m == Move::Up || m == Move::Down) m_wantCol = wantCol;
}

void Buffer::wordAt(Pos p, Pos* start, Pos* end) const {
    p = clamp(p);
    const std::string& l = m_lines[p.line];
    int a = p.col, b = p.col;
    while (a > 0 && isIdent(l[a - 1])) --a;
    while (b < (int)l.size() && isIdent(l[b])) ++b;
    *start = Pos{p.line, a};
    *end = Pos{p.line, b};
}

Pos Buffer::endOf(Pos at, std::string_view s) const {
    size_t nl = s.rfind('\n');
    if (nl == std::string_view::npos) return Pos{at.line, at.col + (int)s.size()};
    return Pos{at.line + (int)std::count(s.begin(), s.end(), '\n'), (int)(s.size() - nl - 1)};
}

Pos Buffer::rawInsert(Pos at, std::string_view s) {
    std::vector<std::string> parts = splitLines(s);
    std::string& l = m_lines[at.line];
    std::string tail = l.substr(at.col);
    l.erase(at.col);
    l += parts[0];
    if (parts.size() == 1) {
        l += tail;
        return Pos{at.line, at.col + (int)parts[0].size()};
    }
    Pos end{at.line + (int)parts.size() - 1, (int)parts.back().size()};
    parts.back() += tail;
    m_lines.insert(m_lines.begin() + at.line + 1, std::make_move_iterator(parts.begin() + 1), std::make_move_iterator(parts.end()));
    return end;
}

void Buffer::rawErase(Pos a, Pos b) {
    if (a.line == b.line) {
        m_lines[a.line].erase(a.col, b.col - a.col);
        return;
    }
    m_lines[a.line] = m_lines[a.line].substr(0, a.col) + m_lines[b.line].substr(b.col);
    m_lines.erase(m_lines.begin() + a.line + 1, m_lines.begin() + b.line + 1);
}

void Buffer::record(Edit e) {
    m_redo.clear();
    e.revAfter = m_revision = m_nextRevision++;
    if (m_mergeOpen && e.merge && !m_undo.empty() && m_undo.back().merge == e.merge) {
        Edit& last = m_undo.back();
        if (e.merge == 1 && e.removed.empty() && last.removed.empty() && e.at == endOf(last.at, last.inserted)) {
            // Words and the spaces after them are separate steps.
            char prev = last.inserted.back(), next = e.inserted.front();
            if (!(isSpace(prev) && !isSpace(next))) {
                last.inserted += e.inserted;
                last.cursorAfter = e.cursorAfter;
                last.revAfter = e.revAfter;
                return;
            }
        }
        if (e.merge == 2 && e.inserted.empty() && last.inserted.empty() && endOf(e.at, e.removed) == last.at) {
            last.removed = e.removed + last.removed;
            last.at = e.at;
            last.cursorAfter = e.cursorAfter;
            last.revAfter = e.revAfter;
            return;
        }
    }
    m_undo.push_back(std::move(e));
    m_mergeOpen = m_undo.back().merge != 0;
    if (m_undo.size() > 2000) m_undo.erase(m_undo.begin());
}

void Buffer::replace(Pos a, Pos b, std::string_view s) {
    a = clamp(a), b = clamp(b);
    if (b < a) std::swap(a, b);
    Edit e;
    e.at = a;
    e.removed = textRange(a, b);
    e.inserted = std::string(s);
    e.cursorBefore = m_cursor;
    e.anchorBefore = m_anchor;
    e.revBefore = m_revision;
    rawErase(a, b);
    Pos end = rawInsert(a, s);
    m_cursor = m_anchor = end;
    e.cursorAfter = end;
    m_wantCol = -1;
    m_mergeOpen = false;
    record(std::move(e));
}

void Buffer::insert(std::string_view s) {
    bool typing = s.size() == 1 && s[0] != '\n' && !hasSelection();
    // Typed characters may join the previous step; record() decides.
    bool open = m_mergeOpen;
    replace(selStart(), selEnd(), s);
    if (typing) {
        m_mergeOpen = open;
        Edit last = std::move(m_undo.back());
        m_undo.pop_back();
        last.merge = 1;
        record(std::move(last));
    }
}

void Buffer::deleteSelection() { replace(selStart(), selEnd(), ""); }

// --- Auto-closing brackets and quotes ------------------------------------------

static char closerOf(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        default: return 0;
    }
}

int Buffer::autoClosers() const { return m_closerRevision == m_revision ? (int)m_closers.size() : 0; }

void Buffer::dropStaleClosers() {
    if (m_closerRevision != m_revision) {
        m_closers.clear();
        return;
    }
    // The cursor left the pair: off the line, or past the closer.
    while (!m_closers.empty() && (m_closers.back().line != m_cursor.line || m_closers.back() < m_cursor)) m_closers.pop_back();
}

// Moves the closers on `line` at or after `col` by `by` bytes.
static void shiftClosers(std::vector<Pos>& closers, int line, int col, int by) {
    for (Pos& p : closers)
        if (p.line == line && p.col >= col) p.col += by;
}

void Buffer::type(std::string_view s) {
    dropStaleClosers();
    const char c = s.size() == 1 ? s[0] : 0;
    const std::string& l = m_lines[m_cursor.line];
    // Over a closer type() put there: step over it.
    if (c && !hasSelection() && !m_closers.empty() && m_closers.back() == m_cursor && m_cursor.col < (int)l.size() &&
        l[m_cursor.col] == c) {
        m_closers.pop_back();
        moveTo(Pos{m_cursor.line, m_cursor.col + 1});
        return;
    }
    const char close = closerOf(c);
    if (close && hasSelection() && selStart().line == selEnd().line) {
        // Wraps the selection, which stays selected inside.
        Pos a = selStart();
        std::string inner = selectedText();
        replace(a, selEnd(), std::string(1, c) + inner + close);
        select(Pos{a.line, a.col + 1}, Pos{a.line, a.col + 1 + (int)inner.size()});
        m_closers.clear();
        return;
    }
    if (close && !hasSelection()) {
        const int col = m_cursor.col;
        const char next = col < (int)l.size() ? l[col] : 0;
        bool pair = !next || std::strchr(" \t;:.,=)]}>", next);
        if (pair && (c == '"' || c == '\'')) {
            // Not after a word ("don't"), nor in a string or a comment.
            if (col > 0 && (isIdent(l[col - 1]) || ((unsigned char)l[col - 1] & 0x80))) pair = false;
            int level = -1;  // no long string or comment open
            bool comment = false;
            for (int i = 0; pair && i <= m_cursor.line; ++i) {
                std::vector<Token> toks = tokenizeLine(m_lines[i], &level, &comment);
                if (i < m_cursor.line) continue;
                for (const Token& t : toks) {
                    if (t.kind != Tok::String && t.kind != Tok::Comment && t.kind != Tok::Error) continue;
                    // At a token's end the cursor is still in it when the token
                    // runs on: a comment, an unfinished string, a long string.
                    const bool runsOn = t.col + t.len == (int)l.size() && (t.kind != Tok::String || level >= 0);
                    if (col > t.col && (col < t.col + t.len || (runsOn && col == t.col + t.len))) pair = false;
                }
            }
        }
        if (pair) {
            Pos at = m_cursor;
            shiftClosers(m_closers, at.line, at.col, 2);
            insert(std::string{c, close});
            m_cursor = m_anchor = Pos{at.line, at.col + 1};
            m_undo.back().cursorAfter = m_cursor;
            m_closers.push_back(m_cursor);
            m_closerRevision = m_revision;
            return;
        }
    }
    const bool sel = hasSelection();
    const Pos at = m_cursor;
    insert(s);
    if (!sel && s.find('\n') == std::string_view::npos) {
        shiftClosers(m_closers, at.line, at.col, (int)s.size());
        m_closerRevision = m_revision;
    } else {
        m_closers.clear();
    }
}

static bool opensBlock(const std::string& line) {
    std::string t = line;
    size_t c = t.find("--");
    if (c != std::string::npos) t = t.substr(0, c);
    t = trim(t);
    if (t.empty()) return false;
    auto endsWord = [&](const char* w) {
        size_t n = std::char_traits<char>::length(w);
        return t.size() >= n && t.compare(t.size() - n, n, w) == 0 && (t.size() == n || !isIdent(t[t.size() - n - 1]));
    };
    if (endsWord("then") || endsWord("do") || endsWord("else") || endsWord("repeat")) return true;
    if (t.back() == '{' || t.back() == '(') return true;
    // function f(a, b) / x = function() with the body on the next lines.
    size_t f = t.rfind("function");
    return f != std::string::npos && t.back() == ')' && t.find("end", f) == std::string::npos;
}

static bool closesBlock(const std::string& line) {
    std::string t = trim(line);
    for (const char* w : {"end", "else", "elseif", "until"}) {
        size_t n = std::char_traits<char>::length(w);
        if (t.compare(0, n, w) == 0 && (t.size() == n || !isIdent(t[n]))) return true;
    }
    return !t.empty() && t[0] == '}';
}

void Buffer::newline() {
    if (hasSelection()) deleteSelection();
    const std::string& l = m_lines[m_cursor.line];
    std::string unit = indentOf(l).find('\t') != std::string::npos ? "\t" : std::string(kTab, ' ');
    // A line that closes a block lines up with the line that opened it.
    if (closesBlock(l) && m_cursor.line > 0) {
        int prev = m_cursor.line - 1;
        while (prev > 0 && trim(m_lines[prev]).empty()) --prev;
        std::string want = indentOf(m_lines[prev]);
        if (!opensBlock(m_lines[prev]) && want.size() >= unit.size()) want.erase(want.size() - unit.size());
        std::string have = indentOf(l);
        if (have.size() > want.size()) {
            int col = m_cursor.col - (int)(have.size() - want.size());
            replace(Pos{m_cursor.line, 0}, Pos{m_cursor.line, (int)have.size()}, want);
            moveTo(Pos{m_cursor.line, std::max(col, (int)want.size())});
        }
    }
    const std::string& cur = m_lines[m_cursor.line];
    std::string indent = indentOf(cur.substr(0, m_cursor.col));
    if (opensBlock(cur.substr(0, m_cursor.col))) indent += unit;
    // Typing over the indentation: drop the blanks the cursor sits in.
    Pos from = m_cursor, to = m_cursor;
    while (to.col < (int)cur.size() && isSpace(cur[to.col])) ++to.col;
    replace(from, to, "\n" + indent);
}

void Buffer::backspace() {
    if (hasSelection()) return deleteSelection();
    if (m_cursor == Pos()) return;
    dropStaleClosers();
    {
        // Between an opener and its auto-inserted closer: both go.
        const std::string& l = m_lines[m_cursor.line];
        const int col = m_cursor.col;
        if (!m_closers.empty() && m_closers.back() == m_cursor && col > 0 && col < (int)l.size() && closerOf(l[col - 1]) == l[col]) {
            m_closers.pop_back();
            shiftClosers(m_closers, m_cursor.line, col, -2);
            replace(Pos{m_cursor.line, col - 1}, Pos{m_cursor.line, col + 1}, "");
            m_closerRevision = m_revision;
            return;
        }
    }
    bool open = m_mergeOpen;
    Pos to = m_cursor, from = m_cursor;
    const std::string& l = m_lines[m_cursor.line];
    // In leading blanks, back to the previous tab stop.
    if (m_cursor.col > 0 && m_cursor.col <= (int)indentOf(l).size() && l[m_cursor.col - 1] == ' ') {
        int col = columnOf(l, m_cursor.col);
        int stop = (col - 1) / kTab * kTab;
        while (from.col > 0 && l[from.col - 1] == ' ' && columnOf(l, from.col) > stop) --from.col;
    } else {
        Pos keep = m_cursor;
        move(Move::Left, false);
        from = m_cursor;
        m_cursor = m_anchor = keep;
    }
    replace(from, to, "");
    m_mergeOpen = open;
    Edit last = std::move(m_undo.back());
    m_undo.pop_back();
    last.merge = 2;
    record(std::move(last));
    // Typing fixes inside a pair keep its closer.
    if (from.line == to.line) {
        shiftClosers(m_closers, to.line, to.col, from.col - to.col);
        m_closerRevision = m_revision;
    }
}

void Buffer::del() {
    if (hasSelection()) return deleteSelection();
    Pos from = m_cursor;
    move(Move::Right, false);
    Pos to = m_cursor;
    m_cursor = m_anchor = from;
    if (from != to) replace(from, to, "");
}

void Buffer::indent() {
    if (!hasSelection() || selStart().line == selEnd().line) {
        int col = columnOf(m_lines[m_cursor.line], m_cursor.col);
        insert(std::string(kTab - col % kTab, ' '));
        return;
    }
    Pos a = selStart(), b = selEnd();
    int last = b.col == 0 ? b.line - 1 : b.line;
    std::string out;
    for (int l = a.line; l <= last; ++l) {
        if (l > a.line) out += '\n';
        out += (m_lines[l].empty() ? "" : std::string(kTab, ' ')) + m_lines[l];
    }
    Pos end{last, (int)m_lines[last].size()};
    replace(Pos{a.line, 0}, end, out);
    select(Pos{a.line, 0}, Pos{last, (int)m_lines[last].size()});
}

void Buffer::outdent() {
    Pos a = selStart(), b = selEnd();
    int last = hasSelection() && b.col == 0 && b.line > a.line ? b.line - 1 : b.line;
    std::string out;
    bool changed = false;
    for (int l = a.line; l <= last; ++l) {
        const std::string& s = m_lines[l];
        int n = 0;
        if (!s.empty() && s[0] == '\t') n = 1;
        else
            while (n < kTab && n < (int)s.size() && s[n] == ' ') ++n;
        changed |= n > 0;
        if (l > a.line) out += '\n';
        out += s.substr(n);
    }
    if (!changed) return;
    bool sel = hasSelection();
    Pos cur = m_cursor;
    int before = (int)m_lines[cur.line].size();
    replace(Pos{a.line, 0}, Pos{last, (int)m_lines[last].size()}, out);
    if (sel) select(Pos{a.line, 0}, Pos{last, (int)m_lines[last].size()});
    else moveTo(Pos{cur.line, std::max(0, cur.col - (before - (int)m_lines[cur.line].size()))});
}

void Buffer::toggleComment() {
    Pos a = selStart(), b = selEnd();
    int last = hasSelection() && b.col == 0 && b.line > a.line ? b.line - 1 : b.line;
    bool all = true;
    size_t minIndent = std::string::npos;
    for (int l = a.line; l <= last; ++l) {
        std::string t = trim(m_lines[l]);
        if (t.empty()) continue;
        all &= t.compare(0, 2, "--") == 0;
        minIndent = std::min(minIndent, indentOf(m_lines[l]).size());
    }
    if (minIndent == std::string::npos) return;
    std::string out;
    for (int l = a.line; l <= last; ++l) {
        std::string s = m_lines[l];
        if (!trim(s).empty()) {
            size_t i = indentOf(s).size();
            if (all) s.erase(i, s.compare(i, 3, "-- ") == 0 ? 3 : 2);
            else s.insert(minIndent, "-- ");
        }
        if (l > a.line) out += '\n';
        out += s;
    }
    bool sel = hasSelection();
    Pos cur = m_cursor;
    int before = (int)m_lines[cur.line].size();
    replace(Pos{a.line, 0}, Pos{last, (int)m_lines[last].size()}, out);
    if (sel) select(Pos{a.line, 0}, Pos{last, (int)m_lines[last].size()});
    else moveTo(Pos{cur.line, std::max(0, cur.col + ((int)m_lines[cur.line].size() - before))});
}

bool Buffer::undo() {
    if (m_undo.empty()) return false;
    Edit e = std::move(m_undo.back());
    m_undo.pop_back();
    rawErase(e.at, endOf(e.at, e.inserted));
    rawInsert(e.at, e.removed);
    m_cursor = clamp(e.cursorBefore);
    m_anchor = clamp(e.anchorBefore);
    m_revision = e.revBefore;
    m_redo.push_back(std::move(e));
    m_mergeOpen = false;
    m_wantCol = -1;
    return true;
}

bool Buffer::redo() {
    if (m_redo.empty()) return false;
    Edit e = std::move(m_redo.back());
    m_redo.pop_back();
    rawErase(e.at, endOf(e.at, e.removed));
    rawInsert(e.at, e.inserted);
    m_cursor = m_anchor = clamp(e.cursorAfter);
    m_revision = e.revAfter;
    m_undo.push_back(std::move(e));
    m_mergeOpen = false;
    m_wantCol = -1;
    return true;
}

static size_t findIn(const std::string& hay, std::string_view needle, size_t from, bool matchCase) {
    if (matchCase) return hay.find(needle.data(), from, needle.size());
    auto lower = [](char c) { return (char)std::tolower((unsigned char)c); };
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(hay[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

std::optional<std::pair<Pos, Pos>> Buffer::find(std::string_view needle, Pos from, bool forward, bool matchCase) const {
    if (needle.empty() || needle.find('\n') != std::string_view::npos) return std::nullopt;
    // All matches in order; few enough in a script that a scan is fine.
    std::vector<Pos> hits;
    for (int l = 0; l < lineCount(); ++l)
        for (size_t at = findIn(m_lines[l], needle, 0, matchCase); at != std::string::npos;
             at = findIn(m_lines[l], needle, at + 1, matchCase))
            hits.push_back(Pos{l, (int)at});
    if (hits.empty()) return std::nullopt;
    Pos hit = hits.front();
    if (forward) {
        auto it = std::find_if(hits.begin(), hits.end(), [&](Pos p) { return from <= p; });
        hit = it == hits.end() ? hits.front() : *it;
    } else {
        auto it = std::find_if(hits.rbegin(), hits.rend(), [&](Pos p) { return p < from; });
        hit = it == hits.rend() ? hits.back() : *it;
    }
    return std::make_pair(hit, Pos{hit.line, hit.col + (int)needle.size()});
}

bool Buffer::selectionMatches(std::string_view needle, bool matchCase) const {
    if (!hasSelection() || selStart().line != selEnd().line) return false;
    std::string t = selectedText();
    return t.size() == needle.size() && findIn(t, needle, 0, matchCase) == 0;
}

bool Buffer::replaceNext(std::string_view needle, std::string_view with, bool matchCase) {
    if (needle.empty()) return false;
    const bool did = selectionMatches(needle, matchCase);
    if (did) replace(selStart(), selEnd(), with);
    // After the replacement, so `with` containing `needle` is not found again.
    if (auto m = find(needle, did ? m_cursor : selStart(), true, matchCase)) select(m->first, m->second);
    return did;
}

int Buffer::replaceAll(std::string_view needle, std::string_view with, bool matchCase) {
    if (needle.empty() || needle.find('\n') != std::string_view::npos) return 0;
    int n = 0, first = -1, last = -1;
    std::vector<std::string> out(m_lines.size());
    std::vector<char> changed(m_lines.size(), 0);
    for (int l = 0; l < lineCount(); ++l) {
        const std::string& line = m_lines[l];
        size_t at = 0;
        bool any = false;
        for (size_t hit; (hit = findIn(line, needle, at, matchCase)) != std::string::npos; at = hit + needle.size()) {
            out[l].append(line, at, hit - at);
            out[l] += with;
            ++n;
            any = true;
        }
        if (!any) continue;
        out[l].append(line, at, std::string::npos);
        changed[l] = 1;
        if (first < 0) first = l;
        last = l;
    }
    if (!n) return 0;
    std::string text;
    for (int l = first; l <= last; ++l) {
        if (l > first) text += '\n';
        text += changed[l] ? out[l] : m_lines[l];
    }
    const Pos keep = m_cursor;
    replace(Pos{first, 0}, Pos{last, (int)m_lines[last].size()}, text);
    m_cursor = m_anchor = clamp(keep);
    m_undo.back().cursorAfter = m_cursor;
    return n;
}

int Buffer::countMatches(std::string_view needle, bool matchCase) const {
    if (needle.empty()) return 0;
    int n = 0;
    for (const std::string& l : m_lines)
        for (size_t at = findIn(l, needle, 0, matchCase); at != std::string::npos; at = findIn(l, needle, at + 1, matchCase)) ++n;
    return n;
}

// --- Tokenizer --------------------------------------------------------------

// `[`, `=`*, `[` at i: the level (count of =), else -1.
static int longOpen(const std::string& s, size_t i) {
    if (i >= s.size() || s[i] != '[') return -1;
    size_t j = i + 1;
    while (j < s.size() && s[j] == '=') ++j;
    return j < s.size() && s[j] == '[' ? (int)(j - i - 1) : -1;
}

// End of `]`, `=`*level, `]` at or after i, or npos.
static size_t longClose(const std::string& s, size_t i, int level) {
    std::string close = "]" + std::string(level, '=') + "]";
    size_t at = s.find(close, i);
    return at == std::string::npos ? at : at + close.size();
}

std::vector<Token> tokenizeLine(const std::string& s, int* longLevel, bool* longComment) {
    std::vector<Token> raw;
    size_t i = 0, n = s.size();
    if (*longLevel >= 0) {
        size_t end = longClose(s, 0, *longLevel);
        Tok k = *longComment ? Tok::Comment : Tok::String;
        if (end == std::string::npos) {
            if (n) raw.push_back({0, (int)n, k});
            return raw;
        }
        raw.push_back({0, (int)end, k});
        i = end;
        *longLevel = -1;
    }
    while (i < n) {
        char c = s[i];
        size_t start = i;
        if (isSpace(c)) {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            int level = longOpen(s, i + 2);
            if (level >= 0) {
                size_t end = longClose(s, i + 4 + level, level);
                if (end == std::string::npos) {
                    *longLevel = level;
                    *longComment = true;
                    end = n;
                }
                raw.push_back({(int)start, (int)(end - start), Tok::Comment});
                i = end;
            } else {
                raw.push_back({(int)start, (int)(n - start), Tok::Comment});
                i = n;
            }
            continue;
        }
        if (int level = longOpen(s, i); level >= 0) {
            size_t end = longClose(s, i + 2 + level, level);
            if (end == std::string::npos) {
                *longLevel = level;
                *longComment = false;
                end = n;
            }
            raw.push_back({(int)start, (int)(end - start), Tok::String});
            i = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            ++i;
            bool closed = false;
            while (i < n) {
                if (s[i] == '\\') i += 2;
                else if (s[i++] == c) {
                    closed = true;
                    break;
                }
            }
            i = std::min(i, n);
            raw.push_back({(int)start, (int)(i - start), closed ? Tok::String : Tok::Error});
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                while (i < n && (std::isxdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
            } else {
                while (i < n && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
                if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                    ++i;
                    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
                    while (i < n && std::isdigit((unsigned char)s[i])) ++i;
                }
            }
            raw.push_back({(int)start, (int)(i - start), Tok::Number});
            continue;
        }
        if (isIdentStart(c) || ((unsigned char)c & 0x80)) {
            while (i < n && (isIdent(s[i]) || ((unsigned char)s[i] & 0x80))) ++i;
            raw.push_back({(int)start, (int)(i - start), Tok::Identifier});
            continue;
        }
        // Longest operator first.
        static const char* const ops[] = {"...", "..", "==", "~=", "<=", ">=", "::"};
        size_t len = 1;
        for (const char* op : ops) {
            size_t l = std::char_traits<char>::length(op);
            if (s.compare(i, l, op) == 0) {
                len = l;
                break;
            }
        }
        raw.push_back({(int)start, (int)len, Tok::Operator});
        i += len;
    }

    // Names: keywords, the API, calls and definitions.
    auto text = [&](size_t k) { return std::string_view(s).substr(raw[k].col, raw[k].len); };
    for (size_t k = 0; k < raw.size(); ++k) {
        if (raw[k].kind != Tok::Identifier) continue;
        std::string_view w = text(k);
        if (w == "nil" || w == "true" || w == "false" || w == "self") {
            raw[k].kind = Tok::Constant;
            continue;
        }
        if (isKeyword(w)) {
            raw[k].kind = Tok::Keyword;
            continue;
        }
        bool member = k >= 2 && raw[k - 1].kind == Tok::Operator && (text(k - 1) == "." || text(k - 1) == ":");
        if (member) {
            if (raw[k - 2].kind == Tok::Api && findApi(text(k - 2), w)) {
                raw[k].kind = Tok::ApiMember;
                continue;
            }
        } else if (const ApiSymbol* a = findApi("", w); a && a->kind != ApiKind::Callback) {
            raw[k].kind = Tok::Api;
            continue;
        }
        size_t next = raw[k].col + raw[k].len;
        while (next < n && isSpace(s[next])) ++next;
        bool called = next < n && (s[next] == '(' || s[next] == '"' || s[next] == '{');
        // function a.b:c(): every part after `function` is the definition.
        bool defined = false;
        for (size_t j = k; j > 0; --j) {
            if (raw[j - 1].kind == Tok::Keyword && text(j - 1) == "function") {
                defined = true;
                break;
            }
            if (!(raw[j - 1].kind == Tok::Operator && (text(j - 1) == "." || text(j - 1) == ":")) &&
                raw[j - 1].kind != Tok::Identifier)
                break;
        }
        if (called || defined) raw[k].kind = Tok::Function;
    }
    return raw;
}

std::vector<std::vector<Token>> tokenize(const std::vector<std::string>& lines) {
    std::vector<std::vector<Token>> out;
    out.reserve(lines.size());
    int level = -1;
    bool comment = false;
    for (const std::string& l : lines) out.push_back(tokenizeLine(l, &level, &comment));
    return out;
}

// --- Live check -------------------------------------------------------------

Diagnostic check(const std::string& source) {
    Diagnostic d;
    try {
        splash::compileLua(source, "=script");
    } catch (const std::runtime_error& e) {
        // "script:12: 'end' expected near <eof>"
        std::string msg = e.what();
        d.line = 0;
        d.message = msg;
        if (msg.compare(0, 7, "script:") == 0) {
            char* end = nullptr;
            long line = std::strtol(msg.c_str() + 7, &end, 10);
            if (end && *end == ':') {
                d.line = (int)std::max(0L, line - 1);
                d.message = trim(std::string(end + 1));
            }
        }
        // At the end of the file: on its last line with text, not the blank after it.
        if (d.message.find("<eof>") != std::string::npos) {
            std::vector<std::string> lines = splitLines(source);
            d.line = std::min(d.line, (int)lines.size() - 1);
            while (d.line > 0 && trim(lines[d.line]).empty()) --d.line;
        }
    }
    return d;
}

// --- Definitions --------------------------------------------------------------

namespace {

struct Lex {
    std::string text;
    Pos pos;
    Tok kind;
};

// Every token but comments, with its text.
std::vector<Lex> lexAll(const std::vector<std::string>& lines, Pos stop = Pos{1 << 30, 0}) {
    std::vector<Lex> out;
    std::vector<std::vector<Token>> toks = tokenize(lines);
    for (int l = 0; l < (int)lines.size() && l <= stop.line; ++l)
        for (const Token& t : toks[l]) {
            if (t.kind == Tok::Comment) continue;
            if (Pos{l, t.col} < stop) out.push_back({lines[l].substr(t.col, t.len), Pos{l, t.col}, t.kind});
        }
    return out;
}

bool isName(const Lex& t) {
    return t.kind == Tok::Identifier || t.kind == Tok::Function || t.kind == Tok::Api || t.kind == Tok::ApiMember ||
           (t.kind == Tok::Constant && t.text == "self");
}

bool isOp(const Lex& t, const char* op) { return t.kind == Tok::Operator && t.text == op; }

// Parameters from the `(` at k; returns the index after `)`.
size_t params(const std::vector<Lex>& t, size_t k, std::vector<Definition>* defs, std::string* sig) {
    if (k >= t.size() || !isOp(t[k], "(")) return k;
    *sig += "(";
    ++k;
    bool first = true;
    while (k < t.size() && !isOp(t[k], ")")) {
        if (isName(t[k]) || isOp(t[k], "...")) {
            if (!first) *sig += ", ";
            *sig += t[k].text;
            first = false;
            if (isName(t[k])) defs->push_back({t[k].text, t[k].pos, DefKind::Parameter, ""});
        }
        ++k;
    }
    *sig += ")";
    return k < t.size() ? k + 1 : k;
}

}  // namespace

std::vector<Definition> definitions(const std::vector<std::string>& lines) {
    std::vector<Definition> defs;
    std::vector<Lex> t = lexAll(lines);
    std::set<std::string> known;
    for (size_t k = 0; k < t.size(); ++k) {
        const Lex& tk = t[k];
        bool local = tk.kind == Tok::Keyword && tk.text == "local";
        if (local && k + 2 < t.size() && t[k + 1].text == "function" && isName(t[k + 2])) {
            Definition d{t[k + 2].text, t[k + 2].pos, DefKind::LocalFunction, "function " + t[k + 2].text};
            size_t at = defs.size();
            defs.push_back(d);
            std::string sig;  // params() appends to defs
            params(t, k + 3, &defs, &sig);
            defs[at].signature += sig;
            known.insert(d.name);
            k += 2;
            continue;
        }
        if (local) {
            size_t j = k + 1;
            while (j < t.size() && isName(t[j])) {
                defs.push_back({t[j].text, t[j].pos, DefKind::Local, ""});
                known.insert(t[j].text);
                if (j + 1 < t.size() && isOp(t[j + 1], ",")) j += 2;
                else break;
            }
            continue;
        }
        if (tk.kind == Tok::Keyword && tk.text == "function") {
            size_t j = k + 1;
            std::string name;
            Pos pos = tk.pos;
            while (j < t.size() && isName(t[j])) {
                name += t[j].text;
                pos = t[j].pos;
                if (j + 1 < t.size() && (isOp(t[j + 1], ".") || isOp(t[j + 1], ":"))) {
                    name += t[j + 1].text;
                    j += 2;
                } else {
                    ++j;
                    break;
                }
            }
            size_t at = defs.size();
            if (!name.empty()) {
                defs.push_back({name, pos, DefKind::Function, "function " + name});
                known.insert(name);
            }
            std::string sig;
            params(t, j, &defs, &sig);
            if (!name.empty()) defs[at].signature += sig;
            continue;
        }
        if (tk.kind == Tok::Keyword && tk.text == "for") {
            size_t j = k + 1;
            while (j < t.size() && isName(t[j])) {
                defs.push_back({t[j].text, t[j].pos, DefKind::Local, ""});
                if (j + 1 < t.size() && isOp(t[j + 1], ",")) j += 2;
                else break;
            }
            continue;
        }
        // name = ... at the start of a statement: a global, the first time.
        if (isName(tk) && k + 1 < t.size() && isOp(t[k + 1], "=") && !known.count(tk.text)) {
            bool statement = k == 0 || t[k - 1].pos.line < tk.pos.line || isOp(t[k - 1], ";");
            bool member = k > 0 && (isOp(t[k - 1], ".") || isOp(t[k - 1], ":"));
            if (statement && !member) {
                defs.push_back({tk.text, tk.pos, DefKind::Global, ""});
                known.insert(tk.text);
            }
        }
    }
    return defs;
}

// The name at p and the `Table.` before it, if any.
static bool nameAt(const std::vector<std::string>& lines, Pos p, std::string* qualifier, std::string* name, Pos* start,
                   Pos* end) {
    if (p.line < 0 || p.line >= (int)lines.size()) return false;
    const std::string& l = lines[p.line];
    int a = std::min(p.col, (int)l.size()), b = a;
    while (a > 0 && isIdent(l[a - 1])) --a;
    while (b < (int)l.size() && isIdent(l[b])) ++b;
    if (a == b || !isIdentStart(l[a])) return false;
    *name = l.substr(a, b - a);
    *start = Pos{p.line, a};
    *end = Pos{p.line, b};
    qualifier->clear();
    int q = a;
    while (q > 0 && isSpace(l[q - 1])) --q;
    if (q > 0 && (l[q - 1] == '.' || l[q - 1] == ':') && !(q > 1 && l[q - 2] == '.')) {
        int e = q - 1;
        while (e > 0 && isSpace(l[e - 1])) --e;
        int s = e;
        while (s > 0 && isIdent(l[s - 1])) --s;
        if (s < e) *qualifier = l.substr(s, e - s);
    }
    return true;
}

Lookup lookup(const std::vector<std::string>& lines, Pos p) {
    Lookup r;
    std::string q, name;
    if (!nameAt(lines, p, &q, &name, &r.start, &r.end)) return r;
    std::string full = q.empty() ? name : q + "." + name;
    std::vector<Definition> defs = definitions(lines);
    const Definition* best = nullptr;
    for (const Definition& d : defs) {
        std::string dn = d.name;
        std::replace(dn.begin(), dn.end(), ':', '.');
        if (dn != full) continue;
        // The last one at or before p, else the first one after.
        if (d.pos <= r.start) best = &d;
        else if (!best) best = &d;
    }
    // A local shadows the API; the API's own names otherwise.
    if (best && !(q.empty() && best->kind == DefKind::Global && findApi("", name))) r.def = *best;
    if (!r.def) r.api = q.empty() ? findApi("", name) : (isApiTable(q) ? findApi(q, name) : nullptr);
    return r;
}

Lookup callAt(const std::vector<std::string>& lines, Pos p) {
    std::vector<Lex> t = lexAll(lines, p);
    int depth = 0, arg = 0;
    for (size_t k = t.size(); k-- > 0;) {
        if (t[k].pos.line < p.line - 30) break;
        if (isOp(t[k], ")") || isOp(t[k], "}") || isOp(t[k], "]")) ++depth;
        else if (isOp(t[k], "{") || isOp(t[k], "[")) {
            if (depth == 0) return Lookup();
            --depth;
        } else if (isOp(t[k], "(")) {
            if (depth == 0) {
                if (k == 0 || !isName(t[k - 1])) return Lookup();
                if (k >= 2 && t[k - 2].kind == Tok::Keyword && t[k - 2].text == "function") return Lookup();
                Lookup r = lookup(lines, t[k - 1].pos);
                r.arg = arg;
                return r;
            }
            --depth;
        } else if (depth == 0 && isOp(t[k], ",")) ++arg;
        else if (depth == 0 && t[k].kind == Tok::Keyword && t[k].text != "function" && t[k].text != "nil" &&
                 t[k].text != "not" && t[k].text != "and" && t[k].text != "or")
            return Lookup();
    }
    return Lookup();
}

// --- Completion -------------------------------------------------------------

// 0 best; -1 no match. Prefix, then case-insensitive prefix, then substring,
// then the letters in order.
static int matchScore(std::string_view label, std::string_view prefix) {
    if (prefix.empty()) return 0;
    if (label.substr(0, prefix.size()) == prefix) return 0;
    auto lower = [](std::string_view s) {
        std::string o(s);
        for (char& c : o) c = (char)std::tolower((unsigned char)c);
        return o;
    };
    std::string l = lower(label), p = lower(prefix);
    if (l.compare(0, p.size(), p) == 0) return 1;
    if (l.find(p) != std::string::npos) return 2;
    size_t i = 0;
    for (char c : l)
        if (i < p.size() && c == p[i]) ++i;
    return i == p.size() && p.size() >= 3 ? 3 : -1;
}

static ItemKind kindOf(const ApiSymbol& s) {
    switch (s.kind) {
        case ApiKind::Table: return ItemKind::ApiTable;
        case ApiKind::Function: return ItemKind::ApiFunction;
        case ApiKind::Constant: return ItemKind::ApiConstant;
        case ApiKind::Callback: return ItemKind::Callback;
    }
    return ItemKind::ApiFunction;
}

static int kindRank(ItemKind k) {
    switch (k) {
        case ItemKind::Local: return 0;
        case ItemKind::Function: return 0;
        case ItemKind::Field: return 1;
        case ItemKind::Callback: return 1;
        case ItemKind::ApiFunction: return 1;
        case ItemKind::ApiConstant: return 1;
        case ItemKind::ApiTable: return 2;
        case ItemKind::Keyword: return 3;
    }
    return 3;
}

Completions complete(const std::vector<std::string>& lines, Pos p, bool force) {
    Completions out;
    if (p.line < 0 || p.line >= (int)lines.size()) return out;
    const std::string& l = lines[p.line];
    p.col = std::clamp(p.col, 0, (int)l.size());

    // Nothing inside comments and strings.
    {
        std::vector<std::vector<Token>> toks = tokenize(std::vector<std::string>(lines.begin(), lines.begin() + p.line + 1));
        for (const Token& t : toks.back())
            if ((t.kind == Tok::Comment || t.kind == Tok::String || t.kind == Tok::Error) && t.col < p.col &&
                (p.col < t.col + t.len || (p.col == t.col + t.len && (t.kind != Tok::String || t.len < 2))))
                return out;
    }

    int a = p.col;
    while (a > 0 && isIdent(l[a - 1])) --a;
    if (a < p.col && std::isdigit((unsigned char)l[a])) return out;
    out.from = Pos{p.line, a};
    out.prefix = l.substr(a, p.col - a);

    std::string q;
    bool method = false;
    {
        int k = a;
        while (k > 0 && isSpace(l[k - 1])) --k;
        if (k > 0 && (l[k - 1] == '.' || l[k - 1] == ':') && !(k > 1 && l[k - 2] == '.')) {
            method = l[k - 1] == ':';
            int e = k - 1, s = e;
            while (s > 0 && isIdent(l[s - 1])) --s;
            q = l.substr(s, e - s);
            if (q.empty()) return out;
        }
    }

    std::map<std::string, Completion> found;
    auto add = [&](const std::string& label, ItemKind kind, const std::string& detail, const ApiSymbol* sym) {
        if (label.empty() || matchScore(label, out.prefix) < 0) return;
        auto it = found.find(label);
        if (it == found.end() || kindRank(kind) < kindRank(it->second.kind)) found[label] = Completion{label, kind, detail, sym};
    };
    std::vector<Lex> toks = lexAll(lines);
    // The word being typed is not a candidate for itself.
    auto typing = [&](const Lex& t) { return t.pos.line == p.line && t.pos.col == a; };

    if (!q.empty()) {
        if (isApiTable(q)) {
            for (const ApiSymbol& s : api())
                if (q == s.table && *s.name) add(s.name, kindOf(s), s.signature, &s);
        } else {
            // Fields the file uses or defines on that name.
            for (const Definition& d : definitions(lines)) {
                std::string pre = q + (method ? ":" : ".");
                if (d.name.compare(0, pre.size(), pre) == 0) add(d.name.substr(pre.size()), ItemKind::Function, d.signature, nullptr);
                std::string pre2 = q + (method ? "." : ":");
                if (d.name.compare(0, pre2.size(), pre2) == 0) add(d.name.substr(pre2.size()), ItemKind::Function, d.signature, nullptr);
            }
            for (size_t k = 2; k < toks.size(); ++k)
                if (isName(toks[k]) && !typing(toks[k]) && (isOp(toks[k - 1], ".") || isOp(toks[k - 1], ":")) && toks[k - 2].text == q)
                    add(toks[k].text, ItemKind::Field, "", nullptr);
        }
    } else {
        if (out.prefix.empty() && !force) return out;
        bool afterFunction = false;
        {
            int k = a;
            while (k > 0 && isSpace(l[k - 1])) --k;
            afterFunction = k >= 8 && l.compare(k - 8, 8, "function") == 0 && (k == 8 || !isIdent(l[k - 9]));
        }
        for (const Definition& d : definitions(lines))
            if (d.name.find_first_of(".:") == std::string::npos && !(d.pos.line == p.line && d.pos.col == a))
                add(d.name, d.kind == DefKind::Function || d.kind == DefKind::LocalFunction ? ItemKind::Function : ItemKind::Local,
                    d.signature, nullptr);
        for (const Lex& t : toks)
            if ((t.kind == Tok::Identifier || t.kind == Tok::Function) && !typing(t) &&
                !(found.count(t.text)))
                add(t.text, t.kind == Tok::Function ? ItemKind::Function : ItemKind::Local, "", nullptr);
        for (const ApiSymbol& s : api()) {
            bool global = !*s.table || !*s.name;
            if (!global) continue;
            if (s.kind == ApiKind::Callback && !afterFunction) continue;
            add(*s.name ? s.name : s.table, kindOf(s), s.signature, &s);
        }
        if (!afterFunction)
            for (const char* k : kKeywords) add(k, ItemKind::Keyword, "", nullptr);
        if (!afterFunction) add("self", ItemKind::Keyword, "", nullptr);
    }

    for (auto& [label, c] : found) out.items.push_back(std::move(c));
    std::stable_sort(out.items.begin(), out.items.end(), [&](const Completion& x, const Completion& y) {
        int sx = matchScore(x.label, out.prefix), sy = matchScore(y.label, out.prefix);
        if (sx != sy) return sx < sy;
        if (kindRank(x.kind) != kindRank(y.kind)) return kindRank(x.kind) < kindRank(y.kind);
        // Alphabetical, ignoring case, like a word list.
        for (size_t i = 0; i < x.label.size() && i < y.label.size(); ++i) {
            int a = std::tolower((unsigned char)x.label[i]), b = std::tolower((unsigned char)y.label[i]);
            if (a != b) return a < b;
        }
        return x.label.size() < y.label.size();
    });
    // Only the word itself, already typed: nothing to offer.
    if (out.items.size() == 1 && out.items[0].label == out.prefix) out.items.clear();
    return out;
}

// --- Script -----------------------------------------------------------------

Script::Stamp Script::stamp() const {
    Stamp s;
    std::error_code ec;
    s.time = std::filesystem::last_write_time(m_path, ec);
    if (ec) return Stamp();
    s.size = std::filesystem::file_size(m_path, ec);
    s.exists = !ec;
    return s;
}

bool Script::read(std::string* text, std::string* error) const {
    std::ifstream in(m_path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read " + m_path.filename().string();
        return false;
    }
    text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The buffer's text for a file's: no byte order mark, no carriage returns.
static std::string bufferText(std::string text, bool* crlf) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    *crlf = text.find("\r\n") != std::string::npos;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

bool Script::open(const std::filesystem::path& file, std::string* error) {
    m_path = file;
    std::error_code ec;
    std::string text;
    m_diskText.clear();
    m_stamp = Stamp();
    if (std::filesystem::exists(file, ec)) {
        m_stamp = stamp();
        if (!read(&m_diskText, error)) return false;
        text = bufferText(m_diskText, &m_crlf);
    }
    buffer.setText(text);
    m_savedRevision = std::filesystem::exists(file, ec) ? buffer.revision() : 0;
    m_checkedRevision = m_tokenRevision = 0;
    return true;
}

bool Script::changedOnDisk() {
    const Stamp now = stamp();
    if (!now.exists || now == m_stamp) return false;
    std::string text;
    if (!read(&text, nullptr)) return false;
    if (text == m_diskText) {
        m_stamp = now;  // touched, or written back the same
        return false;
    }
    return true;
}

bool Script::deletedOnDisk() const {
    if (!m_stamp.exists) return false;
    std::error_code ec;
    return !std::filesystem::exists(m_path, ec) && !ec;
}

void Script::keepMine() {
    const Stamp now = stamp();
    std::string text;
    if (now.exists && read(&text, nullptr)) {
        m_stamp = now;
        m_diskText = std::move(text);
    }
}

bool Script::reload(std::string* error) {
    const Stamp now = stamp();
    std::string raw;
    if (!read(&raw, error)) return false;
    bool crlf = false;
    const std::string text = bufferText(raw, &crlf);
    const std::string old = buffer.text();
    if (text != old) {
        // Only the part that differs is replaced, so the cursor can stay put.
        size_t pre = 0, suf = 0;
        const size_t n = std::min(old.size(), text.size());
        while (pre < n && old[pre] == text[pre]) ++pre;
        while (suf < n - pre && old[old.size() - 1 - suf] == text[text.size() - 1 - suf]) ++suf;
        while (pre > 0 && isCont(old[pre])) --pre;
        while (suf > 0 && isCont(old[old.size() - suf])) --suf;
        auto posOf = [&](size_t off) {
            Pos p;
            for (size_t i = 0; i < off; ++i) {
                if (old[i] == '\n') ++p.line, p.col = 0;
                else ++p.col;
            }
            return p;
        };
        auto offOf = [&](Pos p) {
            size_t off = 0;
            for (int l = 0; l < p.line; ++l) off += buffer.line(l).size() + 1;
            return off + p.col;
        };
        size_t cur = offOf(buffer.cursor());
        const long delta = (long)text.size() - (long)old.size();
        if (cur >= old.size() - suf) cur += delta;
        else if (cur > pre) cur = std::min(cur, text.size() - suf);
        buffer.replace(posOf(pre), posOf(old.size() - suf), std::string_view(text).substr(pre, text.size() - suf - pre));
        Pos at;
        for (size_t i = 0; i < cur && i < text.size(); ++i) {
            if (text[i] == '\n') ++at.line, at.col = 0;
            else ++at.col;
        }
        buffer.moveTo(buffer.clamp(at));
    }
    m_crlf = crlf;
    m_stamp = now;
    m_diskText = std::move(raw);
    m_savedRevision = buffer.revision();
    return true;
}

bool Script::save(std::string* error) {
    std::error_code ec;
    if (m_path.has_parent_path()) std::filesystem::create_directories(m_path.parent_path(), ec);
    std::string text = buffer.text();
    if (m_crlf) {
        std::string crlf;
        for (char c : text) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        text = std::move(crlf);
    }
    // Write beside and rename, so a failed write leaves the old file.
    std::filesystem::path tmp = m_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), (std::streamsize)text.size());
        if (!out) {
            if (error) *error = "cannot write " + m_path.filename().string();
            return false;
        }
    }
    std::filesystem::rename(tmp, m_path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "cannot write " + m_path.filename().string();
        return false;
    }
    m_savedRevision = buffer.revision();
    m_diskText = std::move(text);
    m_stamp = stamp();
    return true;
}

std::string Script::title() const {
    std::u8string u = m_path.filename().u8string();
    return std::string(u.begin(), u.end());
}

const Diagnostic& Script::diagnose() {
    if (m_checkedRevision != buffer.revision()) {
        m_diag = check(buffer.text());
        m_checkedRevision = buffer.revision();
    }
    return m_diag;
}

const std::vector<std::vector<Token>>& Script::tokens() {
    if (m_tokenRevision != buffer.revision()) {
        m_tokens = tokenize(buffer.lines());
        m_tokenRevision = buffer.revision();
    }
    return m_tokens;
}

}  // namespace editor::lua
