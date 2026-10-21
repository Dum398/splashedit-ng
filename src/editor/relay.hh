// Play's relay: a PSNL server inside the editor, so Play can run 3 to 10
// linked consoles (each a pcsx-redux) with nothing else installed. Every
// console is redux's SIO1 client to it. It is a port of the default relay
// mode of psnl_server (the Python reference server): rooms by scene hash,
// slots 1..10 (slot 0 is the relay itself, which is in no game), reliable
// Events acked and fanned out, ObjectState and AppData relayed, Ping
// answered, and a 30 Hz snapshot of every member's own avatar. Unlike psnl it
// also tells every member when the room changes (an unsolicited HelloAck), so
// each console's player count stays current, and it gives each room an
// authority: its first member, then, when the authority leaves, the lowest
// slot still in it. The HelloAck's hostSlot names it, so that console is
// psxsplash's host (yourSlot == hostSlot) and the rest are clients; a later
// ack hands authority over. psxsplash needs no change for any of it.
//
// The pure parts (CRC, frames, redux's protobuf wire, rooms and the snapshot)
// have no sockets and are tested on their own; Relay runs them on a thread
// with a listening socket. No SDL, no ImGui.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace editor::relay {

// ---- PSNL frames (psxsplash src/netprotocol.hh) ----

inline constexpr uint8_t kSync = 0xA5;
inline constexpr size_t kHeaderSize = 8;    // <BBBBHH type, srcSlot, flags, reserved, seq, length
inline constexpr size_t kMaxPayload = 512;  // longer is a mis-sync
inline constexpr uint32_t kMagic = 0x4C4E5350;  // "PSNL"
inline constexpr uint16_t kProtoVersion = 1;
inline constexpr uint8_t kRelaySlot = 0;    // the relay: a sender and receiver in the log, never the authority
inline constexpr uint8_t kNoSlot = 0xFF;
inline constexpr uint8_t kFirstPlayerSlot = 1;
inline constexpr uint8_t kMaxSlots = 11;    // slots 0..10; players 1..10

enum PacketType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Snapshot = 3,
    Event = 4,
    EventAck = 5,
    Ping = 6,
    Bye = 7,
    ObjectState = 8,
    AppData = 9,
    Pong = 10,
};
inline constexpr int kPacketTypes = 10;  // 1..kPacketTypes
// "Hello", "HelloAck", ...; "Type N" for one psxsplash does not have.
std::string packetTypeName(uint8_t type);

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), chainable: crc16("123456789") == 0x29B1.
uint16_t crc16(const uint8_t* data, size_t n, uint16_t crc = 0xFFFF);

struct Frame {
    uint8_t type = 0, srcSlot = kNoSlot, flags = 0, reserved = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> payload;
};

// [sync][header][payload][crc16 LE over header + payload]. A payload over
// kMaxPayload is cut to it (the relay never builds one).
std::vector<uint8_t> encodeFrame(uint8_t type, const std::vector<uint8_t>& payload, uint8_t srcSlot, uint16_t seq,
                                 uint8_t flags = 0);

// The console's receive state machine: hunts for the sync byte, drops one
// byte and hunts again on a bad CRC or an impossible length. Feed it any
// chunks; whole valid frames come out.
class FrameParser {
  public:
    void feed(const uint8_t* data, size_t n, std::vector<Frame>& out);
    uint64_t frames = 0, crcErrors = 0, resyncs = 0;

  private:
    std::vector<uint8_t> m_buf;
};

// ---- pcsx-redux's SIO1 client wire (src/core/sio1.h) ----
//
// [u8 len][SIOPayload] repeated; SIOPayload{1: DataTransfer{1: bytes data},
// 2: FlowControl{1: bool dxr, 2: bool xts}}. Redux ends the session on a
// message it cannot decode, so the relay only ever sends DataTransfers of at
// most kMaxSioData bytes, and never a FlowControl (one with xts off would
// stop redux sending).
inline constexpr size_t kMaxSioData = 245;  // the largest whose message still fits the u8 length
// `n` bytes as DataTransfer messages of at most kMaxSioData each, appended to `out`.
void encodeSio(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

class SioDecoder {
  public:
    // Appends the data bytes of every whole message to `out`. A message it
    // cannot read is skipped and counted, not fatal.
    void feed(const uint8_t* data, size_t n, std::vector<uint8_t>& out);
    uint64_t messages = 0, flowControls = 0, decodeErrors = 0;

  private:
    std::vector<uint8_t> m_buf;
};

// ---- payloads ----

struct HelloPayload {  // <IHHII magic, protoVersion, reserved, sceneHash, tiebreak
    uint32_t magic = kMagic, sceneHash = 0, tiebreak = 0;
    uint16_t protoVersion = kProtoVersion;
};
std::optional<HelloPayload> parseHello(const std::vector<uint8_t>& payload);
std::vector<uint8_t> packHello(const HelloPayload& h);

struct HelloAckPayload {  // <IHBBIB3s magic, protoVersion, yourSlot, hostSlot, sceneHash, playerCount
    uint32_t magic = kMagic, sceneHash = 0;
    uint16_t protoVersion = kProtoVersion;
    uint8_t yourSlot = kNoSlot, hostSlot = kRelaySlot, playerCount = 0;  // hostSlot 0 (the relay): nobody is host
};
std::optional<HelloAckPayload> parseHelloAck(const std::vector<uint8_t>& payload);
std::vector<uint8_t> packHelloAck(const HelloAckPayload& a);

// ---- snapshots (psnl_server/rooms.py, Room.build_snapshot) ----

struct Avatar {
    int32_t px = 0, py = 0, pz = 0, yaw = 0;
    uint8_t flags = 0x01;  // SNAP_FLAG_ACTIVE
};
// One snapshot for `members` (slot, avatar), byte for byte what psnl builds:
// sorted by slot; compact format 1 (an origin and 16-bit offsets) from 2
// avatars when the spread fits in 16 bits at some shift up to 15, else the
// 20-byte legacy records.
std::vector<uint8_t> buildSnapshot(std::vector<std::pair<uint8_t, Avatar>> members);

// ---- the relay's rules, without sockets ----

// One routed packet, for the Network view. `from` is the sender's slot:
// kRelaySlot for the relay, kNoSlot for a console that has none yet (then
// `conn` says which). `to` lists the slots it went to (kRelaySlot: the relay
// took it). Snapshots and Ping/Pong a sender sends within a second fold into
// one entry, `count` of them. A row with `authority` set is no packet: the
// room (`conn`) got that authority, after `previous` (kNoSlot: the room's
// first); `type` is 0 and `to` the members it was told to.
struct Packet {
    uint64_t id = 0;
    double time = 0;  // seconds since the relay started
    uint8_t from = kNoSlot, type = 0;
    int conn = 0;
    uint16_t seq = 0;
    uint32_t size = 0;  // frame bytes, all `count` of them
    uint32_t count = 1;
    std::vector<uint8_t> to;
    uint8_t authority = kNoSlot, previous = kNoSlot;
    bool handOff() const { return authority != kNoSlot; }
};

// One accepted connection: `index` counts from 1 in accept order (console k
// of a Play is the relay's connection k).
struct Connection {
    int index = 0;
    uint8_t slot = kNoSlot;
    int room = 0;  // 0: none
    uint8_t authority = kNoSlot;  // its room's authority slot
    bool isAuthority() const { return joined && slot != kNoSlot && authority == slot; }
    uint32_t sceneHash = 0;
    bool open = true, joined = false;
    double connectedAt = 0, lastSeen = 0;
    uint64_t framesIn = 0, framesOut = 0, bytesIn = 0, bytesOut = 0;
};

inline constexpr double kSnapshotSeconds = 1.0 / 30;
inline constexpr double kPeerTimeoutSeconds = 20;
inline constexpr size_t kPacketLog = 4096;

class Hub {
  public:
    // A new connection; its index (1, 2, ...).
    int connect(double now);
    // PSNL bytes connection `conn` sent.
    void receive(int conn, const uint8_t* data, size_t n, double now);
    // The connection is gone: it leaves its room.
    void disconnect(int conn, double now);
    // Call often: sends the snapshots due and drops members silent for kPeerTimeoutSeconds.
    void tick(double now);

    // PSNL bytes to send, per connection, since the last call.
    std::vector<std::pair<int, std::vector<uint8_t>>> takeOutput();
    // Connections the hub ended (Bye, timeout) for the caller to close, since the last call.
    std::vector<int> takeClosed();

    const std::map<int, Connection>& connections() const { return m_conns; }
    const std::deque<Packet>& packets() const { return m_log; }
    uint64_t packetCount() const { return m_nextPacket - 1; }
    // Players in `conn`'s scene group of its room (the HelloAck's playerCount).
    int visiblePlayers(int conn) const;
    // When true, a room change is not announced to the members it did not
    // happen to (psnl's own behaviour; for tests of what the addition fixes).
    bool quietRoomChanges = false;

  private:
    struct Room {
        int id = 0;
        std::optional<uint32_t> sceneHash;
        std::map<uint8_t, int> members;  // slot -> connection
        uint8_t authority = kNoSlot;     // psxsplash's host: the slot whose world objects count
    };
    struct Session {
        int id = 0;
        FrameParser parser;
        bool connected = false;  // said Hello and was answered
        std::optional<uint16_t> rxSeq;
        uint16_t txSeq = 1;
        Avatar avatar;
        std::vector<uint8_t> out;
    };
    void dispatch(Session& s, const Frame& f, double now);
    void onHello(Session& s, const Frame& f, double now);
    void onReliable(Session& s, const Frame& f, double now);
    bool join(Session& s, uint32_t sceneHash, double now);
    void leave(Session& s, double now);
    void sendAck(Session& s, double now);
    void announce(int roomId, int except, double now);
    void setAuthority(Room& r, uint8_t slot, double now);
    void send(Session& s, uint8_t type, const std::vector<uint8_t>& payload, std::optional<uint16_t> seq);
    void sendRaw(Session& s, const std::vector<uint8_t>& frame);
    void close(Session& s, double now, bool told);
    Connection& info(const Session& s) { return m_conns.at(s.id); }
    void log(double now, uint8_t from, int conn, uint8_t type, uint16_t seq, size_t size, std::vector<uint8_t> to);
    Room* room(int id);

    std::map<int, Session> m_sessions;
    std::map<int, Connection> m_conns;  // every connection's info, kept after it closes
    std::vector<Room> m_rooms;
    int m_nextConn = 1, m_nextRoom = 1;
    double m_nextSnapshot = 0;
    std::vector<int> m_closed;
    std::deque<Packet> m_log;
    uint64_t m_nextPacket = 1;
    std::map<std::pair<int, uint8_t>, uint64_t> m_folding;  // (conn, type) -> the entry still taking more
};

// ---- the server ----

// Listens on 127.0.0.1 at a port the system picks and runs a Hub on its own
// thread. Thread safe.
class Relay {
  public:
    Relay();
    ~Relay();
    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // False with `error` set if it cannot listen.
    bool start(std::string* error);
    // Closes every connection and the listener. What it routed stays readable.
    void stop();
    bool running() const { return m_running; }
    uint16_t port() const { return m_port; }
    // Connections accepted since start().
    int accepted() const { return m_accepted; }
    // Seconds since start(), the clock of Connection and Packet times.
    double seconds() const;

    std::vector<Connection> connections() const;
    // The routed packets kept (the newest kPacketLog), oldest first, with id over `after`.
    std::vector<Packet> packets(uint64_t after = 0) const;
    uint64_t packetCount() const;

  private:
    void run();
    struct Impl;
    std::unique_ptr<Impl> m;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_stop{false};
    std::atomic<int> m_accepted{0};
    uint16_t m_port = 0;
};

// The Network view's filter: packet types shown (bit t for type t) and the
// slot a packet must come from or go to (-1: any).
struct PacketFilter {
    uint32_t types = ~0u;
    int slot = -1;
    bool shows(const Packet& p) const;
};
// The row's type in words: packetTypeName, or "Authority slot 2 (was 1)".
std::string describeType(const Packet& p);
// "relay", "slot 3", "conn 2" (a console with no slot yet).
std::string describeEnd(uint8_t slot, int conn);
// The recipients in words: "slot 2, 3", "relay".
std::string describeTo(const std::vector<uint8_t>& to);

}  // namespace editor::relay
