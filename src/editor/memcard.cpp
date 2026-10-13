#include "editor/memcard.hh"

#include <algorithm>
#include <system_error>
#include <unordered_set>

#include "splashpack.hh"
#include "texture.hh"

namespace fs = std::filesystem;

namespace editor {

std::string memCardFilename(const splash::MemCardSettings& mc, const std::string& key) {
    const std::string name = mc.region + mc.product + key;
    return name.substr(0, std::min(splash::kMemCardFilenameMax, name.size()));
}

int memCardKeyRoom(const splash::MemCardSettings& mc) {
    return std::max(0, int(splash::kMemCardFilenameMax) - int(mc.region.size() + mc.product.size()));
}

namespace {

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool printable(char c) { return c >= 0x20 && c <= 0x7e; }

std::string keepPrintable(const std::string& s) {
    std::string out;
    for (char c : s)
        if (printable(c)) out += c;
    return out;
}

// Titles, fixes and labels by which field splash::checkMemCard's message is
// about. An unknown message still shows, as a line with no button.
MemCardProblem classify(const std::string& msg, bool error) {
    MemCardProblem p;
    p.error = error;
    p.body = msg;
    auto set = [&](const char* title, MemCardFix fix, const char* label) {
        p.title = title;
        p.fix = fix;
        p.fixLabel = label;
    };
    if (has(msg, "Memory card region"))
        set(error ? "Region must be 2 letters" : "Region the BIOS does not know", MemCardFix::RegionBA, "Use BA (America)");
    else if (has(msg, "product code is empty"))
        set("No product code", MemCardFix::ProductDefault, "Use SLUS-00000");
    else if (has(msg, "Memory card product code") && has(msg, "characters, the splashpack"))
        set("Product code too long", MemCardFix::ProductCut, "Cut to 10 characters");
    else if (has(msg, "Memory card product code"))
        set("Product code is not plain ASCII", MemCardFix::ProductAscii, "Remove those characters");
    else if (has(msg, "save title is empty"))
        set("No save title", MemCardFix::TitleScene, "Use the scene's name");
    else if (has(msg, "Memory card save title") && has(msg, "the BIOS title holds"))
        set("Save title too long", MemCardFix::TitleCut, "Cut to 32 characters");
    else if (has(msg, "Memory card save title") && has(msg, "not plain ASCII"))
        set("Save title is not plain ASCII", MemCardFix::TitleAscii, "Remove those characters");
    else if (has(msg, "Memory card save title") && has(msg, "shows a '?'"))
        set("Save title shows a '?'", MemCardFix::TitleDropMarks, "Leave them out");
    else if (has(msg, "memory card icon frames"))
        set("Too many icon frames", MemCardFix::IconsFirst3, "Keep the first 3");
    else if (has(msg, "have no icon"))
        set("No icon", MemCardFix::AddIcon, "Add an icon");
    else
        p.title = "Memory card";
    return p;
}

}  // namespace

std::vector<MemCardProblem> memCardProblems(const splash::MemCardSettings& mc) {
    std::vector<std::string> errors, warnings;
    splash::checkMemCard(mc, errors, warnings);
    std::vector<MemCardProblem> out;
    for (const std::string& e : errors) out.push_back(classify(e, true));
    for (const std::string& w : warnings) out.push_back(classify(w, false));
    return out;
}

std::string memCardTitleFrom(const std::string& name) {
    std::string t;
    for (char c : name) {
        if (c == '_') c = ' ';
        if (!printable(c) || std::string("\"'-\\~").find(c) != std::string::npos) continue;
        if (c == ' ' && (t.empty() || t.back() == ' ')) continue;
        t += c;
    }
    if (t.size() > splash::kMemCardTitleMax) t.resize(splash::kMemCardTitleMax);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t.empty() ? "SAVE" : t;
}

bool applyMemCardFix(splash::MemCardSettings& mc, MemCardFix fix, const std::string& sceneName) {
    const splash::MemCardSettings before = mc;
    switch (fix) {
        case MemCardFix::None:
        case MemCardFix::AddIcon: return false;
        case MemCardFix::RegionBA: mc.region = "BA"; break;
        case MemCardFix::ProductDefault: mc.product = splash::MemCardSettings().product; break;
        case MemCardFix::ProductCut:
            if (mc.product.size() > splash::kMemCardProductMax) mc.product.resize(splash::kMemCardProductMax);
            break;
        case MemCardFix::ProductAscii: mc.product = keepPrintable(mc.product); break;
        case MemCardFix::TitleCut:
            if (mc.title.size() > splash::kMemCardTitleMax) mc.title.resize(splash::kMemCardTitleMax);
            break;
        case MemCardFix::TitleAscii: mc.title = keepPrintable(mc.title); break;
        case MemCardFix::TitleScene: mc.title = memCardTitleFrom(sceneName); break;
        case MemCardFix::TitleDropMarks:
            std::erase_if(mc.title, [](char c) { return std::string("\"'-\\~").find(c) != std::string::npos; });
            break;
        case MemCardFix::IconsFirst3:
            if (mc.icons.size() > splash::kMemCardIconsMax) mc.icons.resize(splash::kMemCardIconsMax);
            break;
    }
    return mc.region != before.region || mc.product != before.product || mc.title != before.title || mc.icons != before.icons;
}

MemCardIcon memCardIcon(const std::vector<std::string>& icons, const fs::path& projectRoot) {
    MemCardIcon out;
    const int n = splash::kMemCardIconSize;
    std::vector<splash::Image> usable;
    std::vector<size_t> usableAt;
    for (size_t i = 0; i < icons.size() && i < splash::kMemCardIconsMax; i++) {
        MemCardIcon::Frame fr;
        fr.path = icons[i];
        std::error_code ec;
        const fs::path file = projectRoot / fs::path(icons[i]);
        if (icons[i].empty() || !fs::is_regular_file(file, ec)) {
            fr.error = "not found";
        } else {
            try {
                splash::Image img = splash::loadImage(file);
                fr.width = img.width;
                fr.height = img.height;
                if (img.width != n || img.height != n) {
                    fr.error = std::to_string(img.width) + "x" + std::to_string(img.height) + ", not 16x16";
                } else {
                    usable.push_back(std::move(img));
                    usableAt.push_back(i);
                }
            } catch (const std::exception& e) {
                fr.error = e.what();
            }
        }
        out.frames.push_back(std::move(fr));
    }
    if (usable.empty()) return out;

    // As the export's memCardTable: one image, frames stacked top to bottom,
    // converted to 4 bpp with one palette (entry 0 transparent when any texel is).
    splash::Image stack;
    stack.width = n;
    stack.height = n * int(usable.size());
    stack.pixels.resize(size_t(n) * size_t(stack.height));
    bool cutout = false;
    std::unordered_set<uint32_t> colors;
    for (size_t f = 0; f < usable.size(); f++)
        for (int r = 0; r < n; r++)
            for (int x = 0; x < n; x++) {
                const splash::Image::Px& p = usable[f].pixels[size_t(n - 1 - r) * n + x];
                stack.pixels[size_t(stack.height - 1 - (int(f) * n + r)) * n + x] = p;
                if (p.a < 0.5f) {
                    cutout = true;
                    continue;
                }
                auto c5 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 31.0f + 0.5f); };
                colors.insert(c5(p.r) | c5(p.g) << 5 | c5(p.b) << 10);
            }
    out.colors = int(colors.size());
    out.paletteColors = cutout ? 15 : 16;
    const splash::Image shown = splash::decodeTexture(splash::convertTexture(stack, splash::BitDepth::Bpp4, cutout));
    for (size_t f = 0; f < usable.size(); f++) {
        std::vector<uint32_t>& rgba = out.frames[usableAt[f]].rgba;
        rgba.resize(size_t(n) * n);
        for (int r = 0; r < n; r++)
            for (int x = 0; x < n; x++) {
                const splash::Image::Px& p = shown.pixels[size_t(shown.height - 1 - (int(f) * n + r)) * n + x];
                auto u8 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                rgba[size_t(r) * n + x] = p.a > 0 ? (u8(p.r) | u8(p.g) << 8 | u8(p.b) << 16 | 0xff000000u) : 0u;
            }
    }
    return out;
}

}  // namespace editor
