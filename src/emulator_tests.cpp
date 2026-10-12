// Emulator::stop on a process that ignores being asked to close, the way
// pcsx-redux without its UI ignores WM_CLOSE on Windows. stop() must still
// return, and the process must be gone. Also a stand-in pcsx-redux for
// tests/gamekeys.sh (SPLASHEDIT_FAKE_REDUX set).
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <csignal>
#endif

// Child mode: ignore every polite way to end, say so, then sit for a minute.
static int stubbornChild() {
#ifdef _WIN32
    MSG msg;
    PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE);  // gives this thread a message queue
    SetConsoleCtrlHandler(nullptr, TRUE);
#else
    std::signal(SIGTERM, SIG_IGN);
#endif
    std::printf("ready\n");
    std::fflush(stdout);
    for (int i = 0; i < 6000; ++i) {
#ifdef _WIN32
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        }
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    return 0;
}

// Child mode standing in for pcsx-redux under Play: publishes an empty
// display block the way redux -shmdisplay does (see game_view.cpp), so the
// editor attaches its Game view, then waits to be stopped.
#ifndef _WIN32
static std::string g_shmName;
static void unlinkAndExit(int) {
    shm_unlink(g_shmName.c_str());
    _exit(0);
}
#endif

static int fakeRedux() {
    const uint32_t vramOffset = 4096, size = vramOffset + 1024 * 512 * 2;
    std::string name = "pcsx-redux-display-";
#ifdef _WIN32
    name += std::to_string(GetCurrentProcessId());
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name.c_str());
    if (!h) return 1;
    uint32_t* mem = (uint32_t*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    name += std::to_string(getpid());
    g_shmName = name;
    std::signal(SIGTERM, unlinkAndExit);
    int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, size) != 0) return 1;
    uint32_t* mem = (uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) return 1;
#endif
    if (!mem) return 1;
    mem[2] = 64;          // headerSize
    mem[3] = vramOffset;  // vramOffset
    mem[0] = 0x44535850;  // magic "PXSD", last so a reader never sees a half header
    std::printf("fake redux up\n");
    std::fflush(stdout);
#ifdef _WIN32
    Sleep(120000);
#else
    sleep(120);
    shm_unlink(name.c_str());
#endif
    return 0;
}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--stubborn-child")) return stubbornChild();
    // Play passes redux arguments, so this mode is chosen by the environment.
    if (std::getenv("SPLASHEDIT_FAKE_REDUX")) return fakeRedux();

    Emulator emu;
    std::string error;
    check(emu.start({argv[0], "--stubborn-child"}, &error), "child starts");
    bool ready = false;
    for (Uint64 t = SDL_GetTicks(); !ready && SDL_GetTicks() - t < 10000; SDL_Delay(10)) {
        emu.poll();
        for (const std::string& l : emu.output()) ready = ready || l == "ready";
    }
    check(ready, "child is ignoring polite stops");
    Uint64 t0 = SDL_GetTicks();
    emu.stop();
    Uint64 took = SDL_GetTicks() - t0;
    std::printf("stop() took %llu ms\n", (unsigned long long)took);
    check(took < 5000, "stop() returns although the child ignores being asked to close");
    check(!emu.running(), "nothing running after stop()");
    std::printf(failures ? "%d FAILED\n" : "All tests passed\n", failures);
    return failures ? 1 : 0;
}
