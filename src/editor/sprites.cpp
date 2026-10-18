#include "editor/sprites.hh"

#include <algorithm>
#include <filesystem>

#include "editor/document.hh"
#include "splashpack.hh"

namespace editor {

namespace {

// "name (N)" -> ("name", N); anything else -> (itself, 1).
int splitNumbered(const std::string& s, std::string* base) {
    if (s.size() > 4 && s.back() == ')') {
        size_t open = s.rfind(" (");
        if (open != std::string::npos && open + 2 < s.size() - 1) {
            const std::string digits = s.substr(open + 2, s.size() - open - 3);
            if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }) && digits.size() < 9) {
                *base = s.substr(0, open);
                return std::stoi(digits);
            }
        }
    }
    *base = s;
    return 1;
}

std::string uniqueAmong(const std::vector<std::string>& names, const std::string& name) {
    std::string base;
    splitNumbered(name, &base);
    bool taken = false;
    int next = 1;
    for (const std::string& n : names) {
        if (n == name) taken = true;
        std::string b;
        const int k = splitNumbered(n, &b);
        if (b == base) next = std::max(next, k + 1);
    }
    if (!taken) return name;
    return base + " (" + std::to_string(std::max(next, 2)) + ")";
}

bool validSheet(const Document& doc, int sheet) { return sheet >= 0 && sheet < int(doc.scene().spriteSheets.size()); }

}  // namespace

std::string uniqueSpriteSheetName(const std::vector<splash::SpriteSheet>& sheets, const std::string& base) {
    std::vector<std::string> names;
    for (const splash::SpriteSheet& s : sheets) names.push_back(s.name);
    return uniqueAmong(names, base.empty() ? "sheet" : base);
}

std::string uniqueSpriteAnimName(const std::vector<splash::SpriteSheet>& sheets, const std::string& base) {
    std::vector<std::string> names;
    for (const splash::SpriteSheet& s : sheets)
        for (const splash::SpriteAnim& a : s.anims) names.push_back(a.name);
    return uniqueAmong(names, base.empty() ? "anim" : base);
}

int addSpriteSheet(Document& doc, const std::string& texture) {
    if (int(doc.scene().spriteSheets.size()) >= splash::kMaxSpriteSheets) return -1;
    int at = -1;
    doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& sheets) {
        splash::SpriteSheet s;
        s.texture = texture;
        s.name = uniqueSpriteSheetName(sheets, std::filesystem::path(texture).stem().string());
        splash::SpriteAnim a;
        a.name = uniqueSpriteAnimName(sheets, a.name);
        s.anims.push_back(a);
        sheets.push_back(std::move(s));
        at = int(sheets.size()) - 1;
    });
    return at;
}

bool deleteSpriteSheet(Document& doc, int sheet) {
    if (!validSheet(doc, sheet)) return false;
    return doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& sheets) { sheets.erase(sheets.begin() + sheet); });
}

bool editSpriteSheet(Document& doc, int sheet, const std::function<void(splash::SpriteSheet&)>& fn, const std::string& mergeKey) {
    if (!validSheet(doc, sheet)) return false;
    return doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& sheets) { fn(sheets[size_t(sheet)]); }, mergeKey);
}

int addSpriteAnim(Document& doc, int sheet, int cells) {
    if (!validSheet(doc, sheet)) return -1;
    int at = -1;
    doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& sheets) {
        splash::SpriteSheet& s = sheets[size_t(sheet)];
        splash::SpriteAnim a;
        a.name = uniqueSpriteAnimName(sheets, "anim");
        int next = 0;
        for (const splash::SpriteAnim& o : s.anims) next = std::max(next, o.firstFrame + std::max(o.frameCount, 1));
        a.firstFrame = cells > 0 && next >= cells ? 0 : next;
        s.anims.push_back(a);
        at = int(s.anims.size()) - 1;
    });
    return at;
}

bool deleteSpriteAnim(Document& doc, int sheet, int anim) {
    if (!validSheet(doc, sheet) || anim < 0 || anim >= int(doc.scene().spriteSheets[size_t(sheet)].anims.size())) return false;
    return doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& sheets) {
        auto& anims = sheets[size_t(sheet)].anims;
        anims.erase(anims.begin() + anim);
    });
}

SpriteGrid spriteGrid(const splash::SpriteSheet& s, int width, int height) {
    SpriteGrid g;
    if (s.cellW < 1 || s.cellH < 1 || width < 1 || height < 1) return g;
    g.cols = width / s.cellW;
    g.rows = height / s.cellH;
    g.whole = width % s.cellW == 0 && height % s.cellH == 0;
    return g;
}

int spriteAnimFrameAt(const splash::SpriteAnim& a, long long vsyncs) {
    if (a.frameCount < 1) return 0;
    const long long dur = std::max(a.frameDuration, 1);
    const long long step = std::max(vsyncs, 0LL) / dur;
    if (a.loop) return int(step % a.frameCount);
    return int(std::min<long long>(step, a.frameCount - 1));
}

}  // namespace editor
