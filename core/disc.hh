// Disc images: an ISO 9660 filesystem in raw Mode 2 Form 1 sectors (.bin)
// plus its .cue, the form a PS1 boots from and a burner writes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "splashpack.hh"

namespace splash {

constexpr size_t kRawSectorSize = 2352;

// XA subheader submode bits.
constexpr uint8_t kSubmodeEor = 0x01, kSubmodeData = 0x08, kSubmodeEof = 0x80;

// One raw sector at `lba`: sync, BCD header (lba + 150), subheader written
// twice, 2048 bytes of `data`, EDC, then P and Q parity over the sector with
// its address zeroed.
void encodeForm1(uint8_t* out, uint32_t lba, const uint8_t* data, uint8_t submode);
// The same as Form 2: subheader with `submode` twice, 2324 bytes of `data`
// (zeros when null), EDC over the subheader and data at raw 2348, no ECC.
void encodeForm2(uint8_t* out, uint32_t lba, const uint8_t* data, uint8_t submode);

// One sector of a disc licence (the system area, LBA 0-15): its 8-byte XA
// subheader as the file holds it, and its data. Form 2 (sub[2] & 0x20) uses
// all 2324 bytes, Form 1 the first 2048.
struct LicenceSector {
    uint8_t sub[8];
    uint8_t data[2324];
};

// Splits a licence file into 12 or 16 sectors. Accepted sizes: 12 or 16
// sectors of 2336 bytes (mkpsxiso/PsyQ: subheader, then the data; the rest is
// EDC/ECC, recomputed), of 2352 bytes (raw .bin sectors: the 16-byte sync and
// header are skipped, then as 2336), or of 2048 bytes (data only, as
// psximager's .sys: each gets the Form 1 data subheader 00 00 08 00 00 00 08
// 00). Any other size returns no sectors and sets `error`, naming the size.
std::vector<LicenceSector> parseLicence(const std::vector<uint8_t>& bytes, std::string& error);

// The "psxsplash" licence: Unity splashedit's SPLASHLICENSE.DAT (MIT,
// psxsplash; 12 x 2336 bytes), its "Created with psxsplash" text at LBA 4.
const std::vector<uint8_t>& psxsplashLicence();

// A project's disc settings file (disc.json): {"volume": "...", "licence":
// "..."}. `volume` empty = the volume name comes from the title. `licence`
// "" = none, "psxsplash" = psxsplashLicence(), else a licence file (relative
// paths against the project root). A missing or unreadable file, or a value
// that is not a string, reads as the defaults. writeDiscSettings returns
// false when the file cannot be written.
struct DiscSettings {
    std::string volume;
    std::string licence;
};
DiscSettings readDiscSettings(const std::filesystem::path& file);
bool writeDiscSettings(const std::filesystem::path& file, const DiscSettings& s);

// The licence sectors `licence` names (as DiscSettings::licence): none for
// "", else parseLicence of the file or the built-in bytes. Sets `error` (and
// returns none) when the file cannot be read or has a bad size.
std::vector<LicenceSector> resolveLicence(const std::string& licence, const std::filesystem::path& projectRoot,
                                          std::string& error);

// A file on the disc. `path` is "NAME.EXT" or "DIR/NAME.EXT", upper case, no
// ";1": the writer adds the version.
struct DiscFile {
    std::string path;
    std::vector<uint8_t> data;
};

struct DiscInfo {
    uint32_t sectors = 0;  // every sector in the .bin, trailing gap included
};

// Writes `bin` and a .cue next to it. With no `licence`, sectors 0-15 are
// empty Form 1 data sectors. Otherwise sector i < licence.size() is
// licence[i] with its subheader kept: Form 2 EDC when sub[2] & 0x20, else
// Form 1 EDC/ECC; the rest up to 15 are blank Form 2 (subheader 00 00 20 00
// 00 00 20 00, EDC), as mkpsxiso writes LBA 12-15. At most 16 licence
// sectors are used. Throws std::runtime_error on a bad name or a write error.
DiscInfo writeDiscImage(const std::vector<DiscFile>& files, const std::string& volume, const std::filesystem::path& bin,
                        const std::vector<LicenceSector>& licence = {});

// A bootable disc: SYSTEM.CNF, the engine as PSX.EXE, and each scene's files
// under SCENE<i>/ as SCENE_<i>.SPK, .VRM, .SPU (and .GEO when it streams),
// where psxsplash's CD-ROM loader opens them; scene 0 is the one the game
// starts in and Lua's Scene.Load(i) opens scene i. `engine` must be a
// LOADER=cdrom build. The volume name is `disc.volume`, or `title` when that
// is empty (letters, digits and _ kept, upper-cased, at most 31; PSXSPLASH
// when nothing is left). `disc.licence` goes into sectors 0-15 (see
// resolveLicence and writeDiscImage); a licence that cannot be read or has a
// bad size is an error. Errors land in the result; with more than one scene,
// each error and warning starts with its scene's index and, when `names`
// gives one, its name ("scene 1 (cave): ..."). No image is written when any
// scene fails. The stats are scene 0's. Every scene packs VRAM with `vram`
// (the app passes the project's vram.json, see readVramSettings); the cli
// passes the project's disc.json as `disc`.
struct DiscResult {
    ExportResult exported;
    DiscInfo disc;
};
DiscResult exportDisc(const std::vector<const Scene*>& scenes, const std::filesystem::path& projectRoot,
                      const std::filesystem::path& engine, const std::filesystem::path& bin, const std::string& title,
                      const std::vector<std::string>& names = {}, const VramSettings& vram = {},
                      const DiscSettings& disc = {});
// The same for one scene, as scene 0.
DiscResult exportDisc(const Scene& scene, const std::filesystem::path& projectRoot, const std::filesystem::path& engine,
                      const std::filesystem::path& bin, const std::string& title, const VramSettings& vram = {},
                      const DiscSettings& disc = {});

// What the engine file is: a CD-ROM build, a PCdrv build, or not a PS-X EXE.
enum class EngineKind { NotExe, Cdrom, Pcdrv };
EngineKind engineKind(const std::vector<uint8_t>& exe);

}  // namespace splash
