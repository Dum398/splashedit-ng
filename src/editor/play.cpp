#include "editor/play.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>

#include "disc.hh"
#include "editor/project.hh"
#include "scene.hh"

namespace fs = std::filesystem;

namespace editor {

// Paths travel as UTF-8 std::string (settings file, UI, process arguments).
static std::string utf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
static fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

PlayTools loadPlayTools(const fs::path& file) {
    PlayTools t;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        fs::path p = fromUtf8(value);
        if (key == "redux") t.redux = p;
        else if (key == "psxsplash") t.psxsplash = p;
        else if (key == "bios") t.bios = p;
        else if (key == "disc") t.disc = p;
        else if (key == "port") t.port = value;
        else if (key == "overlay_perf") t.overlayPerf = value == "1";
        else if (key == "overlay_rooms") t.overlayRooms = value == "1";
        else if (key == "boot_logo") t.bootLogo = value != "0";
        else if (key == "players") {
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            t.players = value.empty() || *end ? 1 : int(std::clamp<long>(n, 1, kMaxPlayers));
        } else if (key == "relay") t.relay = value == "1";
    }
    return t;
}

bool savePlayTools(const fs::path& file, const PlayTools& t) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out << "redux=" << utf8(t.redux) << "\n"
        << "psxsplash=" << utf8(t.psxsplash) << "\n"
        << "bios=" << utf8(t.bios) << "\n"
        << "disc=" << utf8(t.disc) << "\n"
        << "port=" << t.port << "\n"
        << "overlay_perf=" << (t.overlayPerf ? 1 : 0) << "\n"
        << "overlay_rooms=" << (t.overlayRooms ? 1 : 0) << "\n"
        << "boot_logo=" << (t.bootLogo ? 1 : 0) << "\n"
        << "players=" << std::clamp(t.players, 1, kMaxPlayers) << "\n"
        << "relay=" << (t.relay ? 1 : 0) << "\n";
    return bool(out);
}

static void fromEnv(fs::path& p, const char* name) {
    if (!p.empty()) return;
    if (const char* v = std::getenv(name); v && *v) p = fromUtf8(v);
}

static fs::path onPath(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    const char sep = ';';
    const std::string exe = std::string(name) + ".exe";
#else
    const char sep = ':';
    const std::string exe = name;
#endif
    std::string all = path;
    for (size_t start = 0; start <= all.size();) {
        size_t end = all.find(sep, start);
        if (end == std::string::npos) end = all.size();
        if (end > start) {
            fs::path p = fromUtf8(all.substr(start, end - start)) / exe;
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p;
        }
        start = end + 1;
    }
    return {};
}

static void fromBundle(fs::path& p, const fs::path& bundle, std::initializer_list<const char*> candidates) {
    if (!p.empty() || bundle.empty()) return;
    std::error_code ec;
    for (const char* c : candidates) {
        fs::path f = bundle / fromUtf8(c);
        if (fs::is_regular_file(f, ec)) {
            p = f;
            return;
        }
    }
}

// A saved path that no longer exists (an old build, a moved folder) must not
// hide the copy the package carries.
static void dropMissing(fs::path& p) {
    std::error_code ec;
    if (!p.empty() && !fs::is_regular_file(p, ec)) p.clear();
}

// On Windows pcsx-redux.exe only relaunches pcsx-redux.main through the shell and
// exits, so its pid never owns the display block and its output is lost. Run
// the real binary when it sits beside the launcher.
static void skipLauncher(fs::path& p) {
    std::error_code ec;
    if (p.filename() != "pcsx-redux.exe") return;
    fs::path main = p.parent_path() / "pcsx-redux.main";
    if (fs::is_regular_file(main, ec)) p = main;
}

PlayTools withDefaults(PlayTools t, const fs::path& bundle) {
    dropMissing(t.redux);
    dropMissing(t.psxsplash);
    dropMissing(t.disc);
    fromEnv(t.redux, "SPLASHEDIT_REDUX");
    fromEnv(t.psxsplash, "SPLASHEDIT_PSXSPLASH");
    fromEnv(t.bios, "SPLASHEDIT_BIOS");
    fromEnv(t.disc, "SPLASHEDIT_DISC_ENGINE");
    if (t.port.empty())
        if (const char* v = std::getenv("SPLASHEDIT_PORT"); v && *v) t.port = v;
    fromBundle(t.redux, bundle,
               {"redux/pcsx-redux.exe", "redux/PCSX-Redux.app/Contents/MacOS/PCSX-Redux", "redux/pcsx-redux"});
    fromBundle(t.psxsplash, bundle, {"engine/psxsplash.ps-exe"});
    fromBundle(t.disc, bundle, {"engine/psxsplash-cdrom.ps-exe"});
    if (t.redux.empty()) t.redux = onPath("pcsx-redux");
    skipLauncher(t.redux);
    return t;
}

// Drops the first of `suffixes` that `stem` ends with (and is more than).
static void dropSuffix(std::string& stem, std::initializer_list<const char*> suffixes) {
    for (const char* suffix : suffixes) {
        const std::string sfx = suffix;
        if (stem.size() > sfx.size() && stem.compare(stem.size() - sfx.size(), sfx.size(), sfx) == 0) {
            stem.resize(stem.size() - sfx.size());
            return;
        }
    }
}

fs::path overlayEngine(const fs::path& engine, bool perf, bool rooms, bool nologo) {
    if ((!perf && !rooms && !nologo) || engine.empty()) return engine;
    // A variant picked as the psxsplash build names the same family as the plain build.
    std::string stem = utf8(engine.stem());
    const std::string plain = stem;
    dropSuffix(stem, {"-nologo"});
    dropSuffix(stem, {"-perf-rooms", "-perf", "-rooms"});
    std::string want = stem;
    if (perf && rooms) want += "-perf-rooms";
    else if (perf) want += "-perf";
    else if (rooms) want += "-rooms";
    if (nologo) want += "-nologo";
    if (want == plain) return engine;
    fs::path name = fromUtf8(want);
    name += engine.extension();
    return engine.parent_path() / name;
}

fs::path discEngine(const fs::path& disc, bool nologo) {
    if (!nologo || disc.empty()) return disc;
    std::string stem = utf8(disc.stem());
    const std::string plain = stem;
    dropSuffix(stem, {"-nologo"});
    if (nologo) stem += "-nologo";
    if (stem == plain) return disc;
    fs::path name = fromUtf8(stem);
    name += disc.extension();
    return disc.parent_path() / name;
}

// `nologo` when it is there, else `logo`, saying so in `note`.
static fs::path logoFallback(const fs::path& logo, const fs::path& nologo, const char* what, std::string* note) {
    std::error_code ec;
    if (fs::is_regular_file(nologo, ec)) return nologo;
    if (note)
        *note = std::string(what) + " without the boot logo not found: " + utf8(nologo) + ". Booting " +
                utf8(logo.filename()) + " instead, so the boot logo plays.";
    return logo;
}

fs::path engineFor(const PlayTools& t, std::string* note) {
    if (note) note->clear();
    const fs::path logo = overlayEngine(t.psxsplash, t.overlayPerf, t.overlayRooms);
    if (t.bootLogo || logo.empty()) return logo;
    return logoFallback(logo, overlayEngine(t.psxsplash, t.overlayPerf, t.overlayRooms, true), "The psxsplash build", note);
}

fs::path discEngineFor(const fs::path& disc, bool bootLogo, std::string* note) {
    if (note) note->clear();
    const fs::path logo = discEngine(disc, false);
    if (bootLogo || logo.empty()) return logo;
    return logoFallback(logo, discEngine(disc, true), "The psxsplash disc build", note);
}

// "The psxsplash build", naming the overlays it is asked to have.
static std::string engineWhat(const PlayTools& t) {
    std::string s = "The psxsplash build";
    if (t.overlayPerf && t.overlayRooms) s += " with the performance and room debug overlays";
    else if (t.overlayPerf) s += " with the performance overlay";
    else if (t.overlayRooms) s += " with the room debug overlay";
    return s;
}

std::vector<std::string> missingTools(const PlayTools& t) {
    std::vector<std::string> out;
    std::error_code ec;
    auto need = [&](const fs::path& p, const char* what) {
        if (p.empty()) out.push_back(std::string(what) + " is not set.");
        else if (!fs::is_regular_file(p, ec)) out.push_back(std::string(what) + " not found: " + utf8(p));
    };
    need(t.redux, "pcsx-redux");
    if (t.psxsplash.empty()) need(t.psxsplash, "The psxsplash build (.ps-exe)");
    else need(engineFor(t), engineWhat(t).c_str());
    if (!t.bios.empty() && !fs::is_regular_file(t.bios, ec)) out.push_back("BIOS not found: " + utf8(t.bios));
    return out;
}

std::vector<std::string> missingDiscTools(const PlayTools& t) {
    std::vector<std::string> out;
    std::error_code ec;
    if (t.disc.empty()) out.push_back("The psxsplash disc build (.ps-exe) is not set.");
    else if (!fs::is_regular_file(t.disc, ec)) out.push_back("The psxsplash disc build not found: " + utf8(t.disc));
    return out;
}

std::vector<std::string> missingHardwareTools(const PlayTools& t) {
    std::vector<std::string> out;
    std::error_code ec;
    if (t.psxsplash.empty()) out.push_back("The psxsplash build (.ps-exe) is not set.");
    else if (!fs::is_regular_file(engineFor(t), ec)) out.push_back(engineWhat(t) + " not found: " + utf8(engineFor(t)));
    if (t.port.empty()) out.push_back("No serial port is set.");
    return out;
}

static const char* const kSceneExts[] = {".splashpack", ".vram", ".spu", ".geo", ".loading"};

static std::string sceneStem(size_t i) { return "scene_" + std::to_string(i); }

static splash::ExportOptions playOptions(const fs::path& projectRoot) {
    splash::ExportOptions opt;
    opt.luaBytecode = true;
    opt.vram = loadVramSettings(projectRoot);
    return opt;
}

static void removeSceneFiles(const fs::path& dir, size_t i) {
    std::error_code ec;
    for (const char* ext : kSceneExts) fs::remove(dir / (sceneStem(i) + ext), ec);
}

// Exports `scene` alone as dir/scene_<i>.*, removing that scene's old files
// first so a failed export leaves none of them to boot.
static splash::ExportResult exportScene(const splash::Scene& scene, const fs::path& projectRoot, const fs::path& dir,
                                        size_t i) {
    removeSceneFiles(dir, i);
    return splash::exportBuild({&scene}, projectRoot, [&](size_t) { return dir / (sceneStem(i) + ".splashpack"); },
                               playOptions(projectRoot))
        .front();
}

splash::ExportResult exportForPlay(const std::vector<const splash::Scene*>& scenes, const fs::path& projectRoot,
                                   const fs::path& dir, const std::vector<std::string>& names) {
    splash::ExportResult r;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        r.errors.push_back("Cannot create " + utf8(dir) + ": " + ec.message());
        return r;
    }
    // Stale files from an earlier play must not be booted if this export fails,
    // nor scenes the list no longer has be opened by Scene.Load.
    std::vector<fs::path> stale;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        const std::string name = utf8(e.path().filename());
        unsigned long n = 0;
        int used = 0;
        if (std::sscanf(name.c_str(), "scene_%lu%n", &n, &used) != 1 || n < scenes.size()) continue;
        const std::string ext = name.substr(size_t(used));
        for (const char* known : kSceneExts)
            if (ext == known) stale.push_back(e.path());
    }
    for (const fs::path& f : stale) fs::remove(f, ec);
    fs::remove(dir / "reload.flag", ec);
    // Scene 0's render buffers come out as the build's largest (exportBuild).
    for (size_t i = 0; i < scenes.size(); ++i) removeSceneFiles(dir, i);
    std::vector<splash::ExportResult> all = splash::exportBuild(
        scenes, projectRoot, [&](size_t i) { return dir / (sceneStem(i) + ".splashpack"); }, playOptions(projectRoot));
    for (size_t i = 0; i < scenes.size(); ++i) {
        const std::string tag = splash::sceneTag(i, scenes.size(), i < names.size() ? names[i] : std::string());
        splash::ExportResult& one = all[i];
        for (const std::string& m : one.errors) r.errors.push_back(tag + m);
        for (const std::string& m : one.warnings) r.warnings.push_back(tag + m);
        if (i == 0) r.stats = std::move(one.stats);
    }
    return r;
}

splash::ExportResult exportForPlay(const splash::Scene& scene, const fs::path& projectRoot, const fs::path& dir) {
    return exportForPlay(std::vector<const splash::Scene*>{&scene}, projectRoot, dir);
}

std::vector<const splash::Scene*> BuildScenes::pointers() const {
    std::vector<const splash::Scene*> out;
    for (const splash::Scene& s : scenes) out.push_back(&s);
    return out;
}

BuildScenes loadBuildScenes(const fs::path& project, const std::vector<fs::path>& files, const fs::path& openFile,
                            const splash::Scene& open) {
    BuildScenes b;
    const fs::path openKey = openFile.lexically_normal();
    for (const fs::path& f : files) {
        b.names.push_back(utf8(f.stem()));
        if (f.lexically_normal() == openKey) {
            b.scenes.push_back(open);
            continue;
        }
        try {
            b.scenes.push_back(splash::loadScene(project / f));
        } catch (const std::exception& e) {
            b.scenes.emplace_back();
            b.errors.push_back("Cannot load " + utf8(f) + ": " + e.what());
        }
    }
    return b;
}

BuildReport buildReport(const std::string& what, size_t scenes, const splash::ExportResult& build,
                        const std::optional<std::string>& openTag, const splash::ExportResult* live) {
    BuildReport r;
    r.what = what;
    r.scenes = scenes;
    // A message about the open scene that its live export also lists is shown once, there.
    auto listed = [&](const std::string& m, const std::vector<std::string>& in) {
        if (!openTag || !live || m.compare(0, openTag->size(), *openTag) != 0) return false;
        const std::string bare = m.substr(openTag->size());
        for (const std::string& l : in)
            if (l == bare) return true;
        return false;
    };
    for (const std::string& m : build.errors)
        if (!listed(m, live ? live->errors : std::vector<std::string>())) r.errors.push_back(m);
    for (const std::string& m : build.warnings)
        if (!listed(m, live ? live->warnings : std::vector<std::string>())) r.warnings.push_back(m);
    return r;
}

std::string buildFailure(const std::string& what, const splash::ExportResult& build) {
    if (build.errors.empty()) return std::string();
    if (build.errors.size() == 1) return what + ": " + build.errors.front();
    return what + ", " + std::to_string(build.errors.size()) + " errors: " + build.errors.front();
}

bool parseRenderPeak(const std::string& line, RenderPeak* out) {
    const char* tag = "psxsplash: render peak depth ";
    size_t at = line.find(tag);
    if (at == std::string::npos) return false;
    RenderPeak p;
    if (std::sscanf(line.c_str() + at + std::strlen(tag), "%d of %u, bump %u of %u", &p.depth, &p.orderingTable, &p.bump,
                    &p.bumpSize) != 4)
        return false;
    *out = p;
    return true;
}

bool parseRenderLost(const std::string& line, RenderLost* out) {
    const char* tag = "psxsplash: render lost ot ";
    size_t at = line.find(tag);
    if (at == std::string::npos) return false;
    RenderLost l;
    if (std::sscanf(line.c_str() + at + std::strlen(tag), "%u skinnear %u leaf %u clamp %u of %u tris", &l.ot, &l.skinNear,
                    &l.leaf, &l.clamp, &l.tris) != 5)
        return false;
    *out = l;
    return true;
}

std::string describeRenderLost(const RenderLost& lost) {
    // "triangles" once, on the first cause that counts triangles: "5 triangles
    // past the draw distance, 9 squashed at the screen edge".
    std::string s;
    bool named = false;
    auto add = [&](uint32_t n, const char* kind, const char* what, bool triangles) {
        if (!n) return;
        if (!s.empty()) s += ", ";
        s += std::to_string(n) + kind;
        if (!triangles || !named) s += n == 1 ? (triangles ? " triangle" : " piece") : (triangles ? " triangles" : " pieces");
        named = named || triangles;
        s += what;
    };
    add(lost.ot, "", " past the draw distance", true);
    add(lost.skinNear, " skinned", " too close to the camera", true);
    add(lost.clamp, "", " squashed at the screen edge", true);
    add(lost.leaf, " near-camera", " drawn rough", false);
    return s;
}

RenderLost maxRenderLost(const RenderLost& a, const RenderLost& b) {
    RenderLost m;
    m.ot = std::max(a.ot, b.ot);
    m.skinNear = std::max(a.skinNear, b.skinNear);
    m.leaf = std::max(a.leaf, b.leaf);
    m.clamp = std::max(a.clamp, b.clamp);
    m.tris = std::max(a.tris, b.tris);
    return m;
}

bool writeReloadFlag(const fs::path& dir, unsigned generation) {
    const fs::path tmp = dir / "reload.flag.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << generation;
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dir / "reload.flag", ec);
    return !ec;
}

splash::ExportResult reloadForPlay(const splash::Scene& scene, const fs::path& projectRoot, const fs::path& dir,
                                   unsigned generation, size_t index) {
    const fs::path next = dir / "next";
    std::error_code ec;
    fs::remove_all(next, ec);
    fs::create_directories(next, ec);
    if (ec) {
        splash::ExportResult r;
        r.errors.push_back("Cannot create " + utf8(next) + ": " + ec.message());
        return r;
    }
    splash::ExportResult r = exportScene(scene, projectRoot, next, index);
    if (!r.ok()) return r;
    for (const fs::directory_entry& e : fs::directory_iterator(next, ec)) {
        fs::rename(e.path(), dir / e.path().filename(), ec);
        if (ec) {
            r.errors.push_back("Cannot replace " + utf8(dir / e.path().filename()) + ": " + ec.message());
            return r;
        }
    }
    if (!writeReloadFlag(dir, generation)) r.errors.push_back("Cannot write " + utf8(dir / "reload.flag"));
    return r;
}

std::vector<std::string> reduxCommand(const PlayTools& t, const fs::path& dir, const fs::path& captureScript,
                                      const fs::path& console) {
    std::vector<std::string> a = {utf8(t.redux), "-run", "-fastboot", "-no-ui", "-shmdisplay"};
    if (!t.bios.empty()) {
        a.push_back("-bios");
        a.push_back(utf8(t.bios));
    }
    a.insert(a.end(), {"-loadexe", utf8(engineFor(t)), "-pcdrv", "-pcdrvbase", utf8(dir)});
    if (!console.empty()) a.insert(a.end(), {"-portable", utf8(console)});
    if (!captureScript.empty() && !console.empty()) a.insert(a.end(), {"-dofile", utf8(console / "capturedir.lua")});
    if (!captureScript.empty()) a.insert(a.end(), {"-dofile", utf8(captureScript)});
    return a;
}

bool requestCapture(const fs::path& dir, std::string* error) {
    std::error_code ec;
    for (const char* stale : {"capture.gpd", "capture.err", "capture.gpd.part"}) fs::remove(dir / stale, ec);
    const fs::path tmp = dir / "capture.flag.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "1";
        if (!out) {
            if (error) *error = "Cannot write " + utf8(tmp);
            return false;
        }
    }
    fs::rename(tmp, dir / "capture.flag", ec);
    if (ec) {
        if (error) *error = "Cannot write capture.flag: " + ec.message();
        return false;
    }
    return true;
}

CaptureState pollCapture(const fs::path& dir, std::string* message) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "capture.gpd", ec)) return CaptureState::Done;
    if (fs::is_regular_file(dir / "capture.err", ec)) {
        std::ifstream in(dir / "capture.err", std::ios::binary);
        std::string why((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (message) *message = why.empty() ? std::string("the capture failed") : why;
        return CaptureState::Failed;
    }
    return CaptureState::Waiting;
}

}  // namespace editor
