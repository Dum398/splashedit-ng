#include "editor/play.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>

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
        << "port=" << t.port << "\n";
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

std::vector<std::string> missingTools(const PlayTools& t) {
    std::vector<std::string> out;
    std::error_code ec;
    auto need = [&](const fs::path& p, const char* what) {
        if (p.empty()) out.push_back(std::string(what) + " is not set.");
        else if (!fs::is_regular_file(p, ec)) out.push_back(std::string(what) + " not found: " + utf8(p));
    };
    need(t.redux, "pcsx-redux");
    need(t.psxsplash, "The psxsplash build (.ps-exe)");
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
    else if (!fs::is_regular_file(t.psxsplash, ec)) out.push_back("The psxsplash build not found: " + utf8(t.psxsplash));
    if (t.port.empty()) out.push_back("No serial port is set.");
    return out;
}

static const char* const kSceneExts[] = {".splashpack", ".vram", ".spu", ".geo"};

static std::string sceneStem(size_t i) { return "scene_" + std::to_string(i); }

// Exports `scene` as dir/scene_<i>.*, removing that scene's old files first so
// a failed export leaves none of them to boot.
static splash::ExportResult exportScene(const splash::Scene& scene, const fs::path& projectRoot, const fs::path& dir,
                                        size_t i) {
    splash::ExportResult r;
    std::error_code ec;
    for (const char* ext : kSceneExts) fs::remove(dir / (sceneStem(i) + ext), ec);
    splash::ExportOptions opt;
    opt.luaBytecode = true;
    try {
        r = splash::exportSplashpack(scene, projectRoot, dir / (sceneStem(i) + ".splashpack"), opt);
    } catch (const std::exception& e) {
        r.errors.push_back(e.what());
    }
    return r;
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
    for (size_t i = 0; i < scenes.size(); ++i) {
        std::string tag;
        if (scenes.size() > 1) {
            tag = "scene " + std::to_string(i);
            if (i < names.size() && !names[i].empty()) tag += " (" + names[i] + ")";
            tag += ": ";
        }
        splash::ExportResult one = exportScene(*scenes[i], projectRoot, dir, i);
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

std::vector<std::string> reduxCommand(const PlayTools& t, const fs::path& dir) {
    std::vector<std::string> a = {utf8(t.redux), "-run", "-fastboot", "-no-ui", "-shmdisplay"};
    if (!t.bios.empty()) {
        a.push_back("-bios");
        a.push_back(utf8(t.bios));
    }
    a.insert(a.end(), {"-loadexe", utf8(t.psxsplash), "-pcdrv", "-pcdrvbase", utf8(dir)});
    return a;
}

}  // namespace editor
