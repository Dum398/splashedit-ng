// A colour as the hex text a colour field shows (#RRGGBB), and back, for the
// field's Copy / Paste menu. Kept apart from the widget (no ImGui) so
// editor_tests can check it.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace ui {

// "#RRGGBB", each channel clamped to 0..1 and rounded to 0..255.
inline std::string colourHex(const std::array<float, 3>& c) {
    auto b = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", b(c[0]), b(c[1]), b(c[2]));
    return buf;
}

// A colour from "#RRGGBB", "RRGGBB" or "#RGB" (any case, surrounding
// whitespace allowed); nothing for any other text.
inline std::optional<std::array<float, 3>> parseColourHex(std::string_view s) {
    auto space = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    const bool hash = !s.empty() && s.front() == '#';
    if (hash) s.remove_prefix(1);
    if (!(s.size() == 6 || (hash && s.size() == 3))) return std::nullopt;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::array<float, 3> out{};
    for (int i = 0; i < 3; i++) {
        int v;
        if (s.size() == 3) {
            const int n = nibble(s[size_t(i)]);
            v = n * 17;
            if (n < 0) return std::nullopt;
        } else {
            const int hi = nibble(s[size_t(i) * 2]), lo = nibble(s[size_t(i) * 2 + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            v = hi * 16 + lo;
        }
        out[size_t(i)] = float(v) / 255.0f;
    }
    return out;
}

}  // namespace ui
