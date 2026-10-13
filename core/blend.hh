// .blend files, read by running Blender in the background to export them as
// glTF, which import.cpp then reads like any other .glb.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace splash {

// The Blender to run: SPLASHEDIT_BLENDER when set (even to a file that is not
// there, which exportBlend then reports), else blender on PATH (blender.exe on
// Windows), else the newest "C:/Program Files/Blender Foundation/Blender*/"
// on Windows or /Applications/Blender.app on macOS. Empty when none is found.
std::filesystem::path findBlender();

// Thrown by exportBlend when `cancel` was set while Blender ran.
struct BlendCancelled : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Runs Blender on `blend` to write it as a GLB to `glb`, modifiers applied,
// skins and animations kept. Throws std::runtime_error with a message for the
// user when Blender is not found, fails, times out or writes nothing; a
// failure carries the last lines Blender printed. When `cancel` becomes true
// while Blender runs, Blender (and anything it started) is killed and
// BlendCancelled is thrown.
void exportBlend(const std::filesystem::path& blend, const std::filesystem::path& glb,
                 const std::atomic<bool>* cancel = nullptr);

// exportBlend run on a worker thread into a fresh temporary folder (named
// splashedit-blend-*), so the caller's thread stays free while Blender works.
// The folder is removed when this is destroyed; destroying it while Blender
// still runs cancels it and waits for the thread first.
class BlendExport {
  public:
    explicit BlendExport(std::filesystem::path blend);
    ~BlendExport();
    BlendExport(const BlendExport&) = delete;
    BlendExport& operator=(const BlendExport&) = delete;

    const std::filesystem::path& blend() const;
    // Blender has finished, failed, or been cancelled.
    bool done() const;
    // Kills Blender; done() soon after, with get() throwing BlendCancelled.
    void cancel();
    // Waits for done(), then returns the GLB Blender wrote, or throws what
    // exportBlend threw (BlendCancelled after cancel()).
    const std::filesystem::path& get();

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace splash
