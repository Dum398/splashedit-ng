#include "editor/log_view.hh"

#include <algorithm>

namespace editor {

void restartLog(LogView& v) {
    v.clearedAt = 0;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

bool containsNoCase(const std::string& line, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(line.begin(), line.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return lower(a) == lower(b); });
    return it != line.end();
}

static uint64_t clearPoint(uint64_t total, const LogView& v) {
    return total < v.clearedAt ? 0 : v.clearedAt;
}

uint64_t linesSinceClear(uint64_t total, const LogView& v) {
    return total - clearPoint(total, v);
}

std::vector<size_t> visibleLines(const std::deque<std::string>& lines, uint64_t total, const LogView& v) {
    std::vector<size_t> out;
    const uint64_t kept = std::min<uint64_t>(lines.size(), total);
    const uint64_t first = total - kept;  // line number of lines[lines.size() - kept]
    const size_t skip = lines.size() - size_t(kept);
    const uint64_t from = clearPoint(total, v);
    for (size_t i = skip; i < lines.size(); ++i) {
        if (first + (i - skip) < from) continue;
        if (!containsNoCase(lines[i], v.filter)) continue;
        out.push_back(i);
    }
    return out;
}

std::string joinLines(const std::deque<std::string>& lines, const std::vector<size_t>& which) {
    std::string out;
    for (size_t i : which) {
        if (i >= lines.size()) continue;
        out += lines[i];
        out += '\n';
    }
    return out;
}

}  // namespace editor
