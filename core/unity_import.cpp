#include "unity_import.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <system_error>

#include "audio.hh"
#include "disc.hh"
#include "import.hh"
#include "texture.hh"

namespace splash::unity {

namespace fs = std::filesystem;

// ---- YNode ----

const YNode* YNode::find(std::string_view key) const {
    if (kind != Map) return nullptr;
    for (const auto& [k, v] : map)
        if (k == key) return &v;
    return nullptr;
}

YNode* YNode::find(std::string_view key) {
    if (kind != Map) return nullptr;
    for (auto& [k, v] : map)
        if (k == key) return &v;
    return nullptr;
}

const YNode& YNode::operator[](std::string_view key) const {
    static const YNode null;
    const YNode* n = find(key);
    return n ? *n : null;
}

YNode& YNode::set(std::string_view key, YNode value) {
    if (kind != Map) {
        *this = YNode();
        kind = Map;
    }
    if (YNode* n = find(key)) return *n = std::move(value);
    map.emplace_back(std::string(key), std::move(value));
    return map.back().second;
}

const std::string& YNode::str() const {
    static const std::string empty;
    return kind == Scalar ? scalar : empty;
}

double YNode::num(double fallback) const {
    if (kind != Scalar || scalar.empty()) return fallback;
    const char* s = scalar.c_str();
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    return end == s ? fallback : v;
}

long long YNode::integer(long long fallback) const {
    if (kind != Scalar || scalar.empty()) return fallback;
    const char* s = scalar.c_str();
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s, &end, 10);
    if (end == s) return fallback;
    if (errno == ERANGE) v = (long long)std::strtoull(s, &end, 10);  // a uint64 past int64 (rgba, fileIDs)
    if (*end == '.' || *end == 'e' || *end == 'E') return (long long)std::llround(num(double(fallback)));
    return v;
}

bool YNode::flag(bool fallback) const {
    if (kind != Scalar || scalar.empty()) return fallback;
    if (scalar == "true" || scalar == "True") return true;
    if (scalar == "false" || scalar == "False") return false;
    return integer(fallback ? 1 : 0) != 0;
}

YNode YNode::scalarOf(std::string s) {
    YNode n;
    n.kind = Scalar;
    n.scalar = std::move(s);
    return n;
}

// ---- YAML parser ----

namespace {

std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return trimRight(s);
}

// Folds a quoted scalar's raw line breaks as YAML does: one break is a space,
// each blank line a newline. `raw` holds the text between the quotes.
std::string unquote(std::string_view raw, char q) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\n') {
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
            int blanks = 0;
            size_t j = i + 1;
            for (;;) {
                size_t k = j;
                while (k < raw.size() && (raw[k] == ' ' || raw[k] == '\t')) ++k;
                if (k < raw.size() && raw[k] == '\n') {
                    ++blanks;
                    j = k + 1;
                } else {
                    j = k;
                    break;
                }
            }
            out += blanks ? std::string(size_t(blanks), '\n') : std::string(" ");
            i = j - 1;
            continue;
        }
        if (q == '\'') {
            if (c == '\'' && i + 1 < raw.size() && raw[i + 1] == '\'') ++i;
            out += c;
            continue;
        }
        if (c != '\\' || i + 1 >= raw.size()) {
            out += c;
            continue;
        }
        const char e = raw[++i];
        auto hex = [&](int digits) {
            unsigned v = 0;
            for (int d = 0; d < digits && i + 1 < raw.size(); ++d) {
                const char h = raw[++i];
                v = v * 16 + unsigned(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
            }
            // UTF-8
            if (v < 0x80) out += char(v);
            else if (v < 0x800) {
                out += char(0xC0 | (v >> 6));
                out += char(0x80 | (v & 0x3F));
            } else if (v < 0x10000) {
                out += char(0xE0 | (v >> 12));
                out += char(0x80 | ((v >> 6) & 0x3F));
                out += char(0x80 | (v & 0x3F));
            } else {
                out += char(0xF0 | (v >> 18));
                out += char(0x80 | ((v >> 12) & 0x3F));
                out += char(0x80 | ((v >> 6) & 0x3F));
                out += char(0x80 | (v & 0x3F));
            }
        };
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case 'x': hex(2); break;
            case 'u': hex(4); break;
            case 'U': hex(8); break;
            case '\n':  // an escaped line break joins the lines
                while (i + 1 < raw.size() && (raw[i + 1] == ' ' || raw[i + 1] == '\t')) ++i;
                break;
            default: out += e; break;
        }
    }
    return out;
}

// Where the quoted scalar starting at s[0] closes, npos while it runs on.
size_t closingQuote(std::string_view s) {
    const char q = s[0];
    for (size_t i = 1; i < s.size(); ++i) {
        if (q == '"' && s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == q) {
            if (q == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
                ++i;
                continue;
            }
            return i;
        }
    }
    return std::string_view::npos;
}

struct FlowParser {
    std::string_view s;
    size_t p = 0;
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    std::string quoted() {
        const size_t end = closingQuote(s.substr(p));
        const char q = s[p];
        if (end == std::string_view::npos) {
            std::string v = unquote(s.substr(p + 1), q);
            p = s.size();
            return v;
        }
        std::string v = unquote(s.substr(p + 1, end - 1), q);
        p += end + 1;
        return v;
    }
    // A plain scalar up to `stops` at this nesting level.
    std::string plain(const char* stops) {
        const size_t a = p;
        while (p < s.size() && !std::strchr(stops, s[p])) ++p;
        return std::string(trim(s.substr(a, p - a)));
    }
    YNode value() {
        ws();
        if (p >= s.size()) return {};
        if (s[p] == '{') return mapping();
        if (s[p] == '[') return sequence();
        if (s[p] == '"' || s[p] == '\'') return YNode::scalarOf(quoted());
        std::string v = plain(",}]");
        return v.empty() ? YNode() : YNode::scalarOf(std::move(v));
    }
    YNode mapping() {
        YNode n;
        n.kind = YNode::Map;
        ++p;
        for (;;) {
            ws();
            if (p >= s.size()) break;
            if (s[p] == '}') {
                ++p;
                break;
            }
            if (s[p] == ',') {
                ++p;
                continue;
            }
            std::string key;
            if (s[p] == '"' || s[p] == '\'') key = quoted();
            else {
                const size_t a = p;
                while (p < s.size() && s[p] != ',' && s[p] != '}' &&
                       !(s[p] == ':' && (p + 1 >= s.size() || s[p + 1] == ' ' || s[p + 1] == ',' || s[p + 1] == '}')))
                    ++p;
                key = std::string(trim(s.substr(a, p - a)));
            }
            ws();
            YNode v;
            if (p < s.size() && s[p] == ':') {
                ++p;
                ws();
                if (p < s.size() && s[p] != ',' && s[p] != '}') v = value();
            }
            n.map.emplace_back(std::move(key), std::move(v));
        }
        return n;
    }
    YNode sequence() {
        YNode n;
        n.kind = YNode::Seq;
        ++p;
        for (;;) {
            ws();
            if (p >= s.size()) break;
            if (s[p] == ']') {
                ++p;
                break;
            }
            if (s[p] == ',') {
                ++p;
                continue;
            }
            n.seq.push_back(value());
        }
        return n;
    }
};

// Whether a flow value's brackets (outside quotes) are all closed.
bool flowBalanced(std::string_view s) {
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const size_t e = closingQuote(s.substr(i));
            if (e == std::string_view::npos) return false;
            i += e;
        } else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return depth <= 0;
}

struct Line {
    int indent = 0;
    std::string_view text;  // without the indent and trailing spaces; empty for a blank line
};

// `key: rest` or `key:` on a line: the key and the rest (trimmed), or nullopt.
std::optional<std::pair<std::string, std::string_view>> keyOf(std::string_view t) {
    if (t.empty() || t[0] == '{' || t[0] == '[' || t[0] == '#') return std::nullopt;
    if (t[0] == '"' || t[0] == '\'') {
        const size_t e = closingQuote(t);
        if (e == std::string_view::npos || e + 1 >= t.size() || t[e + 1] != ':') return std::nullopt;
        if (e + 2 < t.size() && t[e + 2] != ' ') return std::nullopt;
        return std::make_pair(unquote(t.substr(1, e - 1), t[0]), trim(t.substr(e + 2)));
    }
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) return std::make_pair(std::string(trim(t.substr(0, i))), trim(t.substr(i + 1)));
    }
    return std::nullopt;
}

bool isSeqItem(std::string_view t) { return t == "-" || (t.size() >= 2 && t[0] == '-' && t[1] == ' '); }

struct BlockParser {
    std::vector<Line> L;
    size_t i = 0;

    void skipBlank() {
        while (i < L.size() && L[i].text.empty()) ++i;
    }
    YNode node() {
        skipBlank();
        if (i >= L.size()) return {};
        return isSeqItem(L[i].text) ? seq(L[i].indent) : map(L[i].indent);
    }
    // A value whose first text, `rest`, was on the line just taken (at `indent`).
    YNode value(std::string_view rest, int indent) {
        if (rest[0] == '{' || rest[0] == '[') {
            std::string acc(rest);
            while (!flowBalanced(acc) && i < L.size() && (L[i].text.empty() || L[i].indent > indent)) {
                acc += ' ';
                acc += L[i++].text;
            }
            FlowParser fp{acc};
            return fp.value();
        }
        if (rest[0] == '"' || rest[0] == '\'') {
            std::string acc(rest);
            while (closingQuote(acc) == std::string_view::npos && i < L.size()) {
                acc += '\n';
                acc += L[i++].text;
            }
            FlowParser fp{acc};
            return YNode::scalarOf(fp.quoted());
        }
        if (rest[0] == '|' || rest[0] == '>') {
            const bool literal = rest[0] == '|';
            std::string acc;
            bool first = true;
            while (i < L.size() && (L[i].text.empty() || L[i].indent > indent)) {
                if (!first) acc += literal || L[i].text.empty() ? '\n' : ' ';
                acc += L[i++].text;
                first = false;
            }
            if (rest.find('-') == std::string_view::npos) acc += '\n';
            return YNode::scalarOf(std::move(acc));
        }
        std::string acc(rest);
        while (i < L.size() && !L[i].text.empty() && L[i].indent > indent) {
            acc += ' ';
            acc += L[i++].text;
        }
        return YNode::scalarOf(std::string(trimRight(acc)));
    }
    YNode seq(int indent) {
        YNode out;
        out.kind = YNode::Seq;
        for (;;) {
            skipBlank();
            if (i >= L.size() || L[i].indent != indent || !isSeqItem(L[i].text)) break;
            const std::string_view whole = L[i].text;
            std::string_view rest = whole.size() > 1 ? whole.substr(2) : std::string_view();
            size_t lead = 0;
            while (lead < rest.size() && rest[lead] == ' ') ++lead;
            rest = trimRight(rest.substr(lead));
            const int restIndent = indent + 2 + int(lead);
            if (rest.empty()) {
                ++i;
                skipBlank();
                if (i < L.size() && L[i].indent > indent) out.seq.push_back(node());
                else out.seq.emplace_back();
                continue;
            }
            if (isSeqItem(rest) || keyOf(rest)) {
                // A mapping (or sequence) that starts on the dash line: read it as if the dash were indent.
                L[i].indent = restIndent;
                L[i].text = rest;
                out.seq.push_back(isSeqItem(rest) ? seq(restIndent) : map(restIndent));
                continue;
            }
            ++i;
            out.seq.push_back(value(rest, indent));
        }
        return out;
    }
    YNode map(int indent) {
        YNode out;
        out.kind = YNode::Map;
        for (;;) {
            skipBlank();
            if (i >= L.size()) break;
            const Line& l = L[i];
            if (l.indent < indent) break;
            if (l.indent > indent) {  // nothing it can belong to: skipped
                ++i;
                continue;
            }
            if (isSeqItem(l.text)) break;
            auto kv = keyOf(l.text);
            ++i;
            if (!kv) continue;
            YNode v;
            if (kv->second.empty()) {
                skipBlank();
                if (i < L.size() && L[i].indent > indent) v = node();
                else if (i < L.size() && L[i].indent == indent && isSeqItem(L[i].text)) v = seq(indent);
            } else {
                v = value(kv->second, indent);
            }
            out.map.emplace_back(std::move(kv->first), std::move(v));
        }
        return out;
    }
};

std::vector<Line> linesOf(std::string_view text) {
    std::vector<Line> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string_view::npos) b = text.size();
        std::string_view l = trimRight(text.substr(a, b - a));
        int ind = 0;
        while (size_t(ind) < l.size() && l[size_t(ind)] == ' ') ++ind;
        out.push_back({ind, l.substr(size_t(ind))});
        if (b == text.size()) break;
        a = b + 1;
    }
    return out;
}

}  // namespace

YNode parseFlow(std::string_view text) {
    FlowParser fp{text};
    return fp.value();
}

std::vector<YDoc> parseYaml(std::string_view text) {
    std::vector<YDoc> docs;
    std::vector<Line> lines = linesOf(text);
    // Split at "--- " headers.
    std::vector<std::pair<YDoc, std::vector<Line>>> parts;
    bool any = false;
    for (const Line& l : lines) {
        if (l.indent == 0 && (l.text.starts_with("--- ") || l.text == "---")) {
            any = true;
            YDoc d;
            std::string_view h = l.text.substr(3);
            if (size_t u = h.find("!u!"); u != std::string_view::npos) d.classId = std::atoi(std::string(h.substr(u + 3)).c_str());
            if (size_t a = h.find('&'); a != std::string_view::npos) {
                size_t e = a + 1;
                while (e < h.size() && h[e] != ' ') ++e;
                d.fileId = std::strtoll(std::string(h.substr(a + 1, e - a - 1)).c_str(), nullptr, 10);
            }
            d.stripped = h.find(" stripped") != std::string_view::npos;
            parts.emplace_back(std::move(d), std::vector<Line>{});
            continue;
        }
        if (l.indent == 0 && l.text.starts_with("%")) continue;
        if (!any) {
            if (parts.empty()) parts.emplace_back(YDoc{}, std::vector<Line>{});
        }
        parts.back().second.push_back(l);
    }
    for (auto& [d, ls] : parts) {
        BlockParser bp{std::move(ls)};
        YNode top = bp.node();
        if (any && top.kind == YNode::Map && !top.map.empty()) {
            d.type = top.map.front().first;
            d.body = std::move(top.map.front().second);
        } else {
            d.body = std::move(top);
        }
        docs.push_back(std::move(d));
    }
    return docs;
}

Ref refOf(const YNode& n) {
    Ref r;
    if (n.kind != YNode::Map) return r;
    r.fileId = n["fileID"].integer(0);
    r.guid = n["guid"].str();
    r.type = int(n["type"].integer(0));
    return r;
}

std::array<uint8_t, 4> unpackRgba(uint32_t v) {
    return {uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF), uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF)};
}

std::array<float, 4> colorOf(const YNode& n, std::array<float, 4> fallback) {
    if (n.kind != YNode::Map) return fallback;
    if (const YNode* p = n.find("rgba")) {
        const auto c = unpackRgba(uint32_t(p->integer(0)));
        return {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f};
    }
    return {float(n["r"].num(fallback[0])), float(n["g"].num(fallback[1])), float(n["b"].num(fallback[2])),
            float(n["a"].num(fallback[3]))};
}

std::vector<int32_t> hexInts(std::string_view hex) {
    std::vector<int32_t> out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 8 <= hex.size(); i += 8) {
        uint32_t v = 0;
        bool ok = true;
        for (int b = 0; b < 4; ++b) {
            const int hi = nib(hex[i + size_t(b) * 2]), lo = nib(hex[i + size_t(b) * 2 + 1]);
            if (hi < 0 || lo < 0) ok = false;
            v |= uint32_t((hi << 4) | lo) << (8 * b);
        }
        if (!ok) break;
        out.push_back(int32_t(v));
    }
    return out;
}

// ---- Project index ----

namespace {

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string generic(const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& path) { return lower(fs::path(path).extension().string()); }

}  // namespace

std::string GuidIndex::path(const std::string& guid) const {
    auto it = paths.find(guid);
    return it == paths.end() ? std::string() : it->second;
}

fs::path GuidIndex::file(const std::string& guid) const {
    const std::string p = path(guid);
    return p.empty() ? fs::path() : root / fromUtf8(p);
}

GuidIndex indexGuids(const fs::path& unityProject) {
    GuidIndex idx;
    idx.root = unityProject;
    for (const char* top : {"Assets", "Packages"}) {
        std::error_code ec;
        const fs::path dir = unityProject / top;
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            if (p.extension() != ".meta" || !it->is_regular_file(ec)) continue;
            std::ifstream in(p);
            std::string line;
            for (int n = 0; n < 8 && std::getline(in, line); ++n) {
                if (line.rfind("guid: ", 0) == 0) {
                    std::string g(trim(std::string_view(line).substr(6)));
                    fs::path asset = p;
                    asset.replace_extension();
                    idx.paths.emplace(std::move(g), generic(asset.lexically_relative(unityProject)));
                    break;
                }
            }
        }
    }
    return idx;
}

const std::map<std::string, std::string>& knownScriptGuids() {
    // SplashEdit's Runtime/*.cs.meta (the Unity package), guid -> class.
    static const std::map<std::string, std::string> table = {
        {"9e8622b9ef70dec4d8ccb85de1886656", "PSXAgent"},
        {"dc50b0e64e77dd74e9f85acdcf47e2b0", "PSXAnimationClip"},
        {"0da2803235be654438e86fe9d9a954d4", "PSXAudioClip"},
        {"264e92578fac5014aa24c1e38e116b3b", "PSXAudioEvent"},
        {"dc481397cd94e03409e462478df09d58", "PSXCanvas"},
        {"99c0b28de0bbbf7449afc28106b605dc", "PSXCutsceneClip"},
        {"b6e1524fb8b4b754e965d03e634658e6", "PSXData"},
        {"b5bc0d8f6252841439e81a8406c37859", "PSXFontAsset"},
        {"9b542f4ca31fa6548b8914e96dd0fae2", "PSXInteractable"},
        {"e7ee1563eea737e46af0c78394ef931d", "PSXNavigationSettings"},
        {"a4c1d3e8f7b24a6e9d0f3c5e1a8b7d42", "PSXNavWalkoffZone"},
        {"a192e0a30d827ba40be5c99d32a83a12", "PSXObjectExporter"},
        {"3cc71b54d0db2604087cd4ae7781dc98", "PSXPlayer"},
        {"b4d8e9f3a52c6b05c937fa20e48d1b6f", "PSXPortalLink"},
        {"a3c7d8e2f41b5a94b826e91f3d7c0a5e", "PSXRoom"},
        {"3efc1583a56e9024f8d08551ddfbea56", "PSXSceneExporter"},
        {"c8d5f3a16b2e4790ae3c71d9e4f2b6a8", "PSXSkinnedMeshExporter"},
        {"a3b7e2f41c9d4a8e95f6d0c3b2e1a7f4", "PSXSkinnedObjectExporter"},
        {"d2af3656393d8a64d9e21a246e89b9f5", "PSXSprite"},
        {"bb5e9ab74714f094a9e7b869b59de0dc", "PSXSpriteExporter"},
        {"59d9300751bf4164d845973ba2459642", "PSXSpriteSheet"},
        {"8521dc98f544aad489f0b7fc7ba8e3b5", "PSXTilemap"},
        {"0801f7dcc93e8a94384cc9bbe28a9042", "PSXTilemapExporter"},
        {"1079404698ca1494d8e7ceb3a7054aa2", "PSXTilemapRenderer"},
        {"72b9d8d2e8eafba46a30ba345beb9692", "PSXTriggerBox"},
        {"6d0070761745bb147bb74f85ca7960cd", "PSXUIBox"},
        {"00606032435f7154cb260ebf56f477a9", "PSXUIImage"},
        {"5bedac32cdb6b554394fc5be5c6f6fbe", "PSXUILine"},
        {"a3c1e93bdff6a714ca5b47989b5b2401", "PSXUIProgressBar"},
        {"29eca857313c556a0adc1c0d4e4556e8", "PSXUISprite"},
        {"ca4c021b4490c8f4fb9de4e14055b9e3", "PSXUIText"},
        {"80b463d892ec456d9310c0bfc0d047fa", "PSXWorldStreaming"},
        {"00e64fcbfc4e23e4dbe284131fa4d89b", "LuaFile"},
    };
    return table;
}

std::string monoClass(const YNode& mono, const GuidIndex& index) {
    const std::string& id = mono["m_EditorClassIdentifier"].str();
    if (!id.empty()) {
        std::string_view v = id;
        if (size_t c = v.rfind("::"); c != std::string_view::npos) v = v.substr(c + 2);
        if (size_t d = v.rfind('.'); d != std::string_view::npos) v = v.substr(d + 1);
        if (!v.empty()) return std::string(v);
    }
    const Ref s = refOf(mono["m_Script"]);
    if (s.guid.empty()) return {};
    const auto& known = knownScriptGuids();
    if (auto it = known.find(s.guid); it != known.end()) return it->second;
    const std::string p = index.path(s.guid);
    if (extOf(p) == ".cs") return fs::path(fromUtf8(p)).stem().string();
    return {};
}

// ---- Built-in meshes ----

namespace {

// Adds a triangle facing `out` (Unity: clockwise seen from the front, so
// cross(b - a, c - a) points out).
void addTri(Mesh& m, int a, int b, int c, Vec3 out) {
    const Vec3 pa = m.positions[size_t(a)], pb = m.positions[size_t(b)], pc = m.positions[size_t(c)];
    const Vec3 n = cross(pb - pa, pc - pa);
    if (sqrMagnitude(n) < 1e-12f) return;  // degenerate (a pole)
    if (dot(n, out) < 0) std::swap(b, c);
    auto& s = m.submeshes[0];
    s.push_back(a);
    s.push_back(b);
    s.push_back(c);
}

Mesh unityCube() {
    Mesh m;
    const float h = 0.5f;
    m.positions = {{h, -h, h},  {-h, -h, h}, {h, h, h},   {-h, h, h},   {h, h, -h},  {-h, h, -h}, {h, -h, -h}, {-h, -h, -h},
                   {h, h, h},   {-h, h, h},  {h, h, -h},  {-h, h, -h},  {h, -h, -h}, {h, -h, h},  {-h, -h, h}, {-h, -h, -h},
                   {-h, -h, h}, {-h, h, h},  {-h, h, -h}, {-h, -h, -h}, {h, -h, -h}, {h, h, -h},  {h, h, h},   {h, -h, h}};
    m.normals = {{0, 0, 1},  {0, 0, 1},  {0, 0, 1},  {0, 0, 1},  {0, 1, 0},  {0, 1, 0},  {0, 0, -1}, {0, 0, -1},
                 {0, 1, 0},  {0, 1, 0},  {0, 0, -1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},
                 {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {1, 0, 0},  {1, 0, 0},  {1, 0, 0},  {1, 0, 0}};
    m.uv = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 1}, {1, 1}, {0, 1}, {1, 1}, {0, 0}, {1, 0}, {0, 0}, {1, 0},
            {0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 0}};
    m.submeshes = {{0, 2, 3, 0, 3, 1, 8, 4, 5, 8, 5, 9, 10, 6, 7, 10, 7, 11, 12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23}};
    return m;
}

Mesh unityQuad() {
    Mesh m;
    m.positions = {{-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {-0.5f, 0.5f, 0}, {0.5f, 0.5f, 0}};
    m.normals = {{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}};
    m.uv = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    m.submeshes = {{0, 3, 1, 3, 0, 2}};
    return m;
}

// 10 x 10 units in XZ, 11 x 11 vertices, facing +Y. Vertex 0 is (5, 0, 5)
// with UV (0, 0), as in Unity's plane.
Mesh unityPlane() {
    Mesh m;
    m.submeshes.resize(1);
    for (int j = 0; j <= 10; ++j)
        for (int i = 0; i <= 10; ++i) {
            m.positions.push_back({5.f - float(i), 0, 5.f - float(j)});
            m.normals.push_back({0, 1, 0});
            m.uv.push_back({float(i) / 10.f, float(j) / 10.f});
        }
    for (int j = 0; j < 10; ++j)
        for (int i = 0; i < 10; ++i) {
            const int a = j * 11 + i, b = a + 1, c = a + 11, d = c + 1;
            addTri(m, a, b, d, {0, 1, 0});
            addTri(m, a, d, c, {0, 1, 0});
        }
    return m;
}

// Rings of a surface of revolution about Y: each ring a (y, radius, normal y,
// normal radial, v) row, joined to the next with quads facing out.
struct Ring {
    float y, r, ny, nr, v;
};
void addRevolution(Mesh& m, const std::vector<Ring>& rings, int segs) {
    const int base = int(m.positions.size());
    const float twoPi = 6.28318530718f;
    for (const Ring& rg : rings)
        for (int s = 0; s <= segs; ++s) {
            const float a = twoPi * float(s) / float(segs);
            const float cx = std::cos(a), sz = std::sin(a);
            m.positions.push_back({rg.r * cx, rg.y, rg.r * sz});
            Vec3 n{rg.nr * cx, rg.ny, rg.nr * sz};
            m.normals.push_back(normalized(n));
            m.uv.push_back({1.f - float(s) / float(segs), rg.v});
        }
    for (size_t k = 0; k + 1 < rings.size(); ++k)
        for (int s = 0; s < segs; ++s) {
            const int a = base + int(k) * (segs + 1) + s, b = a + 1, c = a + segs + 1, d = c + 1;
            const Vec3 out = normalized(m.normals[size_t(a)] + m.normals[size_t(b)] + m.normals[size_t(c)] + m.normals[size_t(d)]);
            addTri(m, a, b, d, out);
            addTri(m, a, d, c, out);
        }
}

void addCap(Mesh& m, float y, float r, int segs, bool top) {
    const int center = int(m.positions.size());
    const Vec3 n{0, top ? 1.f : -1.f, 0};
    m.positions.push_back({0, y, 0});
    m.normals.push_back(n);
    m.uv.push_back({0.5f, 0.5f});
    const float twoPi = 6.28318530718f;
    for (int s = 0; s <= segs; ++s) {
        const float a = twoPi * float(s) / float(segs);
        m.positions.push_back({r * std::cos(a), y, r * std::sin(a)});
        m.normals.push_back(n);
        m.uv.push_back({0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)});
    }
    for (int s = 0; s < segs; ++s) addTri(m, center, center + 1 + s, center + 2 + s, n);
}

Mesh unitySphere() {
    Mesh m;
    m.submeshes.resize(1);
    const int rings = 10, segs = 16;
    std::vector<Ring> rs;
    for (int k = 0; k <= rings; ++k) {
        const float lat = -1.57079632679f + 3.14159265359f * float(k) / float(rings);
        rs.push_back({0.5f * std::sin(lat), 0.5f * std::cos(lat), std::sin(lat), std::cos(lat), float(k) / float(rings)});
    }
    addRevolution(m, rs, segs);
    return m;
}

Mesh unityCylinder() {
    Mesh m;
    m.submeshes.resize(1);
    const int segs = 16;
    addRevolution(m, {{-1, 0.5f, 0, 1, 0}, {1, 0.5f, 0, 1, 1}}, segs);
    addCap(m, 1, 0.5f, segs, true);
    addCap(m, -1, 0.5f, segs, false);
    return m;
}

Mesh unityCapsule() {
    Mesh m;
    m.submeshes.resize(1);
    const int half = 5, segs = 16;
    std::vector<Ring> rs;
    for (int k = 0; k <= half; ++k) {  // bottom hemisphere, centred at y = -0.5
        const float lat = -1.57079632679f + 1.57079632679f * float(k) / float(half);
        const float y = -0.5f + 0.5f * std::sin(lat);
        rs.push_back({y, 0.5f * std::cos(lat), std::sin(lat), std::cos(lat), (y + 1.f) * 0.5f});
    }
    for (int k = 0; k <= half; ++k) {  // top hemisphere, centred at y = 0.5
        const float lat = 1.57079632679f * float(k) / float(half);
        const float y = 0.5f + 0.5f * std::sin(lat);
        rs.push_back({y, 0.5f * std::cos(lat), std::sin(lat), std::cos(lat), (y + 1.f) * 0.5f});
    }
    addRevolution(m, rs, segs);
    return m;
}

}  // namespace

std::optional<Primitive> builtinMesh(long long fileId) {
    switch (fileId) {
        case 10202: return Primitive{"cube", true, unityCube()};
        case 10209: return Primitive{"plane", true, unityPlane()};
        case 10210: return Primitive{"quad", true, unityQuad()};
        case 10207: return Primitive{"sphere", false, unitySphere()};
        case 10206: return Primitive{"cylinder", false, unityCylinder()};
        case 10208: return Primitive{"capsule", false, unityCapsule()};
        default: return std::nullopt;
    }
}

// ---- Scenes ----

namespace {

bool holdsSceneExporter(const std::string& text, const GuidIndex& index) {
    if (text.find("RuntimeCode.PSXSceneExporter") != std::string::npos) return true;
    if (text.find("3efc1583a56e9024f8d08551ddfbea56") != std::string::npos) return true;
    for (const auto& [g, p] : index.paths)
        if (p.ends_with("/PSXSceneExporter.cs") && text.find(g) != std::string::npos) return true;
    return false;
}

}  // namespace

std::vector<SceneEntry> listScenes(const fs::path& unityProject, const GuidIndex& index) {
    std::vector<SceneEntry> all;
    std::error_code ec;
    const fs::path assets = unityProject / "Assets";
    if (fs::is_directory(assets, ec))
        for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() != ".unity" || !it->is_regular_file(ec)) continue;
            SceneEntry e;
            e.name = it->path().stem().string();
            e.path = generic(it->path().lexically_relative(unityProject));
            e.hasExporter = holdsSceneExporter(readText(it->path()), index);
            all.push_back(std::move(e));
        }
    std::sort(all.begin(), all.end(), [](const SceneEntry& a, const SceneEntry& b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    // EditorBuildSettings' scenes first, in its order.
    std::vector<std::string> order;
    for (const YDoc& d : parseYaml(readText(unityProject / "ProjectSettings" / "EditorBuildSettings.asset")))
        for (const YNode& s : d.body["m_Scenes"].seq) order.push_back(s["path"].str());
    std::vector<SceneEntry> out;
    for (const std::string& p : order)
        for (SceneEntry& e : all)
            if (e.path == p && !e.inBuildSettings) {
                e.inBuildSettings = true;
                out.push_back(e);
            }
    for (const SceneEntry& e : all)
        if (!e.inBuildSettings) out.push_back(e);
    return out;
}

std::vector<SceneEntry> listScenes(const fs::path& unityProject) { return listScenes(unityProject, indexGuids(unityProject)); }

std::vector<SceneEntry> defaultScenes(const std::vector<SceneEntry>& all) {
    std::vector<SceneEntry> out;
    for (const SceneEntry& e : all)
        if (e.hasExporter) out.push_back(e);
    return out;
}

// ---- Report ----

std::string ReportItem::line() const {
    static const char* const tags[] = {"[later]", "[left out]", "[changed]", "[missing]"};
    std::string where = scene.empty() ? "project" : scene;
    if (!object.empty()) where += " / " + object;
    return std::string(tags[kind]) + " " + where + ": " + what;
}

int ImportReport::skipped() const {
    int n = 0;
    for (const ReportItem& it : items) n += it.kind != ReportItem::Changed;
    return n;
}

std::string ImportReport::summary() const {
    return "imported scenes=" + std::to_string(scenes.size()) + " objects=" + std::to_string(objects) + " meshes=" +
           std::to_string(meshes) + " scripts=" + std::to_string(scripts) + " textures=" + std::to_string(textures) +
           " skipped=" + std::to_string(skipped());
}

std::string ImportReport::text() const {
    std::string t = "Unity import report\n\n" + summary() + "\n";
    if (!error.empty()) t += "error: " + error + "\n";
    t += "\n[later]     a SplashEdit feature this import does not bring over yet\n"
         "[left out]  no psxsplash equivalent\n"
         "[changed]   brought over, but not exactly\n"
         "[missing]   a file the Unity project refers to is not there\n\n";
    for (const ReportItem& it : items) t += it.line() + "\n";
    return t;
}

// ---- UI ----

namespace {

struct V2 {
    float x, y;
};
V2 v2Of(const YNode& n, float x, float y) { return {float(n["x"].num(x)), float(n["y"].num(y))}; }
// Mathf.RoundToInt: to the nearest, halves to even.
int roundEven(float f) { return int(std::nearbyint(double(f))); }
// Mathf.Approximately.
bool approx(float a, float b) {
    return std::fabs(b - a) < std::max(1e-6f * std::max(std::fabs(a), std::fabs(b)), 1.401298e-45f * 8);
}

}  // namespace

void bakeLayout(const YNode& rt, float scaleX, float scaleY, UIElement& e) {
    // Unity's RectTransform defaults.
    const V2 aMin = v2Of(rt["m_AnchorMin"], 0.5f, 0.5f), aMax = v2Of(rt["m_AnchorMax"], 0.5f, 0.5f);
    const V2 pos = v2Of(rt["m_AnchoredPosition"], 0, 0), size = v2Of(rt["m_SizeDelta"], 100, 100);
    const V2 pivot = v2Of(rt["m_Pivot"], 0.5f, 0.5f);
    // Top-down anchors: the byte the exporter writes is round(a * 255) of these.
    e.anchorMin = {aMin.x, 1.f - aMax.y};
    e.anchorMax = {aMax.x, 1.f - aMin.y};
    if (approx(aMin.x, aMax.x) && approx(aMin.y, aMax.y)) {
        // One anchor point: anchoredPosition is the pivot's offset from it.
        float px = pos.x * scaleX, py = -pos.y * scaleY;
        const float pw = size.x * scaleX, ph = size.y * scaleY;  // rect.size = sizeDelta here
        px -= pivot.x * pw;
        py -= (1.f - pivot.y) * ph;
        e.rect = {roundEven(px), roundEven(py), std::max(1, roundEven(pw)), std::max(1, roundEven(ph))};
    } else {
        // Stretched: RectTransform.offsetMin/offsetMax, then the insets.
        const V2 offMin{pos.x - size.x * pivot.x, pos.y - size.y * pivot.y};
        const V2 offMax{pos.x + size.x * (1.f - pivot.x), pos.y + size.y * (1.f - pivot.y)};
        const float left = offMin.x * scaleX, right = offMax.x * scaleX;
        const float top = -offMax.y * scaleY, bottom = -offMin.y * scaleY;
        e.rect = {roundEven(left), roundEven(top), roundEven(right - left), roundEven(bottom - top)};
    }
}

// ---- Model internal IDs ----

uint64_t xxh64(std::string_view s, uint64_t seed) {
    constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                       P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto le = [&](size_t at, int bytes) {
        uint64_t v = 0;
        for (int b = 0; b < bytes; ++b) v |= uint64_t(uint8_t(s[at + size_t(b)])) << (8 * b);
        return v;
    };
    auto round = [&](uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; };
    const size_t n = s.size();
    size_t p = 0;
    uint64_t h;
    if (n >= 32) {
        uint64_t v[4] = {seed + P1 + P2, seed + P2, seed, seed - P1};
        for (; p + 32 <= n; p += 32)
            for (int k = 0; k < 4; ++k) v[k] = round(v[k], le(p + size_t(k) * 8, 8));
        h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (uint64_t x : v) h = (h ^ round(0, x)) * P1 + P4;
    } else {
        h = seed + P5;
    }
    h += n;
    for (; p + 8 <= n; p += 8) h = rotl(h ^ round(0, le(p, 8)), 27) * P1 + P4;
    if (p + 4 <= n) {
        h = rotl(h ^ (le(p, 4) * P1), 23) * P2 + P3;
        p += 4;
    }
    for (; p < n; ++p) h = rotl(h ^ (uint64_t(uint8_t(s[p])) * P5), 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

long long modelObjectId(std::string_view type, std::string_view path, int n) {
    const std::string key = "Type:" + std::string(type) + "->" + std::string(path) + std::to_string(n);
    return (long long)xxh64(key);
}

// ---- The importer ----

namespace {

// Unity component class IDs this import names in the report.
const char* builtinClassName(int id) {
    switch (id) {
        case 20: return "Camera";
        case 23: return "MeshRenderer";
        case 33: return "MeshFilter";
        case 54: return "Rigidbody";
        case 64: return "MeshCollider";
        case 65: return "BoxCollider";
        case 81: return "AudioListener";
        case 82: return "AudioSource";
        case 95: return "Animator";
        case 108: return "Light";
        case 111: return "Animation";
        case 114: return "MonoBehaviour";
        case 135: return "SphereCollider";
        case 136: return "CapsuleCollider";
        case 137: return "SkinnedMeshRenderer";
        case 143: return "CharacterController";
        case 198: return "ParticleSystem";
        case 199: return "ParticleSystemRenderer";
        case 212: return "SpriteRenderer";
        case 222: return "CanvasRenderer";
        case 223: return "Canvas";
        case 225: return "CanvasGroup";
        case 1660057539: return "SceneRoots";
        default: return nullptr;
    }
}

bool isUiClass(const std::string& c) {
    return c == "PSXUIText" || c == "PSXUIImage" || c == "PSXUIBox" || c == "PSXUIProgressBar" || c == "PSXUILine" ||
           c == "PSXUISprite";
}

// What one SplashEdit class becomes in a report line.
std::string laterWhat(const std::string& c) { return c + " not imported yet"; }

std::string safeStem(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (std::isalnum(c) || c == '_' || c == '-' || c >= 0x80) ? char(c) : '_';
    return out.empty() ? std::string("unnamed") : out;
}

struct UComp {
    int classId = 0;
    std::string cls;  // MonoBehaviour class, or the built-in name
    const YNode* body = nullptr;
};

struct UObj {
    std::string name;
    bool active = true;
    Transform t;
    std::vector<UComp> comps;
    int parent = -1;
    std::vector<int> children;
    long long order = 0;  // appearance in its file, for root order
    // An object of a model placed in the scene: the mesh and materials the
    // model's own MeshFilter/MeshRenderer would give it.
    std::string modelMesh;
    std::vector<Material> modelMats;
    bool modelSkinned = false;  // that mesh is the model's SkinnedMeshRenderer's
    std::string modelGuid;      // the placed model, for its clips
    std::map<int, Ref> matOverrides;  // m_Materials.Array.data[i] overrides
    // The RectTransform (class 224) body: m_AnchorMin/Max, m_AnchoredPosition,
    // m_SizeDelta, m_Pivot. nullptr for a plain Transform.
    const YNode* rect = nullptr;
};

struct ModelAsset {
    ImportedModel model;
    std::string error;
    // internal fileID -> (class, name) from the model's .meta
    std::map<long long, std::pair<int, std::string>> ids;
    bool preserveHierarchy = false;  // the .meta keeps a lone top-level node below the root
    // The .meta's clipAnimations (authored clip settings), in order; empty
    // when Unity makes one clip per take.
    struct Clip {
        long long id = 0;  // internalID, 0 when the .meta has none
        std::string name, take;
        bool loop = false;
        long long firstFrame = 0;
    };
    std::vector<Clip> clipSettings;
};

// The internal IDs of a model's .meta: internalIDToNameTable (Unity 2019.3+)
// or fileIDToRecycleName (older; class = fileID / 100000).
std::map<long long, std::pair<int, std::string>> metaIds(const fs::path& meta) {
    std::map<long long, std::pair<int, std::string>> out;
    for (const YDoc& d : parseYaml(readText(meta))) {
        std::function<void(const YNode&)> walk = [&](const YNode& n) {
            if (n.kind == YNode::Map) {
                for (const auto& [k, v] : n.map) {
                    if (k == "internalIDToNameTable")
                        for (const YNode& e : v.seq) {
                            const YNode& first = e["first"];
                            if (first.kind == YNode::Map && !first.map.empty())
                                out[first.map.front().second.integer()] = {std::atoi(first.map.front().first.c_str()), e["second"].str()};
                        }
                    else if (k == "fileIDToRecycleName" && v.kind == YNode::Map)
                        for (const auto& [id, name] : v.map) {
                            const long long f = std::atoll(id.c_str());
                            out[f] = {int(f / 100000), name.str()};
                        }
                    else walk(v);
                }
            }
        };
        walk(d.body);
    }
    return out;
}

// Sets `path` (Unity's propertyPath: "m_LocalPosition.x",
// "m_Materials.Array.data[0]", "flatVertexColor.rgba") in `body`.
void applyProperty(YNode& body, const std::string& path, const YNode& value) {
    std::vector<std::string> parts;
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find('.', a);
        if (b == std::string::npos) b = path.size();
        parts.push_back(path.substr(a, b - a));
        a = b + 1;
    }
    YNode* cur = &body;
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p == "Array" && i + 1 < parts.size()) {
            const std::string& next = parts[i + 1];
            if (cur->kind != YNode::Seq) {
                *cur = YNode();
                cur->kind = YNode::Seq;
            }
            if (next == "size") {
                cur->seq.resize(size_t(std::max(0LL, value.integer(0))));
                return;
            }
            if (next.starts_with("data[")) {
                const size_t idx = size_t(std::atoll(next.c_str() + 5));
                if (idx > 4096) return;
                if (cur->seq.size() <= idx) cur->seq.resize(idx + 1);
                cur = &cur->seq[idx];
                ++i;
                continue;
            }
            return;
        }
        YNode* n = cur->find(p);
        if (!n) n = &cur->set(p, YNode());
        cur = n;
    }
    *cur = value;
}

class Importer {
  public:
    Importer(const fs::path& unity, const GuidIndex& index, const fs::path& dest, ImportReport& report)
        : m_unity(unity), m_index(index), m_dest(dest), m_report(report) {}

    Scene convert(const fs::path& sceneFile, const std::string& sceneName);
    // PSXData: memory card settings for every scene, VRAM areas for the project.
    void readPsxData();
    const MemCardSettings& memCard() const { return m_memCard; }
    const std::vector<VramSettings::Area>& vramAreas() const { return m_areas; }
    bool hasPsxData() const { return m_psxData; }
    // A CD track's file copied into music/; its project path.
    std::string copyMusic(const fs::path& file) { return copyAsset(file, "music", nullptr); }
    // Project-level notes once every scene is in: CD music played from Lua
    // when no tracks were given.
    void finish(bool cdTracksGiven);

  private:
    struct Unit {
        std::vector<int> roots;
        std::map<long long, int> byFid;  // GameObject and Transform fileIDs -> object
    };

    Unit buildUnit(std::vector<YDoc>& docs, int depth);
    Unit instantiate(const YDoc& instanceDoc, int depth);
    Unit instantiateModel(const Ref& source, const YNode& mods);
    std::vector<YDoc>& keep(std::vector<YDoc> docs) {
        m_docStore.push_back(std::make_unique<std::vector<YDoc>>(std::move(docs)));
        return *m_docStore.back();
    }
    const std::vector<YDoc>& parsedFile(const fs::path& p);

    // Appends what object `idx` becomes to `into`: an Object, or for a
    // PSXCanvas a scene canvas and the 3D objects found under it.
    void convertObject(int idx, const std::string& parentPath, std::vector<Object>& into);
    // The canvas of the PSXCanvas object `idx`, added to the scene; its 3D
    // objects (no RectTransform) go to `hoisted` when given, else are reported.
    void convertCanvas(int idx, const std::string& path, std::vector<Object>* hoisted);
    std::optional<UIElement> elementOf(const UObj& o, const std::string& path, float scaleX, float scaleY, bool& usesCanvasFont);
    void loadingScreen(const Ref& prefab);
    void tilemapOf(const YNode& renderer, const std::string& path);
    // The scene's name for a PSXFontAsset / PSXSpriteSheet asset, converting it
    // into the scene the first time; empty when it cannot be. `cells`: the
    // sheet's cell count (0 when its texture's size is unknown).
    std::string fontOf(const YNode& ref, const std::string& path);
    std::string sheetOf(const YNode& ref, const std::string& path, int* cells = nullptr);
    const YNode* assetBody(const Ref& r, const std::string& cls);
    bool activeInHierarchy(int idx) const;
    void reportAssetsIn(const YNode& body);
    void sceneSettings(const YNode& exp, SceneSettings& st);
    std::optional<MeshComponent> meshOf(const UObj& o, const UComp* mf, const UComp* mr, const std::string& path);
    // A PSXSkinnedObjectExporter on object `idx`: the mesh of the
    // SkinnedMeshRenderer on it or under it, and the skin playing its clips.
    void skinnedObject(int idx, const UComp& exp, const std::string& path, Object& o);
    std::string clipOf(const Ref& r, const std::string& path);
    bool underSkinned(int idx) const;
    Material materialOf(const Ref& r, const std::string& path);
    std::string copyAsset(const fs::path& src, const std::string& folder, int* counter);
    std::string luaOf(const YNode& ref, const std::string& path, const std::string& field);
    std::string textureOf(const Ref& r, const std::string& path);
    ModelAsset* model(const std::string& guid, const std::string& path);
    std::string primitive(long long fileId, const std::string& path);
    void item(ReportItem::Kind k, const std::string& object, const std::string& what) {
        m_report.items.push_back({k, m_scene, object, what});
    }

    fs::path m_unity;
    const GuidIndex& m_index;
    fs::path m_dest;
    ImportReport& m_report;
    std::string m_scene;
    std::vector<UObj> m_objs;
    std::vector<std::unique_ptr<std::vector<YDoc>>> m_docStore;
    std::map<std::string, std::vector<YDoc>> m_parsed;  // file -> docs, parsed once
    std::map<std::string, std::string> m_copied;         // source file -> project path
    std::set<std::string> m_taken;                       // project paths given out
    std::map<std::string, std::string> m_primitives;     // name -> mesh path
    std::map<std::string, std::unique_ptr<ModelAsset>> m_models;
    std::map<std::string, std::string> m_clipFiles;  // "guid:fileID" of a clip -> its .anim
    std::set<std::string> m_sceneAssetsReported;
    std::vector<std::string> m_luaFiles;
    MemCardSettings m_memCard;
    std::vector<VramSettings::Area> m_areas;
    bool m_psxData = false;
    // The scene being converted, and its fonts and sheets by asset path.
    Scene* m_out = nullptr;
    std::map<std::string, std::string> m_fonts, m_sheets;
    std::map<std::string, int> m_sheetCells;
    bool m_tilemapSeen = false;
    Ref m_loading;
};

const std::vector<YDoc>& Importer::parsedFile(const fs::path& p) {
    const std::string key = generic(p);
    auto it = m_parsed.find(key);
    if (it == m_parsed.end()) it = m_parsed.emplace(key, parseYaml(readText(p))).first;
    return it->second;
}

Importer::Unit Importer::buildUnit(std::vector<YDoc>& docs, int depth) {
    Unit u;
    std::map<long long, YDoc*> byFid;
    for (YDoc& d : docs) byFid[d.fileId] = &d;
    std::map<long long, Unit> instances;
    std::vector<int> mine;  // objects made here and instance roots, for the root order

    // Objects.
    for (size_t k = 0; k < docs.size(); ++k) {
        YDoc& d = docs[k];
        if (d.classId != 1 || d.stripped) continue;
        UObj o;
        o.name = d.body["m_Name"].str();
        o.active = d.body["m_IsActive"].flag(true);
        o.order = (long long)k;
        m_objs.push_back(std::move(o));
        u.byFid[d.fileId] = int(m_objs.size()) - 1;
        mine.push_back(int(m_objs.size()) - 1);
    }
    // Prefab instances.
    for (size_t k = 0; k < docs.size(); ++k) {
        YDoc& d = docs[k];
        if (d.classId != 1001 || d.stripped) continue;
        Unit inst = depth < 8 ? instantiate(d, depth + 1) : Unit{};
        if (inst.roots.empty()) continue;
        m_objs[size_t(inst.roots[0])].order = (long long)k;
        mine.push_back(inst.roots[0]);
        instances[d.fileId] = std::move(inst);
    }
    // fileID -> object, through stripped stand-ins into instances.
    std::function<int(long long)> obj = [&](long long fid) -> int {
        if (fid == 0) return -1;
        if (auto it = u.byFid.find(fid); it != u.byFid.end()) return it->second;
        auto d = byFid.find(fid);
        if (d == byFid.end()) return -1;
        if (d->second->stripped) {
            auto in = instances.find(refOf(d->second->body["m_PrefabInstance"]).fileId);
            if (in == instances.end()) return -1;
            auto s = in->second.byFid.find(refOf(d->second->body["m_CorrespondingSourceObject"]).fileId);
            return s == in->second.byFid.end() ? -1 : s->second;
        }
        return -1;
    };
    // Transforms.
    std::vector<YDoc*> transforms;
    for (YDoc& d : docs) {
        if ((d.classId != 4 && d.classId != 224) || d.stripped) continue;
        const int o = obj(refOf(d.body["m_GameObject"]).fileId);
        if (o < 0) continue;
        Transform& t = m_objs[size_t(o)].t;
        const YNode &p = d.body["m_LocalPosition"], &r = d.body["m_LocalRotation"], &s = d.body["m_LocalScale"];
        t.position = {float(p["x"].num(0)), float(p["y"].num(0)), float(p["z"].num(0))};
        t.rotation = {float(r["x"].num(0)), float(r["y"].num(0)), float(r["z"].num(0)), float(r["w"].num(1))};
        t.scale = {float(s["x"].num(1)), float(s["y"].num(1)), float(s["z"].num(1))};
        m_objs[size_t(o)].rect = d.classId == 224 ? &d.body : nullptr;
        u.byFid[d.fileId] = o;
        transforms.push_back(&d);
    }
    // Components: each GameObject's m_Component list in order, then any
    // added to a prefab instance's object (their GameObject is stripped).
    std::set<long long> attached;
    for (YDoc& d : docs) {
        if (d.classId != 1 || d.stripped) continue;
        const int o = obj(d.fileId);
        for (const YNode& c : d.body["m_Component"].seq) {
            const long long cf = refOf(c["component"]).fileId;
            auto it = byFid.find(cf);
            if (it == byFid.end() || it->second->stripped) continue;
            const YDoc& cd = *it->second;
            if (cd.classId == 4 || cd.classId == 224) continue;
            attached.insert(cf);
            m_objs[size_t(o)].comps.push_back({cd.classId, cd.classId == 114 ? monoClass(cd.body, m_index) : "", &cd.body});
        }
    }
    for (YDoc& d : docs) {
        if (d.stripped || d.classId == 1 || d.classId == 4 || d.classId == 224 || d.classId == 1001 || attached.count(d.fileId))
            continue;
        const YNode* go = d.body.find("m_GameObject");
        if (!go) continue;
        const int o = obj(refOf(*go).fileId);
        if (o < 0) continue;
        m_objs[size_t(o)].comps.push_back({d.classId, d.classId == 114 ? monoClass(d.body, m_index) : "", &d.body});
    }
    // Hierarchy: each transform's m_Children in order, then children that
    // name a parent whose list does not hold them (a prefab instance's object).
    auto attach = [&](int child, int parent) {
        if (child < 0 || parent < 0 || child == parent || m_objs[size_t(child)].parent >= 0) return;
        for (int a = parent; a >= 0; a = m_objs[size_t(a)].parent)
            if (a == child) return;  // a cycle in a broken file
        m_objs[size_t(child)].parent = parent;
        m_objs[size_t(parent)].children.push_back(child);
    };
    for (YDoc* d : transforms) {
        const int p = obj(d->fileId);
        for (const YNode& c : d->body["m_Children"].seq) attach(obj(refOf(c).fileId), p);
    }
    for (YDoc* d : transforms) attach(obj(d->fileId), obj(refOf(d->body["m_Father"]).fileId));
    for (YDoc& d : docs)
        if (d.classId == 1001 && !d.stripped && instances.count(d.fileId))
            attach(instances[d.fileId].roots[0], obj(refOf(d.body["m_Modification"]["m_TransformParent"]).fileId));
    // Roots: SceneRoots' order when the file has one, then order of appearance.
    std::vector<int> roots;
    for (YDoc& d : docs)
        if (d.classId == 1660057539)
            for (const YNode& r : d.body["m_Roots"].seq) {
                const int o = obj(refOf(r).fileId);
                if (o >= 0 && m_objs[size_t(o)].parent < 0 && std::find(roots.begin(), roots.end(), o) == roots.end()) roots.push_back(o);
            }
    std::stable_sort(mine.begin(), mine.end(), [&](int a, int b) { return m_objs[size_t(a)].order < m_objs[size_t(b)].order; });
    for (int o : mine)
        if (m_objs[size_t(o)].parent < 0 && std::find(roots.begin(), roots.end(), o) == roots.end()) roots.push_back(o);
    u.roots = std::move(roots);
    return u;
}

Importer::Unit Importer::instantiate(const YDoc& inst, int depth) {
    const YNode& mod = inst.body["m_Modification"];
    const Ref source = refOf(inst.body["m_SourcePrefab"]);
    const fs::path file = m_index.file(source.guid);
    const std::string ext = extOf(m_index.path(source.guid));
    // The name the instance has, for a missing source's stand-in.
    std::string name;
    for (const YNode& m : mod["m_Modifications"].seq)
        if (m["propertyPath"].str() == "m_Name") name = m["value"].str();
    if (ext != ".prefab") {
        if (!file.empty() && canImportModel(file)) return instantiateModel(source, mod);
        UObj o;
        o.name = name.empty() ? "Missing prefab" : name;
        m_objs.push_back(std::move(o));
        item(ReportItem::Missing, m_objs.back().name,
             "prefab " + (file.empty() ? "with guid " + source.guid : m_index.path(source.guid)) + " not found; an empty object stands in");
        Unit u;
        u.roots.push_back(int(m_objs.size()) - 1);
        return u;
    }
    // A copy of the prefab's documents with the overrides applied.
    std::vector<YDoc>& docs = keep(parsedFile(file));
    std::map<long long, YDoc*> byFid;
    for (YDoc& d : docs) byFid[d.fileId] = &d;
    std::set<long long> removed;
    for (const YNode& r : mod["m_RemovedComponents"].seq) removed.insert(refOf(r).fileId);
    for (const YNode& r : mod["m_RemovedGameObjects"].seq) removed.insert(refOf(r).fileId);
    int lost = 0;
    for (const YNode& m : mod["m_Modifications"].seq) {
        const Ref target = refOf(m["target"]);
        const std::string& prop = m["propertyPath"].str();
        const Ref objRef = refOf(m["objectReference"]);
        YNode value = !objRef.null() ? m["objectReference"] : m["value"].isNull() ? YNode::scalarOf("") : m["value"];
        if (target.guid == source.guid || target.guid.empty()) {
            auto it = byFid.find(target.fileId);
            if (it != byFid.end()) {
                applyProperty(it->second->body, prop, value);
                continue;
            }
        }
        // A nested prefab's object: hand the override down to that instance.
        bool passed = false;
        for (YDoc& d : docs)
            if (d.classId == 1001 && refOf(d.body["m_SourcePrefab"]).guid == target.guid) {
                YNode& list = d.body.find("m_Modification") ? *d.body.find("m_Modification") : d.body.set("m_Modification", YNode());
                YNode* mods = list.find("m_Modifications");
                if (!mods) mods = &list.set("m_Modifications", YNode());
                if (mods->kind != YNode::Seq) {
                    *mods = YNode();
                    mods->kind = YNode::Seq;
                }
                mods->seq.push_back(m);
                passed = true;
                break;
            }
        if (!passed) ++lost;
    }
    if (!removed.empty())
        docs.erase(std::remove_if(docs.begin(), docs.end(), [&](const YDoc& d) { return removed.count(d.fileId) > 0; }), docs.end());
    Unit u = buildUnit(docs, depth);
    if (u.roots.empty()) {
        UObj o;
        o.name = name.empty() ? fs::path(file).stem().string() : name;
        m_objs.push_back(std::move(o));
        u.roots.push_back(int(m_objs.size()) - 1);
        item(ReportItem::Missing, m_objs.back().name, "prefab " + m_index.path(source.guid) + " holds no object");
    }
    if (u.roots.size() > 1) u.roots.resize(1);
    if (lost)
        item(ReportItem::Changed, m_objs[size_t(u.roots[0])].name,
             std::to_string(lost) + (lost == 1 ? " prefab override" : " prefab overrides") + " could not be applied (nested prefab)");
    return u;
}

// A model file placed in the scene: an object per node of the model, named
// and placed as the model's nodes, each node with a mesh carrying it for a
// PSXObjectExporter added to it. Overrides of name, transform, active flag
// and materials apply.
Importer::Unit Importer::instantiateModel(const Ref& source, const YNode& mod) {
    Unit u;
    ModelAsset* ma = model(source.guid, "");
    const std::string stem = m_index.file(source.guid).stem().string();
    const int root = int(m_objs.size());
    m_objs.emplace_back();
    m_objs.back().name = stem;
    std::vector<int> nodeObj;
    if (ma && ma->error.empty()) {
        const ImportedModel& im = ma->model;
        // Unity makes a lone top-level node the prefab root.
        int tops = 0;
        for (const ImportedNode& n : im.nodes) tops += n.parent < 0;
        const bool single = tops == 1 && !ma->preserveHierarchy;
        for (size_t k = 0; k < im.nodes.size(); ++k) {
            const ImportedNode& n = im.nodes[k];
            int o = root;
            if (!single || n.parent >= 0) {
                o = int(m_objs.size());
                m_objs.emplace_back();
                m_objs.back().name = n.name;
                m_objs.back().t = n.transform;
                const int p = n.parent >= 0 ? nodeObj[size_t(n.parent)] : root;
                m_objs[size_t(o)].parent = p;
                m_objs[size_t(p)].children.push_back(o);
            }
            if (!n.mesh.empty()) {
                m_objs[size_t(o)].modelMesh = n.mesh;
                m_objs[size_t(o)].modelMats = n.materials;
                m_objs[size_t(o)].modelSkinned = n.skinned;
                m_objs[size_t(o)].modelGuid = source.guid;
            }
            nodeObj.push_back(o);
        }
        if (im.nodes.empty() && !im.mesh.empty()) {
            m_objs[size_t(root)].modelMesh = im.mesh;
            m_objs[size_t(root)].modelMats = im.materials;
        }
        // The model's internal GameObject / Transform IDs, by name.
        for (const auto& [fid, cn] : ma->ids) {
            if (cn.first != 1 && cn.first != 4) continue;
            if (cn.second == stem || (single && !im.nodes.empty() && cn.second == im.nodes[0].name)) {
                u.byFid[fid] = root;
                continue;
            }
            for (size_t k = 0; k < im.nodes.size(); ++k)
                if (im.nodes[k].name == cn.second) {
                    u.byFid[fid] = nodeObj[k];
                    break;
                }
        }
        // Unity 2019.3+ leaves that table empty: the IDs are hashes of each
        // object's path in the model. Where the table names an ID, it wins.
        std::map<std::string, int> taken;
        auto hashed = [&](const std::string& path, int o) {
            for (const char* t : {"GameObject", "Transform", "MeshFilter", "MeshRenderer", "SkinnedMeshRenderer"}) {
                const std::string p = std::strcmp(t, "GameObject") == 0 ? path : path + "/" + t;
                u.byFid.emplace(modelObjectId(t, p, taken[std::string(t) + "->" + p]++), o);
            }
        };
        std::vector<std::string> paths(im.nodes.size());
        hashed("//RootNode/root", root);
        for (size_t k = 0; k < im.nodes.size(); ++k) {
            const ImportedNode& n = im.nodes[k];
            if (single && n.parent < 0) {
                paths[k] = "//RootNode/root";
                continue;
            }
            paths[k] = (n.parent >= 0 ? paths[size_t(n.parent)] : std::string("//RootNode/root")) + "/" + n.name;
            hashed(paths[k], nodeObj[k]);
        }
    }
    for (const YNode& m : mod["m_Modifications"].seq) {
        const Ref target = refOf(m["target"]);
        auto it = u.byFid.find(target.fileId);
        const int o = it != u.byFid.end() ? it->second : (target.guid == source.guid && u.byFid.empty() ? root : -1);
        if (o < 0) continue;
        UObj& ob = m_objs[size_t(o)];
        const std::string& p = m["propertyPath"].str();
        const YNode& v = m["value"];
        if (p == "m_Name") ob.name = v.str();
        else if (p == "m_IsActive") ob.active = v.flag(true);
        else if (p == "m_LocalPosition.x") ob.t.position.x = float(v.num());
        else if (p == "m_LocalPosition.y") ob.t.position.y = float(v.num());
        else if (p == "m_LocalPosition.z") ob.t.position.z = float(v.num());
        else if (p == "m_LocalRotation.x") ob.t.rotation.x = float(v.num());
        else if (p == "m_LocalRotation.y") ob.t.rotation.y = float(v.num());
        else if (p == "m_LocalRotation.z") ob.t.rotation.z = float(v.num());
        else if (p == "m_LocalRotation.w") ob.t.rotation.w = float(v.num(1));
        else if (p == "m_LocalScale.x") ob.t.scale.x = float(v.num(1));
        else if (p == "m_LocalScale.y") ob.t.scale.y = float(v.num(1));
        else if (p == "m_LocalScale.z") ob.t.scale.z = float(v.num(1));
        else if (p.starts_with("m_Materials.Array.data["))
            ob.matOverrides[std::atoi(p.c_str() + 23)] = refOf(m["objectReference"]);
    }
    u.roots.push_back(root);
    return u;
}

std::string Importer::copyAsset(const fs::path& src, const std::string& folder, int* counter) {
    const std::string key = generic(src);
    if (auto it = m_copied.find(key); it != m_copied.end()) return it->second;
    std::error_code ec;
    fs::create_directories(m_dest / folder, ec);
    const std::string stem = safeStem(src.stem().string()), ext = lower(src.extension().string());
    std::string rel = folder + "/" + stem + ext;
    for (int n = 2; m_taken.count(rel); ++n) rel = folder + "/" + stem + "_" + std::to_string(n) + ext;
    if (!fs::copy_file(src, m_dest / fromUtf8(rel), fs::copy_options::overwrite_existing, ec)) return {};
    m_taken.insert(rel);
    m_copied[key] = rel;
    if (counter) ++*counter;
    return rel;
}

std::string Importer::luaOf(const YNode& refNode, const std::string& path, const std::string& field) {
    const Ref r = refOf(refNode);
    if (r.null()) return {};
    const fs::path f = m_index.file(r.guid);
    std::error_code ec;
    if (f.empty() || !fs::is_regular_file(f, ec)) {
        item(ReportItem::Missing, path, field + ": the Lua file (guid " + r.guid + ") is not in the project");
        return {};
    }
    const bool fresh = !m_copied.count(generic(f));
    std::string rel = copyAsset(f, "scripts", &m_report.scripts);
    if (fresh && !rel.empty()) m_luaFiles.push_back(rel);
    return rel;
}

std::string Importer::textureOf(const Ref& r, const std::string& path) {
    if (r.null() || r.guid.empty()) return {};
    const fs::path f = m_index.file(r.guid);
    std::error_code ec;
    if (f.empty() || !fs::is_regular_file(f, ec)) {
        item(ReportItem::Missing, path, "texture (guid " + r.guid + ") is not in the project");
        return {};
    }
    const std::string ext = lower(f.extension().string());
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") {
        item(ReportItem::LeftOut, path, "texture " + m_index.path(r.guid) + " is a " + ext + " file; save it as PNG to use it");
        return {};
    }
    return copyAsset(f, "textures", &m_report.textures);
}

Material Importer::materialOf(const Ref& r, const std::string& path) {
    Material mat;
    if (r.null() || r.guid.empty()) return mat;
    const fs::path f = m_index.file(r.guid);
    if (f.empty() || extOf(m_index.path(r.guid)) != ".mat") return mat;  // a built-in default material
    for (const YDoc& d : parsedFile(f)) {
        if (d.classId != 21) continue;
        const YNode& props = d.body["m_SavedProperties"];
        Ref tex;
        for (const char* want : {"_BaseMap", "_MainTex"}) {
            for (const YNode& e : props["m_TexEnvs"].seq)
                if (const YNode* t = e.find(want)) {
                    const Ref c = refOf((*t)["m_Texture"]);
                    if (!c.null() && tex.null()) tex = c;
                }
            if (!tex.null()) break;
        }
        bool haveColor = false;
        for (const char* want : {"_BaseColor", "_Color"}) {
            for (const YNode& e : props["m_Colors"].seq)
                if (const YNode* c = e.find(want); c && !haveColor) {
                    mat.color = colorOf(*c);
                    haveColor = true;
                }
        }
        mat.texture = textureOf(tex, path);
        break;
    }
    return mat;
}

ModelAsset* Importer::model(const std::string& guid, const std::string& path) {
    if (auto it = m_models.find(guid); it != m_models.end()) return it->second.get();
    auto ma = std::make_unique<ModelAsset>();
    const fs::path src = m_index.file(guid);
    try {
        std::string rel = copyAsset(src, "models", nullptr);
        if (rel.empty()) throw std::runtime_error("cannot copy " + generic(src));
        const fs::path copy = m_dest / fromUtf8(rel);
        ImportSettings s;
        s.source = generic(fs::absolute(src));
        // Unity's model import scale, when the .meta sets one.
        for (const YDoc& d : parseYaml(readText(fs::path(src) += ".meta")))
            if (const YNode* mi = d.body.find("ModelImporter"))
                if (const YNode* meshes = mi->find("meshes")) {
                    s.scale = float((*meshes)["globalScale"].num(1));
                    ma->preserveHierarchy = (*meshes)["preserveHierarchy"].flag(false);
                }
        for (const YDoc& d : parseYaml(readText(fs::path(src) += ".meta")))
            if (const YNode* mi = d.body.find("ModelImporter"))
                if (const YNode* an = mi->find("animations"))
                    for (const YNode& c : (*an)["clipAnimations"].seq)
                        ma->clipSettings.push_back({c["internalID"].integer(0), c["name"].str(), c["takeName"].str(),
                                                    c["loopTime"].flag(false), c["firstFrame"].integer(0)});
        saveImportSettings(copy, s);
        ma->model = importModel(copy, m_dest, s);
        ma->ids = metaIds(fs::path(src) += ".meta");
        ++m_report.meshes;
        m_report.textures += int(ma->model.textures.size());
        for (const std::string& w : ma->model.warnings) item(ReportItem::Changed, path, m_index.path(guid) + ": " + w);
    } catch (const std::exception& e) {
        ma->error = e.what();
        item(ReportItem::Missing, path, "model " + m_index.path(guid) + " could not be imported: " + e.what());
    }
    return (m_models[guid] = std::move(ma)).get();
}

std::string Importer::primitive(long long fileId, const std::string& path) {
    std::optional<Primitive> p = builtinMesh(fileId);
    if (!p) {
        item(ReportItem::LeftOut, path, "built-in mesh #" + std::to_string(fileId) + " is not one this import knows");
        return {};
    }
    if (!p->exact) item(ReportItem::Changed, path, "Unity's " + p->name + " is replaced by a lower-polygon " + p->name + " of the same size");
    auto it = m_primitives.find(p->name);
    if (it != m_primitives.end()) return it->second;
    std::error_code ec;
    fs::create_directories(m_dest / "meshes", ec);
    const std::string rel = "meshes/unity_" + p->name + ".mesh";
    saveMesh(p->mesh, m_dest / rel);
    m_taken.insert(rel);
    ++m_report.meshes;
    return m_primitives[p->name] = rel;
}

std::optional<MeshComponent> Importer::meshOf(const UObj& o, const UComp* mf, const UComp* mr, const std::string& path) {
    MeshComponent mc;
    std::vector<Material> modelMats;
    if (mf) {
        const Ref r = refOf((*mf->body)["m_Mesh"]);
        if (r.null()) return std::nullopt;
        if (r.guid == kBuiltinGuid) {
            mc.mesh = primitive(r.fileId, path);
        } else {
            const fs::path f = m_index.file(r.guid);
            if (f.empty()) {
                item(ReportItem::Missing, path, "mesh (guid " + r.guid + ") is not in the project");
                return std::nullopt;
            }
            if (!canImportModel(f)) {
                item(ReportItem::LeftOut, path, "mesh " + m_index.path(r.guid) + " is not a model file (glTF, FBX, OBJ, .blend); not imported");
                return std::nullopt;
            }
            ModelAsset* ma = model(r.guid, path);
            if (!ma || !ma->error.empty()) return std::nullopt;
            const ImportedModel& im = ma->model;
            // The node whose mesh the MeshFilter names, by the .meta's name for its fileID.
            const ImportedNode* node = nullptr;
            if (auto it = ma->ids.find(r.fileId); it != ma->ids.end())
                for (const ImportedNode& n : im.nodes)
                    if (!n.mesh.empty() && n.name == it->second.second) node = &n;
            int withMesh = 0;
            for (const ImportedNode& n : im.nodes) withMesh += !n.mesh.empty();
            if (!node && withMesh == 1)
                for (const ImportedNode& n : im.nodes)
                    if (!n.mesh.empty()) node = &n;
            if (node) {
                mc.mesh = node->mesh;
                modelMats = node->materials;
            } else {
                mc.mesh = im.mesh;
                modelMats = im.materials;
                if (withMesh > 1)
                    item(ReportItem::Changed, path, "the whole of " + m_index.path(r.guid) + " is drawn; the one mesh it uses could not be told apart");
            }
        }
    } else if (!o.modelMesh.empty()) {
        mc.mesh = o.modelMesh;
        modelMats = o.modelMats;
    }
    if (mc.mesh.empty()) return std::nullopt;
    // One material per submesh: the renderer's, the model's own where it names an embedded one.
    size_t subs = 1;
    try {
        subs = std::max<size_t>(1, loadMesh(m_dest / fromUtf8(mc.mesh)).submeshes.size());
    } catch (const std::exception&) {
    }
    std::vector<YNode> mats;
    if (mr) mats = (*mr->body)["m_Materials"].seq;
    for (size_t k = 0; k < subs; ++k) {
        Ref r;
        if (k < mats.size()) r = refOf(mats[k]);
        if (auto ov = o.matOverrides.find(int(k)); ov != o.matOverrides.end()) r = ov->second;
        const bool embedded = !r.guid.empty() && extOf(m_index.path(r.guid)) != ".mat";
        if ((!mr && k < modelMats.size()) || (embedded && k < modelMats.size())) mc.materials.push_back(modelMats[k]);
        else mc.materials.push_back(materialOf(r, path));
    }
    return mc;
}

bool Importer::underSkinned(int idx) const {
    for (int i = idx; i >= 0; i = m_objs[size_t(i)].parent)
        for (const UComp& c : m_objs[size_t(i)].comps)
            if (c.classId == 114 && c.cls == "PSXSkinnedObjectExporter") return true;
    return false;
}

// The first `units` UTF-16 code units of `s`, as Unity's exporter cuts a clip
// name (String.Substring(0, 24)).
static std::string cutUtf16(const std::string& s, size_t units) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        const unsigned char c = uint8_t(s[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4, u = len == 4 ? 2 : 1;
        if (n + u > units) break;
        n += u, i += len;
    }
    return s.substr(0, i);
}

// A clip name as a file name.
static std::string clipFileName(const std::string& name) {
    std::string s;
    for (unsigned char c : name) s += std::isalnum(c) || c == '-' || c == '_' ? char(c) : '_';
    return s.empty() ? std::string("clip") : s;
}

// The .anim a PSXSkinnedObjectExporter's clip `r` plays: the model's take it
// was cut from, under Unity's name and loop flag for the clip. A clip's
// fileID is the .meta's clipAnimations[k].internalID, else
// xxh64("Type:AnimationClip-><take><n>"), n counting earlier clips of that
// take; with no clipAnimations Unity makes one clip per take, named after it,
// that does not loop. Empty, reported, when it cannot be had.
std::string Importer::clipOf(const Ref& r, const std::string& path) {
    const std::string key = r.guid + ":" + std::to_string(r.fileId);
    if (auto it = m_clipFiles.find(key); it != m_clipFiles.end()) return it->second;
    const fs::path f = m_index.file(r.guid);
    if (f.empty()) {
        item(ReportItem::Missing, path, "animation clip (guid " + r.guid + ") is not in the project");
        return {};
    }
    if (!canImportModel(f)) {
        item(ReportItem::LeftOut, path, "animation clip " + m_index.path(r.guid) +
                                            " is a Unity animation asset; only clips inside a model file (glTF, FBX, .blend) are imported");
        return {};
    }
    ModelAsset* ma = model(r.guid, path);
    if (!ma || !ma->error.empty()) return {};
    std::string name, take;
    bool loop = false, cut = false, found = false;
    std::map<std::string, int> perTake;
    for (const ModelAsset::Clip& c : ma->clipSettings) {
        const int n = perTake[c.take]++;
        if (found || (c.id ? c.id : modelObjectId("AnimationClip", c.take, n)) != r.fileId) continue;
        name = c.name, take = c.take, loop = c.loop, cut = c.firstFrame > 0, found = true;
    }
    if (found) cut |= perTake[take] > 1;
    if (!found && ma->clipSettings.empty())
        for (const std::string& t : ma->model.clipTakes)
            if (modelObjectId("AnimationClip", t, perTake[t]++) == r.fileId) {
                name = take = t, found = true;
                break;
            }
    // Before Unity 2019.3 the .meta names its IDs (class 74, AnimationClip).
    if (auto it = ma->ids.find(r.fileId); !found && it != ma->ids.end() && it->second.first == 74) {
        name = take = it->second.second, found = true;
        for (const ModelAsset::Clip& c : ma->clipSettings)
            if (c.name == name) take = c.take, loop = c.loop, cut = c.firstFrame > 0 || perTake[c.take] > 1;
    }
    const std::string model = m_index.path(r.guid);
    if (!found) {
        item(ReportItem::Missing, path, "animation clip #" + std::to_string(r.fileId) + " is not one of the clips of " + model);
        return {};
    }
    const auto& takes = ma->model.clipTakes;
    const size_t i = size_t(std::find(takes.begin(), takes.end(), take) - takes.begin());
    if (i >= takes.size() || i >= ma->model.clips.size()) {
        item(ReportItem::LeftOut, path, "animation clip '" + name + "' of " + model + " has no skeleton animation this import could bring over");
        return {};
    }
    if (cut)
        item(ReportItem::Changed, path, "animation clip '" + name + "' is a part of take '" + take + "' of " + model + "; it plays the whole take");
    const std::string& orig = ma->model.clips[i];
    AnimClip a;
    try {
        a = loadAnim(m_dest / fromUtf8(orig));
    } catch (const std::exception& e) {
        item(ReportItem::Missing, path, "animation clip '" + name + "': " + e.what());
        return {};
    }
    std::string want = cutUtf16(name, 24);
    if (want.empty()) want = cutUtf16(take, 24);
    if (a.name == want && a.loop == loop) return m_clipFiles[key] = orig;
    // Unity's name or loop flag differs from the model's own clip: a copy
    // outside the model's folder, which a reimport of the model clears.
    a.name = want;
    a.loop = loop;
    const std::string dir = "anims/" + safeStem(fs::path(fromUtf8(orig)).parent_path().filename().string());
    const std::string base = clipFileName(want);
    std::string rel = dir + "/" + base + ".anim";
    for (int n = 2; m_taken.count(rel); ++n) rel = dir + "/" + base + "_" + std::to_string(n) + ".anim";
    std::error_code ec;
    fs::create_directories(m_dest / fromUtf8(dir), ec);
    try {
        saveAnim(a, m_dest / fromUtf8(rel));
    } catch (const std::exception& e) {
        item(ReportItem::Missing, path, "animation clip '" + name + "': " + e.what());
        return {};
    }
    m_taken.insert(rel);
    return m_clipFiles[key] = rel;
}

// Unity's exporter draws the mesh of GetComponentInChildren<SkinnedMeshRenderer>()
// at the exporter's object and bakes the clips its AnimationClips lists
// (PSXSkinnedMeshExporter.CreateProxy, BakeSkinData).
void Importer::skinnedObject(int idx, const UComp& exp, const std::string& path, Object& o) {
    const YNode& b = *exp.body;
    o.startActive = b["IsActive"].flag(true);
    // The renderer on the object, else on its first active descendant, depth first.
    int at = -1;
    const UComp* smr = nullptr;
    std::function<void(int)> find = [&](int i) {
        const UObj& u = m_objs[size_t(i)];
        if (at >= 0 || (i != idx && !u.active)) return;
        for (const UComp& c : u.comps)
            if (c.classId == 137 && !refOf((*c.body)["m_Mesh"]).null()) {
                at = i, smr = &c;
                return;
            }
        if (u.modelSkinned) {
            at = i;
            return;
        }
        for (int ch : u.children) find(ch);
    };
    find(idx);
    if (at < 0) {
        item(ReportItem::LeftOut, path, "PSXSkinnedObjectExporter with no SkinnedMeshRenderer on or under it: Unity exports nothing for it either");
        return;
    }
    const UObj& su = m_objs[size_t(at)];
    std::optional<MeshComponent> mc = meshOf(su, smr, smr, path);
    if (mc) {
        const long long bpp = b["BitDepth"].integer(8);
        mc->bitDepth = bpp == 4 ? BitDepth::Bpp4 : bpp == 15 || bpp == 16 ? BitDepth::Bpp16 : BitDepth::Bpp8;
        const long long vc = b["ColorMode"].integer(0);
        mc->vertexColors = vc == 1 ? VertexColorMode::Flat : vc == 2 ? VertexColorMode::Mesh : VertexColorMode::Baked;
        const YNode& fc = b["FlatVertexColor"];
        if (!fc.isNull()) {
            const auto c = colorOf(fc, {128 / 255.f, 128 / 255.f, 128 / 255.f, 1});
            mc->flatColor = {uint8_t(std::lround(c[0] * 255)), uint8_t(std::lround(c[1] * 255)), uint8_t(std::lround(c[2] * 255))};
        }
        mc->smoothNormals = b["SmoothNormals"].flag(true);
        o.mesh = std::move(mc);
    }
    const std::string lua = luaOf(b["LuaFile"], path, "script");
    if (!lua.empty()) o.script = ScriptComponent{lua, {}};
    if (!o.mesh) return;

    SkinComponent sk;
    sk.fps = std::clamp(int(b["TargetFPS"].integer(15)), 1, 30);
    for (const YNode& c : b["AnimationClips"].seq) {
        const Ref r = refOf(c);
        if (r.null()) continue;
        const std::string a = clipOf(r, path);
        if (!a.empty() && std::find(sk.clips.begin(), sk.clips.end(), a) == sk.clips.end()) sk.clips.push_back(a);
    }
    if (sk.clips.size() > 16) {
        item(ReportItem::Changed, path, std::to_string(sk.clips.size()) + " animation clips, a skin plays at most 16: the first 16 are kept");
        sk.clips.resize(16);
    }
    if (sk.clips.empty()) {
        // psxsplash draws no skinned mesh without a clip: the model's own stand in.
        const std::string guid = smr ? refOf((*smr->body)["m_Mesh"]).guid : su.modelGuid;
        ModelAsset* ma = guid.empty() ? nullptr : model(guid, path);
        if (ma && ma->error.empty())
            for (size_t k = 0; k < ma->model.clips.size() && k < 16; ++k) sk.clips.push_back(ma->model.clips[k]);
        if (sk.clips.empty())
            item(ReportItem::LeftOut, path, "no animation clip, and its model has none: psxsplash draws no skinned mesh without one, so it gets no skin");
        else
            item(ReportItem::Changed, path, "PSXSkinnedObjectExporter lists no animation clip this import could bring over; the skin plays its model's " +
                                                std::to_string(sk.clips.size()) + " clips instead, as psxsplash draws no skinned mesh without one");
    }
    if (!sk.clips.empty()) o.skin = std::move(sk);
}

void Importer::reportAssetsIn(const YNode& body) {
    std::function<void(const YNode&)> walk = [&](const YNode& n) {
        if (n.kind == YNode::Map) {
            const Ref r = refOf(n);
            if (!r.guid.empty() && n.find("guid")) {
                const std::string p = m_index.path(r.guid);
                if (extOf(p) == ".asset" && !m_sceneAssetsReported.count(p)) {
                    m_sceneAssetsReported.insert(p);
                    std::string cls;
                    for (const YDoc& d : parsedFile(m_index.file(r.guid)))
                        if (d.classId == 114) {
                            cls = monoClass(d.body, m_index);
                            break;
                        }
                    if (cls.starts_with("PSX")) {
                        const std::string what = cls == "PSXFontAsset" ? "font" : cls == "PSXSpriteSheet" ? "sprite sheet"
                                               : cls == "PSXTilemap" ? "tilemap" : cls;
                        item(ReportItem::Later, "", what + " " + p + " not imported yet");
                    }
                }
                return;
            }
            for (const auto& kv : n.map) walk(kv.second);
        } else if (n.kind == YNode::Seq) {
            for (const YNode& e : n.seq) walk(e);
        }
    };
    walk(body);
}

bool Importer::activeInHierarchy(int idx) const {
    for (int i = idx; i >= 0; i = m_objs[size_t(i)].parent)
        if (!m_objs[size_t(i)].active) return false;
    return true;
}

// The body of the `cls` MonoBehaviour in the asset `r` names (a ScriptableObject .asset).
const YNode* Importer::assetBody(const Ref& r, const std::string& cls) {
    if (r.null() || r.guid.empty()) return nullptr;
    const fs::path f = m_index.file(r.guid);
    std::error_code ec;
    if (f.empty() || !fs::is_regular_file(f, ec)) return nullptr;
    for (const YDoc& d : parsedFile(f))
        if (d.classId == 114 && monoClass(d.body, m_index) == cls) return &d.body;
    return nullptr;
}

namespace {
std::string uniqueName(const std::string& want, const std::function<bool(const std::string&)>& taken) {
    std::string n = want;
    for (int k = 2; taken(n); ++k) n = want + "_" + std::to_string(k);
    return n;
}
// Unity's byte[] fields: two hex digits a byte.
std::vector<int> hexBytes(std::string_view hex) {
    std::vector<int> out;
    for (size_t i = 0; i + 2 <= hex.size(); i += 2) {
        char* end = nullptr;
        const std::string two(hex.substr(i, 2));
        const long v = std::strtol(two.c_str(), &end, 16);
        if (!end || *end) break;
        out.push_back(int(v));
    }
    return out;
}
BitDepth depthOf(long long bpp) { return bpp == 4 ? BitDepth::Bpp4 : bpp == 15 || bpp == 16 ? BitDepth::Bpp16 : BitDepth::Bpp8; }
}  // namespace

// PSXFontAsset: its fontTexture bitmap (256 wide, glyph cells from 0x20, ink
// = alpha above 0.5, as PSXFontAsset.ConvertTo4BPP reads it) and its stored
// advance widths: what the exporter packs. The source TTF/OTF it was made
// from is not used (a UIFont is either a bitmap or a TTF).
std::string Importer::fontOf(const YNode& refNode, const std::string& path) {
    const Ref r = refOf(refNode);
    if (r.null()) return {};
    const std::string asset = m_index.path(r.guid);
    if (auto it = m_fonts.find(r.guid); it != m_fonts.end()) return it->second;
    m_fonts[r.guid] = "";  // tried once per scene
    const YNode* b = assetBody(r, "PSXFontAsset");
    if (!b) {
        item(ReportItem::Missing, path, "font (guid " + r.guid + ") is not in the project; its text uses the system font");
        return {};
    }
    const fs::path tex = m_index.file(refOf((*b)["fontTexture"]).guid);
    std::error_code ec;
    if (tex.empty() || !fs::is_regular_file(tex, ec)) {
        item(ReportItem::Missing, path, "font " + asset + " has no bitmap (fontTexture); its text uses the system font");
        return {};
    }
    UIFont f;
    f.name = uniqueName((*b)["m_Name"].str().empty() ? fs::path(asset).stem().string() : (*b)["m_Name"].str(), [&](const std::string& n) {
        return std::any_of(m_out->fonts.begin(), m_out->fonts.end(), [&](const UIFont& x) { return x.name == n; });
    });
    f.glyphWidth = int((*b)["glyphWidth"].integer(8));
    f.glyphHeight = int((*b)["glyphHeight"].integer(16));
    // The bitmap as the exporter samples it: 256 texels wide, rows from the
    // top, anything past the source's edge empty.
    try {
        const Image img = loadImage(tex);
        if (img.width == 256) {
            f.bitmap = copyAsset(tex, "fonts", nullptr);
        } else {
            const int perRow = std::max(1, 256 / std::max(1, f.glyphWidth));
            const int h = (95 + perRow - 1) / perRow * std::max(1, f.glyphHeight);
            Image out;
            out.width = 256;
            out.height = h;
            out.pixels.assign(size_t(256 * h), Image::Px{0, 0, 0, 0});
            for (int y = 0; y < h && y < img.height; ++y)
                for (int x = 0; x < 256 && x < img.width; ++x)
                    out.pixels[size_t((h - 1 - y) * 256 + x)] = img.pixels[size_t((img.height - 1 - y) * img.width + x)];
            fs::create_directories(m_dest / "fonts", ec);
            std::string rel = "fonts/" + safeStem(tex.stem().string()) + "_256.png";
            for (int n = 2; m_taken.count(rel); ++n) rel = "fonts/" + safeStem(tex.stem().string()) + "_256_" + std::to_string(n) + ".png";
            savePng(out, m_dest / fromUtf8(rel));
            m_taken.insert(rel);
            f.bitmap = rel;
            item(ReportItem::Changed, path,
                 "font " + asset + ": its bitmap is " + std::to_string(img.width) + " pixels wide; saved 256 wide as the exporter reads it");
        }
    } catch (const std::exception& e) {
        item(ReportItem::Missing, path, "font " + asset + ": its bitmap cannot be read (" + e.what() + "); its text uses the system font");
        return {};
    }
    // PSXUIExporter: the stored widths, else the cell width for every character.
    const std::vector<int> adv = hexBytes((*b)["storedAdvanceWidths"].str());
    if (adv.size() >= 96) f.advances.assign(adv.begin(), adv.begin() + 96);
    else f.advances.assign(96, f.glyphWidth);
    m_out->fonts.push_back(f);
    return m_fonts[r.guid] = f.name;
}

// PSXSpriteSheet: added to the scene the first time something uses it (a
// PSXSprite, a PSXUISprite, the tilemap's tileset), as the exporter packs it.
std::string Importer::sheetOf(const YNode& refNode, const std::string& path, int* cells) {
    if (cells) *cells = 0;
    const Ref r = refOf(refNode);
    if (r.null()) return {};
    const std::string asset = m_index.path(r.guid);
    if (auto it = m_sheets.find(r.guid); it != m_sheets.end()) {
        if (cells) *cells = m_sheetCells[r.guid];
        return it->second;
    }
    m_sheets[r.guid] = "";
    const YNode* b = assetBody(r, "PSXSpriteSheet");
    if (!b) {
        item(ReportItem::Missing, path, "sprite sheet (guid " + r.guid + ") is not in the project");
        return {};
    }
    SpriteSheet sh;
    const YNode* nm = b->find("sheetName");
    const std::string want = nm ? nm->str() : std::string("sheet");
    sh.name = uniqueName(want, [&](const std::string& n) {
        return std::any_of(m_out->spriteSheets.begin(), m_out->spriteSheets.end(), [&](const SpriteSheet& x) { return x.name == n; });
    });
    if (sh.name != want)
        item(ReportItem::Changed, path, "sprite sheet " + asset + " is named " + sh.name + ": another sheet of this scene is named " + want);
    const Ref tex = refOf((*b)["sourceTexture"]);
    if (tex.null()) {
        item(ReportItem::LeftOut, path, "sprite sheet " + asset + " has no texture, so there is nothing to pack");
        return {};
    }
    sh.texture = textureOf(tex, path);
    if (sh.texture.empty()) return {};
    sh.bitDepth = depthOf((*b)["bitDepth"].integer(4));
    sh.cellW = std::max(1, int((*b)["cellWidth"].integer(16)));
    sh.cellH = std::max(1, int((*b)["cellHeight"].integer(16)));
    for (const YNode& a : (*b)["animations"].seq) {
        SpriteAnim an;
        if (const YNode* n = a.find("animationName")) an.name = n->str();
        an.firstFrame = int(a["firstFrame"].integer(0));
        an.frameCount = int(a["frameCount"].integer(1));
        an.frameDuration = int(a["frameDuration"].integer(6));
        an.loop = a["loop"].flag(true);
        sh.anims.push_back(an);
    }
    int count = 0;
    try {
        const Image img = loadImage(m_dest / fromUtf8(sh.texture));
        count = std::max(1, img.width / sh.cellW) * std::max(1, img.height / sh.cellH);
    } catch (const std::exception&) {
    }
    m_sheetCells[r.guid] = count;
    if (cells) *cells = count;
    m_out->spriteSheets.push_back(sh);
    return m_sheets[r.guid] = sh.name;
}

// One UI element, as PSXUIExporter.CollectAllElementsInHierarchyOrder takes
// it: the first of Sprite, Image, Box, Line, Text, ProgressBar on the object.
std::optional<UIElement> Importer::elementOf(const UObj& o, const std::string& path, float scaleX, float scaleY, bool& usesCanvasFont) {
    static const char* const order[] = {"PSXUISprite", "PSXUIImage", "PSXUIBox", "PSXUILine", "PSXUIText", "PSXUIProgressBar"};
    const UComp* pick = nullptr;
    int rank = 99;
    for (const UComp& c : o.comps)
        for (int k = 0; k < 6; ++k)
            if (c.classId == 114 && c.cls == order[k] && k < rank) {
                rank = k;
                pick = &c;
            }
    if (!pick) return std::nullopt;
    for (const UComp& c : o.comps)
        if (&c != pick && c.classId == 114 && isUiClass(c.cls))
            item(ReportItem::LeftOut, path, c.cls + " beside a " + pick->cls + ": Unity exports one UI element per object, the " + pick->cls);
    if (!o.rect) {
        item(ReportItem::LeftOut, path, pick->cls + " without a RectTransform: Unity does not export it either");
        return std::nullopt;
    }
    const YNode& b = *pick->body;
    auto col = [&](const char* key, std::array<float, 4> def) {
        const auto c = colorOf(b[key], def);
        return std::array<float, 3>{c[0], c[1], c[2]};
    };
    auto str = [&](const char* key, const char* def) {
        const YNode* n = b.find(key);
        return n ? n->str() : std::string(def);
    };
    UIElement e;
    bakeLayout(*o.rect, scaleX, scaleY, e);
    e.visible = b["startVisible"].flag(true);
    const std::string& cls = pick->cls;
    if (cls == "PSXUISprite") {
        e.type = UIElementType::Image;
        e.name = str("elementName", "sprite");
        e.color = col("tint", {0.5f, 0.5f, 0.5f, 1});
        int cells = 0;
        e.sheet = sheetOf(b["sheet"], path, &cells);
        // PSXUISprite.ResolvedCell: clamped into the sheet.
        const int cell = int(b["cell"].integer(0));
        e.cell = e.sheet.empty() ? 0 : cells > 0 ? std::clamp(cell, 0, cells - 1) : std::max(0, cell);
        if (refOf(b["sheet"]).null())
            item(ReportItem::LeftOut, path, "PSXUISprite without a sheet: it draws nothing recognisable in Unity's export either");
    } else if (cls == "PSXUIImage") {
        e.type = UIElementType::Image;
        e.name = str("elementName", "image");
        e.color = col("tintColor", {1, 1, 1, 1});
        e.texture = textureOf(refOf(b["sourceTexture"]), path);
        e.bitDepth = depthOf(b["bitDepth"].integer(8));
        e.cutout = false;  // PSXTexture2D.CreateFromTexture2D's default for UI images
    } else if (cls == "PSXUIBox") {
        e.type = UIElementType::Box;
        e.name = str("elementName", "box");
        e.color = col("boxColor", {0, 0, 0, 1});
    } else if (cls == "PSXUILine") {
        e.type = UIElementType::Line;
        e.name = str("elementName", "line");
        e.color = col("lineColor", {0, 0, 0, 1});
        // The exporter stores each endpoint coordinate as a byte.
        const V2 p1 = v2Of(b["point1"], 0, 0), p2 = v2Of(b["point2"], 0, 0);
        e.from = {int(uint8_t(roundEven(p1.x))), int(uint8_t(roundEven(p1.y)))};
        e.to = {int(uint8_t(roundEven(p2.x))), int(uint8_t(roundEven(p2.y)))};
    } else if (cls == "PSXUIText") {
        e.type = UIElementType::Text;
        e.name = str("elementName", "text");
        e.color = col("textColor", {1, 1, 1, 1});
        e.text = str("defaultText", "");
        if (!refOf(b["fontOverride"]).null()) e.font = fontOf(b["fontOverride"], path);
        else usesCanvasFont = true;
    } else {
        e.type = UIElementType::Progress;
        e.name = str("elementName", "progress");
        e.color = col("fillColor", {0, 1, 0, 1});
        e.background = col("backgroundColor", {0.2f, 0.2f, 0.2f, 1});
        e.value = std::clamp(int(b["initialValue"].integer(0)), 0, 100);
    }
    return e;
}

// A PSXCanvas: its settings, and every UI element below it depth-first in
// sibling order, inactive objects too (PSXUIExporter.CollectCanvasesInternal).
// Scale: the PS1 resolution over the canvas RectTransform's size.
void Importer::convertCanvas(int idx, const std::string& path, std::vector<Object>* hoisted) {
    const UObj& c = m_objs[size_t(idx)];
    const UComp* pc = nullptr;
    for (const UComp& k : c.comps)
        if (k.classId == 114 && k.cls == "PSXCanvas") pc = &k;
    if (!pc) return;
    const YNode& b = *pc->body;
    UICanvas cv;
    const YNode* nm = b.find("canvasName");
    cv.name = nm ? nm->str() : std::string("canvas");
    cv.visible = b["startVisible"].flag(true);
    cv.sortOrder = std::clamp(int(b["sortOrder"].integer(0)), 0, 255);
    float cw = 320, ch = 240;
    if (c.rect) {
        const V2 sd = v2Of((*c.rect)["m_SizeDelta"], 320, 240);
        if (sd.x > 0) cw = sd.x;
        if (sd.y > 0) ch = sd.y;
    }
    const float sx = 320.f / cw, sy = 240.f / ch;
    bool usesDefault = false;
    std::function<void(int, const std::string&)> walk = [&](int i, const std::string& p) {
        for (int k : m_objs[size_t(i)].children) {
            const UObj& o = m_objs[size_t(k)];
            const std::string op = p + "/" + o.name;
            if (!o.rect) {
                // A 3D object under the canvas: converted where the canvas was.
                if (hoisted) {
                    const size_t before = hoisted->size();
                    convertObject(k, p, *hoisted);
                    bool on = true;
                    for (int a = i; a >= 0 && a != idx; a = m_objs[size_t(a)].parent) on = on && m_objs[size_t(a)].active;
                    if (hoisted->size() > before && !on) hoisted->back().active = false;
                    item(ReportItem::Changed, op, "a 3D object under the canvas " + path + ": placed where the canvas was, without the UI objects' transforms");
                } else {
                    item(ReportItem::LeftOut, op, "a 3D object in a loading screen prefab: only its canvas is used");
                }
                continue;
            }
            bool uses = false;
            if (std::optional<UIElement> e = elementOf(o, op, sx, sy, uses)) {
                cv.elements.push_back(std::move(*e));
                usesDefault = usesDefault || uses;
            }
            std::vector<std::string> other;
            for (const UComp& k2 : o.comps) {
                if (k2.classId == 222 || k2.classId == 223 || k2.classId == 225 || (k2.classId == 114 && isUiClass(k2.cls))) continue;
                if (k2.classId == 114 && k2.cls == "PSXCanvas") {
                    other.push_back("a PSXCanvas inside the canvas " + cv.name + " (its elements are in that canvas, as Unity exports them)");
                    continue;
                }
                const char* n = builtinClassName(k2.classId);
                other.push_back(k2.classId == 114 ? (k2.cls.empty() ? std::string("a script whose class is unknown") : k2.cls)
                                                  : n ? std::string(n) : "Unity component #" + std::to_string(k2.classId));
            }
            if (!other.empty()) {
                std::string list;
                for (const std::string& s : other) list += (list.empty() ? "" : ", ") + s;
                item(ReportItem::LeftOut, op, list + " on a UI object, not brought over");
            }
            walk(k, op);
        }
    };
    walk(idx, path);
    // Unity packs a canvas's default font only when one of its texts uses it.
    if (usesDefault && !refOf(b["defaultFont"]).null()) cv.defaultFont = fontOf(b["defaultFont"], path);
    m_out->canvases.push_back(std::move(cv));
}

// The scene's PSXTilemapRenderer: its PSXTilemap asset becomes Scene.tilemap.
void Importer::tilemapOf(const YNode& rb, const std::string& path) {
    if (m_tilemapSeen) {
        item(ReportItem::LeftOut, path, "a second PSXTilemapRenderer: a scene has one tilemap (Unity refuses to export this scene)");
        return;
    }
    m_tilemapSeen = true;
    const Ref r = refOf(rb["tilemap"]);
    if (r.null()) return;
    const std::string asset = m_index.path(r.guid);
    const YNode* b = assetBody(r, "PSXTilemap");
    if (!b) {
        item(ReportItem::Missing, path, "tilemap (guid " + r.guid + ") is not in the project");
        return;
    }
    Tilemap t;
    if (refOf((*b)["tileset"]).null()) item(ReportItem::LeftOut, path, "tilemap " + asset + " has no tileset (Unity refuses to export it)");
    t.tileset = sheetOf((*b)["tileset"], path);
    const int w = std::max(1, int((*b)["width"].integer(32))), h = std::max(1, int((*b)["height"].integer(32)));
    t.width = std::min(w, Tilemap::kMaxSide);
    t.height = std::min(h, Tilemap::kMaxSide);
    if (t.width != w || t.height != h)
        item(ReportItem::Changed, path, "tilemap " + asset + " is " + std::to_string(w) + "x" + std::to_string(h) + "; cut to " +
                                            std::to_string(t.width) + "x" + std::to_string(t.height) + " (Unity refuses more than 256 a side)");
    for (const YNode& bn : (*b)["brushes"].seq) {
        TileBrush br;
        if (const YNode* n = bn.find("name")) br.name = n->str();
        br.cell = int(bn["tilesetCell"].integer(0));
        br.walkable = bn["walkable"].flag(true);
        br.kind = int(bn["objectKind"].integer(0));
        br.id = int(bn["objectId"].integer(-1));
        const auto c = colorOf(bn["editorColor"], {1, 1, 1, 1});
        br.color = {c[0], c[1], c[2]};
        t.brushes.push_back(br);
    }
    if (const YNode* k = b->find("objectKindNames"); k && k->kind == YNode::Seq) {
        t.kindNames.clear();
        for (const YNode& n : k->seq) t.kindNames.push_back(n.str());
    }
    // cells: little-endian int32s as hex, row by row; PSXTilemap.EnsureCells
    // fills a short list with -1 and cuts a long one.
    const std::string& hex = (*b)["cells"].str();
    const std::vector<int32_t> cells = hexInts(hex);
    const size_t need = size_t(w) * size_t(h);
    if (hex.size() != need * 8)
        item(ReportItem::Changed, path,
             "tilemap " + asset + " stores " + std::to_string(hex.size() / 8) + " cells for its " + std::to_string(w) + "x" + std::to_string(h) +
                 " map; the missing ones are empty and extra ones dropped, as Unity does");
    t.cells.assign(size_t(t.width) * size_t(t.height), -1);
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (i < cells.size()) t.cells[size_t(y * t.width + x)] = cells[i];
        }
    m_out->tilemap = std::move(t);
}

// LoadingScreenPrefab: the first PSXCanvas in the prefab (the root first,
// then depth-first, inactive ones too: GetComponentInChildren(true)) becomes
// a canvas of the scene, named so it does not clash, and the scene's loading canvas.
void Importer::loadingScreen(const Ref& prefab) {
    const std::string p = m_index.path(prefab.guid);
    const fs::path file = m_index.file(prefab.guid);
    if (file.empty() || extOf(p) != ".prefab") {
        item(ReportItem::Missing, "", "loading screen " + (p.empty() ? "(guid " + prefab.guid + ")" : p) + " is not a prefab in the project");
        return;
    }
    std::vector<YDoc>& docs = keep(parsedFile(file));
    const Unit u = buildUnit(docs, 1);
    int found = -1;
    std::function<void(int)> find = [&](int i) {
        if (found >= 0) return;
        for (const UComp& c : m_objs[size_t(i)].comps)
            if (c.classId == 114 && c.cls == "PSXCanvas") {
                found = i;
                return;
            }
        for (int k : m_objs[size_t(i)].children) find(k);
    };
    for (int r : u.roots) find(r);
    if (found < 0) {
        item(ReportItem::LeftOut, "", "loading screen " + p + " holds no PSXCanvas, so Unity shows none either");
        return;
    }
    const size_t before = m_out->canvases.size();
    convertCanvas(found, fs::path(p).stem().string() + " (loading screen)/" + m_objs[size_t(found)].name, nullptr);
    if (m_out->canvases.size() == before) return;
    UICanvas& cv = m_out->canvases.back();
    const std::string want = cv.name;
    auto taken = [&](const std::string& n) {
        for (size_t i = 0; i + 1 < m_out->canvases.size(); ++i)
            if (m_out->canvases[i].name == n) return true;
        return false;
    };
    if (taken(want)) {
        cv.name = uniqueName(want + "_loading", taken);
        item(ReportItem::Changed, "", "loading screen " + p + ": its canvas " + want + " is named " + cv.name + " here, the scene has a canvas " + want);
    }
    m_out->settings.loadingCanvas = cv.name;
}

void Importer::convertObject(int idx, const std::string& parentPath, std::vector<Object>& into) {
    const UObj& u = m_objs[size_t(idx)];
    const std::string path = parentPath.empty() ? u.name : parentPath + "/" + u.name;
    for (const UComp& c : u.comps)
        if (c.classId == 114 && c.cls == "PSXCanvas") {
            // FindObjectsByType<PSXCanvas> skips inactive objects.
            if (!activeInHierarchy(idx)) item(ReportItem::LeftOut, path, "inactive UI canvas: Unity does not export it either");
            else convertCanvas(idx, path, &into);
            return;
        }
    Object o;
    o.name = u.name;
    o.active = u.active;
    o.transform = u.t;
    const UComp *exp = nullptr, *mf = nullptr, *mr = nullptr;
    const UComp* skin = nullptr;
    std::vector<std::string> leftOut;
    for (const UComp& c : u.comps) {
        if (c.classId == 33) mf = &c;
        else if (c.classId == 23) mr = &c;
        else if (c.classId == 114 && c.cls == "PSXObjectExporter") exp = &c;
        else if (c.classId == 114 && c.cls == "PSXSkinnedObjectExporter") {
            if (!skin) skin = &c;
        } else if (c.classId == 137 && underSkinned(idx)) continue;  // drawn through the PSXSkinnedObjectExporter
        else if (c.classId == 114 && c.cls == "PSXSceneExporter") continue;  // the scene's settings
        else if (c.classId == 108) {
            const YNode& b = *c.body;
            const int type = int(b["m_Type"].integer(2));
            if (type > 2) {
                leftOut.push_back("area Light");
                continue;
            }
            LightComponent l;
            l.kind = type == 0 ? LightKind::Spot : type == 1 ? LightKind::Directional : LightKind::Point;
            const auto col = colorOf(b["m_Color"]);
            l.color = {col[0], col[1], col[2]};
            l.intensity = float(b["m_Intensity"].num(1));
            l.range = float(b["m_Range"].num(10));
            l.spotAngle = float(b["m_SpotAngle"].num(30));
            l.innerSpotAngle = float(b["m_InnerSpotAngle"].num(0));
            l.enabled = b["m_Enabled"].flag(true);
            // Realtime (4) or Mixed (1) point lights are runtime lights (PSXPointLightExporter.IsRuntime).
            const long long mode = b["m_Lightmapping"].integer(4);
            l.runtime = l.kind == LightKind::Point && (mode == 4 || mode == 1);
            o.light = l;
        } else if (c.classId == 114 && c.cls == "PSXAudioClip") {
            const YNode& b = *c.body;
            AudioComponent a;
            a.clipName = b["ClipName"].str();
            a.sampleRate = int(b["SampleRate"].integer(22050));
            a.loop = b["Loop"].flag(false);
            a.defaultVolume = int(b["DefaultVolume"].integer(100));
            a.trimLeadingSilence = b["TrimLeadingSilence"].flag(false);
            const Ref r = refOf(b["Clip"]);
            const fs::path f = m_index.file(r.guid);
            std::error_code ec;
            if (r.null()) {
            } else if (f.empty() || !fs::is_regular_file(f, ec)) {
                item(ReportItem::Missing, path, "audio clip (guid " + r.guid + ") is not in the project");
            } else if (!isAudioFile(f)) {
                item(ReportItem::LeftOut, path, "audio clip " + m_index.path(r.guid) + " is not a WAV, AIFF, MP3, Ogg Vorbis or FLAC file");
            } else {
                a.clip = copyAsset(f, "audio", nullptr);
            }
            o.audio = a;
        } else if (c.classId == 114 && (c.cls == "PSXSprite" || c.cls == "PSXTilemapRenderer")) {
            // FindObjectsByType skips inactive objects: their sheet or map is not exported.
            if (!activeInHierarchy(idx))
                item(ReportItem::LeftOut, path, c.cls + " on an inactive object: Unity does not export its " + (c.cls == "PSXSprite" ? "sheet" : "tilemap") + " either");
            else if (c.cls == "PSXSprite") sheetOf((*c.body)["sheet"], path);
            else tilemapOf(*c.body, path);
        } else if (c.classId == 114 && isUiClass(c.cls)) {
            item(ReportItem::LeftOut, path, "UI element (" + c.cls + ") outside a canvas: Unity does not export it either");
        } else if (c.classId == 114 && c.cls.starts_with("PSX")) {
            item(ReportItem::Later, path, laterWhat(c.cls));
            reportAssetsIn(*c.body);
        } else if (c.classId == 114) {
            leftOut.push_back(c.cls.empty() ? std::string("a script whose class is unknown") : "script " + c.cls);
        } else {
            const char* n = builtinClassName(c.classId);
            leftOut.push_back(n ? std::string(n) : "Unity component #" + std::to_string(c.classId));
        }
    }
    // Unity skips a PSXObjectExporter on or under a PSXSkinnedObjectExporter.
    const bool covered = underSkinned(idx);
    if (skin) {
        skinnedObject(idx, *skin, path, o);
    } else if (exp && !covered) {
        const YNode& b = *exp->body;
        auto field = [&](const char* now, const char* before) -> const YNode& {
            const YNode* n = b.find(now);
            if (!n) n = b.find(before);
            static const YNode null;
            return n ? *n : null;
        };
        o.startActive = field("isActive", "IsActive").flag(true);
        std::optional<MeshComponent> mc = meshOf(u, mf, mr, path);
        if (mc) {
            const long long bpp = field("bitDepth", "BitDepth").integer(8);
            mc->bitDepth = bpp == 4 ? BitDepth::Bpp4 : bpp == 15 || bpp == 16 ? BitDepth::Bpp16 : BitDepth::Bpp8;
            const long long vc = b["vertexColorMode"].integer(0);
            mc->vertexColors = vc == 1 ? VertexColorMode::Flat : vc == 2 ? VertexColorMode::Mesh : VertexColorMode::Baked;
            const YNode& fc = b["flatVertexColor"];
            if (!fc.isNull()) {
                const auto c = colorOf(fc, {128 / 255.f, 128 / 255.f, 128 / 255.f, 1});
                mc->flatColor = {uint8_t(std::lround(c[0] * 255)), uint8_t(std::lround(c[1] * 255)), uint8_t(std::lround(c[2] * 255))};
            }
            mc->smoothNormals = b["smoothNormals"].flag(true);
            const long long dl = b["dynamicLighting"].integer(0);
            mc->dynamicLighting = dl == 1 ? DynamicLighting::On : dl == 2 ? DynamicLighting::Off : dl == 3 ? DynamicLighting::Smooth
                                                                                                           : DynamicLighting::Auto;
            mc->uvOffsetMaterial = int(b["uvOffsetMaterial"].integer(0));
            o.mesh = std::move(mc);
        }
        const long long ct = field("collisionType", "collisionType").integer(0);
        const bool platform = b["isPlatform"].flag(false);
        if (ct != 0 || platform) {
            ColliderComponent cc;
            cc.kind = ct == 1 ? ColliderKind::Static : ct == 2 ? ColliderKind::Dynamic : ColliderKind::None;
            cc.platform = platform;
            o.collider = cc;
        }
        const std::string lua = luaOf(field("luaFile", "LuaFile"), path, "script");
        if (!lua.empty()) o.script = ScriptComponent{lua, {}};
    } else if (!covered && ((mf && !refOf((*mf->body)["m_Mesh"]).null()) || !u.modelMesh.empty())) {
        item(ReportItem::LeftOut, path, "has a mesh but no PSXObjectExporter, so Unity did not export it either; it is kept without its mesh");
    }
    if (!leftOut.empty()) {
        std::string list;
        for (const std::string& s : leftOut) list += (list.empty() ? "" : ", ") + s;
        item(ReportItem::LeftOut, path, list + " (no PS1 equivalent)");
    }
    ++m_report.objects;
    for (int ch : u.children) convertObject(ch, path, o.children);
    into.push_back(std::move(o));
}

void Importer::sceneSettings(const YNode& b, SceneSettings& st) {
    // Fields missing from the file take PSXSceneExporter's initialisers.
    st.gteScaling = float(b["GTEScaling"].num(100));
    st.sceneType = b["SceneType"].integer(0) == 1 ? SceneType::Interior : SceneType::Exterior;
    st.fog.enabled = b["FogEnabled"].flag(false);
    const auto fc = colorOf(b["FogColor"], {0.5f, 0.5f, 0.6f, 1});
    st.fog.color = {fc[0], fc[1], fc[2]};
    st.fog.density = int(b["FogDensity"].integer(5));
    st.networkId = b["SceneNetworkId"].str();
    st.streamWorld = b["StreamWorldGeometry"].flag(false);
    st.script = luaOf(b["SceneLuaFile"], "", "scene script");
    m_loading = refOf(b["LoadingScreenPrefab"]);  // converted once the scene's own canvases are in
    for (const char* what : {"Cutscenes", "Animations"}) {
        size_t n = 0;
        for (const YNode& c : b[what].seq) n += !refOf(c).null();
        if (n)
            item(ReportItem::Later, "",
                 std::to_string(n) + " " + (std::string(what) == "Cutscenes" ? (n == 1 ? "cutscene" : "cutscenes") : (n == 1 ? "animation" : "animations")) +
                     " not imported yet");
    }
}

Scene Importer::convert(const fs::path& sceneFile, const std::string& sceneName) {
    m_scene = sceneName;
    m_objs.clear();
    m_sceneAssetsReported.clear();
    m_fonts.clear();
    m_sheets.clear();
    m_sheetCells.clear();
    m_tilemapSeen = false;
    m_loading = {};
    Scene scene;
    m_out = &scene;
    std::vector<YDoc>& docs = keep(parseYaml(readText(sceneFile)));
    Unit u = buildUnit(docs, 0);
    // The scene exporter: its settings; its object goes when it holds nothing else.
    const UComp* exporter = nullptr;
    int exporterObj = -1;
    for (size_t i = 0; i < m_objs.size(); ++i)
        for (const UComp& c : m_objs[i].comps)
            if (c.classId == 114 && c.cls == "PSXSceneExporter") {
                if (!exporter) {
                    exporter = &c;
                    exporterObj = int(i);
                } else {
                    item(ReportItem::LeftOut, m_objs[i].name, "a second PSXSceneExporter; the first one's settings are used");
                }
            }
    if (exporter) sceneSettings(*exporter->body, scene.settings);
    else item(ReportItem::Changed, "", "the scene has no PSXSceneExporter; it gets the default scene settings");
    if (m_psxData) scene.settings.memCard = m_memCard;
    for (int r : u.roots) {
        if (r == exporterObj && m_objs[size_t(r)].children.empty() && m_objs[size_t(r)].comps.size() == 1) continue;
        convertObject(r, "", scene.objects);
    }
    if (!m_loading.null()) loadingScreen(m_loading);
    m_out = nullptr;
    return scene;
}

void Importer::readPsxData() {
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(m_unity / "Assets", fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it->path().extension() != ".asset" || !it->is_regular_file(ec)) continue;
        const std::string t = readText(it->path());
        if (t.find("b6e1524fb8b4b754e965d03e634658e6") != std::string::npos || t.find("RuntimeCode.PSXData\n") != std::string::npos)
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    if (found.empty()) return;
    m_psxData = true;
    const std::string saved = m_scene;
    m_scene.clear();
    if (found.size() > 1)
        item(ReportItem::Changed, "", "several PSXData assets; " + generic(found[0].lexically_relative(m_unity)) + " is used");
    for (const YDoc& d : parsedFile(found[0])) {
        if (d.classId != 114) continue;
        const YNode& b = d.body;
        m_memCard.enabled = b["MemCardEnabled"].flag(false);
        if (const YNode* n = b.find("MemCardRegion")) m_memCard.region = n->str();
        if (const YNode* n = b.find("MemCardProduct")) m_memCard.product = n->str();
        if (const YNode* n = b.find("MemCardTitle")) m_memCard.title = n->str();
        for (const YNode& icon : b["MemCardIcons"].seq) {
            const std::string t = textureOf(refOf(icon), "PSXData");
            if (!t.empty()) m_memCard.icons.push_back(t);
        }
        for (const YNode& a : b["ProhibitedAreas"].seq)
            m_areas.push_back(clampVramArea({int(a["X"].integer()), int(a["Y"].integer()), int(a["Width"].integer(1)),
                                             int(a["Height"].integer(1))}));
        if (b["OutputResolution"]["x"].num(320) != 320 || b["OutputResolution"]["y"].num(240) != 240)
            item(ReportItem::LeftOut, "PSXData", "the output resolution: psxsplash always draws 320x240");
        break;
    }
    m_scene = saved;
}

void Importer::finish(bool cdTracksGiven) {
    m_scene.clear();
    if (cdTracksGiven) return;
    for (const std::string& rel : m_luaFiles)
        if (readText(m_dest / fromUtf8(rel)).find("PlayCDDA") != std::string::npos) {
            item(ReportItem::Later, "",
                 "CD music: " + rel + " plays CD tracks, and Unity keeps its CD music list in the editor's preferences, "
                 "not in the project; give the tracks to the import (--cd-track) or add them under the Disc settings");
            break;
        }
}

bool isInside(const fs::path& child, const fs::path& parent) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec), p = fs::weakly_canonical(parent, ec);
    auto ci = c.begin(), pi = p.begin();
    for (; pi != p.end(); ++pi, ++ci)
        if (ci == c.end() || *ci != *pi) {
            if (pi != p.end() && pi->empty()) continue;  // a trailing separator
            return false;
        }
    return true;
}

}  // namespace

Scene convertScene(const fs::path& unityProject, const GuidIndex& index, const fs::path& scenePath, const fs::path& dest,
                   ImportReport& report) {
    Importer imp(unityProject, index, dest, report);
    imp.readPsxData();
    return imp.convert(scenePath, scenePath.stem().string());
}

ImportReport importProject(const fs::path& unityProject, const fs::path& dest, const ImportOptions& options) {
    ImportReport report;
    std::error_code ec;
    if (!fs::is_directory(unityProject / "Assets", ec)) {
        report.error = "this is not a Unity project (there is no Assets folder in " + generic(unityProject) + ")";
        return report;
    }
    if (dest.empty()) {
        report.error = "choose a folder for the new project";
        return report;
    }
    if (isInside(dest, unityProject)) {
        report.error = "the new project cannot go inside the Unity project; choose a folder outside it";
        return report;
    }
    if (fs::exists(dest, ec) && (!fs::is_directory(dest, ec) || !fs::is_empty(dest, ec))) {
        report.error = generic(dest) + " is not empty; choose a new or empty folder";
        return report;
    }
    const GuidIndex index = indexGuids(unityProject);
    const std::vector<SceneEntry> all = listScenes(unityProject, index);
    std::vector<SceneEntry> chosen;
    if (options.scenes.empty()) {
        chosen = defaultScenes(all);
        if (chosen.empty()) {
            report.error = "the Unity project has no scene with a PSXSceneExporter";
            return report;
        }
    } else {
        for (const std::string& want : options.scenes) {
            const SceneEntry* hit = nullptr;
            for (const SceneEntry& e : all)
                if (e.name == want || e.path == want || e.path == "Assets/" + want || e.name + ".unity" == want) {
                    hit = &e;
                    break;
                }
            if (!hit) {
                report.error = "no scene named " + want + " in the Unity project";
                return report;
            }
            if (std::none_of(chosen.begin(), chosen.end(), [&](const SceneEntry& e) { return e.path == hit->path; })) chosen.push_back(*hit);
        }
    }
    // CD tracks: numbered 2, 3, ... without a gap, each an audio file.
    std::vector<std::pair<int, std::string>> tracks = options.cdTracks;
    std::stable_sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < tracks.size(); ++i) {
        const int want = int(i) + 2;
        const auto& [n, file] = tracks[i];
        if (n < 2) {
            report.error = "CD track " + std::to_string(n) + ": music tracks start at 2 (track 1 is the data)";
            return report;
        }
        if (n != want) {
            report.error = n == want - 1 ? "CD track " + std::to_string(n) + " is given twice"
                                         : "CD track " + std::to_string(n) + " is given but not track " + std::to_string(want) +
                                               "; music tracks are numbered from 2 without a gap";
            return report;
        }
        const fs::path f = fromUtf8(file);
        if (!fs::is_regular_file(f, ec)) {
            report.error = "CD track " + std::to_string(n) + ": " + file + " is not a file";
            return report;
        }
        if (!isAudioFile(f)) {
            report.error = "CD track " + std::to_string(n) + ": " + file + " is not a WAV, AIFF, MP3, Ogg Vorbis or FLAC file";
            return report;
        }
    }
    fs::create_directories(dest, ec);
    if (ec) {
        report.error = "cannot create " + generic(dest) + ": " + ec.message();
        return report;
    }
    for (const char* sub : {"meshes", "textures", "scripts"}) fs::create_directories(dest / sub, ec);
    Importer imp(unityProject, index, dest, report);
    imp.readPsxData();
    std::set<std::string> names;
    std::string list;
    for (const SceneEntry& e : chosen) {
        Scene s;
        try {
            s = imp.convert(unityProject / fromUtf8(e.path), e.name);
        } catch (const std::exception& ex) {
            report.items.push_back({ReportItem::Missing, e.name, "", std::string("the scene could not be read: ") + ex.what()});
            continue;
        }
        std::string file = safeStem(e.name);
        for (int n = 2; names.count(lower(file)); ++n) file = safeStem(e.name) + "_" + std::to_string(n);
        names.insert(lower(file));
        file += ".scene";
        saveScene(s, dest / fromUtf8(file));
        report.scenes.push_back(file);
        list += file + "\n";
    }
    if (!tracks.empty()) {
        DiscSettings disc;
        for (const auto& t : tracks) disc.tracks.push_back(imp.copyMusic(fromUtf8(t.second)));
        writeDiscSettings(dest / "disc.json", disc);
    }
    imp.finish(!tracks.empty());
    {
        std::ofstream out(dest / "scenes.list", std::ios::binary);
        out << list;
    }
    if (!imp.vramAreas().empty()) {
        VramSettings vs;
        vs.prohibited = imp.vramAreas();
        writeVramSettings(dest / "vram.json", vs);
    }
    std::ofstream(dest / "import-report.txt", std::ios::binary) << report.text();
    return report;
}

}  // namespace splash::unity
