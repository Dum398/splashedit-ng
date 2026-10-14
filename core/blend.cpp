#include "blend.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace splash {

namespace {

constexpr int kTimeoutSeconds = 120;
constexpr size_t kTailLines = 8;

std::string u8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// An environment variable in the path's native encoding: UTF-16 on Windows,
// where _dupenv_s would give the ANSI code page and lose any character
// outside it (a user name in the path, say).
fs::path::string_type getEnv(const char* name) {
#ifdef _WIN32
    const std::wstring wname(name, name + std::strlen(name));
    for (DWORD size = 256;;) {
        std::wstring v(size, L'\0');
        const DWORD n = GetEnvironmentVariableW(wname.c_str(), v.data(), size);
        if (n == 0) return {};
        if (n < size) return v.substr(0, n);
        size = n;  // too small: n is the size needed, terminator included
    }
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// The last few non-empty lines of a file Blender wrote its output to.
std::string tail(const fs::path& log) {
    std::ifstream f(log, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.find_first_not_of(" \t") != std::string::npos) lines.push_back(l);
    }
    std::string r;
    for (size_t i = lines.size() > kTailLines ? lines.size() - kTailLines : 0; i < lines.size(); ++i)
        r += "\n  " + lines[i];
    return r;
}

// How a run of Blender ended.
struct Ran {
    enum How { Exited, TimedOut, Cancelled } how = Exited;
    int code = 0;  // exit code when Exited
};

// Runs `args` (args[0] the program's path) with stdin empty and stdout and
// stderr written to the given files (files, not pipes, so a chatty Blender
// can never block on a full pipe). Kills it, and whatever it started, when it
// runs past kTimeoutSeconds or `cancel` becomes true; waits for it either way.
// Throws when it cannot be started.
#ifdef _WIN32
// One argument as CommandLineToArgvW (and the MSVC runtime) splits it back:
// quoted when empty or holding blanks or quotes; inside the quotes, a run of
// backslashes is doubled when a quote or the closing quote follows it, and
// each quote is escaped.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (auto it = a.begin();; ++it) {
        size_t slashes = 0;
        while (it != a.end() && *it == L'\\') ++it, ++slashes;
        if (it == a.end()) {
            r.append(slashes * 2, L'\\');
            break;
        }
        if (*it == L'"') r.append(slashes * 2 + 1, L'\\');
        else r.append(slashes, L'\\');
        r += *it;
    }
    return r + L"\"";
}

std::string winError(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string r;
    if (msg) {
        r = u8(fs::path(msg));
        LocalFree(msg);
    }
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r' || r.back() == ' ' || r.back() == '.')) r.pop_back();
    return r.empty() ? "error " + std::to_string(code) : r + " (error " + std::to_string(code) + ")";
}

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() {
        if (h) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

Ran run(const std::vector<fs::path>& args, const fs::path& out, const fs::path& err, const std::atomic<bool>* cancel) {
    std::wstring cmd;
    for (const fs::path& a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(a.wstring());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    Handle o(CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!o.h) throw std::runtime_error("could not write " + u8(out) + ": " + winError(GetLastError()));
    Handle e(CreateFileW(err.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!e.h) throw std::runtime_error("could not write " + u8(err) + ": " + winError(GetLastError()));
    Handle in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!in.h) throw std::runtime_error("could not open NUL: " + winError(GetLastError()));

    // Blender inherits these three handles only, not every inheritable handle
    // the editor has open at the time (pcsx-redux's pipes, say, opened on
    // another thread), which would otherwise stay open as long as it runs.
    HANDLE inherit[3] = {in.h, o.h, e.h};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attrBuf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size))
        throw std::runtime_error("could not run Blender: " + winError(GetLastError()));
    struct Attrs {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~Attrs() { DeleteProcThreadAttributeList(a); }
    } attrsGuard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr))
        throw std::runtime_error("could not run Blender: " + winError(GetLastError()));

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in.h;
    si.StartupInfo.hStdOutput = o.h;
    si.StartupInfo.hStdError = e.h;
    si.lpAttributeList = attrs;

    // A job holding Blender and anything it starts: a timeout or cancel ends
    // them all, and so does the editor exiting (the job's last handle closing).
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    PROCESS_INFORMATION pi{};
    // Started suspended so it is in the job before it can start anything.
    const BOOL ok = CreateProcessW(args[0].c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    if (!ok) throw std::runtime_error("could not run Blender (" + u8(args[0]) + "): " + winError(GetLastError()));
    Handle process(pi.hProcess), thread(pi.hThread);
    // Fails when the editor itself is in a job that forbids nesting (before
    // Windows 8); then only Blender's own process is killed.
    const bool inJob = job.h && AssignProcessToJobObject(job.h, pi.hProcess);
    ResumeThread(pi.hThread);
    auto kill = [&] {
        if (inJob) TerminateJobObject(job.h, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    };
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const DWORD w = WaitForSingleObject(pi.hProcess, 20);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            const DWORD why = GetLastError();
            kill();
            throw std::runtime_error("could not wait for Blender: " + winError(why));
        }
        if (cancel && cancel->load()) {
            kill();
            return {Ran::Cancelled};
        }
        if (GetTickCount64() - start >= ULONGLONG(kTimeoutSeconds) * 1000) {
            kill();
            return {Ran::TimedOut};
        }
    }
    DWORD c = 1;
    GetExitCodeProcess(pi.hProcess, &c);
    return {Ran::Exited, int(c)};
}
#else
Ran run(const std::vector<fs::path>& args, const fs::path& out, const fs::path& err, const std::atomic<bool>* cancel) {
    std::vector<std::string> strs;
    for (const fs::path& a : args) strs.push_back(a.string());
    std::vector<char*> argv;
    for (std::string& s : strs) argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // In a process group of its own, so a timeout or cancel kills whatever
    // Blender started along with it.
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) throw std::runtime_error("could not run Blender (" + strs[0] + "): " + std::strerror(rc));
    int status = 0;
    auto killAll = [&] {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    };
    for (int waited = 0;; waited += 20) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            const int why = errno;
            killAll();
            throw std::runtime_error(std::string("could not wait for Blender: ") + std::strerror(why));
        }
        if (cancel && cancel->load()) {
            killAll();
            return {Ran::Cancelled};
        }
        if (waited >= kTimeoutSeconds * 1000) {
            killAll();
            return {Ran::TimedOut};
        }
        timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    // Blender is done; anything it left running in its group goes too.
    kill(-pid, SIGKILL);
    if (WIFEXITED(status)) return {Ran::Exited, WEXITSTATUS(status)};
    return {Ran::Exited, 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0)};
}
#endif

// A Python string literal holding `s`.
std::string pyString(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\\' || c == '\'') r += '\\';
        r += c;
    }
    return r + "'";
}

}  // namespace

fs::path findBlender() {
    using native = fs::path::string_type;
    const native env = getEnv("SPLASHEDIT_BLENDER");
    if (!env.empty()) return fs::path(env);
#ifdef _WIN32
    const native::value_type sep = L';';
    const char* exe = "blender.exe";
#else
    const native::value_type sep = ':';
    const char* exe = "blender";
#endif
    const native path = getEnv("PATH");
    for (size_t at = 0; at <= path.size();) {
        size_t end = path.find(sep, at);
        if (end == native::npos) end = path.size();
        native dir = path.substr(at, end - at);
#ifdef _WIN32
        // PATH entries may be quoted ("C:\Program Files\Blender Foundation\Blender 4.2").
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
#endif
        at = end + 1;
        if (dir.empty()) continue;
        fs::path p = fs::path(dir) / exe;
        if (isFile(p)) return p;
    }
#ifdef _WIN32
    // The newest of "Blender 4.2", "Blender 4.10"... by version number.
    native pf = getEnv("ProgramFiles");
    if (pf.empty()) pf = L"C:\\Program Files";
    fs::path best;
    std::vector<int> bestVersion;
    std::error_code ec;
    for (fs::directory_iterator d(fs::path(pf) / "Blender Foundation", ec), end; !ec && d != end; d.increment(ec)) {
        const std::string name = u8(d->path().filename());
        if (name.rfind("Blender", 0) != 0 || !isFile(d->path() / exe)) continue;
        std::vector<int> v;
        for (size_t i = 0; i < name.size();) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                ++i;
                continue;
            }
            int n = 0;
            while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) n = n * 10 + (name[i++] - '0');
            v.push_back(n);
        }
        if (best.empty() || v > bestVersion) best = d->path() / exe, bestVersion = v;
    }
    if (!best.empty()) return best;
#elif defined(__APPLE__)
    const fs::path app = "/Applications/Blender.app/Contents/MacOS/Blender";
    if (isFile(app)) return app;
#endif
    return {};
}

void exportBlend(const fs::path& blend, const fs::path& glb, const std::atomic<bool>* cancel) {
    const std::string what = u8(blend.filename());
    const fs::path blender = findBlender();
    if (blender.empty())
        throw std::runtime_error(what +
                                 ": importing a .blend file needs Blender, which was not found. Install Blender "
                                 "(so that blender is on PATH) or set SPLASHEDIT_BLENDER to its executable; or "
                                 "export the model from Blender as glTF (.glb) and import that instead");
    if (!isFile(blender))
        throw std::runtime_error(what + ": SPLASHEDIT_BLENDER is set to " + u8(blender) +
                                 ", which is not a file. Set it to the Blender executable, or unset it to use the "
                                 "blender on PATH");
    // One line: a newline in an argument is one more thing to go wrong in a
    // Windows command line.
    const std::string expr = "import bpy; bpy.ops.export_scene.gltf(filepath=" + pyString(u8(fs::path(glb.generic_u8string()))) +
                             ", export_format='GLB', export_apply=True, export_animations=True, export_skins=True)";
    const fs::path out = glb.parent_path() / "blender-stdout.txt";
    const fs::path err = glb.parent_path() / "blender-stderr.txt";
    std::error_code ec;
    fs::remove(glb, ec);
    if (cancel && cancel->load()) throw BlendCancelled(what + ": import cancelled");
    const Ran ran =
        run({blender, "--background", fs::absolute(blend), "--python-exit-code", "1", "--python-expr", expr}, out, err, cancel);
    if (ran.how != Ran::Exited) fs::remove(glb, ec);  // half written
    if (ran.how == Ran::Cancelled) throw BlendCancelled(what + ": import cancelled");
    // Blender prints most of its own errors to stdout, Python's to stderr.
    std::string said = tail(err);
    if (said.empty()) said = tail(out);
    if (ran.how == Ran::TimedOut)
        throw std::runtime_error(what + ": Blender did not finish exporting it within " + std::to_string(kTimeoutSeconds) +
                                 " s. Export it from Blender as glTF (.glb) and import that instead" +
                                 (said.empty() ? std::string() : ". Blender said:" + said));
    if (ran.code != 0)
        throw std::runtime_error(what + ": Blender could not export it as glTF (exit code " + std::to_string(ran.code) +
                                 "). Check that it opens in Blender (" + u8(blender) +
                                 "), or export it from Blender as glTF (.glb) and import that instead" +
                                 (said.empty() ? std::string() : ". Blender said:" + said));
    if (!isFile(glb))
        throw std::runtime_error(what + ": Blender finished without writing the glTF export. Check that " + u8(blender) +
                                 " is Blender 2.8 or later with the glTF exporter enabled" +
                                 (said.empty() ? std::string() : ". Blender said:" + said));
}

namespace {

// A fresh, empty folder for one export, named splashedit-blend-*.
fs::path makeTempDir(const fs::path& blend) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (!ec) {
        std::random_device rd;
        for (int n = 0; n < 16; ++n) {
            fs::path dir = base / ("splashedit-blend-" + std::to_string(rd()));
            if (fs::create_directory(dir, ec) && !ec) return dir;
        }
    }
    throw std::runtime_error(u8(blend.filename()) + ": could not make a temporary folder for Blender's export");
}

}  // namespace

struct BlendExport::Impl {
    fs::path blend, dir, glb;
    std::atomic<bool> cancel{false}, finished{false};
    std::exception_ptr error;  // written by the worker before `finished`
    std::thread worker;
};

BlendExport::BlendExport(fs::path blend) : m(new Impl) {
    m->blend = std::move(blend);
    m->dir = makeTempDir(m->blend);
    m->glb = m->dir / (m->blend.filename().u8string() + u8".glb");
    try {
        m->worker = std::thread([i = m.get()] {
            try {
                exportBlend(i->blend, i->glb, &i->cancel);
            } catch (...) {
                i->error = std::current_exception();
            }
            i->finished.store(true, std::memory_order_release);
        });
    } catch (...) {
        std::error_code ec;
        fs::remove_all(m->dir, ec);
        throw;
    }
}

BlendExport::~BlendExport() {
    m->cancel = true;
    if (m->worker.joinable()) m->worker.join();
    std::error_code ec;
    fs::remove_all(m->dir, ec);
}

const fs::path& BlendExport::blend() const { return m->blend; }

bool BlendExport::done() const { return m->finished.load(std::memory_order_acquire); }

void BlendExport::cancel() { m->cancel = true; }

const fs::path& BlendExport::get() {
    if (m->worker.joinable()) m->worker.join();
    if (m->error) std::rethrow_exception(m->error);
    return m->glb;
}

}  // namespace splash
