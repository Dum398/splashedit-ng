// Play: export the open scene where psxsplash's PCdrv loader looks for it and
// boot it in pcsx-redux. This half has no SDL; the process lives in the app.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/document.hh"
#include "splashpack.hh"

namespace editor {

// The programs Play runs. `bios` is optional: empty lets pcsx-redux use its own.
// `disc` is the CD-ROM build of psxsplash that Export puts on the disc.
struct PlayTools {
    std::filesystem::path redux, psxsplash, bios, disc;
    std::string port;  // Run on hardware: a serial port, or tcp:HOST:PORT
};

// key=value lines: redux=, psxsplash=, bios=, disc=, port=. A missing file reads as empty.
PlayTools loadPlayTools(const std::filesystem::path& file);
bool savePlayTools(const std::filesystem::path& file, const PlayTools& tools);

// Fills empty entries from SPLASHEDIT_REDUX, SPLASHEDIT_PSXSPLASH,
// SPLASHEDIT_BIOS, SPLASHEDIT_DISC_ENGINE and SPLASHEDIT_PORT, then from the copies shipped in
// `bundle` (the directory holding the editor: redux/ and engine/), then looks
// for pcsx-redux on PATH.
PlayTools withDefaults(PlayTools tools, const std::filesystem::path& bundle = {});

// One line per thing Play still needs; empty when it can run.
std::vector<std::string> missingTools(const PlayTools& tools);

// The same for Export: only the disc build.
std::vector<std::string> missingDiscTools(const PlayTools& tools);

// The same for Run on hardware: the psxsplash build and a port.
std::vector<std::string> missingHardwareTools(const PlayTools& tools);

// Writes dir/scene_<i>.{splashpack,vram,spu} (and .geo when it streams) for
// each scene in order, the names psxsplash's PCdrv loader opens: the game
// starts in scene 0 and Lua's Scene.Load(i) opens scene i. Scene files from
// an earlier export numbered past the last scene are removed. With more than
// one scene, each error and warning starts with its scene's index and, when
// `names` gives one, its name ("scene 1 (cave): ..."). The stats are scene 0's.
// Every scene packs VRAM with the project's vram.json (loadVramSettings),
// as does reloadForPlay.
splash::ExportResult exportForPlay(const std::vector<const splash::Scene*>& scenes,
                                   const std::filesystem::path& projectRoot, const std::filesystem::path& dir,
                                   const std::vector<std::string>& names = {});
// The same for one scene, as scene 0.
splash::ExportResult exportForPlay(const splash::Scene& scene, const std::filesystem::path& projectRoot,
                                   const std::filesystem::path& dir);

// The scenes of a build, loaded in `files` order (project-relative paths, as
// buildScenes gives them). The entry naming `openFile` is `open` itself, so
// unsaved edits are what plays; every other is read from `project`. A file
// that does not load is an error naming it. `names` are the file stems.
struct BuildScenes {
    std::vector<splash::Scene> scenes;
    std::vector<std::string> names;
    std::vector<std::string> errors;
    std::vector<const splash::Scene*> pointers() const;
};
BuildScenes loadBuildScenes(const std::filesystem::path& project, const std::vector<std::filesystem::path>& files,
                            const std::filesystem::path& openFile, const splash::Scene& open);

// Live edits while the game runs. psxsplash re-reads dir/reload.flag every
// few frames when it existed at boot, and reloads the scene in place when its
// contents change. writeReloadFlag stores `generation` there; reloadForPlay
// exports `scene` as scene `index` into dir/next, moves its files over
// dir/scene_<index>.*, then bumps the flag; the other scenes' files are left
// as they are. A failed export leaves the running scene's files alone.
bool writeReloadFlag(const std::filesystem::path& dir, unsigned generation);
splash::ExportResult reloadForPlay(const splash::Scene& scene, const std::filesystem::path& projectRoot,
                                   const std::filesystem::path& dir, unsigned generation, size_t index = 0);

// What a development (PCdrv) psxsplash reports using, from its line
// "psxsplash: render peak depth D of N, bump B of M".
struct RenderPeak {
    int depth = 0;
    uint32_t orderingTable = 0, bump = 0, bumpSize = 0;
};
// Parses one output line; false if it is not a peak report.
bool parseRenderPeak(const std::string& line, RenderPeak* out);

// Program and arguments that boot psxsplash on the files in `dir`.
std::vector<std::string> reduxCommand(const PlayTools& tools, const std::filesystem::path& dir);

}  // namespace editor
