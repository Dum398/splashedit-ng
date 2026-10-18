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

std::vector<DirectionSet> directionSets(const splash::SpriteSheet& s) {
    std::vector<DirectionSet> out;
    const int n = int(s.anims.size());
    for (int i = 0; i < n;) {
        const std::string& name = s.anims[size_t(i)].name;
        int found = 0;
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_n") == 0) {
            const std::string base = name.substr(0, name.size() - 2);
            for (int count : {8, 4}) {
                const std::vector<std::string>& suf = splash::spriteDirectionSuffixes(count);
                bool all = i + count <= n;
                for (int d = 0; all && d < count; d++) all = s.anims[size_t(i + d)].name == base + "_" + suf[size_t(d)];
                if (all) {
                    out.push_back({base, count, i});
                    found = count;
                    break;
                }
            }
        }
        i += found ? found : 1;
    }
    return out;
}

const DirectionSet* directionSetOf(const std::vector<DirectionSet>& sets, int anim) {
    for (const DirectionSet& d : sets)
        if (anim >= d.first && anim < d.first + d.count) return &d;
    return nullptr;
}

std::string facingLuaLine(const DirectionSet& set) {
    return "Sprite.SetFacingFromYaw(sprite, \"" + set.base + "_n\", " + std::to_string(set.count) + ", yaw)";
}

namespace {

// UTF-16 code units in UTF-8 `s`: psxsplash keeps 24 of them of a name.
size_t utf16Units(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = uint8_t(s[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

}  // namespace

DirectionSetSpec defaultDirectionSet(const splash::SpriteSheet& s, int directions, int cols, int rows) {
    DirectionSetSpec d;
    d.directions = directions == 4 ? 4 : 8;
    d.framesPerDirection = std::max(cols, 1);
    int next = 0;
    for (const splash::SpriteAnim& o : s.anims) next = std::max(next, o.firstFrame + std::max(o.frameCount, 1));
    if (cols > 0) next = (next + cols - 1) / cols * cols;  // the start of the next row
    const int cells = cols * rows;
    d.firstCell = cells > 0 && next + d.directions * d.framesPerDirection > cells ? 0 : next;
    return d;
}

int addDirectionSet(Document& doc, int sheet, const DirectionSetSpec& spec, int cells, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return -1;
    };
    if (!validSheet(doc, sheet)) return fail("No sprite sheet is selected.");
    const std::vector<splash::SpriteSheet>& sheets = doc.scene().spriteSheets;
    const std::vector<std::string>& suffixes = splash::spriteDirectionSuffixes(spec.directions);
    if (suffixes.empty()) return fail("A direction set has 4 or 8 directions.");
    const int n = spec.directions;
    if (spec.base.empty()) return fail("Type a base name: the animations are named base_n, base_e and so on.");
    if (utf16Units(spec.base) + 3 > 24)
        return fail("'" + spec.base + "' is too long: psxsplash keeps 24 characters of a name and " + spec.base +
                    "_sw needs " + std::to_string(utf16Units(spec.base) + 3) + ".");
    if (spec.framesPerDirection < 1 || spec.framesPerDirection > 255) return fail("Frames per direction must be 1 to 255.");
    if (spec.frameDuration < 1 || spec.frameDuration > 255) return fail("The frame time must be 1 to 255 vsyncs.");
    if (spec.firstCell < 0) return fail("The first cell must be 0 or more.");
    for (const std::string& suf : suffixes) {
        const std::string name = spec.base + "_" + suf;
        for (const splash::SpriteSheet& s : sheets)
            for (const splash::SpriteAnim& a : s.anims)
                if (a.name == name)
                    return fail("'" + name + "' is already an animation in sheet '" + s.name +
                                "': Sprite.AnimIndex searches every sheet, so pick another base name.");
    }
    if (cells <= 0) return fail("The sheet's cells are unknown: give it a texture that is a whole number of cells first.");
    const int last = spec.firstCell + n * spec.framesPerDirection - 1;
    if (last >= cells)
        return fail(std::to_string(n) + " x " + std::to_string(spec.framesPerDirection) + " frames from cell " +
                    std::to_string(spec.firstCell) + " end on cell " + std::to_string(last) + ", past the sheet's " +
                    std::to_string(cells) + " cells (0.." + std::to_string(cells - 1) + ").");
    size_t total = 0;
    for (const splash::SpriteSheet& s : sheets) total += s.anims.size();
    if (total + size_t(n) > size_t(splash::kMaxSpriteAnims))
        return fail(std::to_string(n) + " more animations would make " + std::to_string(total + size_t(n)) +
                    ", psxsplash loads at most " + std::to_string(splash::kMaxSpriteAnims) + ".");
    int at = -1;
    doc.editSpriteSheets([&](std::vector<splash::SpriteSheet>& all) {
        auto& anims = all[size_t(sheet)].anims;
        at = int(anims.size());
        for (int d = 0; d < n; d++) {
            splash::SpriteAnim a;
            a.name = spec.base + "_" + suffixes[size_t(d)];
            a.firstFrame = spec.firstCell + d * spec.framesPerDirection;
            a.frameCount = spec.framesPerDirection;
            a.frameDuration = spec.frameDuration;
            a.loop = spec.loop;
            anims.push_back(a);
        }
    });
    if (error) error->clear();
    return at;
}

int spriteAnimFrameAt(const splash::SpriteAnim& a, long long vsyncs) {
    if (a.frameCount < 1) return 0;
    const long long dur = std::max(a.frameDuration, 1);
    const long long step = std::max(vsyncs, 0LL) / dur;
    if (a.loop) return int(step % a.frameCount);
    return int(std::min<long long>(step, a.frameCount - 1));
}

}  // namespace editor
