// The project's string table as the editor edits it (View > Strings, the UI
// element's text key, the canvas view's preview language, the Blocks key
// lists). strings.json is a project file, not part of the scene, so it keeps
// its own history: each edit is one undo step and is written at once.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "strings.hh"

namespace editor {

class StringsDoc {
public:
    // Reads `project`/strings.json when the project differs or the file
    // changed on disk since (its history then starts over).
    void sync(const std::filesystem::path& project);
    const splash::StringTable& table() const { return m_table; }
    const std::filesystem::path& project() const { return m_project; }
    // Bumped by every change (sync that read, edit, undo, redo).
    unsigned revision() const { return m_revision; }

    // Each true return is one undo step, already saved. False with *error
    // set when refused (the table is unchanged).
    bool setText(const std::string& key, int lang, const std::string& text);
    bool addKey(const std::string& key, std::string* error);
    bool renameKey(const std::string& from, const std::string& to, std::string* error);
    bool deleteKey(const std::string& key);
    bool addLanguage(const std::string& id, const std::string& name, std::string* error);
    bool setLanguage(int lang, const std::string& id, const std::string& name, std::string* error);
    bool deleteLanguage(int lang, std::string* error);
    bool setRememberOnCard(bool on);
    bool importCsv(const std::string& csv, std::string* error);
    std::string exportCsv() const { return splash::stringTableToCsv(m_table); }

    bool undo();
    bool redo();
    bool canUndo() const { return m_at > 0; }
    bool canRedo() const { return m_at < m_history.size(); }

    // The canvas view's preview language (an index of table().languages;
    // not saved). Clamped to the table.
    int previewLanguage() const;
    void setPreviewLanguage(int lang);

    // A free key "<base>", "<base>_2", ...
    std::string freeKey(const std::string& base) const;

private:
    bool commit(splash::StringTable next, std::string* error);
    std::filesystem::path m_project;
    std::filesystem::file_time_type m_stamp{};
    bool m_exists = false;
    splash::StringTable m_table;
    // Undo: the table before each edit; redo: after.
    std::vector<std::pair<splash::StringTable, splash::StringTable>> m_history;
    size_t m_at = 0;
    unsigned m_revision = 1;
    int m_preview = 0;
};

// The one the editor uses.
StringsDoc& projectStrings();

// Indices of entries whose key or any text contains query (ASCII
// case-insensitive); all of them for an empty query.
std::vector<int> filterStrings(const splash::StringTable& t, const std::string& query);

// `scene` with each keyed UI text showing its key's text in language `lang`
// (the default language's where that is empty); unknown keys and keys with no
// default text keep their own text, as the export does.
splash::Scene previewScene(const splash::Scene& scene, const splash::StringTable& t, int lang);

}  // namespace editor
