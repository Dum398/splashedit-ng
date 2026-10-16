#include "editor/lua_editor_view.hh"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

#include "blocks.hh"
#include "editor/blocks_view.hh"
#include "editor/lua_editor.hh"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ui {

using namespace theme;
namespace lua = editor::lua;

struct ScriptTabs::Tab {
    lua::Script script;
    ImVec2 scroll{0, 0};
    bool focused = false;
    bool reveal = false, revealCenter = false;  // scroll the cursor into view
    double lastInput = 0;                       // the caret blink restarts on input
    double lastEdit = 0;                        // the compile check waits for a pause
    uint64_t seenRevision = 0;
    // Completion.
    bool compOpen = false, compForced = false;
    lua::Completions comp;
    int compSel = 0, compTop = 0;
    uint64_t compRevision = 0;
    lua::Pos compCursor;
    ImRect compRect;
    // Find and replace.
    bool findOpen = false, findFocus = false;
    lua::FindOptions opts;  // inSelection searches the buffer's find scope
    bool replaceOpen = false, replaceFocus = false;
    bool findEditing = false, replaceEditing = false;  // last frame
    std::string query, searched, replacement;
    ImRect findRect;
    // The file changed on disk under unsaved edits: Reload or Keep mine.
    bool diskChanged = false;
    // The file was deleted on disk: Save writes it again, or Close.
    bool diskDeleted = false;
    bool closeAsked = false;  // Close on that bar, done by the next tabStrip
    // API docs shown by go-to-definition on an API name, until the cursor moves.
    const lua::ApiSymbol* peek = nullptr;
    lua::Pos peekAt;
    // Caches, by revision and position.
    uint64_t callRevision = 0;
    lua::Pos callPos{-1, -1};
    lua::Lookup call;
    uint64_t hoverRevision = 0;
    lua::Pos hoverPos{-1, -1};
    lua::Lookup hover;
    double hoverSince = 0;
    int flashLine = -1;  // after go-to-definition
    double flashAt = 0;
    // A .blocks file: the block editor shows it. The buffer keeps the file's
    // text (saving, the dirty mark and changes on disk work as for Lua); each
    // block edit, undo and redo is one buffer edit, so the buffer's undo
    // steps keep pace with the block editor's.
    std::unique_ptr<BlocksView> blocks;
    uint64_t blocksRev = 0;     // the buffer revision the block editor shows
    bool blocksSynced = false;  // false: the next sync starts its undo over
};

ScriptTabs::ScriptTabs() = default;
ScriptTabs::~ScriptTabs() = default;

static std::string utf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

static bool isBlocksFile(const std::filesystem::path& p) { return p.extension() == ".blocks"; }

// A block script's text; blank (a file not written yet) is an empty script.
static std::optional<splash::BlockScript> readBlocks(const std::string& text, const std::string& name, std::string* error) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return splash::BlockScript{};
    try {
        return splash::parseBlocks(text, name);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

// The block editor catches up with the buffer when something else changed
// it (opened, reloaded, rewritten): one undo step there, as in the buffer.
static void syncBlocks(ScriptTabs::Tab& t) {
    lua::Buffer& b = t.script.buffer;
    if (t.blocksSynced && b.revision() == t.blocksRev) return;
    std::string err;
    std::optional<splash::BlockScript> s = readBlocks(b.text(), t.script.title(), &err);
    if (!s) {
        t.blocks->error = err;
    } else if (!t.blocksSynced || !t.blocks->error.empty()) {
        t.blocks->error.clear();
        t.blocks->reset(std::move(*s));
    } else {
        t.blocks->commit(std::move(*s));
    }
    t.blocksSynced = true;
    t.blocksRev = b.revision();
}

static void replaceAll(lua::Buffer& b, const std::string& text) {
    b.breakUndoMerge();
    b.replace({0, 0}, {b.lineCount() - 1, (int)b.line(b.lineCount() - 1).size()}, text);
    b.breakUndoMerge();
    b.moveTo({0, 0});
}

// What the block editor did this frame, into the buffer: an edit replaces
// the text, an undo or redo steps the buffer the same way (and writes the
// text when the buffer's step was not the same one).
static void applyBlocks(ScriptTabs::Tab& t, const BlocksView::Result& res) {
    lua::Buffer& b = t.script.buffer;
    for (const BlocksView::Result::Op& op : res.ops) {
        if (op.kind == BlocksView::Result::Op::Edit) {
            replaceAll(b, op.text);
            continue;
        }
        const bool stepped = op.kind == BlocksView::Result::Op::Undo ? b.undo() : b.redo();
        bool same = stepped && b.text() == op.text;
        if (stepped && !same) {
            std::optional<splash::BlockScript> now = readBlocks(b.text(), std::string(), nullptr);
            same = now && *now == t.blocks->script();
        }
        if (!same) replaceAll(b, op.text);
    }
    t.blocksRev = b.revision();
}

bool ScriptTabs::open(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    std::filesystem::path want = std::filesystem::weakly_canonical(file, ec);
    if (ec) want = file;
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        std::filesystem::path have = std::filesystem::weakly_canonical(m_tabs[i]->script.path(), ec);
        if ((ec ? m_tabs[i]->script.path() : have) == want) {
            for (auto& t : m_tabs) t->focused = false;
            m_active = (int)i;
            m_tabs[i]->focused = true;
            return true;
        }
    }
    auto t = std::make_unique<Tab>();
    if (!t->script.open(file, error)) return false;
    if (isBlocksFile(file)) {
        t->blocks = std::make_unique<BlocksView>();
        syncBlocks(*t);
    } else {
        t->script.diagnose();
    }
    t->focused = true;
    t->lastInput = ImGui::GetTime();
    for (auto& o : m_tabs) o->focused = false;
    m_tabs.push_back(std::move(t));
    m_active = (int)m_tabs.size() - 1;
    return true;
}

bool ScriptTabs::hasKeyboard() const { return m_active >= 0 && m_active < (int)m_tabs.size() && m_tabs[m_active]->focused; }

int ScriptTabs::dirtyCount() const {
    int n = 0;
    for (const auto& t : m_tabs) n += t->script.dirty();
    return n;
}

bool ScriptTabs::saveAll(std::string* error) {
    bool ok = true;
    for (auto& t : m_tabs) {
        if (!t->script.dirty()) continue;
        std::string err;
        if (t->script.save(&err)) {
            t->diskChanged = t->diskDeleted = false;
            m_saved.push_back(t->script.path());
        } else if (ok) {
            ok = false;
            if (error) *error = err;
        }
    }
    return ok;
}

void ScriptTabs::closeAll() {
    m_tabs.clear();
    if (m_active >= 0) m_active = kScene;
    m_closing = -1;
}

void ScriptTabs::moved(const std::filesystem::path& from, const std::filesystem::path& to) {
    const std::filesystem::path f = from.lexically_normal(), t = to.lexically_normal();
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        std::unique_ptr<Tab>& tab = m_tabs[i];
        const std::filesystem::path p = tab->script.path().lexically_normal();
        if (p == f && isBlocksFile(f) != isBlocksFile(t)) {
            // Blocks turned into Lua (or back, on undo): the tab is another
            // kind of editor now. Close it and open the file where it is.
            const bool wasActive = m_active == (int)i;
            const int keep = m_active;
            m_tabs.erase(m_tabs.begin() + i);
            if (m_closing == (int)i) m_closing = -1;
            if (m_converting == (int)i) m_converting = -1;
            if (m_menuFor == (int)i) m_menuFor = -1;
            std::string err;
            auto nt = std::make_unique<Tab>();
            if (nt->script.open(t, &err)) {
                if (isBlocksFile(t)) {
                    nt->blocks = std::make_unique<BlocksView>();
                    syncBlocks(*nt);
                } else {
                    nt->script.diagnose();
                }
                nt->lastInput = ImGui::GetTime();
                m_tabs.insert(m_tabs.begin() + i, std::move(nt));
                m_active = keep;
                if (wasActive)
                    for (size_t k = 0; k < m_tabs.size(); ++k) m_tabs[k]->focused = k == i;
            } else {
                if (m_active > (int)i || m_active >= (int)m_tabs.size()) --m_active;
                if (m_active < 0) m_active = kScene;
            }
            continue;
        }
        if (p == f) {
            tab->script.setPath(t);
            continue;
        }
        // Under a moved folder: the same place under its new path.
        const std::filesystem::path rel = p.lexically_relative(f);
        if (!rel.empty() && *rel.begin() != "..") tab->script.setPath(t / rel);
    }
}

void ScriptTabs::reloadFromDisk() {
    for (auto& t : m_tabs) {
        if (t->script.dirty()) continue;
        lua::Script fresh;
        if (!fresh.open(t->script.path(), nullptr) || fresh.buffer.text() == t->script.buffer.text()) continue;
        const lua::Pos at = t->script.buffer.cursor();
        if (!t->script.open(t->script.path(), nullptr)) continue;
        t->script.buffer.moveTo(t->script.buffer.clamp(at));
        if (t->blocks) t->blocksSynced = false;  // a fresh start, as the buffer's undo is
        else t->script.diagnose();
    }
}

int ScriptTabs::rewriteUnsaved(const std::vector<std::filesystem::path>& files,
                               const std::function<std::string(const std::string&)>& fn) {
    auto canon = [](const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c;
    };
    std::vector<std::filesystem::path> want;
    for (const auto& f : files) want.push_back(canon(f));
    int n = 0;
    for (auto& t : m_tabs) {
        if (t->blocks || !t->script.dirty() || std::find(want.begin(), want.end(), canon(t->script.path())) == want.end()) continue;
        lua::Buffer& b = t->script.buffer;
        const std::string before = b.text(), after = fn(before);
        if (after == before) continue;
        const lua::Pos at = b.cursor();
        b.breakUndoMerge();
        b.replace({0, 0}, {b.lineCount() - 1, (int)b.line(b.lineCount() - 1).size()}, after);
        b.breakUndoMerge();
        b.moveTo(b.clamp(at));
        ++n;
    }
    return n;
}

int ScriptTabs::rewriteUnsavedBlocks(const std::vector<std::filesystem::path>& files,
                                     const std::function<std::string(const std::string&)>& fn) {
    auto canon = [](const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c;
    };
    std::vector<std::filesystem::path> want;
    for (const auto& f : files) want.push_back(canon(f));
    int n = 0;
    for (auto& t : m_tabs) {
        if (!t->blocks || !t->script.dirty() || std::find(want.begin(), want.end(), canon(t->script.path())) == want.end()) continue;
        syncBlocks(*t);
        const std::string before = t->script.buffer.text(), after = fn(before);
        if (after == before || !readBlocks(after, std::string(), nullptr)) continue;
        replaceAll(t->script.buffer, after);
        syncBlocks(*t);  // one step in the block editor too
        ++n;
    }
    return n;
}

std::optional<std::filesystem::path> ScriptTabs::takeConvertRequest() {
    std::optional<std::filesystem::path> r;
    r.swap(m_convertRequest);
    return r;
}

std::vector<std::filesystem::path> ScriptTabs::takeSaved() {
    std::vector<std::filesystem::path> out;
    out.swap(m_saved);
    return out;
}

void ScriptTabs::close(int i) {
    if (i < 0 || i >= (int)m_tabs.size()) return;
    if (m_tabs[i]->script.dirty()) {
        m_closing = i;
        m_openClosePrompt = true;
        return;
    }
    m_tabs.erase(m_tabs.begin() + i);
    if (m_active > i || m_active >= (int)m_tabs.size()) --m_active;
    if (m_active >= 0) m_tabs[m_active]->focused = false;
}

static void saveTab(ScriptTabs& tabs, ScriptTabs::Tab& t, std::vector<std::filesystem::path>* saved) {
    std::string err;
    if (t.script.save(&err)) {
        t.diskChanged = t.diskDeleted = false;
        saved->push_back(t.script.path());
        tabs.notice = "Saved " + t.script.title();
        tabs.noticeBad = false;
    } else {
        tabs.notice = "Save failed: " + err;
        tabs.noticeBad = true;
    }
}

// Save, Don't save or Cancel for a changed tab being closed.
void ScriptTabs::closePrompt() {
    const char* id = "##scriptclose";
    if (m_openClosePrompt) {
        m_openClosePrompt = false;
        ImGui::OpenPopup(id);
    }
    if (m_closing < 0 || m_closing >= (int)m_tabs.size()) return;
    const float w = 420;
    ImVec2 d = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::lg, space::lg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!open) {
        m_closing = -1;
        return;
    }
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float y = o.y;
    Tab& t = *m_tabs[m_closing];
    const std::string title = "Save changes to " + t.script.title() + "?";
    text(dl, ImVec2(o.x, y), f.semibold, type::title, color::text, title.c_str());
    y += 26;
    text(dl, ImVec2(o.x, y), f.regular, type::label, color::textDim, "Your changes are lost if you don't save them.");
    y += 34;
    auto width = [&](const char* l) { return space::md * 2 + measure(f.medium, type::body, l).x; };
    float bx = o.x + w - space::lg * 2 - width("Save");
    int done = 0;  // 1 save, 2 discard, 3 cancel
    if (button("save", ImVec2(bx, y), nullptr, "Save", ButtonKind::Primary) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) done = 1;
    bx -= space::sm + width("Don't save");
    if (button("discard", ImVec2(bx, y), nullptr, "Don't save", ButtonKind::Secondary)) done = 2;
    if (button("cancel", ImVec2(o.x, y), nullptr, "Cancel", ButtonKind::Ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = 3;
    y += size::field + 6;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w - space::lg * 2, y - o.y));
    if (done) {
        ImGui::CloseCurrentPopup();
        int i = m_closing;
        if (done == 1) saveTab(*this, t, &m_saved);
        if (done == 2 || (done == 1 && !t.script.dirty())) {
            m_tabs.erase(m_tabs.begin() + i);
            if (m_active > i || m_active >= (int)m_tabs.size()) --m_active;
        }
        m_closing = -1;
    }
    ImGui::EndPopup();
}

// Save, Close and (for blocks) Convert to Lua..., on a script tab's right-click.
void ScriptTabs::tabMenu() {
    const char* id = "##scripttabmenu";
    if (m_openMenu) {
        m_openMenu = false;
        ImGui::OpenPopup(id);
    }
    if (!ImGui::IsPopupOpen(id)) return;
    if (m_menuFor < 0 || m_menuFor >= (int)m_tabs.size()) {
        if (ImGui::BeginPopup(id)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    const int i = m_menuFor;
    Tab& t = *m_tabs[i];
    std::vector<MenuItem> items;
    items.push_back({"Save", "", t.script.dirty() || t.diskDeleted, [this, i] { saveTab(*this, *m_tabs[i], &m_saved); }});
    items.push_back({"Close", "", true, [this, i] { close(i); }});
    if (t.blocks) {
        items.push_back({});
        items.push_back({"Convert to Lua...", "", true, [this, i] {
                             m_converting = i;
                             m_openConvert = true;
                         }});
    }
    float w, h;
    menuSize(items, &w, &h);
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    ImVec2 pos = m_menuPos;
    if (pos.x + w > d.x - space::xs) pos.x -= w;
    if (pos.y + h > d.y - space::xs) pos.y -= h;
    pos.x = std::max(pos.x, space::xs);
    pos.y = std::max(pos.y, space::xs);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    const bool shown = ImGui::BeginPopup(id, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!shown) return;
    std::function<void()> run = menuRows(items, w, h, "tabs.menu");
    if (run || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (run) {
        m_menuFor = -1;
        run();
    }
}

// Convert or Cancel for a blocks tab turning into Lua.
void ScriptTabs::convertPrompt() {
    const char* id = "##scriptconvert";
    if (m_openConvert) {
        m_openConvert = false;
        ImGui::OpenPopup(id);
    }
    if (m_converting < 0 || m_converting >= (int)m_tabs.size() || !m_tabs[m_converting]->blocks) {
        m_converting = -1;
        return;
    }
    const float w = 460;
    ImVec2 d = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::raised);
    ImGui::PushStyleColor(ImGuiCol_Border, color::borderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(space::lg, space::lg));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, radius::card);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!open) {
        m_converting = -1;
        return;
    }
    Fonts& f = fonts();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float inner = w - space::lg * 2;
    float y = o.y;
    Tab& t = *m_tabs[m_converting];
    const std::string title = "Convert " + t.script.title() + " to Lua?";
    text(dl, ImVec2(o.x, y), f.semibold, type::title, color::text, title.c_str());
    y += 26;
    // The body wraps at the prompt's width.
    {
        const std::string body = "The blocks become a Lua script and every reference to the script follows. "
                                 "The editor can't turn Lua back into blocks; Ctrl+Z undoes this.";
        std::string line, word;
        auto flush = [&]() {
            text(dl, ImVec2(o.x, y), f.regular, type::label, color::textDim, line.c_str());
            y += 18;
            line.clear();
        };
        for (size_t k = 0; k <= body.size(); ++k) {
            if (k == body.size() || body[k] == ' ') {
                const std::string next = line.empty() ? word : line + " " + word;
                if (!line.empty() && measure(f.regular, type::label, next.c_str()).x > inner) {
                    flush();
                    line = word;
                } else {
                    line = next;
                }
                word.clear();
            } else {
                word += body[k];
            }
        }
        if (!line.empty()) flush();
    }
    y += 16;
    auto width = [&](const char* l) { return space::md * 2 + measure(f.medium, type::body, l).x; };
    const float bx = o.x + inner - width("Convert");
    int done = 0;  // 1 convert, 2 cancel
    if (button("convert.ok", ImVec2(bx, y), nullptr, "Convert", ButtonKind::Primary) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) done = 1;
    if (button("convert.cancel", ImVec2(o.x, y), nullptr, "Cancel", ButtonKind::Ghost) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = 2;
    y += size::field + 6;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner, y - o.y));
    if (done) {
        ImGui::CloseCurrentPopup();
        if (done == 1) {
            if (t.script.dirty()) saveTab(*this, t, &m_saved);
            if (!t.script.dirty()) m_convertRequest = t.script.path();
        }
        m_converting = -1;
    }
    ImGui::EndPopup();
}

// About once a second: a tab whose file changed on disk reloads when it has
// no unsaved edits, else asks (the bar over its editor). One whose file was
// deleted says so on its tab and offers Save or Close.
void ScriptTabs::pollDisk() {
    const double now = ImGui::GetTime();
    if (m_polledAt >= 0 && now - m_polledAt < 1.0) return;
    m_polledAt = now;
    for (auto& t : m_tabs) {
        const bool changed = t->script.changedOnDisk();
        if (changed && !t->script.dirty()) {
            std::string err;
            if (t->script.reload(&err)) t->compOpen = false;
        }
        t->diskChanged = changed && t->script.dirty();
        t->diskDeleted = t->script.deletedOnDisk();
    }
}

ImRect ScriptTabs::tabStrip(ImDrawList* dl, ImRect r) {
    closePrompt();
    tabMenu();
    convertPrompt();
    pollDisk();
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        if (!m_tabs[i]->closeAsked) continue;
        m_tabs[i]->closeAsked = false;
        close(i);
        break;  // the rest wait a frame: close() may be asking about this one
    }
    Fonts& f = fonts();
    const float h = size::panelHeader;
    ImRect strip(r.Min, ImVec2(r.Max.x, r.Min.y + h));
    dl->AddRectFilled(strip.Min, strip.Max, color::panel, radius::window);
    ImGui::PushID("scripttabs");
    float x = strip.Min.x + space::xs;
    // -3 is "none" for both: -1 and -2 are the Scene and UI tabs.
    int pick = -3, closeIt = -3;
    std::vector<int> order = {kScene, kUi};
    for (int i = 0; i < (int)m_tabs.size(); ++i) order.push_back(i);
    for (int i : order) {
        const bool scene = i < 0;  // Scene or UI: no close button
        std::string label = i == kScene ? std::string("Scene") : i == kUi ? std::string("UI") : m_tabs[i]->script.title();
        if (i >= 0 && m_tabs[i]->diskDeleted) label += " (deleted)";
        const bool blocksTab = i >= 0 && m_tabs[i]->blocks;
        const char* ic = i == kScene ? icon::box : i == kUi ? icon::layoutTemplate : blocksTab ? icon::layers : icon::fileCode;
        const float iw = measure(f.medium, type::icon, ic).x, tw = measure(f.medium, type::label, label.c_str()).x;
        const float w = space::md + iw + space::sm - 2 + tw + (scene ? space::md : 28);
        ImRect tr(ImVec2(x, strip.Min.y + space::xs), ImVec2(x + w, strip.Max.y));
        ImGui::PushID(i);
        nameRect(i == kScene ? std::string("tab.scene") : i == kUi ? std::string("tab.ui") : "tab." + std::to_string(i), tr);
        Hit hit = interact("tab", tr);
        const bool active = i == m_active;
        if (active) dl->AddRectFilled(tr.Min, tr.Max, color::raised, radius::button, ImDrawFlags_RoundCornersTop);
        else if (hit.hovered) dl->AddRectFilled(tr.Min, tr.Max, color::hover, radius::button, ImDrawFlags_RoundCornersTop);
        if (active) dl->AddRectFilled(ImVec2(tr.Min.x + 6, tr.Max.y - 2), ImVec2(tr.Max.x - 6, tr.Max.y), color::accent, 1);
        const float cy = tr.GetCenter().y;
        ImU32 icol = i == kScene ? color::textDim : i == kUi ? rgb(0xf2c25c) : blocksTab ? blocks::events : rgb(0x5ccfe0);
        dl->AddText(f.medium, type::icon, ImVec2(tr.Min.x + space::md, cy - type::icon * 0.5f - 1), icol, ic);
        dl->AddText(f.medium, type::label, ImVec2(tr.Min.x + space::md + iw + space::sm - 2, cy - type::label * 0.5f - 1),
                    active ? color::text : color::textDim, label.c_str());
        if (i == kUi) tooltip("The scene's UI canvases: HUD, menus and prompts at PS1 resolution");
        if (!scene) {
            tooltip(utf8(m_tabs[i]->script.path()).c_str());
            ImRect cr(ImVec2(tr.Max.x - 24, cy - 8), ImVec2(tr.Max.x - 8, cy + 8));
            const bool dirty = m_tabs[i]->script.dirty();
            const bool overClose = cr.Contains(ImGui::GetIO().MousePos);
            if (hit.hovered && overClose) dl->AddRectFilled(cr.Min, cr.Max, color::active, radius::field);
            if (dirty && !(hit.hovered && overClose)) dl->AddCircleFilled(cr.GetCenter(), 3.5f, color::text);
            else if (hit.hovered || active)
                dl->AddText(f.medium, type::caption, cr.GetCenter() - measure(f.medium, type::caption, icon::x) * 0.5f, color::textDim, icon::x);
            if (hit.clicked && overClose) closeIt = i;
            if (hit.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) closeIt = i;
            if (hit.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_menuFor = i;
                m_openMenu = true;
                m_menuPos = ImGui::GetIO().MousePos;
            }
        }
        if (hit.clicked && closeIt != i) pick = i;
        ImGui::PopID();
        x += w + space::xxs;
    }
    ImGui::PopID();
    if (pick != -3) {
        for (auto& t : m_tabs) t->focused = false;
        m_active = pick;
        if (m_active >= 0) m_tabs[m_active]->focused = true;
        if (namedRectsOn()) {
            // Scripted tests: which tab a click brought to the front.
            std::printf("scripttabs: active %s\n",
                        m_active == kScene ? "Scene" : m_active == kUi ? "UI" : m_tabs[m_active]->script.title().c_str());
            std::fflush(stdout);
        }
    }
    if (closeIt >= 0) close(closeIt);
    return ImRect(ImVec2(r.Min.x, strip.Max.y), r.Max);
}

// --- Drawing helpers ----------------------------------------------------------

static ImU32 tokColor(lua::Tok k) {
    switch (k) {
        case lua::Tok::Keyword: return code::keyword;
        case lua::Tok::Constant: return code::constant;
        case lua::Tok::Function: return code::function;
        case lua::Tok::Api: return code::api;
        case lua::Tok::ApiMember: return code::apiMember;
        case lua::Tok::Number: return code::number;
        case lua::Tok::String: return code::string;
        case lua::Tok::Comment: return code::comment;
        case lua::Tok::Operator: return code::op;
        case lua::Tok::Error: return color::bad;
        default: return color::text;
    }
}

// Where the cards may go: the script editor's panel, so none covers the
// inspector beside it. Set by ScriptTabs::draw.
static ImRect g_cardBounds;

// A card on top of everything: a signature in the code font, then wrapped
// docs. `arg` (from 0) brightens that parameter. Drawn above `at` when
// `above`, else below; kept inside g_cardBounds. With `beside` (the
// completion list) it goes right of that, else left, else under or over it.
// Returns its rect.
static ImRect infoCard(ImVec2 at, bool above, const std::string& sig, const std::string& doc, int arg = -1,
                       const ImRect* beside = nullptr) {
    Fonts& f = fonts();
    ImFont* mono = f.mono ? f.mono : f.regular;
    ImRect bounds = g_cardBounds;
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0) bounds = ImRect(ImVec2(0, 0), d);
    bounds.ClipWithFull(ImRect(ImVec2(space::xs, space::xs), d - ImVec2(space::xs, space::xs)));
    // Narrow panels wrap the docs sooner.
    const float wrap = std::max(160.0f, std::min(420.0f, bounds.GetWidth() - space::md * 2 - 2));
    ImVec2 ss = mono->CalcTextSizeA(type::code, FLT_MAX, 0, sig.c_str());
    ImVec2 ds = doc.empty() ? ImVec2(0, 0) : f.regular->CalcTextSizeA(type::label, FLT_MAX, wrap, doc.c_str());
    float w = std::max(ss.x, ds.x) + space::md * 2, h = ss.y + (doc.empty() ? 0 : ds.y + space::sm) + space::sm * 2;
    ImRect r(ImVec2(at.x, above ? at.y - h : at.y), ImVec2(at.x + w, above ? at.y : at.y + h));
    if (beside && r.Max.x > bounds.Max.x) {
        const float gap = 6;
        if (beside->Min.x - gap - w >= bounds.Min.x) {
            r.Min.x = beside->Min.x - gap - w, r.Max.x = beside->Min.x - gap;
        } else {
            // No room on either side: under the list, else over it.
            r.Min.x = beside->Min.x, r.Max.x = beside->Min.x + w;
            const float y = beside->Max.y + gap + h <= bounds.Max.y ? beside->Max.y + gap : beside->Min.y - gap - h;
            r.Min.y = y, r.Max.y = y + h;
        }
    }
    float sx = std::min(0.0f, bounds.Max.x - r.Max.x);
    sx = std::max(sx, bounds.Min.x - (r.Min.x + sx));
    r.Min.x += sx, r.Max.x += sx;
    float sy = std::min(0.0f, bounds.Max.y - r.Max.y);
    sy = std::max(sy, bounds.Min.y - (r.Min.y + sy));
    r.Min.y += sy, r.Max.y += sy;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(r.Min + ImVec2(0, 2), r.Max + ImVec2(0, 4), rgb(0x000000, 70), radius::card);
    dl->AddRectFilled(r.Min, r.Max, color::active, radius::card);
    dl->AddRect(r.Min, r.Max, color::borderStrong, radius::card);
    ImVec2 p = r.Min + ImVec2(space::md, space::sm);
    // The current argument of a call stands out.
    size_t a = std::string::npos, b = std::string::npos;
    if (arg >= 0) {
        size_t open = sig.find('(');
        if (open != std::string::npos) {
            int n = 0, depth = 0;
            size_t start = open + 1;
            for (size_t i = open + 1; i < sig.size(); ++i) {
                char c = sig[i];
                if (c == '(' || c == '{' || c == '[') ++depth;
                else if ((c == ')' || c == '}' || c == ']') && depth > 0) --depth;
                else if ((c == ',' && depth == 0) || (c == ')' && depth == 0)) {
                    if (n == arg) {
                        a = start;
                        b = i;
                        break;
                    }
                    ++n;
                    start = i + 1;
                    if (c == ')') break;
                }
            }
            while (a != std::string::npos && a < b && sig[a] == ' ') ++a;
        }
    }
    if (a != std::string::npos && a < b) {
        float x = p.x;
        auto part = [&](size_t from, size_t to, ImU32 col) {
            dl->AddText(mono, type::code, ImVec2(x, p.y), col, sig.c_str() + from, sig.c_str() + to);
            x += mono->CalcTextSizeA(type::code, FLT_MAX, 0, sig.c_str() + from, sig.c_str() + to).x;
        };
        part(0, a, color::textDim);
        float x0 = x;
        part(a, b, color::accentHover);
        dl->AddLine(ImVec2(x0, p.y + ss.y + 1), ImVec2(x, p.y + ss.y + 1), color::accent);
        part(b, sig.size(), color::textDim);
    } else {
        dl->AddText(mono, type::code, p, code::function, sig.c_str());
    }
    if (!doc.empty())
        dl->AddText(f.regular, type::label, ImVec2(p.x, p.y + ss.y + space::sm), color::text, doc.c_str(), nullptr, wrap);
    return r;
}

static std::string signatureOf(const lua::Lookup& l) {
    if (l.api) return l.api->signature;
    if (l.def) {
        if (!l.def->signature.empty()) return l.def->signature;
        const char* what = l.def->kind == lua::DefKind::Parameter ? "parameter " : l.def->kind == lua::DefKind::Global ? "global " : "local ";
        return what + l.def->name;
    }
    return std::string();
}

static std::string docOf(const lua::Lookup& l) {
    if (l.api) return l.api->doc;
    if (l.def) return "Defined on line " + std::to_string(l.def->pos.line + 1) + ".";
    return std::string();
}

static void squiggle(ImDrawList* dl, float x0, float x1, float y, ImU32 col) {
    const float step = 3;
    std::vector<ImVec2> pts;
    for (float x = x0; x <= x1 + 0.01f; x += step) pts.push_back(ImVec2(x, y + ((int)((x - x0) / step) % 2 ? 1.5f : -1.0f)));
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.2f);
}

// --- The editor ---------------------------------------------------------------

static void openCompletion(ScriptTabs::Tab& t, bool forced) {
    t.compForced = forced;
    t.comp = lua::complete(t.script.buffer.lines(), t.script.buffer.cursor(), forced);
    t.compOpen = !t.comp.items.empty();
    t.compSel = 0;
    t.compTop = 0;
    t.compRevision = t.script.buffer.revision();
    t.compCursor = t.script.buffer.cursor();
}

static void acceptCompletion(ScriptTabs::Tab& t) {
    const lua::Completion& c = t.comp.items[t.compSel];
    std::string text = c.label;
    // An event handler completes to its parameter list.
    if (c.kind == lua::ItemKind::Callback && c.api) {
        std::string sig = c.api->signature;
        if (sig.compare(0, 9, "function ") == 0) text = sig.substr(9);
    }
    t.script.buffer.replace(t.comp.from, t.script.buffer.cursor(), text);
    t.compOpen = false;
}

static void gotoDefinition(ScriptTabs::Tab& t, lua::Pos p, double now) {
    lua::Buffer& b = t.script.buffer;
    lua::Lookup l = lua::lookup(b.lines(), p);
    if (l.def) {
        std::string name = l.def->name;
        size_t dot = name.find_last_of(".:");
        if (dot != std::string::npos) name = name.substr(dot + 1);
        b.select(l.def->pos, lua::Pos{l.def->pos.line, l.def->pos.col + (int)name.size()});
        t.reveal = t.revealCenter = true;
        t.flashLine = l.def->pos.line;
        t.flashAt = now;
        t.peek = nullptr;
    } else if (l.api) {
        t.peek = l.api;
        t.peekAt = l.start;
    }
}

static const char* kindBadge(lua::ItemKind k, ImU32* col) {
    switch (k) {
        case lua::ItemKind::Keyword: *col = code::keyword; return "k";
        case lua::ItemKind::Local: *col = color::text; return "v";
        case lua::ItemKind::Function: *col = code::function; return "f";
        case lua::ItemKind::ApiTable: *col = code::api; return "T";
        case lua::ItemKind::ApiFunction: *col = code::apiMember; return "f";
        case lua::ItemKind::ApiConstant: *col = code::constant; return "c";
        case lua::ItemKind::Callback: *col = color::good; return "e";
        case lua::ItemKind::Field: *col = color::textDim; return "." ;
    }
    *col = color::text;
    return "?";
}

void ScriptTabs::draw(ImDrawList* dl, ImRect r) {
    if (m_active < 0 || m_active >= (int)m_tabs.size()) return;
    Tab& t = *m_tabs[m_active];
    lua::Script& s = t.script;
    lua::Buffer& b = s.buffer;
    Fonts& f = fonts();
    ImFont* mono = f.mono ? f.mono : f.regular;
    ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const float fs = type::code, lineH = std::floor(fs * 1.5f);
    const float charW = mono->CalcTextSizeA(fs, FLT_MAX, 0, "M").x;
    ImGui::PushID(&t);
    g_cardBounds = r;

    dl->AddRectFilled(r.Min, r.Max, color::panel, radius::window, ImDrawFlags_RoundCornersBottom);
    const float statusH = 28, barW = 10;
    ImRect status(ImVec2(r.Min.x, r.Max.y - statusH), r.Max);
    ImRect area(r.Min, ImVec2(r.Max.x, status.Min.y));
    // The file changed on disk while this tab has unsaved edits, or was
    // deleted: a bar over the text with what to do about it.
    ImRect diskBar;
    auto width = [&](const char* l) { return space::md * 2 + measure(f.medium, type::body, l).x; };
    auto diskBarFrame = [&](const char* what, const char* detail) {
        const float bh = 38;
        ImRect bar(area.Min, ImVec2(area.Max.x, area.Min.y + bh));
        diskBar = bar;
        area.Min.y = bar.Max.y;
        dl->AddRectFilled(bar.Min, bar.Max, color::warnSoft);
        dl->AddLine(ImVec2(bar.Min.x, bar.Max.y - 0.5f), ImVec2(bar.Max.x, bar.Max.y - 0.5f), color::border);
        const float cy = bar.GetCenter().y;
        float x = bar.Min.x + space::md;
        text(dl, ImVec2(x, cy - type::icon * 0.5f - 1), f.medium, type::icon, color::warn, icon::warning);
        x += measure(f.medium, type::icon, icon::warning).x + space::sm;
        text(dl, ImVec2(x, cy - type::label * 0.5f - 1), f.semibold, type::label, color::text, what);
        x += measure(f.semibold, type::label, what).x + space::sm;
        text(dl, ImVec2(x, cy - type::label * 0.5f - 1), f.regular, type::label, color::textDim, detail);
        return cy - size::field * 0.5f;
    };
    if (t.diskDeleted) {
        const float by = diskBarFrame("Deleted on disk.", "Save writes it again; Close drops the tab.");
        float bx = diskBar.Max.x - space::md - width("Close");
        if (button("closedeleted", ImVec2(bx, by), nullptr, "Close", ButtonKind::Secondary, nullptr,
                   "Close the tab. With unsaved edits it asks first.")) {
            t.closeAsked = true;
        }
        bx -= space::sm + width("Save");
        if (button("recreate", ImVec2(bx, by), nullptr, "Save", ButtonKind::Primary, nullptr,
                   "Write this text to the file again.")) {
            saveTab(*this, t, &m_saved);
            t.focused = true;
        }
    } else if (t.diskChanged) {
        const float by = diskBarFrame("Changed on disk.", "This tab has unsaved edits.");
        float bx = diskBar.Max.x - space::md - width("Keep mine");
        if (button("keepmine", ImVec2(bx, by), nullptr, "Keep mine", ButtonKind::Secondary, nullptr,
                   "Keep editing your text; saving overwrites the file.")) {
            s.keepMine();
            t.diskChanged = false;
            t.focused = true;
        }
        bx -= space::sm + width("Reload");
        if (button("reload", ImVec2(bx, by), nullptr, "Reload", ButtonKind::Primary, nullptr,
                   "Read the file again. Ctrl+Z gets your edits back.")) {
            std::string err;
            if (s.reload(&err)) {
                t.diskChanged = false;
                t.focused = true;
                t.compOpen = false;
                t.reveal = true;
            } else {
                notice = "Reload failed: " + err;
                noticeBad = true;
            }
        }
    }
    if (t.blocks) {
        syncBlocks(t);
        const BlocksView::Result res = t.blocks->draw(dl, ImRect(area.Min, r.Max), &t.focused, m_blockContext);
        applyBlocks(t, res);
        if (res.save) saveTab(*this, t, &m_saved);
        ImGui::PopID();
        return;
    }
    int digits = 3;
    for (int n = b.lineCount(); n >= 1000; n /= 10) ++digits;
    const float gutterW = digits * charW + 30;
    ImRect body(ImVec2(area.Min.x + gutterW, area.Min.y), ImVec2(area.Max.x - barW, area.Max.y));
    const float padTop = space::sm, padLeft = 6;
    auto origin = [&] { return ImVec2(body.Min.x + padLeft - t.scroll.x, body.Min.y + padTop - t.scroll.y); };
    auto xOf = [&](int line, int col) { return origin().x + lua::columnOf(b.line(line), col) * charW; };
    auto yOf = [&](int line) { return origin().y + line * lineH; };
    auto posAt = [&](ImVec2 m) {
        int line = std::clamp((int)std::floor((m.y - origin().y) / lineH), 0, b.lineCount() - 1);
        int column = std::max(0, (int)std::floor((m.x - origin().x) / charW + 0.5f));
        return lua::Pos{line, lua::byteAt(b.line(line), column)};
    };

    // The compile check runs once typing pauses.
    if (b.revision() != t.seenRevision) {
        t.seenRevision = b.revision();
        t.lastEdit = now;
    }
    if (now - t.lastEdit > 0.25) s.diagnose();
    const lua::Diagnostic& diag = s.diagnostic();

    // --- Mouse ---
    const lua::Pos before = b.cursor();
    const uint64_t revBefore = b.revision();
    ImGui::SetCursorScreenPos(area.Min);
    // The find bar's buttons, drawn later on top, take the clicks over it.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##code", ImMax(ImVec2(area.GetWidth() - barW, area.GetHeight()), ImVec2(1, 1)));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const ImVec2 mouse = io.MousePos;
    const bool overPopup = (t.compOpen && t.compRect.Contains(mouse)) || (t.findOpen && t.findRect.Contains(mouse));
    if (hovered && !overPopup && mouse.x >= body.Min.x)
        ImGui::SetMouseCursor(io.KeyCtrl ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_TextInput);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered && !overPopup) {
            t.focused = true;
            t.lastInput = now;
            t.compOpen = false;
            t.peek = nullptr;
            lua::Pos p = posAt(mouse);
            int clicks = io.MouseClickedCount[0];
            if (io.KeyCtrl) gotoDefinition(t, p, now);
            else if (clicks == 2) {
                lua::Pos a, e;
                b.wordAt(p, &a, &e);
                b.select(a, e);
            } else if (clicks >= 3) b.select(lua::Pos{p.line, 0}, lua::Pos{p.line + 1, 0});
            else b.moveTo(p, io.KeyShift);
        } else if (!overPopup && !(t.compOpen && t.compRect.Contains(mouse)) && !diskBar.Contains(mouse)) {
            t.focused = false;
        }
    }
    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !io.KeyCtrl && !overPopup) {
        b.moveTo(posAt(mouse), true);
        t.reveal = true;
    }
    if (hovered && !overPopup) {
        float wheel = io.MouseWheel, wheelH = io.MouseWheelH;
        if (io.KeyShift && wheelH == 0) std::swap(wheel, wheelH);
        t.scroll.y -= wheel * lineH * 3;
        t.scroll.x -= wheelH * charW * 6;
    }

    // --- Keyboard ---
    if (t.focused) {
        ImGuiContext& g = *GImGui;
        g.PlatformImeData.WantVisible = true;
        g.PlatformImeData.WantTextInput = true;
        g.PlatformImeData.InputPos = ImVec2(xOf(b.cursor().line, b.cursor().col), yOf(b.cursor().line));
        g.PlatformImeData.InputLineHeight = lineH;
        g.PlatformImeData.ViewportId = ImGui::GetWindowViewport()->ID;

        auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
        const int page = std::max(1, (int)(body.GetHeight() / lineH) - 1);
        using M = lua::Buffer::Move;
        bool used = false;
        if (t.compOpen) {
            int n = (int)t.comp.items.size();
            if (key(ImGuiKey_DownArrow)) t.compSel = (t.compSel + 1) % n, used = true;
            else if (key(ImGuiKey_UpArrow)) t.compSel = (t.compSel + n - 1) % n, used = true;
            else if (key(ImGuiKey_PageDown)) t.compSel = std::min(n - 1, t.compSel + 8), used = true;
            else if (key(ImGuiKey_PageUp)) t.compSel = std::max(0, t.compSel - 8), used = true;
            else if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter) || key(ImGuiKey_Tab)) acceptCompletion(t), used = true;
            else if (key(ImGuiKey_Escape)) t.compOpen = false, used = true;
        }
        if (!used) {
            if (key(ImGuiKey_LeftArrow)) b.move(ctrl ? M::WordLeft : M::Left, shift);
            if (key(ImGuiKey_RightArrow)) b.move(ctrl ? M::WordRight : M::Right, shift);
            if (key(ImGuiKey_UpArrow)) {
                if (ctrl) t.scroll.y -= lineH;
                else b.move(M::Up, shift);
            }
            if (key(ImGuiKey_DownArrow)) {
                if (ctrl) t.scroll.y += lineH;
                else b.move(M::Down, shift);
            }
            if (key(ImGuiKey_Home)) b.move(ctrl ? M::Top : M::Home, shift);
            if (key(ImGuiKey_End)) b.move(ctrl ? M::Bottom : M::End, shift);
            if (key(ImGuiKey_PageUp)) b.move(M::Up, shift, page), t.scroll.y -= page * lineH;
            if (key(ImGuiKey_PageDown)) b.move(M::Down, shift, page), t.scroll.y += page * lineH;
            if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) b.newline();
            if (key(ImGuiKey_Tab)) shift ? b.outdent() : b.indent();
            if (key(ImGuiKey_Backspace)) {
                if (ctrl && !b.hasSelection()) b.move(M::WordLeft, true);
                b.backspace();
                if (t.compOpen) openCompletion(t, t.compForced);
            }
            if (key(ImGuiKey_Delete)) {
                if (ctrl && !b.hasSelection()) b.move(M::WordRight, true);
                b.del();
            }
            if (key(ImGuiKey_Escape)) {
                t.peek = nullptr;
                if (t.findOpen) t.findOpen = false;
            }
            if (key(ImGuiKey_F12)) gotoDefinition(t, b.cursor(), now);
            if (key(ImGuiKey_F3) && !t.query.empty()) {
                if (auto m = b.find(t.query, shift ? b.selStart() : b.selEnd(), !shift, t.opts)) b.select(m->first, m->second);
            }
            if (ctrl && !alt) {
                if (key(ImGuiKey_A)) b.selectAll();
                if (key(ImGuiKey_Z)) shift ? b.redo() : b.undo();
                if (key(ImGuiKey_Y)) b.redo();
                if (key(ImGuiKey_C) || key(ImGuiKey_X)) {
                    // Without a selection, the whole line.
                    bool cut = ImGui::IsKeyPressed(ImGuiKey_X, true);
                    if (!b.hasSelection()) {
                        lua::Pos c = b.cursor();
                        bool last = c.line + 1 >= b.lineCount();
                        ImGui::SetClipboardText((b.line(c.line) + "\n").c_str());
                        if (cut) {
                            if (last && c.line > 0) b.select(lua::Pos{c.line - 1, (int)b.line(c.line - 1).size()}, lua::Pos{c.line, (int)b.line(c.line).size()});
                            else b.select(lua::Pos{c.line, 0}, last ? lua::Pos{c.line, (int)b.line(c.line).size()} : lua::Pos{c.line + 1, 0});
                            b.del();
                        }
                    } else {
                        ImGui::SetClipboardText(b.selectedText().c_str());
                        if (cut) b.del();
                    }
                }
                if (key(ImGuiKey_V)) {
                    if (const char* clip = ImGui::GetClipboardText()) {
                        std::string paste;
                        for (const char* c = clip; *c; ++c)
                            if (*c != '\r') paste += *c;
                        b.insert(paste);
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_S, false)) saveTab(*this, t, &m_saved);
                // Ctrl+F finds, Ctrl+H finds and replaces.
                const bool findKey = ImGui::IsKeyPressed(ImGuiKey_F, false), replaceKey = ImGui::IsKeyPressed(ImGuiKey_H, false);
                if (findKey || replaceKey) {
                    t.findOpen = true;
                    t.replaceOpen = replaceKey;
                    t.findFocus = true;
                    if (b.hasSelection() && b.selStart().line == b.selEnd().line) {
                        t.query = b.selectedText();
                        // Literally, with Regex on.
                        if (t.opts.regex) {
                            std::string q;
                            for (char c : t.query) {
                                if (std::strchr("\\^$.|?*+()[]{}", c)) q += '\\';
                                q += c;
                            }
                            t.query = q;
                        }
                    }
                }
                if (key(ImGuiKey_Slash)) b.toggleComment();
                if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) openCompletion(t, true);
                if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                    ImGui::PopID();
                    close(m_active);
                    return;
                }
            }
        }
        // Typed text. Ctrl+Alt is AltGr on some layouts, which types.
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c == 127 || (ctrl && !alt)) continue;
            char buf[5] = {};
            ImTextCharToUtf8(buf, c);
            b.type(buf);
            const bool word = c < 128 && (std::isalnum((int)c) || c == '_');
            if (word && !t.compOpen) openCompletion(t, false);
            else if (c == '.' || c == ':') openCompletion(t, false);
            else if (!word) t.compOpen = false;
        }
        io.InputQueueCharacters.resize(0);
    }
    if (b.cursor() != before || b.revision() != revBefore) {
        t.lastInput = now;
        t.reveal = true;
        if (b.cursor() != t.peekAt) t.peek = nullptr;
    }

    // The completion list follows the text; it closes when the cursor leaves the word.
    if (t.compOpen && (b.revision() != t.compRevision || b.cursor() != t.compCursor)) {
        lua::Pos c = b.cursor();
        if (c.line != t.comp.from.line || c < t.comp.from) t.compOpen = false;
        else {
            std::string keep = t.comp.items[t.compSel].label;
            openCompletion(t, t.compForced);
            for (int i = 0; i < (int)t.comp.items.size(); ++i)
                if (t.comp.items[i].label == keep && !t.comp.prefix.empty()) t.compSel = i;
        }
    }

    // --- Scrolling ---
    const float viewH = body.GetHeight() - padTop, viewW = body.GetWidth() - padLeft;
    if (t.reveal) {
        lua::Pos c = b.cursor();
        float cy = c.line * lineH, cx = lua::columnOf(b.line(c.line), c.col) * charW;
        if (t.revealCenter) t.scroll.y = cy - viewH * 0.4f;
        else if (cy < t.scroll.y) t.scroll.y = cy;
        else if (cy + lineH > t.scroll.y + viewH) t.scroll.y = cy + lineH - viewH;
        if (cx < t.scroll.x + charW * 2) t.scroll.x = std::max(0.0f, cx - charW * 6);
        else if (cx > t.scroll.x + viewW - charW * 4) t.scroll.x = cx - viewW + charW * 8;
        t.reveal = t.revealCenter = false;
    }
    int widest = 0;
    for (const std::string& l : b.lines()) widest = std::max(widest, (int)l.size());
    t.scroll.y = std::clamp(t.scroll.y, 0.0f, std::max(0.0f, (b.lineCount() - 1) * lineH));
    t.scroll.x = std::clamp(t.scroll.x, 0.0f, std::max(0.0f, widest * charW + charW * 8 - viewW));

    // --- Text ---
    const int first = std::max(0, (int)(t.scroll.y / lineH) - 1);
    const int last = std::min(b.lineCount() - 1, first + (int)(body.GetHeight() / lineH) + 2);
    const lua::Pos cur = b.cursor();
    dl->PushClipRect(ImVec2(body.Min.x, area.Min.y), ImVec2(body.Max.x, area.Max.y), true);
    // Current line, find matches, selection, the line go-to-definition landed on.
    if (!b.hasSelection())
        dl->AddRectFilled(ImVec2(body.Min.x, yOf(cur.line)), ImVec2(body.Max.x, yOf(cur.line) + lineH), code::currentLine);
    if (t.flashLine >= 0 && now - t.flashAt < 1.0) {
        int a = (int)(90 * (1.0 - (now - t.flashAt)));
        dl->AddRectFilled(ImVec2(body.Min.x, yOf(t.flashLine)), ImVec2(body.Max.x, yOf(t.flashLine) + lineH), rgb(0x8b7bff, a));
    }
    // The range In selection searches, then the matches.
    if (t.findOpen && t.opts.inSelection && b.findScope()) {
        auto [a, e] = *b.findScope();
        for (int l = std::max(first, a.line); l <= std::min(last, e.line); ++l) {
            float x0 = l == a.line ? xOf(l, a.col) : origin().x;
            float x1 = l == e.line ? xOf(l, e.col) : xOf(l, (int)b.line(l).size()) + charW * 0.6f;
            dl->AddRectFilled(ImVec2(x0, yOf(l)), ImVec2(x1, yOf(l) + lineH), code::scope);
        }
    }
    std::vector<std::pair<lua::Pos, lua::Pos>> found;
    if (t.findOpen && !t.query.empty()) found = b.matches(t.query, t.opts);
    for (const auto& [ma, mb] : found) {
        if (ma.line < first) continue;
        if (ma.line > last) break;
        dl->AddRectFilled(ImVec2(xOf(ma.line, ma.col), yOf(ma.line) + 1), ImVec2(xOf(ma.line, mb.col), yOf(ma.line) + lineH - 1), code::match, 2);
    }
    if (b.hasSelection()) {
        lua::Pos a = b.selStart(), e = b.selEnd();
        for (int l = std::max(first, a.line); l <= std::min(last, e.line); ++l) {
            float x0 = l == a.line ? xOf(l, a.col) : origin().x;
            float x1 = l == e.line ? xOf(l, e.col) : xOf(l, (int)b.line(l).size()) + charW * 0.6f;
            dl->AddRectFilled(ImVec2(x0, yOf(l)), ImVec2(x1, yOf(l) + lineH), code::selection, 2);
        }
    }
    const std::vector<std::vector<lua::Token>>& toks = s.tokens();
    const float textOff = std::floor((lineH - fs) * 0.5f);
    std::string piece;
    for (int l = first; l <= last; ++l) {
        const std::string& line = b.line(l);
        const float y = yOf(l) + textOff;
        for (const lua::Token& tk : toks[l]) {
            float x = xOf(l, tk.col);
            if (x > body.Max.x) break;
            piece.assign(line, tk.col, tk.len);
            if (piece.find('\t') != std::string::npos) {
                // Tabs inside strings and comments, as spaces to the next stop.
                std::string out;
                int col = lua::columnOf(line, tk.col);
                for (char c : piece) {
                    if (c == '\t') {
                        int n = lua::Buffer::kTab - col % lua::Buffer::kTab;
                        out.append(n, ' ');
                        col += n;
                    } else {
                        out += c;
                        col += ((unsigned char)c & 0xc0) != 0x80;
                    }
                }
                piece = out;
            }
            dl->AddText(mono, fs, ImVec2(std::floor(x), y), tokColor(tk.kind), piece.c_str());
        }
    }
    // The compile error: a squiggle under the offending text, the message after the line.
    bool errorCard = false;  // the error's card is up; the name card waits
    if (!diag.ok() && diag.line < b.lineCount()) {
        const std::string& line = b.line(diag.line);
        int a = (int)line.find_first_not_of(" \t"), e = (int)line.size();
        if (a < 0) a = 0, e = 0;
        size_t near = diag.message.find("near '");
        if (near != std::string::npos) {
            size_t q = diag.message.find('\'', near + 6);
            std::string what = diag.message.substr(near + 6, q == std::string::npos ? std::string::npos : q - near - 6);
            size_t at = what.empty() ? std::string::npos : line.find(what);
            if (at != std::string::npos) a = (int)at, e = (int)(at + what.size());
        }
        float x0 = xOf(diag.line, a), x1 = std::max(xOf(diag.line, e), x0 + charW * 2);
        float y = yOf(diag.line) + lineH - 3;
        if (diag.line >= first && diag.line <= last) {
            squiggle(dl, x0, x1, y, color::bad);
            float mx = xOf(diag.line, (int)line.size()) + charW * 3;
            dl->AddRectFilled(ImVec2(mx - charW, yOf(diag.line) + 2), ImVec2(body.Max.x, yOf(diag.line) + lineH - 2), rgb(0xef5a6f, 22), 3);
            dl->AddText(f.regular, type::label, ImVec2(mx, yOf(diag.line) + (lineH - type::label) * 0.5f - 1), rgb(0xef5a6f, 220),
                        diag.message.c_str());
        }
        // Hovering the squiggle shows the whole message.
        if (hovered && ImRect(ImVec2(x0, yOf(diag.line)), ImVec2(x1, yOf(diag.line) + lineH)).Contains(mouse) && (errorCard = true))
            infoCard(ImVec2(mouse.x, yOf(diag.line) + lineH + 2), false, "Line " + std::to_string(diag.line + 1), diag.message);
    }
    // The caret blinks, solid just after input.
    if (t.focused && std::fmod(now - t.lastInput, 1.06) < 0.62) {
        float x = std::floor(xOf(cur.line, cur.col));
        dl->AddRectFilled(ImVec2(x, yOf(cur.line) + 2), ImVec2(x + 2, yOf(cur.line) + lineH - 2), color::accent);
    }
    // Ctrl over a name: it is a link to its definition.
    lua::Pos mp = posAt(mouse);
    const bool overText = hovered && !overPopup && mouse.x >= body.Min.x && mp.col < (int)b.line(mp.line).size() &&
                          mouse.y < yOf(b.lineCount() - 1) + lineH;
    if (overText) {
        if (t.hoverPos != mp || t.hoverRevision != b.revision()) {
            lua::Lookup l = lua::lookup(b.lines(), mp);
            if (l.start != t.hover.start || l.end != t.hover.end || t.hoverRevision != b.revision()) t.hoverSince = now;
            t.hover = l;
            t.hoverPos = mp;
            t.hoverRevision = b.revision();
        }
        if ((t.hover.def || t.hover.api) && io.KeyCtrl) {
            float y = yOf(t.hover.start.line) + lineH - 3;
            dl->AddLine(ImVec2(xOf(t.hover.start.line, t.hover.start.col), y), ImVec2(xOf(t.hover.end.line, t.hover.end.col), y), code::function);
        }
    } else {
        t.hoverPos = lua::Pos{-1, -1};
        t.hover = lua::Lookup();
    }
    dl->PopClipRect();

    // --- Gutter ---
    dl->PushClipRect(area.Min, ImVec2(body.Min.x, area.Max.y), true);
    char num[16];
    for (int l = first; l <= last; ++l) {
        std::snprintf(num, sizeof num, "%d", l + 1);
        float w = mono->CalcTextSizeA(fs, FLT_MAX, 0, num).x;
        dl->AddText(mono, fs, ImVec2(body.Min.x - 12 - w, yOf(l) + textOff), l == cur.line ? color::textDim : code::lineNumber, num);
    }
    if (!diag.ok() && diag.line >= first && diag.line <= last) {
        ImVec2 c(area.Min.x + 11, yOf(diag.line) + lineH * 0.5f);
        dl->AddCircleFilled(c, 4.5f, color::bad);
        if (hovered && ImLengthSqr(mouse - c) < 81)
            infoCard(ImVec2(c.x + 10, c.y + 8), false, "Line " + std::to_string(diag.line + 1), diag.message);
    }
    dl->PopClipRect();

    // --- Scrollbar ---
    {
        const float contentH = (b.lineCount() - 1) * lineH + viewH;
        if (contentH > viewH + 1) {
            ImRect bar(ImVec2(area.Max.x - barW, area.Min.y + 2), ImVec2(area.Max.x - 2, area.Max.y - 2));
            float th = std::max(24.0f, bar.GetHeight() * viewH / contentH);
            float range = (b.lineCount() - 1) * lineH;
            float ty = bar.Min.y + (bar.GetHeight() - th) * (range > 0 ? t.scroll.y / range : 0);
            ImGui::SetCursorScreenPos(bar.Min);
            ImGui::InvisibleButton("##vbar", ImMax(bar.GetSize(), ImVec2(1, 1)));
            if (ImGui::IsItemActive() && bar.GetHeight() > th)
                t.scroll.y = std::clamp(t.scroll.y + io.MouseDelta.y * range / (bar.GetHeight() - th), 0.0f, range);
            bool on = ImGui::IsItemHovered() || ImGui::IsItemActive();
            dl->AddRectFilled(ImVec2(bar.Min.x + 1, ty), ImVec2(bar.Max.x - 1, ty + th), on ? color::borderStrong : color::active, radius::pill);
            // Where the error is, on the bar.
            if (!diag.ok() && b.lineCount() > 1) {
                float ey = bar.Min.y + bar.GetHeight() * diag.line / (float)(b.lineCount() - 1);
                dl->AddRectFilled(ImVec2(bar.Min.x, ey - 1.5f), ImVec2(bar.Max.x, ey + 1.5f), color::bad);
            }
        }
    }

    // --- Find and replace bar ---
    if (t.findOpen) {
        const float fw = 530, rowH = 34, gap = 29;
        const float fh = t.replaceOpen ? gap + rowH : rowH;
        ImRect fr(ImVec2(body.Max.x - fw - space::sm, area.Min.y + space::sm), ImVec2(body.Max.x - space::sm, area.Min.y + space::sm + fh));
        t.findRect = fr;
        dl->AddRectFilled(fr.Min + ImVec2(0, 2), fr.Max + ImVec2(0, 4), rgb(0x000000, 60), radius::card);
        dl->AddRectFilled(fr.Min, fr.Max, color::raised, radius::card);
        dl->AddRect(fr.Min, fr.Max, color::borderStrong, radius::card);
        const float bw = 26, toggleW = 20;
        // The chevron on the left shows or hides the Replace row (Ctrl+H).
        {
            ImRect tr(ImVec2(fr.Min.x + 3, fr.Min.y + 5), ImVec2(fr.Min.x + 3 + toggleW - 2, fr.Max.y - 5));
            Hit h = interact("togglereplace", tr);
            if (h.hovered) dl->AddRectFilled(tr.Min, tr.Max, color::hover, radius::field);
            textCentered(dl, tr, f.medium, type::label, h.hovered ? color::text : color::textDim,
                         t.replaceOpen ? icon::chevronDown : icon::chevronRight);
            tooltip(t.replaceOpen ? "Hide Replace" : "Replace (Ctrl+H)");
            if (h.clicked) {
                t.replaceOpen = !t.replaceOpen;
                if (t.replaceOpen) t.replaceFocus = true;
            }
        }
        const float fieldX = fr.Min.x + 3 + toggleW;
        ImRect field(ImVec2(fieldX, fr.Min.y + 5), ImVec2(fr.Max.x - bw * 7 - 6 - 82, fr.Min.y + rowH - 5));
        // Enter in a field acts and keeps the field (ImGui would let go of it).
        const bool keepActive = io.ConfigInputTextEnterKeepActive;
        io.ConfigInputTextEnterKeepActive = true;
        bool editing = searchField("find", field, "Find", nullptr, &t.query, t.findFocus);
        t.findFocus = false;
        bool replacing = false;
        ImRect rfield;
        if (t.replaceOpen) {
            rfield = ImRect(ImVec2(fieldX, fr.Min.y + gap + 5), ImVec2(field.Max.x, fr.Min.y + gap + rowH - 5));
            replacing = searchField("replace", rfield, "Replace", nullptr, &t.replacement, t.replaceFocus, false);
            t.replaceFocus = false;
        }
        io.ConfigInputTextEnterKeepActive = keepActive;
        const bool inFind = editing || t.findEditing, inReplace = replacing || t.replaceEditing;
        t.findEditing = editing;
        t.replaceEditing = replacing;
        if (editing || replacing) t.focused = false;
        // Tab and Shift+Tab go between the two fields.
        if (t.replaceOpen && inFind && ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyShift) t.replaceFocus = true;
        if (inReplace && ImGui::IsKeyPressed(ImGuiKey_Tab, false) && io.KeyShift) t.findFocus = true;
        if (t.query != t.searched) {
            // Typing a search jumps to the first match from where the cursor was.
            t.searched = t.query;
            if (auto m = b.find(t.query, b.selStart(), true, t.opts)) {
                b.select(m->first, m->second);
                t.reveal = true;
            }
        }
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true);
        bool next = false, prev = false, replaceOne = false, replaceEvery = false;
        if (inFind && enter && !io.KeyCtrl) (io.KeyShift ? prev : next) = true;
        // Enter replaces this match, Ctrl+Alt+Enter all of them (as in VS Code).
        if (inReplace && enter && !io.KeyShift) (io.KeyCtrl && io.KeyAlt ? replaceEvery : replaceOne) = true;
        const int count = (int)found.size();
        int at = 0;
        // Which match the selection is, counting from the top.
        for (int k = 0; k < count; ++k)
            if (found[k].first == b.selStart() && found[k].second == b.selEnd()) {
                at = k + 1;
                break;
            }
        // A pattern that does not compile: the field in red, why under the bar.
        const std::string error = lua::findError(t.query, t.opts);
        if (!error.empty()) {
            dl->AddRect(field.Min, field.Max, color::bad, radius::field, 0, 1.5f);
            ImVec2 es = measure(f.regular, type::caption, error.c_str());
            ImRect er(ImVec2(field.Min.x, fr.Max.y + 2), ImVec2(field.Min.x + es.x + 12, fr.Max.y + 2 + es.y + 8));
            dl->AddRectFilled(er.Min, er.Max, color::raised, radius::field);
            dl->AddRect(er.Min, er.Max, color::bad, radius::field);
            text(dl, er.Min + ImVec2(6, 4), f.regular, type::caption, color::bad, error.c_str());
        }
        char info[32];
        if (!error.empty()) std::snprintf(info, sizeof info, "Bad regex");
        else if (at) std::snprintf(info, sizeof info, "%d of %d", at, count);
        else std::snprintf(info, sizeof info, count == 1 ? "%d match" : "%d matches", count);
        ImVec2 is = measure(f.regular, type::caption, info);
        const float row1 = fr.Min.y + rowH * 0.5f;
        text(dl, ImVec2(field.Max.x + 6, row1 - is.y * 0.5f), f.regular, type::caption,
             (count || t.query.empty()) && error.empty() ? color::textDim : color::bad, info);
        float bx = fr.Max.x - bw * 7 - 6;
        auto small = [&](const char* id, const char* label, bool on, const char* tip, float y0, float w = 0) {
            if (!w) w = bw;
            ImRect br(ImVec2(bx, y0 + 5), ImVec2(bx + w, y0 + rowH - 5));
            bx += w + 1;
            Hit h = interact(id, br);
            if (on || h.hovered) dl->AddRectFilled(br.Min, br.Max, on ? color::accentSoft : color::hover, radius::field);
            textCentered(dl, br, f.medium, type::label, on ? color::text : color::textDim, label);
            tooltip(tip);
            return h.clicked;
        };
        if (small("case", "Aa", t.opts.matchCase, "Match case", fr.Min.y)) t.opts.matchCase = !t.opts.matchCase, t.searched.clear();
        if (small("word", "ab", t.opts.wholeWord, "Whole word", fr.Min.y)) t.opts.wholeWord = !t.opts.wholeWord, t.searched.clear();
        if (small("regex", ".*", t.opts.regex, "Regular expression ($1 in Replace)", fr.Min.y)) t.opts.regex = !t.opts.regex, t.searched.clear();
        if (small("prev", "\xe2\x86\x91", false, "Previous match (Shift+F3)", fr.Min.y)) prev = true;
        if (small("next", "\xe2\x86\x93", false, "Next match (F3)", fr.Min.y)) next = true;
        // In selection: the selection now (or the cursor's line) is the range
        // searched, until this is turned off.
        if (small("insel", "Sel", t.opts.inSelection, "Find in selection", fr.Min.y)) {
            t.opts.inSelection = !t.opts.inSelection;
            if (t.opts.inSelection) {
                lua::Pos a = b.selStart(), e = b.selEnd();
                if (!b.hasSelection()) a = lua::Pos{a.line, 0}, e = lua::Pos{a.line, (int)b.line(a.line).size()};
                b.setFindScope(a, e);
            } else {
                b.clearFindScope();
            }
            t.searched = t.query;
        }
        if (small("close", icon::x, false, "Close (Escape)", fr.Min.y)) t.findOpen = false, t.focused = true;
        if (t.replaceOpen) {
            bx = rfield.Max.x + 6;
            if (small("preserve", "AB", t.opts.preserveCase, "Preserve case", fr.Min.y + gap)) t.opts.preserveCase = !t.opts.preserveCase;
            const float w1 = measure(f.medium, type::label, "Replace").x + 14, w2 = measure(f.medium, type::label, "Replace all").x + 14;
            if (small("replaceone", "Replace", false, "Replace this match and go to the next (Enter)", fr.Min.y + gap, w1)) replaceOne = true;
            if (small("replaceall", "Replace all", false, "Replace every match, one undo step (Ctrl+Alt+Enter)", fr.Min.y + gap, w2))
                replaceEvery = true;
        }
        if (replaceOne && !t.query.empty()) {
            b.replaceNext(t.query, t.replacement, t.opts);
            t.reveal = true;
            t.searched = t.query;
        }
        if (replaceEvery && !t.query.empty()) {
            int n = b.replaceAll(t.query, t.replacement, t.opts);
            notice = n ? "Replaced " + std::to_string(n) + (n == 1 ? " match" : " matches") : "Nothing to replace";
            noticeBad = false;
            t.searched = t.query;
        }
        if ((next || prev) && !t.query.empty())
            if (auto m = b.find(t.query, prev ? b.selStart() : b.selEnd(), next, t.opts)) {
                b.select(m->first, m->second);
                t.reveal = true;
            }
        // Escape empties a field, then closes the bar.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
            ((inFind && t.query.empty()) || (inReplace && t.replacement.empty())))
            t.findOpen = false, t.focused = true;
    } else {
        t.findRect = ImRect();
        t.findEditing = t.replaceEditing = false;
    }

    // --- Popups: completion, signature hint, docs ---
    const ImVec2 caret(xOf(cur.line, cur.col), yOf(cur.line));
    t.compRect = ImRect();
    if (t.compOpen && t.focused) {
        const int n = (int)t.comp.items.size(), rows = std::min(n, 10);
        const float rowH = 24, w = 460;
        t.compSel = std::clamp(t.compSel, 0, n - 1);
        if (t.compSel < t.compTop) t.compTop = t.compSel;
        if (t.compSel >= t.compTop + rows) t.compTop = t.compSel - rows + 1;
        // Inside the panel, as the cards are.
        float px = std::max(r.Min.x + 4, std::min(xOf(t.comp.from.line, t.comp.from.col) - 30, r.Max.x - w - 4));
        float h = rows * rowH + 8;
        bool below = caret.y + lineH + h < std::min(r.Max.y, ImGui::GetIO().DisplaySize.y) - 8;
        ImRect pr(ImVec2(px, below ? caret.y + lineH + 2 : caret.y - h - 2), ImVec2(px + w, below ? caret.y + lineH + 2 + h : caret.y - 2));
        t.compRect = pr;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(pr.Min + ImVec2(0, 3), pr.Max + ImVec2(0, 6), rgb(0x000000, 80), radius::card);
        fg->AddRectFilled(pr.Min, pr.Max, color::raised, radius::card);
        fg->AddRect(pr.Min, pr.Max, color::borderStrong, radius::card);
        if (pr.Contains(mouse) && io.MouseWheel != 0)
            t.compTop = std::clamp(t.compTop - (int)io.MouseWheel * 3, 0, std::max(0, n - rows));
        for (int i = 0; i < rows; ++i) {
            int k = t.compTop + i;
            if (k >= n) break;
            const lua::Completion& c = t.comp.items[k];
            ImRect rr(ImVec2(pr.Min.x + 4, pr.Min.y + 4 + i * rowH), ImVec2(pr.Max.x - 4, pr.Min.y + 4 + (i + 1) * rowH));
            if (rr.Contains(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                t.compSel = k;
                acceptCompletion(t);
                t.focused = true;
                break;
            }
            if (k == t.compSel) fg->AddRectFilled(rr.Min, rr.Max, color::accentSoft, radius::field);
            else if (rr.Contains(mouse)) fg->AddRectFilled(rr.Min, rr.Max, color::hover, radius::field);
            ImU32 bc;
            const char* badge = kindBadge(c.kind, &bc);
            ImRect br(ImVec2(rr.Min.x + 4, rr.Min.y + 4), ImVec2(rr.Min.x + 20, rr.Max.y - 4));
            fg->AddRectFilled(br.Min, br.Max, (bc & 0x00ffffff) | 0x30000000, 3);
            ImVec2 bs = mono->CalcTextSizeA(type::label, FLT_MAX, 0, badge);
            fg->AddText(mono, type::label, br.GetCenter() - bs * 0.5f, bc, badge);
            // The typed part of the label is brighter.
            float lx = rr.Min.x + 28, ly = rr.Min.y + (rowH - fs) * 0.5f;
            size_t typed = c.label.compare(0, t.comp.prefix.size(), t.comp.prefix) == 0 ? t.comp.prefix.size() : 0;
            fg->AddText(mono, fs, ImVec2(lx, ly), color::accent, c.label.c_str(), c.label.c_str() + typed);
            float tx = lx + mono->CalcTextSizeA(fs, FLT_MAX, 0, c.label.c_str(), c.label.c_str() + typed).x;
            fg->AddText(mono, fs, ImVec2(tx, ly), color::text, c.label.c_str() + typed);
            if (!c.detail.empty()) {
                float lw = mono->CalcTextSizeA(fs, FLT_MAX, 0, c.label.c_str()).x;
                float room = rr.Max.x - 8 - (lx + lw + 24);
                std::string d = c.detail;
                ImVec2 ds = f.regular->CalcTextSizeA(type::caption, FLT_MAX, 0, d.c_str());
                while (ds.x > room && d.size() > 4) {
                    d = d.substr(0, d.size() - 4) + "...";
                    ds = f.regular->CalcTextSizeA(type::caption, FLT_MAX, 0, d.c_str());
                }
                if (ds.x <= room)
                    fg->AddText(f.regular, type::caption, ImVec2(rr.Max.x - 8 - ds.x, rr.Min.y + (rowH - type::caption) * 0.5f - 1), color::textFaint,
                                d.c_str());
            }
        }
        // The selected item's docs beside the list.
        if (t.compSel < n) {
            const lua::Completion& c = t.comp.items[t.compSel];
            if (c.api && (*c.api->doc || c.kind != lua::ItemKind::ApiTable))
                infoCard(ImVec2(pr.Max.x + 6, pr.Min.y), false, c.api->signature, c.api->doc, -1, &pr);
        }
    }
    // Inside a call's parentheses: the signature, the current argument lit.
    if (t.focused && !t.peek) {
        if (t.callRevision != b.revision() || t.callPos != cur) {
            t.call = lua::callAt(b.lines(), cur);
            t.callRevision = b.revision();
            t.callPos = cur;
        }
        std::string sig = signatureOf(t.call);
        if (!sig.empty() && (t.call.api || (t.call.def && !t.call.def->signature.empty())))
            infoCard(ImVec2(caret.x - 20, caret.y - 4), true, sig, t.compOpen ? std::string() : docOf(t.call), t.call.arg);
    }
    if (t.peek) {
        lua::Lookup l;
        l.api = t.peek;
        infoCard(ImVec2(xOf(t.peekAt.line, t.peekAt.col), yOf(t.peekAt.line) + lineH + 2), false, signatureOf(l), docOf(l));
    }
    // Resting the mouse on a name shows what it is.
    if (overText && (t.hover.def || t.hover.api) && now - t.hoverSince > 0.45 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !errorCard)
        infoCard(ImVec2(xOf(t.hover.start.line, t.hover.start.col), yOf(t.hover.start.line) + lineH + 2), false, signatureOf(t.hover),
                 docOf(t.hover));

    // --- Status line ---
    dl->AddLine(ImVec2(status.Min.x, status.Min.y), ImVec2(status.Max.x, status.Min.y), color::border);
    {
        const float cy = status.GetCenter().y;
        float x = status.Min.x + space::md;
        if (diag.ok()) {
            dl->AddCircleFilled(ImVec2(x + 4, cy), 4, color::good);
            text(dl, ImVec2(x + 14, cy - type::caption * 0.5f - 1), f.regular, type::caption, color::textDim, "Compiles for the PS1");
        } else {
            std::string msg = "Line " + std::to_string(diag.line + 1) + ": " + diag.message;
            ImRect er(ImVec2(x, status.Min.y + 3), ImVec2(std::min(status.Max.x - 260, x + 22 + measure(f.regular, type::caption, msg.c_str()).x),
                                                         status.Max.y - 3));
            Hit h = interact("goerror", er);
            if (h.hovered) dl->AddRectFilled(er.Min, er.Max, color::hover, radius::field);
            dl->AddCircleFilled(ImVec2(x + 4, cy), 4, color::bad);
            dl->PushClipRect(er.Min, er.Max, true);
            text(dl, ImVec2(x + 14, cy - type::caption * 0.5f - 1), f.regular, type::caption, color::bad, msg.c_str());
            dl->PopClipRect();
            if (h.clicked && diag.line < b.lineCount()) {
                b.moveTo(lua::Pos{diag.line, (int)b.line(diag.line).size()});
                t.reveal = t.revealCenter = true;
                t.focused = true;
            }
        }
        char where[64];
        std::snprintf(where, sizeof where, "Ln %d, Col %d", cur.line + 1, lua::columnOf(b.line(cur.line), cur.col) + 1);
        std::string right = std::string(where) + "     Lua 5.2 (psxlua)";
        ImVec2 rs = measure(f.regular, type::caption, right.c_str());
        text(dl, ImVec2(status.Max.x - space::md - rs.x, cy - rs.y * 0.5f), f.regular, type::caption, color::textFaint, right.c_str());
    }
    ImGui::PopID();
}

}  // namespace ui
