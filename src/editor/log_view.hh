// What the Play and Run on hardware output popups show of a run's lines: a
// filter, a clear point and line wrapping. The lines are the newest ones a
// run kept, `total` the count it printed. No SDL, no ImGui.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace editor {

struct LogView {
    std::string filter;     // shown lines contain this, ASCII case-insensitive; empty shows all
    uint64_t clearedAt = 0; // line number (from 0, since the run started) the first shown line may have
    bool wrap = false;      // long lines wrap instead of scrolling sideways
};

// A new run started: lines count from 0 again, so the clear point goes. The
// filter and wrapping stay.
void restartLog(LogView& v);

// Indices into `lines` (the newest lines.size() of `total` printed) of the
// lines at or after the clear point that contain the filter. A total below
// the clear point means a new run restarted the count: nothing is cleared.
std::vector<size_t> visibleLines(const std::deque<std::string>& lines, uint64_t total, const LogView& v);

// How many lines there are from the clear point on, of `total` printed.
uint64_t linesSinceClear(uint64_t total, const LogView& v);

// Whether `line` contains `needle`, ASCII case-insensitive (an empty needle always).
bool containsNoCase(const std::string& line, const std::string& needle);

// The lines at `which`, each ended by "\n" (empty for none).
std::string joinLines(const std::deque<std::string>& lines, const std::vector<size_t>& which);

}  // namespace editor
