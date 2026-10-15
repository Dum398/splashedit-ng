// Run on hardware: one background session that uploads psxsplash to a console
// over a Link and then serves its PCdrv files, with what it printed kept for
// the editor to show. No SDL.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "editor/serial.hh"

namespace editor {

class HardwareRun {
  public:
    enum class Phase { Idle, Connecting, Uploading, Running, Failed, Stopped };
    using Opener = std::function<std::unique_ptr<Link>(std::string* err)>;

    HardwareRun() = default;
    ~HardwareRun();
    HardwareRun(const HardwareRun&) = delete;
    HardwareRun& operator=(const HardwareRun&) = delete;

    // Opens the link with `open`, uploads `exe`, then serves files from `dir`
    // until stop(). A session already running is stopped first.
    void start(Opener open, std::filesystem::path exe, std::filesystem::path dir);
    // The usual opener: a serial port (or tcp:HOST:PORT) at 115200.
    static Opener port(std::string spec);
    void stop();

    Phase phase() const { return phase_.load(); }
    bool active() const;  // connecting, uploading or running
    int progress() const { return progress_.load(); }
    std::string error() const;
    // The console's text and the file calls, oldest first, the newest 2000.
    std::deque<std::string> lines() const;
    uint64_t lineCount() const;
    // lines() and lineCount() taken together, so they agree.
    std::deque<std::string> lines(uint64_t* total) const;

  private:
    void add(std::string line);
    void fail(const std::string& why);

    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<int> progress_{0};
    mutable std::mutex m_;
    std::deque<std::string> lines_;
    uint64_t count_ = 0;
    std::string error_;
};

}  // namespace editor
