// .blend files, read by running Blender in the background to export them as
// glTF, which import.cpp then reads like any other .glb.
#pragma once

#include <filesystem>

namespace splash {

// The Blender to run: SPLASHEDIT_BLENDER when set (even to a file that is not
// there, which exportBlend then reports), else blender on PATH (blender.exe on
// Windows), else the newest "C:/Program Files/Blender Foundation/Blender*/"
// on Windows or /Applications/Blender.app on macOS. Empty when none is found.
std::filesystem::path findBlender();

// Runs Blender on `blend` to write it as a GLB to `glb`, modifiers applied,
// skins and animations kept. Throws std::runtime_error with a message for the
// user when Blender is not found, fails, times out or writes nothing; a
// failure carries the last lines Blender printed.
void exportBlend(const std::filesystem::path& blend, const std::filesystem::path& glb);

}  // namespace splash
