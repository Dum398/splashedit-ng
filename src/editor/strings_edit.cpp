#include "editor/strings_edit.hh"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace editor {

void StringsDoc::sync(const fs::path& project) {
    const fs::path file = project.empty() ? fs::path() : splash::stringTablePath(project);
    std::error_code ec;
    const bool exists = !file.empty() && fs::exists(file, ec);
    const fs::file_time_type stamp = exists ? fs::last_write_time(file, ec) : fs::file_time_type{};
    if (project == m_project && exists == m_exists && stamp == m_stamp) return;
    const bool sameProject = project == m_project;
    m_project = project;
    m_exists = exists;
    m_stamp = stamp;
    m_table = exists ? splash::readStringTable(file) : splash::StringTable{};
    m_table.normalise();
    m_history.clear();
    m_at = 0;
    if (!sameProject) m_preview = 0;
    m_revision++;
}

bool StringsDoc::commit(splash::StringTable next, std::string* error) {
    if (m_project.empty()) {
        if (error) *error = "no project is open";
        return false;
    }
    next.normalise();
    const fs::path file = splash::stringTablePath(m_project);
    if (!splash::writeStringTable(file, next)) {
        if (error) *error = "cannot write " + file.string();
        return false;
    }
    m_history.resize(m_at);
    m_history.push_back({m_table, next});
    m_at = m_history.size();
    m_table = std::move(next);
    std::error_code ec;
    m_exists = true;
    m_stamp = fs::last_write_time(file, ec);
    m_revision++;
    return true;
}

bool StringsDoc::setText(const std::string& key, int lang, const std::string& text) {
    const splash::StringEntry* e = m_table.find(key);
    if (!e || lang < 0 || size_t(lang) >= m_table.languages.size() || e->text[size_t(lang)] == text) return false;
    splash::StringTable t = m_table;
    t.find(key)->text[size_t(lang)] = text;
    return commit(std::move(t), nullptr);
}

bool StringsDoc::addKey(const std::string& key, std::string* error) {
    if (!splash::validStringKey(key)) {
        if (error) *error = "a key cannot be empty or hold control characters";
        return false;
    }
    if (m_table.find(key)) {
        if (error) *error = "there is a key '" + key + "' already";
        return false;
    }
    splash::StringTable t = m_table;
    t.entries.push_back({key, std::vector<std::string>(t.languages.size())});
    return commit(std::move(t), error);
}

bool StringsDoc::renameKey(const std::string& from, const std::string& to, std::string* error) {
    if (from == to) return false;
    if (!m_table.find(from)) {
        if (error) *error = "no key '" + from + "'";
        return false;
    }
    if (!splash::validStringKey(to)) {
        if (error) *error = "a key cannot be empty or hold control characters";
        return false;
    }
    if (m_table.find(to)) {
        if (error) *error = "there is a key '" + to + "' already";
        return false;
    }
    splash::StringTable t = m_table;
    t.find(from)->key = to;
    return commit(std::move(t), error);
}

bool StringsDoc::deleteKey(const std::string& key) {
    splash::StringTable t = m_table;
    auto it = std::find_if(t.entries.begin(), t.entries.end(), [&](const splash::StringEntry& e) { return e.key == key; });
    if (it == t.entries.end()) return false;
    t.entries.erase(it);
    return commit(std::move(t), nullptr);
}

bool StringsDoc::addLanguage(const std::string& id, const std::string& name, std::string* error) {
    if (!splash::validLanguageId(id)) {
        if (error) *error = "a language id is 1 to 16 letters, digits, - or _";
        return false;
    }
    if (m_table.languageIndex(id) >= 0) {
        if (error) *error = "there is a language '" + id + "' already";
        return false;
    }
    splash::StringTable t = m_table;
    t.languages.push_back({id, name.empty() ? id : name});
    return commit(std::move(t), error);
}

bool StringsDoc::setLanguage(int lang, const std::string& id, const std::string& name, std::string* error) {
    if (lang < 0 || size_t(lang) >= m_table.languages.size()) return false;
    if (!splash::validLanguageId(id)) {
        if (error) *error = "a language id is 1 to 16 letters, digits, - or _";
        return false;
    }
    const int other = m_table.languageIndex(id);
    if (other >= 0 && other != lang) {
        if (error) *error = "there is a language '" + id + "' already";
        return false;
    }
    const splash::StringLanguage& l = m_table.languages[size_t(lang)];
    const std::string nm = name.empty() ? id : name;
    if (l.id == id && l.name == nm) return false;
    splash::StringTable t = m_table;
    t.languages[size_t(lang)] = {id, nm};
    return commit(std::move(t), error);
}

bool StringsDoc::deleteLanguage(int lang, std::string* error) {
    if (lang < 0 || size_t(lang) >= m_table.languages.size()) return false;
    splash::StringTable t = m_table;
    t.languages.erase(t.languages.begin() + lang);
    for (splash::StringEntry& e : t.entries)
        if (size_t(lang) < e.text.size()) e.text.erase(e.text.begin() + lang);
    return commit(std::move(t), error);
}

bool StringsDoc::setRememberOnCard(bool on) {
    if (m_table.rememberOnCard == on) return false;
    splash::StringTable t = m_table;
    t.rememberOnCard = on;
    return commit(std::move(t), nullptr);
}

bool StringsDoc::importCsv(const std::string& csv, std::string* error) {
    splash::StringTable t = m_table;
    if (!splash::stringTableFromCsv(csv, t, error)) return false;
    return commit(std::move(t), error);
}

bool StringsDoc::undo() {
    if (!canUndo()) return false;
    splash::StringTable before = m_history[m_at - 1].first;
    if (!splash::writeStringTable(splash::stringTablePath(m_project), before)) return false;
    m_at--;
    m_table = std::move(before);
    std::error_code ec;
    m_stamp = fs::last_write_time(splash::stringTablePath(m_project), ec);
    m_revision++;
    return true;
}

bool StringsDoc::redo() {
    if (!canRedo()) return false;
    splash::StringTable after = m_history[m_at].second;
    if (!splash::writeStringTable(splash::stringTablePath(m_project), after)) return false;
    m_at++;
    m_table = std::move(after);
    std::error_code ec;
    m_stamp = fs::last_write_time(splash::stringTablePath(m_project), ec);
    m_revision++;
    return true;
}

int StringsDoc::previewLanguage() const {
    if (m_table.languages.empty()) return 0;
    return std::clamp(m_preview, 0, int(m_table.languages.size()) - 1);
}

void StringsDoc::setPreviewLanguage(int lang) {
    if (lang == m_preview) return;
    m_preview = lang;
    m_revision++;
}

std::string StringsDoc::freeKey(const std::string& base) const {
    if (!m_table.find(base)) return base;
    for (int i = 2;; i++) {
        std::string k = base + "_" + std::to_string(i);
        if (!m_table.find(k)) return k;
    }
}

StringsDoc& projectStrings() {
    static StringsDoc doc;
    return doc;
}

static std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::vector<int> filterStrings(const splash::StringTable& t, const std::string& query) {
    std::vector<int> out;
    const std::string q = lowerAscii(query);
    for (size_t i = 0; i < t.entries.size(); i++) {
        const splash::StringEntry& e = t.entries[i];
        bool hit = q.empty() || lowerAscii(e.key).find(q) != std::string::npos;
        for (size_t l = 0; !hit && l < e.text.size(); l++) hit = lowerAscii(e.text[l]).find(q) != std::string::npos;
        if (hit) out.push_back(int(i));
    }
    return out;
}

splash::Scene previewScene(const splash::Scene& scene, const splash::StringTable& t, int lang) {
    splash::Scene s = scene;
    for (splash::UICanvas& cv : s.canvases)
        for (splash::UIElement& e : cv.elements)
            if (e.type == splash::UIElementType::Text && !e.textKey.empty())
                if (const std::string* text = t.text(e.textKey, lang); text && !text->empty()) e.text = *text;
    return s;
}

}  // namespace editor
