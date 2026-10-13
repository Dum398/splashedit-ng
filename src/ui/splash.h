#pragma once

#include <imgui.h>

// The psxsplash boot logo shown when the editor starts: the same animation
// and jingle psxsplash plays before its first scene, drawn from the engine's
// own 4bpp sheet and scaled up with square pixels. A click or a key skips it.
namespace splash {

// Uploads the logo pieces and loads brand/boot-jingle.wav. Needs a current GL
// context. Returns false (and the splash never shows) if the pieces can't be
// made; a missing jingle only means no sound.
bool load(const char* assetDir, bool sound);

// True while the splash covers the editor. The editor underneath is drawn only
// once the splash starts fading out.
bool covering();

// Draws one frame over everything, in a window that takes all input. `clockMs`
// is any millisecond clock; the animation starts at its first call. Returns
// false once it is over.
bool draw(ImVec2 display, double clockMs);

// Stops the jingle and frees what load() made.
void shutdown();

}  // namespace splash
