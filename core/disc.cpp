#include "disc.hh"

#include "audio.hh"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace splash {

namespace {

constexpr size_t kSector = 2048;
constexpr uint32_t kFirstFree = 18;  // after the system area, the PVD and the terminator
constexpr uint32_t kTrailingGap = 150;

struct Tables {
    uint8_t eccF[256], eccB[256];
    uint32_t edc[256];
    Tables() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t j = (i << 1) ^ ((i & 0x80) ? 0x11d : 0);
            eccF[i] = uint8_t(j);
            eccB[i ^ j] = uint8_t(i);
            uint32_t e = i;
            for (int k = 0; k < 8; k++) e = (e >> 1) ^ ((e & 1) ? 0xd8018001u : 0);
            edc[i] = e;
        }
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

uint32_t edc(const uint8_t* p, size_t n) {
    const Tables& t = tables();
    uint32_t e = 0;
    for (size_t i = 0; i < n; i++) e = (e >> 8) ^ t.edc[(e ^ p[i]) & 0xff];
    return e;
}

// ECMA-130 annex A: one parity block (P: 86 x 24, Q: 52 x 43) over the
// 2340 bytes that start at the header.
void eccBlock(const uint8_t* src, uint32_t majorCount, uint32_t minorCount, uint32_t majorMult, uint32_t minorInc,
              uint8_t* dest) {
    const Tables& t = tables();
    uint32_t size = majorCount * minorCount;
    for (uint32_t major = 0; major < majorCount; major++) {
        uint32_t index = (major >> 1) * majorMult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minorCount; minor++) {
            uint8_t v = src[index];
            index += minorInc;
            if (index >= size) index -= size;
            a ^= v;
            b ^= v;
            a = t.eccF[a];
        }
        a = t.eccB[t.eccF[a] ^ b];
        dest[major] = a;
        dest[major + majorCount] = a ^ b;
    }
}

uint8_t bcd(uint32_t v) { return uint8_t((v / 10) * 16 + v % 10); }

void le16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void be16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
void le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
void be32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (24 - 8 * i));
}
void both16(uint8_t* p, uint32_t v) {
    le16(p, v);
    be16(p + 2, v);
}
void both32(uint8_t* p, uint32_t v) {
    le32(p, v);
    be32(p + 4, v);
}
void padded(uint8_t* p, size_t n, const std::string& s) {
    std::memset(p, ' ', n);
    std::memcpy(p, s.data(), std::min(n, s.size()));
}

bool nameChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// 8.3 upper case, as the BIOS and psyqo match them.
void checkName(const std::string& name, bool dir) {
    size_t dot = name.find('.');
    std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    bool ok = !base.empty() && base.size() <= 8 && ext.size() <= 3 && (!dir || dot == std::string::npos) &&
              std::all_of(base.begin(), base.end(), nameChar) && std::all_of(ext.begin(), ext.end(), nameChar);
    if (!ok) throw std::runtime_error("Not a disc file name: " + name);
}

struct Entry {
    std::string id;  // as recorded: "NAME.EXT;1" or "DIR"
    bool dir = false;
    uint32_t lba = 0, size = 0;
};

struct Dir {
    std::string name;  // "" for the root
    std::vector<Entry> entries;
    uint32_t lba = 0, sectors = 1;
};

uint32_t recordSize(const std::string& id) { return uint32_t(33 + id.size() + ((id.size() & 1) ? 0 : 1)); }

void writeRecord(uint8_t* p, const std::string& id, uint32_t lba, uint32_t size, bool dir, const uint8_t* date) {
    uint32_t len = recordSize(id);
    std::memset(p, 0, len);
    p[0] = uint8_t(len);
    both32(p + 2, lba);
    both32(p + 10, size);
    std::memcpy(p + 18, date, 7);
    p[25] = dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = uint8_t(id.size());
    std::memcpy(p + 33, id.data(), id.size());
}

// Records of one directory, laid into sectors; a record never straddles one.
std::vector<uint8_t> dirBytes(const Dir& d, uint32_t parentLba, uint32_t parentSize, const uint8_t* date) {
    std::vector<uint8_t> out(d.sectors * kSector, 0);
    size_t at = 0;
    auto put = [&](const std::string& id, uint32_t lba, uint32_t size, bool dir) {
        uint32_t len = recordSize(id);
        if (at / kSector != (at + len - 1) / kSector) at = (at / kSector + 1) * kSector;
        writeRecord(&out[at], id, lba, size, dir, date);
        at += len;
    };
    put(std::string(1, '\0'), d.lba, d.sectors * kSector, true);
    put(std::string(1, '\1'), parentLba, parentSize, true);
    for (const Entry& e : d.entries) put(e.id, e.lba, e.size, e.dir);
    return out;
}

uint32_t sectorsFor(const Dir& d) {
    size_t at = 0;
    auto add = [&](uint32_t len) {
        if (at / kSector != (at + len - 1) / kSector) at = (at / kSector + 1) * kSector;
        at += len;
    };
    add(34);
    add(34);
    for (const Entry& e : d.entries) add(recordSize(e.id));
    return uint32_t((at + kSector - 1) / kSector);
}

std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + utf8(p));
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

}  // namespace

namespace {

// Sync, BCD header (lba + 150, mode 2) and the 8-byte subheader; the rest zero.
void mode2Header(uint8_t* out, uint32_t lba, const uint8_t* sub) {
    std::memset(out, 0, kRawSectorSize);
    std::memset(out + 1, 0xff, 10);
    uint32_t a = lba + 150;
    out[12] = bcd(a / 4500);
    out[13] = bcd((a / 75) % 60);
    out[14] = bcd(a % 75);
    out[15] = 2;
    std::memcpy(out + 16, sub, 8);
}

// EDC and P/Q parity of a Form 1 sector whose header, subheader and data are in.
void finishForm1(uint8_t* out) {
    le32(out + 24 + kSector, edc(out + 16, 8 + kSector));
    uint8_t address[4];
    std::memcpy(address, out + 12, 4);
    std::memset(out + 12, 0, 4);
    eccBlock(out + 12, 86, 24, 2, 86, out + 0x81c);
    eccBlock(out + 12, 52, 43, 86, 88, out + 0x8c8);
    std::memcpy(out + 12, address, 4);
}

constexpr size_t kForm2Data = 2324;

}  // namespace

void encodeForm2(uint8_t* out, uint32_t lba, const uint8_t* data, uint8_t submode) {
    const uint8_t sub[8] = {0, 0, submode, 0, 0, 0, submode, 0};
    mode2Header(out, lba, sub);
    if (data) std::memcpy(out + 24, data, kForm2Data);
    le32(out + 24 + kForm2Data, edc(out + 16, 8 + kForm2Data));
}

void encodeForm1(uint8_t* out, uint32_t lba, const uint8_t* data, uint8_t submode) {
    const uint8_t sub[8] = {0, 0, submode, 0, 0, 0, submode, 0};
    mode2Header(out, lba, sub);
    if (data) std::memcpy(out + 24, data, kSector);
    finishForm1(out);
}

namespace {

// A licence sector at `lba`, its subheader as the file has it.
void encodeLicence(uint8_t* out, uint32_t lba, const LicenceSector& s) {
    mode2Header(out, lba, s.sub);
    if (s.sub[2] & 0x20) {
        std::memcpy(out + 24, s.data, kForm2Data);
        le32(out + 24 + kForm2Data, edc(out + 16, 8 + kForm2Data));
    } else {
        std::memcpy(out + 24, s.data, kSector);
        finishForm1(out);
    }
}

const uint8_t kPsxsplashLicence[] = {
#include "licence_psxsplash.inc"
};

}  // namespace

std::vector<LicenceSector> parseLicence(const std::vector<uint8_t>& bytes, std::string& error) {
    struct Layout {
        size_t stride, skip;
        bool sub;
    };
    const Layout layouts[] = {{2336, 0, true}, {kRawSectorSize, 16, true}, {kSector, 0, false}};
    for (const Layout& l : layouts)
        for (size_t count : {size_t(12), size_t(16)}) {
            if (bytes.size() != l.stride * count) continue;
            std::vector<LicenceSector> out(count);
            for (size_t i = 0; i < count; i++) {
                const uint8_t* p = bytes.data() + i * l.stride + l.skip;
                LicenceSector& s = out[i];
                std::memset(s.data, 0, sizeof s.data);
                if (l.sub) {
                    std::memcpy(s.sub, p, 8);
                    std::memcpy(s.data, p + 8, l.stride - l.skip - 8 - 4);
                    // Form 1: past the 2048 data bytes is the old EDC/ECC.
                    if (!(s.sub[2] & 0x20)) std::memset(s.data + kSector, 0, kForm2Data - kSector);
                } else {
                    const uint8_t data[8] = {0, 0, kSubmodeData, 0, 0, 0, kSubmodeData, 0};
                    std::memcpy(s.sub, data, 8);
                    std::memcpy(s.data, p, kSector);
                }
            }
            return out;
        }
    error = "A licence file of " + std::to_string(bytes.size()) +
            " bytes; expected 12 or 16 sectors of 2336 bytes (28032, 37376), 2352 bytes (28224, 37632) "
            "or 2048 bytes (24576, 32768)";
    return {};
}

const std::vector<uint8_t>& psxsplashLicence() {
    static const std::vector<uint8_t> bytes(std::begin(kPsxsplashLicence), std::end(kPsxsplashLicence));
    return bytes;
}

DiscSettings readDiscSettings(const fs::path& file) {
    DiscSettings s;
    if (file.empty()) return s;
    std::ifstream in(file, std::ios::binary);
    if (!in) return s;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return s;
    auto str = [&](const char* key, std::string& v) {
        auto it = j.find(key);
        if (it != j.end() && it->is_string()) v = it->get<std::string>();
    };
    str("volume", s.volume);
    str("licence", s.licence);
    auto tr = j.find("tracks");
    if (tr != j.end() && tr->is_array())
        for (const auto& t : *tr)
            if (t.is_string()) s.tracks.push_back(t.get<std::string>());
    auto logo = j.find("bootLogo");
    if (logo != j.end() && logo->is_boolean()) s.bootLogo = logo->get<bool>();
    return s;
}

bool writeDiscSettings(const fs::path& file, const DiscSettings& s) {
    if (file.empty()) return false;
    nlohmann::json j = nlohmann::json::object();
    j["volume"] = s.volume;
    j["licence"] = s.licence;
    if (!s.tracks.empty()) j["tracks"] = s.tracks;  // written only when there is music
    if (!s.bootLogo) j["bootLogo"] = false;  // written only when it is off
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << '\n';
    out.close();
    return !out.fail();
}

std::string volumeName(const std::string& from) {
    std::string v;
    for (char ch : from) {
        char u = (ch >= 'a' && ch <= 'z') ? char(ch - 32) : ch;
        if (nameChar(u) && v.size() < 31) v += u;
    }
    return v;
}

std::string discVolume(const std::string& volume, const std::string& title) {
    std::string v = volumeName(volume);
    if (v.empty()) v = volumeName(title);
    return v.empty() ? "PSXSPLASH" : v;
}

std::vector<LicenceSector> resolveLicence(const std::string& licence, const fs::path& projectRoot, std::string& error) {
    if (licence.empty()) return {};
    std::vector<uint8_t> bytes;
    std::string name = licence;
    if (licence == "psxsplash") {
        bytes = psxsplashLicence();
    } else {
        fs::path p = fs::path(std::u8string(licence.begin(), licence.end()));
        if (p.is_relative() && !projectRoot.empty()) p = projectRoot / p;
        name = utf8(p);
        try {
            bytes = readFile(p);
        } catch (const std::exception& e) {
            error = std::string("Licence: ") + e.what();
            return {};
        }
    }
    std::string why;
    std::vector<LicenceSector> out = parseLicence(bytes, why);
    if (out.empty()) error = name + ": " + why;
    return out;
}

std::string cueTime(uint32_t lba) {
    char b[16];
    std::snprintf(b, sizeof b, "%02u:%02u:%02u", unsigned(lba / (60 * kSectorsPerSecond)),
                  unsigned((lba / kSectorsPerSecond) % 60), unsigned(lba % kSectorsPerSecond));
    return b;
}

DiscInfo writeDiscImage(const std::vector<DiscFile>& files, const std::string& volume, const fs::path& bin,
                        const std::vector<LicenceSector>& licence, const std::vector<std::vector<int16_t>>& tracks) {
    if (tracks.size() + 1 > kMaxDiscTracks)
        throw std::runtime_error("A disc holds at most " + std::to_string(kMaxDiscTracks) + " tracks; this one has " +
                                 std::to_string(tracks.size() + 1) + " (the data and " + std::to_string(tracks.size()) +
                                 " music)");
    // Directories: the root plus one level, which is all the loader uses.
    std::map<std::string, Dir> dirs;
    dirs[""];
    struct Placed {
        const DiscFile* file;
        std::string dir;
        size_t entry;
    };
    std::vector<Placed> placed;
    for (const DiscFile& f : files) {
        size_t slash = f.path.find('/');
        std::string dir = slash == std::string::npos ? "" : f.path.substr(0, slash);
        std::string name = slash == std::string::npos ? f.path : f.path.substr(slash + 1);
        if (name.find('/') != std::string::npos) throw std::runtime_error("Too deep for the disc: " + f.path);
        checkName(name, false);
        if (!dir.empty()) {
            checkName(dir, true);
            if (!dirs.count(dir)) {
                dirs[dir].name = dir;
                dirs[""].entries.push_back({dir, true});
            }
        }
        for (const Entry& e : dirs[dir].entries)
            if (e.id == name + ";1") throw std::runtime_error("Two files named " + f.path);
        dirs[dir].entries.push_back({name + ";1", false, 0, uint32_t(f.data.size())});
        placed.push_back({&f, dir, 0});
    }
    // ISO 9660 orders records by name (directories sort among the files).
    for (auto& [n, d] : dirs) {
        std::sort(d.entries.begin(), d.entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
        d.sectors = sectorsFor(d);
    }
    for (Placed& p : placed) {
        std::string name = p.file->path.substr(p.dir.empty() ? 0 : p.dir.size() + 1) + ";1";
        auto& es = dirs[p.dir].entries;
        p.entry = size_t(std::find_if(es.begin(), es.end(), [&](const Entry& e) { return e.id == name; }) - es.begin());
    }

    // Layout: path tables, directories (root first), then file data.
    std::vector<std::string> order = {""};
    for (auto& [n, d] : dirs)
        if (!n.empty()) order.push_back(n);
    std::vector<uint8_t> pathTable;
    for (size_t i = 0; i < order.size(); i++) {
        std::string id = order[i].empty() ? std::string(1, '\0') : order[i];
        pathTable.push_back(uint8_t(id.size()));
        pathTable.push_back(0);
        pathTable.resize(pathTable.size() + 6);
        pathTable.insert(pathTable.end(), id.begin(), id.end());
        if (id.size() & 1) pathTable.push_back(0);
    }
    uint32_t tableSectors = uint32_t((pathTable.size() + kSector - 1) / kSector);
    uint32_t lTable = kFirstFree, mTable = lTable + tableSectors;
    uint32_t next = mTable + tableSectors;
    for (const std::string& n : order) {
        dirs[n].lba = next;
        next += dirs[n].sectors;
    }
    for (auto& [n, d] : dirs)
        for (Entry& e : d.entries)
            if (e.dir) {
                e.lba = dirs[e.id].lba;
                e.size = dirs[e.id].sectors * kSector;
            }
    for (const Placed& p : placed) {
        Entry& e = dirs[p.dir].entries[p.entry];
        e.lba = next;
        next += std::max<uint32_t>(1, uint32_t((e.size + kSector - 1) / kSector));
    }
    uint32_t total = next + kTrailingGap;

    // Path tables: little-endian copy then big-endian copy.
    std::vector<uint8_t> lt = pathTable, mt = pathTable;
    for (size_t i = 0, at = 0; i < order.size(); i++) {
        uint32_t parent = 1;
        le32(&lt[at + 2], dirs[order[i]].lba);
        le16(&lt[at + 6], parent);
        be32(&mt[at + 2], dirs[order[i]].lba);
        be16(&mt[at + 6], parent);
        at += 8 + lt[at] + (lt[at] & 1);
    }
    lt.resize(tableSectors * kSector);
    mt.resize(tableSectors * kSector);

    // Recording date, in the 7-byte directory form and the 17-byte PVD form.
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    uint8_t date7[7] = {uint8_t(tm.tm_year), uint8_t(tm.tm_mon + 1), uint8_t(tm.tm_mday), uint8_t(tm.tm_hour),
                        uint8_t(tm.tm_min), uint8_t(tm.tm_sec), 0};
    char date17[40];
    std::snprintf(date17, sizeof date17, "%04d%02d%02d%02d%02d%02d00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    const char noDate[17] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', 0};

    const Dir& root = dirs[""];
    std::array<uint8_t, kSector> pvd{};
    pvd[0] = 1;
    std::memcpy(&pvd[1], "CD001", 5);
    pvd[6] = 1;
    padded(&pvd[8], 32, "PLAYSTATION");
    padded(&pvd[40], 32, volume);
    both32(&pvd[80], total);
    both16(&pvd[120], 1);
    both16(&pvd[124], 1);
    both16(&pvd[128], kSector);
    both32(&pvd[132], uint32_t(pathTable.size()));
    le32(&pvd[140], lTable);
    be32(&pvd[148], mTable);
    writeRecord(&pvd[156], std::string(1, '\0'), root.lba, root.sectors * kSector, true, date7);
    padded(&pvd[190], 128, volume);
    padded(&pvd[318], 128, "");
    padded(&pvd[446], 128, "");
    padded(&pvd[574], 128, "PLAYSTATION");
    padded(&pvd[702], 37, "");
    padded(&pvd[739], 37, "");
    padded(&pvd[776], 37, "");
    std::memcpy(&pvd[813], date17, 17);
    std::memcpy(&pvd[830], date17, 17);
    std::memcpy(&pvd[847], noDate, 17);
    std::memcpy(&pvd[864], noDate, 17);
    pvd[881] = 1;
    std::array<uint8_t, kSector> term{};
    term[0] = 255;
    std::memcpy(&term[1], "CD001", 5);
    term[6] = 1;

    std::error_code ec;
    if (bin.has_parent_path()) fs::create_directories(bin.parent_path(), ec);
    std::ofstream out(bin, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write " + utf8(bin));
    uint8_t raw[kRawSectorSize];
    uint32_t lba = 0;
    auto emit = [&](const uint8_t* data, uint8_t submode) {
        encodeForm1(raw, lba++, data, submode);
        out.write(reinterpret_cast<const char*>(raw), kRawSectorSize);
    };
    // A run of sectors; the last one closes the record and the file.
    auto emitRun = [&](const uint8_t* data, size_t bytes, uint32_t sectors) {
        uint8_t buf[kSector];
        for (uint32_t s = 0; s < sectors; s++) {
            std::memset(buf, 0, kSector);
            size_t off = size_t(s) * kSector;
            if (off < bytes) std::memcpy(buf, data + off, std::min(kSector, bytes - off));
            emit(buf, uint8_t(kSubmodeData | (s + 1 == sectors ? kSubmodeEor | kSubmodeEof : 0)));
        }
    };
    if (licence.empty()) {
        for (; lba < 16;) emit(nullptr, kSubmodeData);
    } else {
        // The licence, then blank Form 2 up to the PVD (mkpsxiso's LBA 12-15).
        for (; lba < 16; lba++) {
            if (lba < licence.size())
                encodeLicence(raw, lba, licence[lba]);
            else
                encodeForm2(raw, lba, nullptr, 0x20);
            out.write(reinterpret_cast<const char*>(raw), kRawSectorSize);
        }
    }
    emit(pvd.data(), kSubmodeData | kSubmodeEor);
    emit(term.data(), kSubmodeData | kSubmodeEor | kSubmodeEof);
    emitRun(lt.data(), lt.size(), tableSectors);
    emitRun(mt.data(), mt.size(), tableSectors);
    for (const std::string& n : order) {
        const Dir& d = dirs[n];
        std::vector<uint8_t> b = dirBytes(d, root.lba, root.sectors * kSector, date7);
        emitRun(b.data(), b.size(), d.sectors);
    }
    for (const Placed& p : placed) {
        const Entry& e = dirs[p.dir].entries[p.entry];
        if (lba != e.lba) throw std::runtime_error("disc layout drifted at " + p.file->path);
        emitRun(p.file->data.data(), p.file->data.size(),
                std::max<uint32_t>(1, uint32_t((e.size + kSector - 1) / kSector)));
    }
    while (lba < total) emit(nullptr, 0);

    // CD-DA tracks: a 150-sector silent pregap, then the PCM, last sector padded.
    DiscInfo info;
    info.dataSectors = total;
    std::memset(raw, 0, kRawSectorSize);
    for (const std::vector<int16_t>& pcm : tracks) {
        for (uint32_t s = 0; s < kAudioPregap; s++, lba++) out.write(reinterpret_cast<const char*>(raw), kRawSectorSize);
        const size_t bytes = pcm.size() * 2;
        const uint32_t sectors = uint32_t((bytes + kRawSectorSize - 1) / kRawSectorSize);
        info.tracks.push_back({lba, sectors});
        uint8_t buf[kRawSectorSize];
        for (uint32_t s = 0; s < sectors; s++, lba++) {
            std::memset(buf, 0, kRawSectorSize);
            const size_t first = size_t(s) * (kRawSectorSize / 2);
            const size_t n = std::min(kRawSectorSize / 2, pcm.size() - first);
            for (size_t k = 0; k < n; k++) le16(buf + 2 * k, uint16_t(pcm[first + k]));
            out.write(reinterpret_cast<const char*>(buf), kRawSectorSize);
        }
    }
    info.sectors = lba;
    out.close();
    if (!out) throw std::runtime_error("Cannot write " + utf8(bin));

    fs::path cue = bin;
    cue.replace_extension(".cue");
    std::ofstream c(cue, std::ios::binary | std::ios::trunc);
    c << "FILE \"" << utf8(bin.filename()) << "\" BINARY\r\n"
      << "  TRACK 01 MODE2/2352\r\n"
      << "    INDEX 01 00:00:00\r\n";
    for (size_t i = 0; i < info.tracks.size(); i++) {
        char num[8];
        std::snprintf(num, sizeof num, "%02u", unsigned(i + 2));
        c << "  TRACK " << num << " AUDIO\r\n"
          << "    INDEX 00 " << cueTime(info.tracks[i].start - kAudioPregap) << "\r\n"
          << "    INDEX 01 " << cueTime(info.tracks[i].start) << "\r\n";
    }
    if (!c) throw std::runtime_error("Cannot write " + utf8(cue));
    return info;
}

EngineKind engineKind(const std::vector<uint8_t>& exe) {
    if (exe.size() < 2048 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) return EngineKind::NotExe;
    auto has = [&](const char* s) {
        size_t n = std::strlen(s);
        return std::search(exe.begin(), exe.end(), s, s + n) != exe.end();
    };
    // Each loader names its files differently (psxsplash src/fileloader.cpp).
    if (has("scene_%d.splashpack")) return EngineKind::Pcdrv;
    return EngineKind::Cdrom;
}

std::string sceneTag(size_t index, size_t count, const std::string& name) {
    if (count <= 1) return std::string();
    std::string tag = "scene " + std::to_string(index);
    if (!name.empty()) tag += " (" + name + ")";
    return tag + ": ";
}

DiscResult exportDisc(const std::vector<const Scene*>& scenes, const fs::path& projectRoot, const fs::path& engine,
                      const fs::path& bin, const std::string& title, const std::vector<std::string>& names,
                      const VramSettings& vram, const DiscSettings& disc) {
    DiscResult r;
    if (scenes.empty()) {
        r.exported.errors.push_back("No scene to put on the disc.");
        return r;
    }
    std::vector<uint8_t> exe;
    try {
        exe = readFile(engine);
    } catch (const std::exception& e) {
        r.exported.errors.push_back(e.what());
        return r;
    }
    switch (engineKind(exe)) {
        case EngineKind::NotExe:
            r.exported.errors.push_back(utf8(engine) + " is not a PS-X EXE.");
            return r;
        case EngineKind::Pcdrv:
            r.exported.errors.push_back(utf8(engine.filename()) +
                                        " is the PCdrv build of psxsplash; a disc needs the CD-ROM build (LOADER=cdrom)");
            return r;
        case EngineKind::Cdrom:
            break;
    }
    std::string licenceError;
    const std::vector<LicenceSector> licence = resolveLicence(disc.licence, projectRoot, licenceError);
    if (!licenceError.empty()) {
        r.exported.errors.push_back(licenceError);
        return r;
    }
    // Music: CD-DA tracks 2 and up.
    std::vector<std::vector<int16_t>> music;
    for (size_t i = 0; i < disc.tracks.size(); i++) {
        const std::string& t = disc.tracks[i];
        fs::path p = fs::path(std::u8string(t.begin(), t.end()));
        if (p.is_relative() && !projectRoot.empty()) p = projectRoot / p;
        const std::string tag = "music track " + std::to_string(i + 2) + ": ";
        std::error_code fe;
        if (!fs::is_regular_file(p, fe)) {
            r.exported.errors.push_back(tag + utf8(p) + ": no such file");
            continue;
        }
        try {
            music.push_back(loadCdAudio(p));
        } catch (const std::exception& e) {
            r.exported.errors.push_back(tag + e.what());
        }
    }
    if (!r.exported.errors.empty()) return r;

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / ("splashedit-disc-" + std::to_string(std::time(nullptr)) + "-" +
                                                 std::to_string(reinterpret_cast<uintptr_t>(&r)));
    fs::create_directories(tmp, ec);
    if (ec) {
        r.exported.errors.push_back("Cannot create " + utf8(tmp) + ": " + ec.message());
        return r;
    }
    try {
        ExportOptions opt;
        opt.luaBytecode = true;
        opt.vram = vram;
        const char* cnf = "BOOT = cdrom:\\PSX.EXE;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n";
        std::vector<DiscFile> files;
        files.push_back({"SYSTEM.CNF", std::vector<uint8_t>(cnf, cnf + std::strlen(cnf))});
        files.push_back({"PSX.EXE", std::move(exe)});
        for (size_t i = 0; i < scenes.size(); ++i) {
            const std::string tag = sceneTag(i, scenes.size(), i < names.size() ? names[i] : std::string());
            const fs::path spk = tmp / ("scene_" + std::to_string(i) + ".splashpack");
            ExportResult one;
            try {
                // Prefab instances filled in from their files as they are now.
                Scene scene = *scenes[i];
                one.errors = resolvePrefabs(scene, projectRoot);
                if (one.ok()) one = exportSplashpack(scene, projectRoot, spk, opt);
                if (one.ok() && !scene.settings.loadingCanvas.empty()) {
                    ExportResult lr = exportLoaderPack(scene, projectRoot,
                                                       fs::path(spk).replace_extension(".loading"), opt);
                    one.errors.insert(one.errors.end(), lr.errors.begin(), lr.errors.end());
                    one.warnings.insert(one.warnings.end(), lr.warnings.begin(), lr.warnings.end());
                }
            } catch (const std::exception& e) {
                one.errors.push_back(e.what());
            }
            for (const std::string& m : one.errors) r.exported.errors.push_back(tag + m);
            for (const std::string& m : one.warnings) r.exported.warnings.push_back(tag + m);
            if (i == 0) r.exported.stats = std::move(one.stats);
            if (!one.errors.empty()) continue;
            const std::string dir = "SCENE" + std::to_string(i) + "/SCENE_" + std::to_string(i);
            fs::path part = spk;
            files.push_back({dir + ".SPK", readFile(spk)});
            files.push_back({dir + ".VRM", readFile(part.replace_extension(".vram"))});
            files.push_back({dir + ".SPU", readFile(part.replace_extension(".spu"))});
            // World streaming reads its regions from here while the game runs.
            if (fs::exists(part.replace_extension(".geo"))) files.push_back({dir + ".GEO", readFile(part)});
            // The loading screen psxsplash draws while this scene loads.
            if (fs::exists(part.replace_extension(".loading"))) files.push_back({dir + ".LDG", readFile(part)});
        }
        if (r.exported.errors.empty()) {
            // The length check needs the layout; write, then drop an over-long image.
            r.disc = writeDiscImage(files, discVolume(disc.volume, title), bin, licence, music);
            const double minutes = double(r.disc.sectors) / (60.0 * kSectorsPerSecond);
            char len[32];
            std::snprintf(len, sizeof len, "%.1f", minutes);
            if (r.disc.sectors > kDiscMaxSectors) {
                r.exported.errors.push_back("The disc is " + std::string(len) +
                                            " minutes long; a CD holds at most 80 minutes (remove music tracks)");
                fs::path cue = bin;
                cue.replace_extension(".cue");
                fs::remove(bin, ec);
                fs::remove(cue, ec);
                r.disc = {};
            } else if (r.disc.sectors > kDiscWarnSectors) {
                r.exported.warnings.push_back("The disc is " + std::string(len) +
                                              " minutes long; past 74 minutes it may not fit every CD-R");
            }
        }
    } catch (const std::exception& e) {
        r.exported.errors.push_back(e.what());
    }
    fs::remove_all(tmp, ec);
    return r;
}

DiscResult exportDisc(const Scene& scene, const fs::path& projectRoot, const fs::path& engine, const fs::path& bin,
                      const std::string& title, const VramSettings& vram, const DiscSettings& disc) {
    return exportDisc(std::vector<const Scene*>{&scene}, projectRoot, engine, bin, title, {}, vram, disc);
}

}  // namespace splash
