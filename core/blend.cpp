#include "blend.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::string getEnv(const char* name) {
#ifdef _WIN32
    char* v = nullptr;
    size_t n = 0;
    std::string r;
    if (_dupenv_s(&v, &n, name) == 0 && v) r = v;
    free(v);
    return r;
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

// Runs `args` (args[0] the program's path) with stdout and stderr written to
// the given files. Returns the exit code, -1 when it did not finish in time
// (it is then killed). Throws when it cannot be started.
#ifdef _WIN32
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

int run(const std::vector<fs::path>& args, const fs::path& out, const fs::path& err) {
    std::wstring cmd;
    for (const fs::path& a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(a.wstring());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE e = CreateFileW(err.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = o;
    si.hStdError = e;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(args[0].c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    const DWORD why = GetLastError();
    for (HANDLE h : {o, e, in})
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) throw std::runtime_error("could not run Blender (" + u8(args[0]) + "): error " + std::to_string(why));
    int code = -1;
    if (WaitForSingleObject(pi.hProcess, DWORD(kTimeoutSeconds) * 1000) == WAIT_OBJECT_0) {
        DWORD c = 1;
        GetExitCodeProcess(pi.hProcess, &c);
        code = int(c);
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}
#else
int run(const std::vector<fs::path>& args, const fs::path& out, const fs::path& err) {
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
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) throw std::runtime_error("could not run Blender (" + strs[0] + "): " + std::strerror(rc));
    int status = 0;
    for (int waited = 0;; waited += 20) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) return -1;
        if (waited >= kTimeoutSeconds * 1000) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return -1;
        }
        timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
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
    const std::string env = getEnv("SPLASHEDIT_BLENDER");
    if (!env.empty()) return fs::path(std::u8string(env.begin(), env.end()));
#ifdef _WIN32
    const char sep = ';';
    const char* exe = "blender.exe";
#else
    const char sep = ':';
    const char* exe = "blender";
#endif
    std::stringstream path(getEnv("PATH"));
    for (std::string dir; std::getline(path, dir, sep);) {
        if (dir.empty()) continue;
        fs::path p = fs::path(dir) / exe;
        if (isFile(p)) return p;
    }
#ifdef _WIN32
    // The newest of "Blender 4.2", "Blender 4.10"... by version number.
    std::string pf = getEnv("ProgramFiles");
    if (pf.empty()) pf = "C:/Program Files";
    fs::path best;
    std::vector<int> bestVersion;
    std::error_code ec;
    for (fs::directory_iterator d(fs::path(pf) / "Blender Foundation", ec), end; !ec && d != end; d.increment(ec)) {
        const std::string name = d->path().filename().string();
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

void exportBlend(const fs::path& blend, const fs::path& glb) {
    const std::string what = u8(blend.filename());
    const fs::path blender = findBlender();
    if (blender.empty())
        throw std::runtime_error(what +
                                 ": importing a .blend file needs Blender, which was not found. Install Blender "
                                 "(so that blender is on PATH) or set SPLASHEDIT_BLENDER to its executable; or "
                                 "export the model from Blender as glTF (.glb) and import that instead");
    if (!isFile(blender))
        throw std::runtime_error(what + ": SPLASHEDIT_BLENDER is set to " + u8(blender) + ", which is not a file");
    const std::string expr = "import bpy\nbpy.ops.export_scene.gltf(filepath=" + pyString(u8(fs::path(glb.generic_u8string()))) +
                             ", export_format='GLB', export_apply=True, export_animations=True, export_skins=True)\n";
    const fs::path out = glb.parent_path() / "blender-stdout.txt";
    const fs::path err = glb.parent_path() / "blender-stderr.txt";
    std::error_code ec;
    fs::remove(glb, ec);
    const int code = run({blender, "--background", fs::absolute(blend), "--python-exit-code", "1", "--python-expr", expr}, out, err);
    // Blender prints most of its own errors to stdout, Python's to stderr.
    std::string said = tail(err);
    if (said.empty()) said = tail(out);
    if (code == -1)
        throw std::runtime_error(what + ": Blender did not finish exporting it within " +
                                 std::to_string(kTimeoutSeconds) + " s" + said);
    if (code != 0)
        throw std::runtime_error(what + ": Blender could not export it as glTF (exit code " + std::to_string(code) +
                                 ")" + (said.empty() ? std::string() : ":" + said));
    if (!isFile(glb))
        throw std::runtime_error(what + ": Blender finished without writing the glTF export" +
                                 (said.empty() ? std::string() : ":" + said));
}

}  // namespace splash
