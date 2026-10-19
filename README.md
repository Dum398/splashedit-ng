# SplashEdit NG

Standalone scene editor for [psxsplash](https://github.com/psxsplash/psxsplash), replacing the Unity-based SplashEdit. Plan and design decisions: psxsplash/splashedit#48.

## Download

[Nightly build](https://github.com/psxsplash/splashedit-ng/releases/tag/nightly), rebuilt from every push to `main`: one archive per OS with the editor, the psxsplash engine and pcsx-redux. Per-OS notes are on the release page.

`engine/` holds psxsplash.ps-exe, which Play and Run on hardware boot, psxsplash-cdrom.ps-exe for disc images, and builds with on-screen debug overlays: Build > Performance Overlay (FPS counter and heap bar) and Room Debug Overlay (room wireframe) make Play and Run on hardware boot psxsplash-perf, psxsplash-rooms or psxsplash-perf-rooms instead. Disc images always get the plain CD-ROM build.

## Building

Needs CMake 3.24+, a C++20 compiler, SDL3 and FreeType. Dear ImGui and stb are fetched at configure time.

```
cmake -S . -B build -G Ninja
cmake --build build
./build/splashedit
```

`--screenshot out.png [--size 1600x960] [--mouse x,y] [--clean] [--no-overlays]` renders a few frames and writes a PNG (`--no-overlays` hides the scene view's trigger, interact and nav overlays), which also works headless under `xvfb-run`. Screenshots skip the startup splash; `--splash` shows it (silent), `--frames N` picks the moment.

The UI follows the monitor's scale setting (e.g. 150% on Windows or X11) and rescales when the window moves to another monitor. `--ui-scale N` (or the `SPLASHEDIT_UI_SCALE` environment variable; the flag wins) sets it instead, N from 0.5 to 4. Without `--size` the window opens that much larger, within the screen; `--size` is always in window coordinates. Scripted `--click`, `--drag`, `--mouse`, `--hittest` and friends take UI coordinates, so a script works at any scale.

Fonts: Inter (SIL OFL 1.1) and Lucide (ISC), licences in `assets/fonts`.

## Branding

`assets/brand` holds the psxsplash logo, icon and "made with" badges, copied from psxsplash/brand where `tools/build_brand.py` generates them. Use these for the window icon, splash/about screens and anything the exporter shows the user; don't draw new logos here. To change the branding, change the generator in psxsplash/brand and copy the output back. The old splat with the PlayStation logo must not be used anywhere.

- `icon.svg`, `icon-*.png`: window and app icon
- `logo.svg` (light backgrounds), `logo-dark.svg`/`logo-dark.png` (dark UI), `logo-mono.svg` (one colour)
- `made-with.svg`, `made-with-square.svg`: badges for exported games
- `bootscreen.svg`: 320x240 reference for the in-engine splash
- `boot-jingle.wav`, `bootlogo-hires.png`: the boot logo's jingle and its pieces at 8x the PS1 screen, for the editor's startup splash. `bootlogo-hires.png` and `src/ui/bootlogo_hires.hh` come from brand `tools/build_bootlogo_hires.py`, which cuts the same pieces as the engine's `build_bootlogo.py`

Colours: `#ff3b3b #ffb400 #2ecc40 #1e90ff #b145ff #ff4fa3`, ink `#16122b`.
