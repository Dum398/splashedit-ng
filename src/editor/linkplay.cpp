#include "editor/linkplay.hh"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace editor {

static std::string utf8(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

fs::path consoleDir(const fs::path& playDir, int index) { return playDir / ("console" + std::to_string(index + 1)); }

std::string linkSettings(bool server, uint16_t port) {
    const std::string p = std::to_string(port);
    if (server)
        return "{\"emulator\":{\"Debug\":{\"SIO1Server\":true,\"SIO1ServerPort\":" + p + ",\"SIO1Client\":false}}}\n";
    return "{\"emulator\":{\"Debug\":{\"SIO1Server\":false,\"SIO1Client\":true,\"SIO1Clienthost\":\"127.0.0.1\","
           "\"SIO1ClientPort\":" +
           p + "}}}\n";
}

std::string captureDirLua(const fs::path& dir) {
    // A long bracket at a level no path holds, so nothing in it needs escaping.
    return "-- SplashEdit: this console's frame capture files live in its own directory.\n"
           "SplashEditCaptureDir = [==[" +
           utf8(dir) + "]==]\n";
}

static bool writeText(const fs::path& file, const std::string& text, std::string* error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (out) return true;
    if (error) *error = "Cannot write " + utf8(file);
    return false;
}

bool prepareConsole(const fs::path& dir, bool server, uint16_t port, std::string* error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "Cannot create " + utf8(dir) + ": " + ec.message();
        return false;
    }
    for (const char* stale : {"capture.flag", "capture.gpd", "capture.err", "capture.gpd.part"}) fs::remove(dir / stale, ec);
    return writeText(dir / "pcsx.json", linkSettings(server, port), error) &&
           writeText(dir / "capturedir.lua", captureDirLua(dir), error);
}

#ifdef _WIN32
using Sock = SOCKET;
static const Sock kNoSock = INVALID_SOCKET;
static void closeSock(Sock s) { closesocket(s); }
struct Net {
    bool ok;
    Net() {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    ~Net() {
        if (ok) WSACleanup();
    }
};
#else
using Sock = int;
static const Sock kNoSock = -1;
static void closeSock(Sock s) { close(s); }
struct Net {
    bool ok = true;
};
#endif

// Binds a fresh TCP socket to 0.0.0.0:`port` (all interfaces, as redux's
// server does) without SO_REUSEADDR; the bound port, or 0 if it failed.
static uint16_t bindAny(uint16_t port) {
    Net net;
    if (!net.ok) return 0;
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSock) return 0;
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    uint16_t got = 0;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in b;
        socklen_t n = sizeof b;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&b), &n) == 0) got = ntohs(b.sin_port);
    }
    closeSock(s);
    return got;
}

uint16_t pickLinkPort() { return bindAny(0); }

bool linkPortTaken(uint16_t port) { return port != 0 && bindAny(port) == 0; }

std::string linkReporterLua() {
    return "-- SplashEdit: reports this console's link session for Play's Game view.\n"
           "-- Added by Play with 2 linked consoles; not part of the scene.\n"
           "local frames = 0\n"
           "function onUpdate(self, dt)\n"
           "    frames = frames + 1\n"
           "    if frames < 60 then return end\n"
           "    frames = 0\n"
           "    local host = 0\n"
           "    if Net.IsHost() then host = 1 end\n"
           "    Debug.Log(\"splashedit-link: state \" .. Net.State() .. \" host \" .. host .. \" slot \" .. Net.LocalSlot() ..\n"
           "              \" players \" .. Net.PlayerCount())\n"
           "end\n";
}

splash::Scene withLinkReporter(splash::Scene scene, const fs::path& script) {
    splash::Object o;
    o.name = kLinkReporter;
    o.script = splash::ScriptComponent{utf8(script), {}};
    // Last, so every object the scene has keeps its index (and actor id).
    scene.objects.push_back(std::move(o));
    return scene;
}

bool parseLinkStatus(const std::string& line, LinkStatus* out) {
    const char* tag = "splashedit-link: ";
    const size_t at = line.find(tag);
    if (at == std::string::npos) return false;
    LinkStatus s;
    int host = 0;
    if (std::sscanf(line.c_str() + at + std::strlen(tag), "state %d host %d slot %d players %d", &s.state, &host, &s.slot,
                    &s.players) != 4)
        return false;
    s.host = host != 0;
    if (out) *out = s;
    return true;
}

std::string describeLinkStatus(const LinkStatus& s, bool networkId) {
    std::string line;
    switch (s.state) {
        case 0: line = "Not linked: the scene has not called Net.Connect()"; break;
        case 1: line = "Connecting..."; break;
        case 2:
            line = "Linked \xc2\xb7 slot " + std::to_string(s.slot) + (s.host ? " \xc2\xb7 host" : " \xc2\xb7 not host") +
                   " \xc2\xb7 " + std::to_string(s.players) + " players";
            break;
        case 3: line = "Link refused: the other console runs another engine version"; break;
        case 4: line = "Link refused: the other console runs another scene"; break;
        default: line = "Link state " + std::to_string(s.state); break;
    }
    if (!networkId) line += " \xc2\xb7 no Network id: set one in Scene > Network";
    return line;
}

}  // namespace editor
