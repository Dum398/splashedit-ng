// Play with 2 linked consoles: two pcsx-redux joined over SIO1, the way two
// PlayStations are by a link cable, so a two-player psxsplash scene (Lua's
// Net.*) runs in the editor. Console 1 is redux's SIO1 server, console 2 its
// client; each reads its own pcsx.json from its own directory (-portable),
// and both boot the same Play directory. No SDL here; the processes live in
// the app.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "scene.hh"

namespace editor {

// Console `index` (0 or 1)'s own directory, <playDir>/console1 or console2:
// its pcsx.json, its memory cards and its frame capture files.
std::filesystem::path consoleDir(const std::filesystem::path& playDir, int index);

// The pcsx.json for one console. pcsx-redux reads its SIO1 settings only from
// there (emulator.Debug), in its default Protobuf mode: console 1 listens on
// `port` (SIO1Server), console 2 connects to 127.0.0.1:`port` (SIO1Client).
std::string linkSettings(bool server, uint16_t port);

// Lua for a -dofile before framecapture.lua: it points the capture at `dir`
// (SplashEditCaptureDir) instead of the shared PCdrv directory, so each
// console answers its own capture.flag.
std::string captureDirLua(const std::filesystem::path& dir);

// Writes `dir`/pcsx.json and `dir`/capturedir.lua (creating `dir`) and clears
// an earlier session's capture files. False with `error` set if it cannot.
bool prepareConsole(const std::filesystem::path& dir, bool server, uint16_t port, std::string* error);

// A TCP port nothing listens on now, picked by the system; 0 if none.
uint16_t pickLinkPort();

// True once something holds `port` (console 1's link server is up). It tries
// to bind the port rather than connect to it: redux's server takes the first
// connection as its link, and a probe would steal it from console 2.
bool linkPortTaken(uint16_t port);

// The status reporter Play adds to each scene it exports for 2 consoles: an
// object named kLinkReporter whose script (linkReporterLua, saved at
// `script`) prints the console's session once a second, as
// "splashedit-link: state S host H slot L players P". psxsplash itself
// prints nothing about its link.
inline constexpr const char* kLinkReporter = "SplashEdit link status";
std::string linkReporterLua();
splash::Scene withLinkReporter(splash::Scene scene, const std::filesystem::path& script);

// One console's session, from a reporter line. state is Net.State():
// 0 not connected, 1 connecting, 2 connected, 3 version mismatch,
// 4 scene mismatch.
struct LinkStatus {
    int state = 0;
    bool host = false;
    int slot = 0;
    int players = 0;
    bool linked() const { return state == 2; }
};
// Parses one output line; false if it is not a reporter line.
bool parseLinkStatus(const std::string& line, LinkStatus* out);
// The status line under a console's view, e.g. "Linked · slot 0 · host · 2
// players". With `networkId` false (the scene has no Network id) it ends
// with a hint to set one, as a networked scene should.
std::string describeLinkStatus(const LinkStatus& status, bool networkId);

}  // namespace editor
