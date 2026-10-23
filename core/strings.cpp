#include "strings.hh"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "font.hh"

namespace fs = std::filesystem;

namespace splash {

int StringTable::languageIndex(const std::string& id) const {
    for (size_t i = 0; i < languages.size(); i++)
        if (languages[i].id == id) return int(i);
    return -1;
}

const StringEntry* StringTable::find(const std::string& key) const {
    for (const StringEntry& e : entries)
        if (e.key == key) return &e;
    return nullptr;
}

StringEntry* StringTable::find(const std::string& key) {
    for (StringEntry& e : entries)
        if (e.key == key) return &e;
    return nullptr;
}

const std::string* StringTable::text(const std::string& key, int lang) const {
    const StringEntry* e = find(key);
    if (!e) return nullptr;
    if (lang >= 0 && size_t(lang) < e->text.size() && !e->text[size_t(lang)].empty()) return &e->text[size_t(lang)];
    static const std::string none;
    return e->text.empty() ? &none : &e->text[0];
}

void StringTable::normalise() {
    for (StringEntry& e : entries) e.text.resize(languages.size());
}

bool validLanguageId(const std::string& id) {
    if (id.empty() || id.size() > 16) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool validStringKey(const std::string& key) {
    if (key.empty()) return false;
    return std::none_of(key.begin(), key.end(), [](char c) { return (unsigned char)c < 0x20; });
}

// ---- strings.json: ordered so the grid keeps the author's order

StringTable readStringTable(const fs::path& file) {
    StringTable t;
    if (file.empty()) return t;
    std::ifstream in(file, std::ios::binary);
    if (!in) return t;
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return t;
    if (auto it = j.find("languages"); it != j.end() && it->is_array())
        for (const auto& l : *it) {
            if (!l.is_object() || !l.contains("id") || !l["id"].is_string()) continue;
            StringLanguage sl{l["id"].get<std::string>(), l.value("name", "")};
            if (!validLanguageId(sl.id) || t.languageIndex(sl.id) >= 0) continue;
            if (sl.name.empty()) sl.name = sl.id;
            t.languages.push_back(std::move(sl));
        }
    if (auto it = j.find("rememberOnCard"); it != j.end() && it->is_boolean()) t.rememberOnCard = it->get<bool>();
    if (auto it = j.find("strings"); it != j.end() && it->is_object())
        for (const auto& [key, v] : it->items()) {
            if (!validStringKey(key) || t.find(key)) continue;
            StringEntry e{key, std::vector<std::string>(t.languages.size())};
            if (v.is_object())
                for (size_t i = 0; i < t.languages.size(); i++)
                    if (auto f = v.find(t.languages[i].id); f != v.end() && f->is_string()) e.text[i] = f->get<std::string>();
            t.entries.push_back(std::move(e));
        }
    return t;
}

bool writeStringTable(const fs::path& file, const StringTable& t) {
    if (file.empty()) return false;
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["languages"] = nlohmann::ordered_json::array();
    for (const StringLanguage& l : t.languages) j["languages"].push_back({{"id", l.id}, {"name", l.name}});
    if (t.rememberOnCard) j["rememberOnCard"] = true;  // written only when on
    nlohmann::ordered_json s = nlohmann::ordered_json::object();
    for (const StringEntry& e : t.entries) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        for (size_t i = 0; i < t.languages.size(); i++) row[t.languages[i].id] = i < e.text.size() ? e.text[i] : "";
        s[e.key] = row;
    }
    j["strings"] = s;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << '\n';
    out.close();
    return !out.fail();
}

// ---- CSV

namespace {

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}

// Rows of fields; false on an unterminated quote.
bool parseCsv(const std::string& in, std::vector<std::vector<std::string>>& rows) {
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    size_t i = 0;
    if (in.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;  // a BOM, as spreadsheets write
    for (; i < in.size(); i++) {
        char c = in[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < in.size() && in[i + 1] == '"') {
                    field += '"';
                    i++;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < in.size() && in[i + 1] == '\n') i++;
            if (any || !field.empty()) {
                row.push_back(std::move(field));
                rows.push_back(std::move(row));
            }
            row.clear();
            field.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
    }
    if (quoted) return false;
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return true;
}

}  // namespace

std::string stringTableToCsv(const StringTable& t) {
    std::string o = "key";
    for (const StringLanguage& l : t.languages) o += "," + csvField(l.id);
    o += "\r\n";
    for (const StringEntry& e : t.entries) {
        o += csvField(e.key);
        for (size_t i = 0; i < t.languages.size(); i++) o += "," + csvField(i < e.text.size() ? e.text[i] : "");
        o += "\r\n";
    }
    return o;
}

bool stringTableFromCsv(const std::string& csv, StringTable& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::vector<std::vector<std::string>> rows;
    if (!parseCsv(csv, rows)) return fail("a quoted field is not closed");
    if (rows.empty() || rows[0].empty() || rows[0][0] != "key") return fail("the first row must start with a \"key\" column");
    StringTable t;
    t.rememberOnCard = out.rememberOnCard;
    for (size_t c = 1; c < rows[0].size(); c++) {
        const std::string& id = rows[0][c];
        if (!validLanguageId(id)) return fail("column " + std::to_string(c + 1) + ": '" + id + "' is not a language id (letters, digits, - and _)");
        if (t.languageIndex(id) >= 0) return fail("language '" + id + "' has two columns");
        int old = out.languageIndex(id);
        t.languages.push_back({id, old >= 0 ? out.languages[size_t(old)].name : id});
    }
    for (size_t r = 1; r < rows.size(); r++) {
        const std::vector<std::string>& row = rows[r];
        if (row.size() == 1 && row[0].empty()) continue;
        if (!validStringKey(row[0])) return fail("row " + std::to_string(r + 1) + ": no key");
        if (t.find(row[0])) return fail("row " + std::to_string(r + 1) + ": key '" + row[0] + "' is there twice");
        StringEntry e{row[0], std::vector<std::string>(t.languages.size())};
        for (size_t c = 1; c < row.size() && c <= t.languages.size(); c++) e.text[c - 1] = row[c];
        t.entries.push_back(std::move(e));
    }
    out = std::move(t);
    return true;
}

// ---- Lua

std::string luaQuote(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20 || c >= 0x7F) {
            // \ddd is decimal in Lua; three digits so a following digit is not taken in.
            char b[5];
            std::snprintf(b, sizeof b, "\\%03u", unsigned(c));
            o += b;
        } else {
            o += char(c);
        }
    }
    return o + "\"";
}

namespace {

bool identChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Level of a long bracket at s[i] ('[' then '='* then '['), or -1.
int longBracket(const std::string& s, size_t i) {
    if (i >= s.size() || s[i] != '[') return -1;
    size_t j = i + 1;
    while (j < s.size() && s[j] == '=') j++;
    return j < s.size() && s[j] == '[' ? int(j - i - 1) : -1;
}

// Skips the long string/comment opening at i (level lv); returns the index after it.
size_t skipLong(const std::string& s, size_t i, int lv, int& line) {
    const std::string close = "]" + std::string(size_t(lv), '=') + "]";
    size_t e = s.find(close, i + size_t(lv) + 2);
    size_t end = e == std::string::npos ? s.size() : e + close.size();
    line += int(std::count(s.begin() + std::ptrdiff_t(i), s.begin() + std::ptrdiff_t(end), '\n'));
    return end;
}

// Reads a quoted literal at s[i] (quote char); value unescaped. Returns the index after it.
size_t readQuoted(const std::string& s, size_t i, std::string& value, int& line) {
    const char q = s[i++];
    value.clear();
    while (i < s.size() && s[i] != q) {
        char c = s[i];
        if (c == '\n') {
            line++;
            break;  // unfinished string
        }
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case 'a': value += '\a'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'v': value += '\v'; break;
            case '\n': value += '\n'; line++; break;
            default:
                if (n >= '0' && n <= '9') {
                    int v = 0, k = 0;
                    while (k < 3 && i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0'), k++;
                    i--;
                    value += char(v);
                } else {
                    value += n;
                }
            }
            i++;
            continue;
        }
        value += c;
        i++;
    }
    return i < s.size() ? i + 1 : i;
}

size_t skipSpace(const std::string& s, size_t i, int& line) {
    while (i < s.size()) {
        if (s[i] == '\n') line++;
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
        else if (s.compare(i, 2, "--") == 0) {
            int lv = longBracket(s, i + 2);
            if (lv >= 0) i = skipLong(s, i + 2, lv, line);
            else {
                size_t e = s.find('\n', i);
                i = e == std::string::npos ? s.size() : e;
            }
        } else break;
    }
    return i;
}

}  // namespace

StringRefs scanStringRefs(const std::string& s) {
    StringRefs r;
    int line = 1;
    size_t i = 0;
    std::string lit;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') {
            line++;
            i++;
            continue;
        }
        if (s.compare(i, 2, "--") == 0) {
            i = skipSpace(s, i, line);
            continue;
        }
        if (c == '"' || c == '\'') {
            i = readQuoted(s, i, lit, line);
            continue;
        }
        if (int lv = longBracket(s, i); lv >= 0) {
            i = skipLong(s, i, lv, line);
            continue;
        }
        if (!identChar(c)) {
            i++;
            continue;
        }
        size_t b = i;
        while (i < s.size() && identChar(s[i])) i++;
        if (s.compare(b, i - b, "Strings") != 0) continue;
        // A field (t.Strings) or a method name is not the global.
        size_t p = b;
        while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) p--;
        if (p > 0 && (s[p - 1] == '.' || s[p - 1] == ':')) continue;
        r.uses = true;
        int at = line;
        size_t j = skipSpace(s, i, at);
        if (j >= s.size() || (s[j] != '.' && s[j] != ':')) continue;
        j = skipSpace(s, j + 1, at);
        if (s.compare(j, 3, "Get") != 0 || (j + 3 < s.size() && identChar(s[j + 3]))) continue;
        j = skipSpace(s, j + 3, at);
        const int callLine = at;
        bool paren = j < s.size() && s[j] == '(';
        if (paren) j = skipSpace(s, j + 1, at);
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
            size_t e = readQuoted(s, j, lit, at);
            size_t k = paren ? skipSpace(s, e, at) : e;
            if (!paren || (k < s.size() && s[k] == ')')) {
                r.keys.push_back(lit);
                continue;
            }
        } else if (int lv = longBracket(s, j); lv >= 0 && !paren) {
            continue;  // Strings.Get [[...]]: rare enough to leave alone
        }
        r.computedLines.push_back(callLine);
    }
    return r;
}

// ---- the bake

size_t stringsLuaHeapBytes(size_t textBytes, size_t cells, size_t elements) {
    return textBytes + 68 * cells + 1200 + 48 * elements;
}

namespace {

constexpr size_t kTextMax = 63;  // splashpack.cpp kUiTextMax

struct Keyed {
    const UICanvas* canvas;
    const UIElement* element;
};

std::string listOf(const std::vector<std::string>& v, size_t most = 5) {
    std::string o;
    for (size_t i = 0; i < v.size() && i < most; i++) o += (i ? ", '" : "'") + v[i] + "'";
    if (v.size() > most) o += " and " + std::to_string(v.size() - most) + " more";
    return o;
}

const char* kLibrary = R"LUA(
local S = {}
_G.Strings = S
local SLOT = "__strings_lang"
local cur = Persist.Get(SLOT) or 0
local function index(l)
  if type(l) == "number" then
    if l >= 1 and l <= #ids then return l end
    return nil
  end
  for i = 1, #ids do if ids[i] == l then return i end end
  return nil
end
if cur < 1 or cur > #ids then
  cur = 1
  if CARD then
    local mc = rawget(_G, "MemCard")
    if mc and rawget(mc, "IsPresent") and mc.IsPresent(0) then
      local t = mc.Load(0, "LANG")
      if type(t) == "table" then cur = index(t.id) or 1 end
    end
    Persist.Set(SLOT, cur)
  end
end
function S.Get(k)
  local s = T[cur][k]
  if s == nil then s = T[1][k] end
  if s == nil then return k end
  return s
end
local H = nil
local function apply()
  if #E == 0 then return end
  if H == nil then
    H = {}
    for i = 1, #E do
      local e = E[i]
      local c = UI.FindCanvas(e[1])
      if c >= 0 then
        local h = UI.FindElement(c, e[2])
        if h >= 0 then H[#H + 1] = h; H[#H + 1] = e[3] end
      end
    end
  end
  for i = 1, #H, 2 do UI.SetText(H[i], S.Get(H[i + 1])) end
end
function S.Language() return ids[cur] end
function S.Languages()
  local t = {}
  for i = 1, #ids do t[i] = ids[i] end
  return t
end
function S.SetLanguage(l)
  local n = index(l)
  if not n then return false end
  cur = n
  Persist.Set(SLOT, cur)
  apply()
  return true
end
function S.Save()
  local mc = rawget(_G, "MemCard")
  if not mc or not rawget(mc, "Save") then return false, "this build has no memory card support" end
  return mc.Save(0, "LANG", { id = ids[cur] })
end
if cur ~= 1 then apply() end
)LUA";

}  // namespace

StringsBake bakeStrings(const Scene& scene, const StringTable& table, const fs::path& root,
                        const std::function<std::string(const std::string&)>& readSource) {
    StringsBake b;
    b.scene = scene;
    // Scripts: the scene's, every object's and every trigger's, once each.
    std::vector<std::string> scripts;
    auto add = [&](const std::string& p) {
        if (!p.empty() && std::find(scripts.begin(), scripts.end(), p) == scripts.end()) scripts.push_back(p);
    };
    add(scene.settings.script);
    for (const FlatObject& fo : flatten(scene)) {
        if (fo.object->script) add(fo.object->script->lua);
        if (fo.object->trigger) add(fo.object->trigger->lua);
    }
    struct Ref {
        std::string script;
        StringRefs refs;
    };
    std::vector<Ref> refs;
    bool uses = false;
    for (const std::string& p : scripts) {
        if (!readSource) break;
        std::string src;
        try {
            src = readSource(p);
        } catch (const std::exception&) {
            continue;  // the export reports unreadable scripts itself
        }
        Ref r{p, scanStringRefs(src)};
        uses = uses || r.refs.uses;
        refs.push_back(std::move(r));
    }
    std::vector<Keyed> keyed;
    for (const UICanvas& cv : scene.canvases)
        for (const UIElement& e : cv.elements)
            if (e.type == UIElementType::Text && !e.textKey.empty()) keyed.push_back({&cv, &e});
    if (keyed.empty() && !uses && scene.settings.stringPrefixes.empty()) return b;
    if (table.languages.empty()) {
        b.warnings.push_back("this scene uses localised strings but the project has no string table (View > Strings): " +
                             std::string(keyed.empty() ? "Strings.Get fails on the console"
                                                       : "keyed UI texts show their own text"));
        return b;
    }

    // The keys carried: keyed elements, literal Get keys, prefixes.
    std::set<std::string> want;
    for (const Keyed& k : keyed) {
        const std::string where = "canvas '" + k.canvas->name + "', element '" + k.element->name + "'";
        if (!table.find(k.element->textKey))
            b.warnings.push_back(where + ": text key '" + k.element->textKey + "' is not in the string table; it shows its own text");
        else
            want.insert(k.element->textKey);
    }
    for (const Ref& r : refs) {
        std::set<std::string> missing;
        for (const std::string& key : r.refs.keys) {
            if (table.find(key)) want.insert(key);
            else missing.insert(key);
        }
        for (const std::string& key : missing)
            b.warnings.push_back("script '" + r.script + "': Strings.Get(\"" + key + "\"): no such key in the string table (it returns the key)");
        if (!r.refs.computedLines.empty() && scene.settings.stringPrefixes.empty())
            b.warnings.push_back("script '" + r.script + "' line " + std::to_string(r.refs.computedLines[0]) +
                                 ": Strings.Get with a computed key; the scene carries only the keys it names, so add the "
                                 "keys' prefix to the scene's String prefixes (scene inspector)");
    }
    for (const StringEntry& e : table.entries)
        for (const std::string& p : scene.settings.stringPrefixes)
            if (!p.empty() && e.key.compare(0, p.size(), p) == 0) want.insert(e.key);
    for (const StringEntry& e : table.entries)
        if (want.count(e.key)) b.keys.push_back(e.key);

    // Keyed texts in the pack: the default language's.
    for (UICanvas& cv : b.scene.canvases)
        for (UIElement& e : cv.elements)
            if (e.type == UIElementType::Text && !e.textKey.empty())
                if (const std::string* t = table.text(e.textKey, 0); t && !t->empty()) e.text = *t;

    // Per language, per keyed element: what the console cannot show.
    std::map<std::string, FontSheet> sheets;
    auto sheetFor = [&](const std::string& font) -> const FontSheet* {
        if (font.empty()) return nullptr;
        auto it = sheets.find(font);
        if (it == sheets.end()) {
            FontSheet sh;
            for (const UIFont& f : scene.fonts)
                if (f.name == font) {
                    try {
                        sh = buildFont(f, root);
                    } catch (const std::exception&) {
                        sh = FontSheet{};  // the UI export reports the font
                    }
                }
            it = sheets.emplace(font, std::move(sh)).first;
        }
        return it->second.glyphWidth > 0 ? &it->second : nullptr;
    };
    auto inked = [](const FontSheet& sh, unsigned char c) {
        const int i = c - 0x20, cols = 256 / sh.glyphWidth;
        const int x0 = (i % cols) * sh.glyphWidth, y0 = (i / cols) * sh.glyphHeight;
        const int w = std::min<int>(sh.advances[size_t(i)], sh.glyphWidth);
        for (int y = 0; y < sh.glyphHeight; y++)
            for (int x = 0; x < w; x++) {
                size_t at = size_t(y0 + y) * 256 + size_t(x0 + x);
                if (at < sh.texels.size() && sh.texels[at]) return true;
            }
        return false;
    };
    for (const Keyed& k : keyed) {
        const UIElement& e = *k.element;
        if (!table.find(e.textKey)) continue;
        const std::string& fontName = effectiveFont(*k.canvas, e);
        const FontSheet* sh = sheetFor(fontName);
        for (size_t l = 0; l < table.languages.size(); l++) {
            const std::string& t = *table.text(e.textKey, int(l));
            const std::string where = "canvas '" + k.canvas->name + "', element '" + e.name + "', key '" + e.textKey +
                                      "', language '" + table.languages[l].id + "'";
            if (t.size() > kTextMax)
                b.warnings.push_back(where + ": text is " + std::to_string(t.size()) + " bytes, cut to " + std::to_string(kTextMax));
            std::string bad, blank;
            int width = 0;
            for (unsigned char c : t.substr(0, kTextMax)) {
                if (c < 0x20 || c > 0x7E) {
                    if (bad.find(char(c)) == std::string::npos) bad += char(c);
                    c = '?';
                } else if (sh && c != ' ' && !inked(*sh, c) && blank.find(char(c)) == std::string::npos) {
                    blank += char(c);
                }
                width += sh ? sh->advances[size_t(c - 0x20)] : 8;
            }
            if (!bad.empty()) b.warnings.push_back(where + ": psxsplash draws only ASCII 0x20-0x7E; \"" + bad + "\" shows as '?'");
            if (!blank.empty())
                b.warnings.push_back(where + ": font '" + fontName + "' has no glyph for \"" + blank + "\" (blank cells)");
            if (e.anchorMin[0] == e.anchorMax[0] && width > e.rect[2])
                b.warnings.push_back(where + ": text is " + std::to_string(width) + " px wide, the element " +
                                     std::to_string(e.rect[2]) + " px");
        }
    }
    // Empty cells of the keys carried, per language.
    for (size_t l = 0; l < table.languages.size(); l++) {
        std::vector<std::string> empty;
        for (const std::string& key : b.keys) {
            const StringEntry* e = table.find(key);
            if (l >= e->text.size() || e->text[l].empty()) empty.push_back(key);
        }
        if (empty.empty()) continue;
        b.warnings.push_back("language '" + table.languages[l].id + "': " + listOf(empty) + (empty.size() == 1 ? " has" : " have") +
                             (l == 0 ? " no text in the default language" : " no text; the default language's shows"));
    }

    // The library.
    std::string L = "-- Strings: generated by SplashEdit from strings.json (core/strings.cpp)\nlocal ids = {";
    for (size_t l = 0; l < table.languages.size(); l++) L += (l ? ", " : "") + luaQuote(table.languages[l].id);
    L += "}\nlocal T = {\n";
    size_t textBytes = 0;
    for (size_t l = 0; l < table.languages.size(); l++) {
        L += "{";
        bool first = true;
        for (const std::string& key : b.keys) {
            const StringEntry* e = table.find(key);
            const std::string t = l < e->text.size() ? e->text[l] : "";
            if (t.empty() && l > 0) continue;  // falls back on the default
            L += (first ? "[" : ",[") + luaQuote(key) + "]=" + luaQuote(t);
            first = false;
            textBytes += t.size();
            b.cells++;
        }
        L += "},\n";
    }
    L += "}\nlocal E = {";
    size_t elements = 0;
    for (const Keyed& k : keyed)
        if (table.find(k.element->textKey)) {
            L += (elements ? ", {" : "{") + luaQuote(k.canvas->name) + ", " + luaQuote(k.element->name) + ", " +
                 luaQuote(k.element->textKey) + "}";
            elements++;
        }
    L += std::string("}\nlocal CARD = ") + (table.rememberOnCard ? "true" : "false") + "\n";
    L += kLibrary;
    b.libraryPath = "<strings>/library.lua";
    b.library = std::move(L);
    b.luaHeapBytes = stringsLuaHeapBytes(textBytes, b.cells, elements);
    return b;
}

}  // namespace splash
