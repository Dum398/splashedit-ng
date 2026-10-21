#include "luascan.hh"

#include <algorithm>
#include <cctype>
#include <set>

namespace splash {

namespace {

struct Tok {
    enum Kind { Name, Punct, Other } kind;
    std::string text;
    int line;
};

bool isNameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// `[` `=`* `[` at p: the level (count of '='), or -1.
int longBracket(const std::string& s, size_t p) {
    if (p >= s.size() || s[p] != '[') return -1;
    size_t q = p + 1;
    while (q < s.size() && s[q] == '=') q++;
    return q < s.size() && s[q] == '[' ? int(q - p - 1) : -1;
}

// Skips the long string or comment opening at p with `level`; counts lines.
size_t skipLong(const std::string& s, size_t p, int level, int& line) {
    p += size_t(level) + 2;
    const std::string close = "]" + std::string(size_t(level), '=') + "]";
    while (p < s.size()) {
        if (s[p] == '\n') line++;
        if (s.compare(p, close.size(), close) == 0) return p + close.size();
        p++;
    }
    return p;
}

std::vector<Tok> tokens(const std::string& s) {
    std::vector<Tok> out;
    int line = 1;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            line++;
            i++;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
        } else if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const int level = longBracket(s, i + 2);
            if (level >= 0) {
                i = skipLong(s, i + 2, level, line);
            } else {
                while (i < s.size() && s[i] != '\n') i++;
            }
        } else if (longBracket(s, i) >= 0) {
            const int at = line;
            i = skipLong(s, i, longBracket(s, i), line);
            out.push_back({Tok::Other, "string", at});
        } else if (c == '"' || c == '\'') {
            const int at = line;
            i++;
            while (i < s.size() && s[i] != c && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    if (s[i + 1] == '\n') line++;
                    i++;
                }
                i++;
            }
            if (i < s.size() && s[i] == c) i++;
            out.push_back({Tok::Other, "string", at});
        } else if (isNameStart(c)) {
            const size_t b = i;
            while (i < s.size() && isNameChar(s[i])) i++;
            out.push_back({Tok::Name, s.substr(b, i - b), line});
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            while (i < s.size() && (isNameChar(s[i]) || s[i] == '.')) {
                const char e = char(std::tolower(static_cast<unsigned char>(s[i])));
                if ((e == 'e' || e == 'p') && i + 1 < s.size() && (s[i + 1] == '+' || s[i + 1] == '-')) i++;
                i++;
            }
            out.push_back({Tok::Other, "number", line});
        } else {
            static const char* const kLong[] = {"...", "..", "==", "~=", "<=", ">=", "::"};
            size_t n = 1;
            for (const char* p : kLong)
                if (s.compare(i, std::char_traits<char>::length(p), p) == 0) {
                    n = std::char_traits<char>::length(p);
                    break;
                }
            out.push_back({Tok::Punct, s.substr(i, n), line});
            i += n;
        }
    }
    return out;
}

bool isKeyword(const std::string& w) {
    static const std::set<std::string> k = {"and",   "break", "do",  "else", "elseif", "end",    "false",
                                            "for",   "function", "goto", "if", "in",   "local",  "nil",
                                            "not",   "or",    "repeat", "return", "then", "true", "until", "while"};
    return k.count(w) != 0;
}

bool is(const std::vector<Tok>& t, long k, Tok::Kind kind, const char* text) {
    return k >= 0 && size_t(k) < t.size() && t[size_t(k)].kind == kind && t[size_t(k)].text == text;
}
bool isPunct(const std::vector<Tok>& t, long k, const char* text) { return is(t, k, Tok::Punct, text); }
bool isWord(const std::vector<Tok>& t, long k, const char* text) { return is(t, k, Tok::Name, text); }

// t[k] is a name being declared: `local x`, `function x`, the later names of
// `local a, x` / `for a, x in`, or a parameter of `function f(a, x)`.
bool declared(const std::vector<Tok>& t, long k) {
    if (isWord(t, k - 1, "local") || isWord(t, k - 1, "function") || isWord(t, k - 1, "for")) return true;
    long j = k - 1;
    while (isPunct(t, j, ",") && j >= 1 && t[size_t(j - 1)].kind == Tok::Name && !isKeyword(t[size_t(j - 1)].text)) j -= 2;
    if (j == k - 1) {
        // The first parameter: `function f(x`.
        if (!isPunct(t, j, "(")) return false;
    } else if (isWord(t, j, "local") || isWord(t, j, "for")) {
        return true;
    } else if (!isPunct(t, j, "(")) {
        return false;
    }
    // Parameters: `function (`, `function name(`, `function a.b:c(`.
    long m = j - 1;
    while (m >= 0 && ((t[size_t(m)].kind == Tok::Name && !isKeyword(t[size_t(m)].text)) || isPunct(t, m, ".") ||
                      isPunct(t, m, ":")))
        m--;
    return isWord(t, m, "function");
}

}  // namespace

std::vector<RuntimeLuaUse> runtimeLuaUses(const std::string& source) {
    static const std::set<std::string> kNames = {"load", "loadstring", "dofile", "loadfile"};
    const std::vector<Tok> t = tokens(source);
    std::vector<RuntimeLuaUse> uses;
    std::set<std::string> own;   // defined by the script itself
    std::vector<char> brackets;  // open ( [ { at each token
    for (long k = 0; k < long(t.size()); k++) {
        const Tok& tk = t[size_t(k)];
        if (tk.kind == Tok::Punct && (tk.text == "(" || tk.text == "[" || tk.text == "{")) brackets.push_back(tk.text[0]);
        if (tk.kind == Tok::Punct && (tk.text == ")" || tk.text == "]" || tk.text == "}") && !brackets.empty())
            brackets.pop_back();
        if (tk.kind != Tok::Name) continue;
        const bool field = isPunct(t, k - 1, ".") || isPunct(t, k - 1, ":");
        // string.dump, not some_table.string.dump.
        if (tk.text == "string" && !field && isPunct(t, k + 1, ".") && isWord(t, k + 2, "dump")) {
            uses.push_back({"string.dump", tk.line});
            continue;
        }
        if (!kNames.count(tk.text)) continue;
        if (field) {
            // _G.load and _ENV.load are the global.
            const bool global = isPunct(t, k - 1, ".") && (isWord(t, k - 2, "_G") || isWord(t, k - 2, "_ENV")) &&
                                !isPunct(t, k - 3, ".") && !isPunct(t, k - 3, ":");
            if (!global) continue;
        } else if (declared(t, k)) {
            own.insert(tk.text);
            continue;
        } else if (isPunct(t, k + 1, "=")) {
            // A table key ({load = f}) is no use; an assignment (load = f) replaces the global.
            if (brackets.empty() || brackets.back() != '{') own.insert(tk.text);
            continue;
        }
        uses.push_back({tk.text, tk.line});
    }
    uses.erase(std::remove_if(uses.begin(), uses.end(), [&](const RuntimeLuaUse& u) { return own.count(u.name) != 0; }),
               uses.end());
    return uses;
}

std::string runtimeLuaWarning(const std::string& script, const std::vector<RuntimeLuaUse>& uses) {
    if (uses.empty()) return "";
    std::vector<const RuntimeLuaUse*> first;
    for (const RuntimeLuaUse& u : uses)
        if (std::none_of(first.begin(), first.end(), [&](const RuntimeLuaUse* f) { return f->name == u.name; }))
            first.push_back(&u);
    auto has = [&](const char* n) {
        return std::any_of(first.begin(), first.end(), [&](const RuntimeLuaUse* f) { return f->name == n; });
    };
    std::string w = script + ": ";
    for (size_t i = 0; i < first.size(); i++)
        w += (i ? ", " : "") + first[i]->name + " on line " + std::to_string(first[i]->line);
    w += ". The engine runs precompiled scripts and has no Lua compiler for text at run time";
    std::vector<std::string> why;
    if (has("load") || has("loadstring")) {
        const std::string n = has("load") && has("loadstring") ? "load and loadstring" : has("load") ? "load" : "loadstring";
        why.push_back(n + " of Lua text fail" + (n.find(" and ") != std::string::npos ? "" : "s") +
                      " with \"parser not loaded\"");
    }
    if (has("dofile") || has("loadfile"))
        why.push_back(std::string(has("dofile") && has("loadfile") ? "dofile and loadfile do" : has("dofile") ? "dofile does" : "loadfile does") +
                      " not exist on the PS1");
    if (has("string.dump")) why.push_back("string.dump fails with \"dumper not loaded\"");
    for (size_t i = 0; i < why.size(); i++) w += (i ? (i + 1 == why.size() ? " and " : ", ") : ": ") + why[i];
    w += ". Write that code in a script as a function and call it, and keep data in tables (Persist or MemCard to save "
         "it) rather than as Lua text.";
    return w;
}

}  // namespace splash
