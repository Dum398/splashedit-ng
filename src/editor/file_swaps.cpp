#include "editor/file_swaps.hh"

#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

namespace editor {

namespace {

std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void put(const fs::path& path, const std::optional<std::string>& bytes) {
    std::error_code ec;
    if (bytes) writeBytes(path, *bytes);
    else fs::remove(path, ec);
}

}  // namespace

bool readBytes(const fs::path& file, std::string* out) {
    FILE* f = std::fopen(utf8(file).c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out->append(buf, n);
    std::fclose(f);
    return true;
}

bool writeBytes(const fs::path& file, const std::string& text) {
    const fs::path tmp = file.parent_path() / fromUtf8("." + utf8(file.filename()) + ".writing");
    FILE* f = std::fopen(utf8(tmp).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) return false;
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

void FileSwapsCommand::apply(Document& doc) {
    if (!m_first)
        for (const File& f : files) put(f.path, f.after);
    m_first = false;
    doc.refreshAssets(recheck);
}

void FileSwapsCommand::revert(Document& doc) {
    for (auto it = files.rbegin(); it != files.rend(); ++it) put(it->path, it->before);
    doc.refreshAssets(recheck);
}

}  // namespace editor
