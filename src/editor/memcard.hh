// The scene inspector's Memory card section, the parts with no UI: the save
// filename it previews, its problems with a fix each, and the icon as the
// console shows it (the frames quantised together into one palette, the way
// the export does).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

// psxsplash's save filename: region + product + key, cut at 20 characters.
std::string memCardFilename(const splash::MemCardSettings& mc, const std::string& key);
// Characters of a MemCard.Save key that make it into the filename (0 when the
// region and product already fill it).
int memCardKeyRoom(const splash::MemCardSettings& mc);

// What a problem's button does to the settings.
enum class MemCardFix {
    None,            // no button: shown as a line of text
    RegionBA,        // region = BA
    ProductDefault,  // product = SLUS-00000
    ProductCut,      // first 10 characters
    ProductAscii,    // drops characters that are not printable ASCII
    TitleCut,        // first 32 characters
    TitleAscii,      // drops characters that are not printable ASCII
    TitleScene,      // the scene's name, made fit
    TitleDropMarks,  // drops " ' - \ ~
    IconsFirst3,     // keeps the first 3 frames
    AddIcon,         // opens the file dialog (the caller does it)
};

struct MemCardProblem {
    bool error = false;  // export fails until fixed
    std::string title;   // short, for the card's heading
    std::string body;    // splash::checkMemCard's message
    MemCardFix fix = MemCardFix::None;
    const char* fixLabel = nullptr;
};

// splash::checkMemCard's errors then warnings, each with a title and a fix.
// Empty when the settings are off.
std::vector<MemCardProblem> memCardProblems(const splash::MemCardSettings& mc);

// Applies `fix`; `sceneName` is used by TitleScene. Returns false when it
// changes nothing (AddIcon never does).
bool applyMemCardFix(splash::MemCardSettings& mc, MemCardFix fix, const std::string& sceneName);

// A title made from `name`: printable ASCII without " ' - \ ~, underscores as
// spaces, at most 32 characters; "SAVE" when nothing is left.
std::string memCardTitleFrom(const std::string& name);

// The icon frames as the console draws them.
struct MemCardIcon {
    struct Frame {
        std::string path;
        std::string error;  // empty when the frame is usable: missing, unreadable or not 16x16
        int width = 0, height = 0;
        // 16x16 texels, top row first, as 0xAABBGGRR (ImGui's IM_COL32); alpha
        // 0 or 255. Empty when the frame has an error.
        std::vector<uint32_t> rgba;
    };
    std::vector<Frame> frames;
    // Distinct 15-bit colours over the usable frames' opaque texels, and how
    // many the shared palette holds (16, or 15 when a texel is transparent).
    int colors = 0, paletteColors = 16;
};
MemCardIcon memCardIcon(const std::vector<std::string>& icons, const std::filesystem::path& projectRoot);

}  // namespace editor
