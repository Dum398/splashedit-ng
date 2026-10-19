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
// "...", "tracks": ["...", ...]}. `volume` empty = the volume name comes from
// the title. `licence` "" = none, "psxsplash" = psxsplashLicence(), else a
// licence file (relative paths against the project root). `tracks` are the
// CD-DA music tracks in disc order, audio files (isAudioFile), relative paths
// against the project root with '/' separators: the first is disc track 2
// (track 1 is the data), which psxsplash's Audio.PlayCDDA(2) plays. A missing
// or unreadable file, or a value that is not a string, reads as the defaults;
// a "tracks" entry that is not a string is skipped. "bootLogo": false = the
// disc boots the psxsplash build without the boot logo (the editor picks
// psxsplash-cdrom-nologo); missing or not a bool reads as true, and it is
// written only when false. writeDiscSettings returns false when the file
// cannot be written.
struct DiscSettings {
    std::string volume;
    std::string licence;
    std::vector<std::string> tracks;
    bool bootLogo = true;
};
DiscSettings readDiscSettings(const std::filesystem::path& file);
bool writeDiscSettings(const std::filesystem::path& file, const DiscSettings& s);

// `from` as a volume name: letters upper-cased, everything but A-Z 0-9 _
// dropped, at most 31 characters (possibly empty).
std::string volumeName(const std::string& from);
// The volume name a disc gets: volumeName(volume), or volumeName(title) when
// that is empty, or PSXSPLASH when both are.
std::string discVolume(const std::string& volume, const std::string& title);

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

// A CD-DA track as written: `start` is its INDEX 01 LBA (after its 150-sector
// pregap), `sectors` its PCM sectors (the pregap not counted).
struct DiscTrack {
    uint32_t start = 0;
    uint32_t sectors = 0;
};

struct DiscInfo {
    uint32_t sectors = 0;      // every sector in the .bin: data, trailing gap, pregaps and audio
    uint32_t dataSectors = 0;  // the data track (track 1), trailing gap included
    std::vector<DiscTrack> tracks;  // disc tracks 2, 3, ...
};

// CD-DA: 2352 bytes of 16-bit stereo per sector, 75 sectors a second.
constexpr uint32_t kAudioPregap = 150;
constexpr size_t kCdFramesPerSector = kRawSectorSize / 4;  // 588 stereo frames
constexpr uint32_t kSectorsPerSecond = 75;
// A disc past 74 minutes may not fit a CD-R; past 80 it does not.
constexpr uint32_t kDiscWarnSectors = 74 * 60 * kSectorsPerSecond;  // 333000
constexpr uint32_t kDiscMaxSectors = 80 * 60 * kSectorsPerSecond;   // 360000
// At most this many tracks on a disc, data included.
constexpr size_t kMaxDiscTracks = 99;

// `lba` as a cue sheet's mm:ss:ff (75 frames a second, from the file start).
std::string cueTime(uint32_t lba);

// Writes `bin` and a .cue next to it. `tracks` are CD-DA audio tracks
// (interleaved stereo 44100 Hz 16-bit PCM, as loadCdAudio), disc tracks 2 and
// up: after the data track's last sector (its trailing gap included) each is
// 150 zero sectors of pregap then its PCM as raw 2352-byte sectors, the last
// one zero-padded; the cue gives each "TRACK nn AUDIO" with INDEX 00 at the
// pregap and INDEX 01 at the PCM. The ISO volume space size stays the data
// track's. More than 98 tracks throw. With no tracks the image is as before.
// With no `licence`, sectors 0-15 are
// empty Form 1 data sectors. Otherwise sector i < licence.size() is
// licence[i] with its subheader kept: Form 2 EDC when sub[2] & 0x20, else
// Form 1 EDC/ECC; the rest up to 15 are blank Form 2 (subheader 00 00 20 00
// 00 00 20 00, EDC), as mkpsxiso writes LBA 12-15. At most 16 licence
// sectors are used. Throws std::runtime_error on a bad name or a write error.
DiscInfo writeDiscImage(const std::vector<DiscFile>& files, const std::string& volume, const std::filesystem::path& bin,
                        const std::vector<LicenceSector>& licence = {},
                        const std::vector<std::vector<int16_t>>& tracks = {});

// A bootable disc: SYSTEM.CNF, the engine as PSX.EXE, and each scene's files
// under SCENE<i>/ as SCENE_<i>.SPK, .VRM, .SPU (and .GEO when it streams),
// where psxsplash's CD-ROM loader opens them; scene 0 is the one the game
// starts in and Lua's Scene.Load(i) opens scene i. `engine` must be a
// LOADER=cdrom build. The volume name is `disc.volume`, or `title` when that
// is empty (letters, digits and _ kept, upper-cased, at most 31; PSXSPLASH
// when nothing is left). `disc.licence` goes into sectors 0-15 (see
// resolveLicence and writeDiscImage); a licence that cannot be read or has a
// bad size is an error. `disc.tracks` become CD-DA tracks 2 and up (paths
// relative to `projectRoot`, decoded with loadCdAudio); one that is missing
// or cannot be decoded is the error "music track <N>: <why>" (N its disc
// track number, 2 for the first). A disc longer than 74 minutes (333000
// sectors) is a warning naming its length in minutes; longer than 80
// (360000) is an error and no image is written. Errors land in the result;
// with more than one scene,
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

// What a build over `count` scenes puts before scene `index`'s errors and
// warnings: "scene 1 (cave): " (the name only when `name` is not empty), or
// nothing when the build has one scene.
std::string sceneTag(size_t index, size_t count, const std::string& name);

// What the engine file is: a CD-ROM build, a PCdrv build, or not a PS-X EXE.
enum class EngineKind { NotExe, Cdrom, Pcdrv };
EngineKind engineKind(const std::vector<uint8_t>& exe);

}  // namespace splash
