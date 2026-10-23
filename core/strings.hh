// Localisation (splashedit-ng#209): the project's string table and what the
// export makes of it.
//
// `project`/strings.json holds the languages (the first is the default) and
// per key one text per language. A UI text element with a textKey shows that
// key's text; the export writes the default language's text into the pack,
// so a scene without Lua reads right.
//
// On the PS1 the strings are a generated Lua library (a key -> text table per
// language) that publishes _G.Strings: Get(key), Language(), SetLanguage(id
// or 1-based index), Languages(), Save(). Measured in Redux
// (ng-deps/loc/value.md), 300 keys x 3 languages took +165 KB of peak heap, so
// each scene carries only the keys it references: its keyed UI elements, the
// literal keys of Strings.Get("...") in its scripts (Blocks compile to that),
// and every key starting with one of the scene's stringPrefixes. The active
// language survives scene loads in a Persist slot; SetLanguage re-applies the
// keyed texts of the scene. A card save blocks for seconds (2.4 s measured),
// so only Strings.Save() writes the card, and the boot read (Persist unset)
// happens only when the table's rememberOnCard is on.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

struct StringLanguage {
    std::string id;    // e.g. "en": letters, digits, '-' and '_'
    std::string name;  // shown in the editor, e.g. "English"
};

struct StringEntry {
    std::string key;
    std::vector<std::string> text;  // one per language, in StringTable::languages order
};

struct StringTable {
    std::vector<StringLanguage> languages;  // first = default
    std::vector<StringEntry> entries;       // keys unique
    // Read the language from the memory card at boot (when nothing set it in
    // this session) - costs ~0.5 s on a card without the save.
    bool rememberOnCard = false;

    int languageIndex(const std::string& id) const;  // -1 when absent
    const StringEntry* find(const std::string& key) const;
    StringEntry* find(const std::string& key);
    // The text of key in language `lang`, or the default language's when that
    // is empty; nullptr for an unknown key.
    const std::string* text(const std::string& key, int lang) const;
    // Every entry has one text per language.
    void normalise();
};

// The card save: region + product + this key (MemoryCardManager::buildFilename).
inline constexpr const char* kStringsCardKey = "LANG";
// The Persist slot holding the active language (index + 1; 0/nil = unset).
inline constexpr const char* kStringsPersistKey = "__strings_lang";

bool validLanguageId(const std::string& id);
bool validStringKey(const std::string& key);

// Missing or unreadable file: an empty table. writeStringTable writes it
// whole; false when it cannot be written.
StringTable readStringTable(const std::filesystem::path& file);
bool writeStringTable(const std::filesystem::path& file, const StringTable& t);
// strings.json in the project root.
inline std::filesystem::path stringTablePath(const std::filesystem::path& project) { return project / "strings.json"; }

// CSV (RFC 4180, UTF-8): a header "key,<id>,<id>..." then one row per key.
std::string stringTableToCsv(const StringTable& t);
// Replaces out's languages and entries from csv (names of languages already in
// `out` are kept, new ones are named by their id). False with *error set
// when csv is not a string table (no key column, a bad language id, a
// duplicate or empty key); out is then unchanged.
bool stringTableFromCsv(const std::string& csv, StringTable& out, std::string* error);

// Strings.Get calls in a Lua source: literal keys, and the lines of calls
// whose argument is not one string literal. `uses` is set when the source
// names the Strings table at all.
struct StringRefs {
    std::vector<std::string> keys;
    std::vector<int> computedLines;
    bool uses = false;
};
StringRefs scanStringRefs(const std::string& luaSource);

// Lua string literal of s (quoted, escaped).
std::string luaQuote(const std::string& s);

// The export's view of a scene with strings.
struct StringsBake {
    Scene scene;  // keyed UI texts replaced by the default language's
    // Virtual Lua file (never on disk), empty when the scene carries none.
    std::string libraryPath, library;
    std::vector<std::string> keys;  // carried, in table order
    size_t cells = 0;               // non-empty key x language texts carried
    size_t luaHeapBytes = 0;        // estimate of what the library keeps on the Lua heap
    std::vector<std::string> warnings;
};
// The Lua heap the library keeps: per carried text its bytes + 68, and 1.2 KB
// (a linear fit of t50 and t300, ng-deps/loc/value.md: 98 B per cell of ~30
// byte texts, +1.2 KB fixed), and ~48 B per keyed UI element (its handle row).
size_t stringsLuaHeapBytes(size_t textBytes, size_t cells, size_t elements);
// readSource reads a project script by its project-relative path (.blocks
// already turned into Lua) and may throw; root resolves fonts for the
// per-language glyph checks.
StringsBake bakeStrings(const Scene& scene, const StringTable& table, const std::filesystem::path& root,
                        const std::function<std::string(const std::string&)>& readSource);

}  // namespace splash
