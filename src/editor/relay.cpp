#include "editor/relay.hh"

#include <algorithm>
#include <chrono>
#include <cstring>

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
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace editor::relay {

// ---- little-endian helpers ----

static void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
static void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string packetTypeName(uint8_t type) {
    static const char* names[] = {"Hello", "HelloAck", "Snapshot", "Event", "EventAck",
                                  "Ping",  "Bye",      "ObjectState", "AppData", "Pong"};
    if (type >= 1 && type <= kPacketTypes) return names[type - 1];
    return "Type " + std::to_string(type);
}

uint16_t crc16(const uint8_t* data, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; ++i) {
        crc = uint16_t(crc ^ (data[i] << 8));
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

std::vector<uint8_t> encodeFrame(uint8_t type, const std::vector<uint8_t>& payload, uint8_t srcSlot, uint16_t seq,
                                 uint8_t flags) {
    const size_t n = std::min(payload.size(), kMaxPayload);
    std::vector<uint8_t> f;
    f.reserve(1 + kHeaderSize + n + 2);
    f.push_back(kSync);
    f.push_back(type);
    f.push_back(srcSlot);
    f.push_back(flags);
    f.push_back(0);
    put16(f, seq);
    put16(f, uint16_t(n));
    f.insert(f.end(), payload.begin(), payload.begin() + std::ptrdiff_t(n));
    put16(f, crc16(f.data() + 1, kHeaderSize + n));
    return f;
}

void FrameParser::feed(const uint8_t* data, size_t n, std::vector<Frame>& out) {
    m_buf.insert(m_buf.end(), data, data + n);
    size_t at = 0;  // consumed so far
    for (;;) {
        while (at < m_buf.size() && m_buf[at] != kSync) ++at;  // hunt for the sync byte
        if (m_buf.size() - at < 1 + kHeaderSize) break;
        const uint8_t* h = m_buf.data() + at + 1;
        const uint16_t len = get16(h + 6);
        if (len > kMaxPayload) {  // a mis-sync: hunt again from the next byte
            ++resyncs;
            ++at;
            continue;
        }
        const size_t total = 1 + kHeaderSize + len + 2;
        if (m_buf.size() - at < total) break;
        if (crc16(h, kHeaderSize + len) != get16(h + kHeaderSize + len)) {
            ++crcErrors;
            ++at;
            continue;
        }
        Frame f;
        f.type = h[0];
        f.srcSlot = h[1];
        f.flags = h[2];
        f.reserved = h[3];
        f.seq = get16(h + 4);
        f.payload.assign(h + kHeaderSize, h + kHeaderSize + len);
        out.push_back(std::move(f));
        ++frames;
        at += total;
    }
    m_buf.erase(m_buf.begin(), m_buf.begin() + std::ptrdiff_t(at));
}

// ---- protobuf ----

static void putVarint(std::vector<uint8_t>& b, uint64_t v) {
    while (v >= 0x80) {
        b.push_back(uint8_t(v | 0x80));
        v >>= 7;
    }
    b.push_back(uint8_t(v));
}

void encodeSio(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    for (size_t at = 0; at < n; at += kMaxSioData) {
        const size_t k = std::min(kMaxSioData, n - at);
        std::vector<uint8_t> inner;  // DataTransfer{1: data}
        inner.push_back(0x0A);
        putVarint(inner, k);
        inner.insert(inner.end(), data + at, data + at + k);
        std::vector<uint8_t> msg;  // SIOPayload{1: DataTransfer}
        msg.push_back(0x0A);
        putVarint(msg, inner.size());
        msg.insert(msg.end(), inner.begin(), inner.end());
        out.push_back(uint8_t(msg.size()));  // at most 251 with k <= 245
        out.insert(out.end(), msg.begin(), msg.end());
    }
}

namespace {
// Reads protobuf fields from [p, end); false when the bytes are not well formed.
struct Reader {
    const uint8_t *p, *end;
    bool varint(uint64_t* v) {
        uint64_t r = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) return false;
            const uint8_t b = *p++;
            r |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) {
                *v = r;
                return true;
            }
        }
        return false;
    }
    // Skips one field of `wire` type; false if it cannot.
    bool skip(unsigned wire) {
        uint64_t v;
        switch (wire) {
            case 0: return varint(&v);
            case 1: if (end - p < 8) return false; p += 8; return true;
            case 2: if (!varint(&v) || uint64_t(end - p) < v) return false; p += v; return true;
            case 5: if (end - p < 4) return false; p += 4; return true;
            default: return false;
        }
    }
};
}  // namespace

void SioDecoder::feed(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    m_buf.insert(m_buf.end(), data, data + n);
    size_t at = 0;
    while (at < m_buf.size()) {
        const size_t size = m_buf[at];
        if (m_buf.size() - at < 1 + size) break;
        const uint8_t* msg = m_buf.data() + at + 1;
        at += 1 + size;
        ++messages;
        Reader r{msg, msg + size};
        std::vector<uint8_t> got;
        bool ok = true, flow = false;
        while (ok && r.p < r.end) {
            uint64_t key;
            if (!r.varint(&key)) { ok = false; break; }
            const unsigned field = unsigned(key >> 3), wire = unsigned(key & 7);
            if (field == 1 && wire == 2) {  // DataTransfer
                uint64_t len;
                if (!r.varint(&len) || uint64_t(r.end - r.p) < len) { ok = false; break; }
                Reader in{r.p, r.p + len};
                r.p += len;
                while (ok && in.p < in.end) {
                    uint64_t ik;
                    if (!in.varint(&ik)) { ok = false; break; }
                    if ((ik >> 3) == 1 && (ik & 7) == 2) {
                        uint64_t dl;
                        if (!in.varint(&dl) || uint64_t(in.end - in.p) < dl) { ok = false; break; }
                        got.insert(got.end(), in.p, in.p + dl);
                        in.p += dl;
                    } else if (!in.skip(unsigned(ik & 7))) ok = false;
                }
            } else {
                if (field == 2) flow = true;
                if (!r.skip(wire)) ok = false;
            }
        }
        if (!ok) {
            ++decodeErrors;
            continue;
        }
        if (got.empty() && flow) ++flowControls;
        out.insert(out.end(), got.begin(), got.end());
    }
    m_buf.erase(m_buf.begin(), m_buf.begin() + std::ptrdiff_t(at));
}

// ---- payloads ----

std::optional<HelloPayload> parseHello(const std::vector<uint8_t>& b) {
    if (b.size() != 16) return std::nullopt;
    HelloPayload h;
    h.magic = get32(&b[0]);
    h.protoVersion = get16(&b[4]);
    h.sceneHash = get32(&b[8]);
    h.tiebreak = get32(&b[12]);
    return h;
}

std::vector<uint8_t> packHello(const HelloPayload& h) {
    std::vector<uint8_t> b;
    put32(b, h.magic);
    put16(b, h.protoVersion);
    put16(b, 0);
    put32(b, h.sceneHash);
    put32(b, h.tiebreak);
    return b;
}

std::optional<HelloAckPayload> parseHelloAck(const std::vector<uint8_t>& b) {
    if (b.size() != 16) return std::nullopt;
    HelloAckPayload a;
    a.magic = get32(&b[0]);
    a.protoVersion = get16(&b[4]);
    a.yourSlot = b[6];
    a.hostSlot = b[7];
    a.sceneHash = get32(&b[8]);
    a.playerCount = b[12];
    return a;
}

std::vector<uint8_t> packHelloAck(const HelloAckPayload& a) {
    std::vector<uint8_t> b;
    put32(b, a.magic);
    put16(b, a.protoVersion);
    b.push_back(a.yourSlot);
    b.push_back(a.hostSlot);
    put32(b, a.sceneHash);
    b.push_back(a.playerCount);
    b.insert(b.end(), 3, 0);
    return b;
}

// ---- snapshots ----

static constexpr int64_t kFp12TwoPi = 25736;  // one turn in 4.12 fixed point

static int64_t normalYaw(int32_t yaw) { return ((int64_t(yaw) % kFp12TwoPi) + kFp12TwoPi) % kFp12TwoPi; }

std::vector<uint8_t> buildSnapshot(std::vector<std::pair<uint8_t, Avatar>> members, std::vector<WorldObject> objects) {
    std::stable_sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint8_t> b;
    const size_t records = members.size() + objects.size();
    if (records >= 2) {  // compact pays for itself from 2 records
        int64_t lo[3], hi[3];
        for (int k = 0; k < 3; ++k) lo[k] = hi[k] = 0;
        for (size_t i = 0; i < records; ++i) {
            const int64_t v[3] = {i < members.size() ? members[i].second.px : objects[i - members.size()].px,
                                  i < members.size() ? members[i].second.py : objects[i - members.size()].py,
                                  i < members.size() ? members[i].second.pz : objects[i - members.size()].pz};
            for (int k = 0; k < 3; ++k) {
                if (i == 0 || v[k] < lo[k]) lo[k] = v[k];
                if (i == 0 || v[k] > hi[k]) hi[k] = v[k];
            }
        }
        const int64_t spread = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        int shift = -1;
        for (int s = 0; s <= 15; ++s)
            if ((spread >> s) <= 0xFFFF) {
                shift = s;
                break;
            }
        if (shift >= 0) {
            // As many objects as fit one frame: 16 bytes of header and origin, 10 an avatar, 11 an object.
            const size_t room = (kMaxPayload - 16 - 10 * members.size()) / 11;
            if (objects.size() > room) objects.resize(room);
            b.push_back(uint8_t(members.size()));
            b.push_back(uint8_t(objects.size()));
            b.push_back(1);  // SNAP_FORMAT_COMPACT
            b.push_back(uint8_t(shift));
            for (int k = 0; k < 3; ++k) put32(b, uint32_t(int32_t(lo[k])));
            for (const auto& [slot, a] : members) {
                b.push_back(slot);
                b.push_back(a.flags);
                put16(b, uint16_t((int64_t(a.px) - lo[0]) >> shift));
                put16(b, uint16_t((int64_t(a.py) - lo[1]) >> shift));
                put16(b, uint16_t((int64_t(a.pz) - lo[2]) >> shift));
                put16(b, uint16_t(normalYaw(a.yaw)));
            }
            for (const WorldObject& o : objects) {
                put16(b, o.actorId);
                b.push_back(o.flags);
                put16(b, uint16_t((int64_t(o.px) - lo[0]) >> shift));
                put16(b, uint16_t((int64_t(o.py) - lo[1]) >> shift));
                put16(b, uint16_t((int64_t(o.pz) - lo[2]) >> shift));
                put16(b, uint16_t(normalYaw(o.yaw)));
            }
            return b;
        }
    }
    const size_t room = (kMaxPayload - 4) / 20 - std::min(members.size(), (kMaxPayload - 4) / 20);
    if (objects.size() > room) objects.resize(room);
    b.push_back(uint8_t(members.size()));
    b.push_back(uint8_t(objects.size()));
    b.push_back(0);  // SNAP_FORMAT_LEGACY
    b.push_back(0);
    for (const auto& [slot, a] : members) {
        b.push_back(slot);
        b.push_back(a.flags);
        put16(b, 0);
        put32(b, uint32_t(a.px));
        put32(b, uint32_t(a.py));
        put32(b, uint32_t(a.pz));
        put32(b, uint32_t(a.yaw));
    }
    for (const WorldObject& o : objects) {
        put16(b, o.actorId);
        b.push_back(o.flags);
        b.push_back(0);
        put32(b, uint32_t(o.px));
        put32(b, uint32_t(o.py));
        put32(b, uint32_t(o.pz));
        put32(b, uint32_t(o.yaw));
    }
    return b;
}

// ---- Hub ----

int Hub::connect(double now) {
    const int id = m_nextConn++;
    Session& s = m_sessions[id];
    s.id = id;
    Connection& c = m_conns[id];
    c.index = id;
    c.connectedAt = c.lastSeen = now;
    return id;
}

Hub::Room* Hub::room(int id) {
    for (Room& r : m_rooms)
        if (r.id == id) return &r;
    return nullptr;
}

int Hub::visiblePlayers(int conn) const {
    auto it = m_conns.find(conn);
    if (it == m_conns.end() || !it->second.room) return 1;
    for (const Room& r : m_rooms) {
        if (r.id != it->second.room) continue;
        int n = 0;
        for (const auto& [slot, member] : r.members)
            if (m_conns.at(member).sceneHash == it->second.sceneHash) ++n;
        return n;
    }
    return 1;
}

void Hub::log(double now, uint8_t from, int conn, uint8_t type, uint16_t seq, size_t size, std::vector<uint8_t> to) {
    const bool folds = type == Snapshot || type == Ping || type == Pong;
    const std::pair<int, uint8_t> key(from == kRelaySlot ? -conn : conn, type);
    if (folds) {
        auto it = m_folding.find(key);
        if (it != m_folding.end() && !m_log.empty() && it->second >= m_log.front().id) {
            Packet& p = m_log[size_t(it->second - m_log.front().id)];
            if (now - p.time < 1.0 && p.to == to && p.from == from) {
                ++p.count;
                p.size += uint32_t(size);
                return;
            }
        }
    }
    Packet p;
    p.id = m_nextPacket++;
    p.time = now;
    p.from = from;
    p.conn = conn;
    p.type = type;
    p.seq = seq;
    p.size = uint32_t(size);
    p.to = std::move(to);
    if (folds) m_folding[key] = p.id;
    m_log.push_back(std::move(p));
    while (m_log.size() > kPacketLog) m_log.pop_front();
}

void Hub::sendRaw(Session& s, const std::vector<uint8_t>& frame) {
    s.out.insert(s.out.end(), frame.begin(), frame.end());
    Connection& c = info(s);
    ++c.framesOut;
    c.bytesOut += frame.size();
}

void Hub::send(Session& s, uint8_t type, const std::vector<uint8_t>& payload, std::optional<uint16_t> seq) {
    uint16_t q;
    if (seq) q = *seq;
    else {
        q = s.txSeq;
        s.txSeq = uint16_t(s.txSeq + 1);
    }
    sendRaw(s, encodeFrame(type, payload, kRelaySlot, q));
}

void Hub::sendAck(Session& s, double now) {
    Connection& c = info(s);
    HelloAckPayload a;
    a.yourSlot = c.slot;
    a.hostSlot = c.authority;  // the room's authority is psxsplash's host
    a.sceneHash = c.sceneHash;
    a.playerCount = uint8_t(visiblePlayers(s.id));
    const uint16_t seq = s.txSeq;
    send(s, HelloAck, packHelloAck(a), std::nullopt);
    log(now, kRelaySlot, s.id, HelloAck, seq, 1 + kHeaderSize + 16 + 2, {c.slot});
}

void Hub::announce(int roomId, int except, double now) {
    if (quietRoomChanges) return;
    Room* r = room(roomId);
    if (!r) return;
    const std::map<uint8_t, int> members = r->members;
    for (const auto& [slot, conn] : members) {
        if (conn == except) continue;
        auto it = m_sessions.find(conn);
        if (it != m_sessions.end() && it->second.connected) sendAck(it->second, now);
    }
}

void Hub::setAuthority(Room& r, uint8_t slot, double now) {
    Packet p;
    p.id = m_nextPacket++;
    p.time = now;
    p.from = kRelaySlot;
    p.conn = r.id;
    p.type = 0;
    p.previous = r.authority;
    p.authority = slot;
    r.authority = slot;
    for (const auto& [member, conn] : r.members) {
        m_conns.at(conn).authority = slot;
        p.to.push_back(member);
    }
    m_log.push_back(std::move(p));
    while (m_log.size() > kPacketLog) m_log.pop_front();
}

bool Hub::join(Session& s, uint32_t sceneHash, double now) {
    Room* target = nullptr;
    for (Room& r : m_rooms) {
        if (r.members.size() >= size_t(kMaxSlots - kFirstPlayerSlot)) continue;
        if (r.sceneHash && *r.sceneHash != sceneHash) continue;
        target = &r;
        break;
    }
    if (!target) {
        m_rooms.push_back(Room{});
        target = &m_rooms.back();
        target->id = m_nextRoom++;
    }
    if (!target->sceneHash) target->sceneHash = sceneHash;
    uint8_t slot = kFirstPlayerSlot;
    while (slot < kMaxSlots && target->members.count(slot)) ++slot;
    if (slot >= kMaxSlots) return false;
    target->members[slot] = s.id;
    Connection& c = info(s);
    c.slot = slot;
    c.room = target->id;
    c.joined = true;
    c.authority = target->authority;
    // The first member is the authority, and stays it while it is in: a later
    // console with a lower slot (one freed by a leaver) does not take it.
    if (target->authority == kNoSlot) setAuthority(*target, slot, now);
    return true;
}

void Hub::leave(Session& s, double now) {
    Connection& c = info(s);
    Room* r = room(c.room);
    const int roomId = c.room;
    c.room = 0;
    c.joined = false;
    c.authority = kNoSlot;
    s.objects.clear();
    if (!r) return;
    r->members.erase(c.slot);
    if (r->members.empty()) {
        m_rooms.erase(std::remove_if(m_rooms.begin(), m_rooms.end(), [&](const Room& x) { return x.id == roomId; }),
                      m_rooms.end());
        return;
    }
    // The last straggler left a scene move: the room follows the rest (rooms.py Room.remove).
    std::optional<uint32_t> only;
    bool one = true;
    for (const auto& [slot, conn] : r->members) {
        const uint32_t h = m_conns.at(conn).sceneHash;
        if (only && *only != h) one = false;
        only = h;
    }
    if (one && only) r->sceneHash = only;
    // The authority left: the lowest slot still in takes over, and the acks below say so.
    if (r->authority == c.slot) setAuthority(*r, r->members.begin()->first, now);
    announce(roomId, s.id, now);
}

void Hub::close(Session& s, double now, bool told) {
    Connection& c = info(s);
    if (c.room) leave(s, now);
    s.connected = false;
    c.open = false;
    if (!told) m_closed.push_back(s.id);
    m_sessions.erase(s.id);
}

void Hub::disconnect(int conn, double now) {
    auto it = m_sessions.find(conn);
    if (it != m_sessions.end()) close(it->second, now, true);
}

void Hub::receive(int conn, const uint8_t* data, size_t n, double now) {
    auto it = m_sessions.find(conn);
    if (it == m_sessions.end()) return;
    Connection& c = info(it->second);
    c.lastSeen = now;
    c.bytesIn += n;
    std::vector<Frame> frames;
    it->second.parser.feed(data, n, frames);
    for (const Frame& f : frames) {
        auto again = m_sessions.find(conn);  // a Bye ends the session
        if (again == m_sessions.end()) break;
        ++info(again->second).framesIn;
        dispatch(again->second, f, now);
    }
}

void Hub::onHello(Session& s, const Frame& f, double now) {
    Connection& c = info(s);
    const size_t size = 1 + kHeaderSize + f.payload.size() + 2;
    const std::optional<HelloPayload> h = parseHello(f.payload);
    log(now, c.slot, s.id, Hello, f.seq, size, {kRelaySlot});
    if (!h || h->magic != kMagic || h->protoVersion != kProtoVersion) return;  // the console latches the mismatch itself
    if (s.connected) {
        // Again: a scene change (or a lost ack). Keep the slot, re-ack.
        const bool moved = c.sceneHash != h->sceneHash;
        c.sceneHash = h->sceneHash;
        if (moved) s.objects.clear();
        s.rxSeq.reset();
        if (Room* r = room(c.room)) {
            bool all = true;
            for (const auto& [slot, conn] : r->members)
                if (m_conns.at(conn).sceneHash != h->sceneHash) all = false;
            if (all) r->sceneHash = h->sceneHash;
        }
        sendAck(s, now);
        if (moved && c.room) announce(c.room, s.id, now);
        return;
    }
    c.sceneHash = h->sceneHash;
    if (!c.room && !join(s, h->sceneHash, now)) return;
    s.connected = true;
    sendAck(s, now);
    announce(info(s).room, s.id, now);
}

void Hub::onReliable(Session& s, const Frame& f, double now) {
    Connection& c = info(s);
    const size_t size = 1 + kHeaderSize + f.payload.size() + 2;
    std::vector<int> others;
    std::vector<uint8_t> slots;
    if (Room* r = room(c.room))
        for (const auto& [slot, conn] : r->members)
            if (conn != s.id && m_sessions.count(conn)) {
                others.push_back(conn);
                slots.push_back(slot);
            }
    const bool dup = s.rxSeq && *s.rxSeq == f.seq;
    const bool relays = !dup && (f.type == ObjectState || f.type == AppData || (f.type == Event && f.payload.size() >= 8));
    log(now, c.slot, s.id, f.type, f.seq, size, relays && !slots.empty() ? slots : std::vector<uint8_t>{kRelaySlot});
    // Ack duplicates too: that is what stops the sender retransmitting.
    send(s, EventAck, {}, f.seq);
    log(now, kRelaySlot, s.id, EventAck, f.seq, 1 + kHeaderSize + 2, {c.slot});
    if (dup) return;
    s.rxSeq = f.seq;
    if (!relays) return;
    // Events go on as their 8 bytes (id, arg), as psnl does; the rest as they came.
    const std::vector<uint8_t> payload =
        f.type == Event ? std::vector<uint8_t>(f.payload.begin(), f.payload.begin() + 8) : f.payload;
    for (int conn : others) send(m_sessions.at(conn), f.type, payload, std::nullopt);
}

void Hub::dispatch(Session& s, const Frame& f, double now) {
    Connection& c = info(s);
    const size_t size = 1 + kHeaderSize + f.payload.size() + 2;
    switch (f.type) {
        case Hello: onHello(s, f, now); break;
        case Snapshot: {
            log(now, c.slot, s.id, Snapshot, f.seq, size, {kRelaySlot});
            // psxsplash sends legacy records: its own avatar, then, from the
            // host, its world objects. The sender's own avatar record is kept,
            // and the world objects only from the room's authority; anything
            // else in it is ignored.
            if (!c.room || f.payload.size() < 4) break;
            const size_t count = f.payload[0], objectCount = f.payload[1];
            size_t off = 4;
            for (size_t i = 0; i < count && off + 20 <= f.payload.size(); ++i, off += 20) {
                const uint8_t* r = f.payload.data() + off;
                if (r[0] != c.slot) continue;
                s.avatar.flags = r[1];
                s.avatar.px = int32_t(get32(r + 4));
                s.avatar.py = int32_t(get32(r + 8));
                s.avatar.pz = int32_t(get32(r + 12));
                s.avatar.yaw = int32_t(get32(r + 16));
            }
            if (!c.isAuthority() || f.payload[2] != 0) break;  // legacy only: the format psxsplash sends
            s.objects.clear();
            for (size_t i = 0; i < objectCount && off + 20 <= f.payload.size(); ++i, off += 20) {
                const uint8_t* r = f.payload.data() + off;
                WorldObject o;
                o.actorId = get16(r);
                o.flags = r[2];
                o.px = int32_t(get32(r + 4));
                o.py = int32_t(get32(r + 8));
                o.pz = int32_t(get32(r + 12));
                o.yaw = int32_t(get32(r + 16));
                s.objects.push_back(o);
            }
            break;
        }
        case Event:
        case ObjectState:
        case AppData: onReliable(s, f, now); break;
        case Ping: {
            log(now, c.slot, s.id, Ping, f.seq, size, {kRelaySlot});
            if (f.payload.size() != 12) break;
            std::vector<uint8_t> pong(f.payload.begin(), f.payload.begin() + 4);  // the echo, as sent
            pong.insert(pong.end(), 8, 0);
            const uint16_t seq = s.txSeq;
            send(s, Pong, pong, std::nullopt);
            log(now, kRelaySlot, s.id, Pong, seq, 1 + kHeaderSize + 12 + 2, {c.slot});
            break;
        }
        case Bye:
            log(now, c.slot, s.id, Bye, f.seq, size, {kRelaySlot});
            close(s, now, false);
            break;
        default: log(now, c.slot, s.id, f.type, f.seq, size, {kRelaySlot}); break;
    }
}

void Hub::tick(double now) {
    std::vector<int> stale;
    for (auto& [id, s] : m_sessions)
        if (now - m_conns.at(id).lastSeen > kPeerTimeoutSeconds) stale.push_back(id);
    for (int id : stale)
        if (auto it = m_sessions.find(id); it != m_sessions.end()) close(it->second, now, false);
    if (now < m_nextSnapshot) return;
    m_nextSnapshot += kSnapshotSeconds;
    if (m_nextSnapshot < now) m_nextSnapshot = now + kSnapshotSeconds;  // overran: resync, no catch-up
    for (const Room& r : m_rooms) {
        std::map<uint32_t, std::vector<std::pair<uint8_t, int>>> groups;  // scene hash -> (slot, conn)
        for (const auto& [slot, conn] : r.members) groups[m_conns.at(conn).sceneHash].push_back({slot, conn});
        for (const auto& [hash, members] : groups) {
            std::vector<std::pair<uint8_t, Avatar>> avatars;
            std::vector<uint8_t> to;
            const std::vector<WorldObject>* objects = nullptr;  // the authority's, when it is in this scene group
            for (const auto& [slot, conn] : members) {
                avatars.push_back({slot, m_sessions.at(conn).avatar});
                to.push_back(slot);
                if (slot == r.authority && !m_sessions.at(conn).objects.empty()) objects = &m_sessions.at(conn).objects;
            }
            const std::vector<uint8_t> frame = encodeFrame(Snapshot, buildSnapshot(avatars), kRelaySlot, 0);
            // The others get the authority's world objects too; the authority has them.
            const std::vector<uint8_t> withObjects =
                objects ? encodeFrame(Snapshot, buildSnapshot(avatars, *objects), kRelaySlot, 0) : frame;
            size_t bytes = 0;
            for (const auto& [slot, conn] : members) {
                const std::vector<uint8_t>& f = slot == r.authority ? frame : withObjects;
                sendRaw(m_sessions.at(conn), f);
                bytes += f.size();
            }
            log(now, kRelaySlot, r.id, Snapshot, 0, bytes, to);
        }
    }
}

std::vector<std::pair<int, std::vector<uint8_t>>> Hub::takeOutput() {
    std::vector<std::pair<int, std::vector<uint8_t>>> out;
    for (auto& [id, s] : m_sessions)
        if (!s.out.empty()) {
            out.push_back({id, std::move(s.out)});
            s.out.clear();
        }
    return out;
}

std::vector<int> Hub::takeClosed() {
    std::vector<int> c;
    c.swap(m_closed);
    return c;
}

// ---- the server ----

#ifdef _WIN32
using Sock = SOCKET;
static const Sock kNoSock = INVALID_SOCKET;
static void closeSock(Sock s) { closesocket(s); }
static bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void nonBlocking(Sock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
using PollFd = WSAPOLLFD;
static int pollFds(PollFd* fds, size_t n, int ms) { return WSAPoll(fds, ULONG(n), ms); }
static int sendFlags() { return 0; }
#else
using Sock = int;
static const Sock kNoSock = -1;
static void closeSock(Sock s) { ::close(s); }
static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void nonBlocking(Sock s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    fcntl(s, F_SETFD, FD_CLOEXEC);  // not into the consoles Play starts
}
using PollFd = pollfd;
static int pollFds(PollFd* fds, size_t n, int ms) { return ::poll(fds, nfds_t(n), ms); }
#ifdef MSG_NOSIGNAL
static int sendFlags() { return MSG_NOSIGNAL; }
#else
static int sendFlags() { return 0; }
#endif
#endif

struct Relay::Impl {
    mutable std::mutex mu;  // hub
    Hub hub;
    Sock listener = kNoSock;
    struct Conn {
        Sock sock = kNoSock;
        int id = 0;
        SioDecoder decoder;
        std::vector<uint8_t> out;
        bool dead = false;
    };
    std::vector<Conn> conns;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#ifdef _WIN32
    bool wsa = false;
#endif
    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); }
};

Relay::Relay() : m(std::make_unique<Impl>()) {}

Relay::~Relay() { stop(); }

bool Relay::start(std::string* error) {
    stop();
    m = std::make_unique<Impl>();
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        if (m->listener != kNoSock) closeSock(m->listener);
        m->listener = kNoSock;
        return false;
    };
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("Winsock did not start");
    m->wsa = true;
#endif
#ifdef SOCK_CLOEXEC
    m->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    m->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (m->listener == kNoSock) return fail("cannot open a socket for the relay");
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(m->listener, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || listen(m->listener, 16) != 0)
        return fail("the relay cannot listen on 127.0.0.1");
    sockaddr_in b;
    socklen_t n = sizeof b;
    if (getsockname(m->listener, reinterpret_cast<sockaddr*>(&b), &n) != 0) return fail("the relay has no port");
    m_port = ntohs(b.sin_port);
    nonBlocking(m->listener);
    m_accepted = 0;
    m_stop = false;
    m_running = true;
    m_thread = std::thread([this] { run(); });
    return true;
}

void Relay::stop() {
    if (m_thread.joinable()) {
        m_stop = true;
        m_thread.join();
    }
    m_running = false;
    if (!m) return;
    for (Impl::Conn& c : m->conns)
        if (c.sock != kNoSock) closeSock(c.sock);
    m->conns.clear();
    if (m->listener != kNoSock) closeSock(m->listener);
    m->listener = kNoSock;
#ifdef _WIN32
    if (m->wsa) WSACleanup();
    m->wsa = false;
#endif
}

void Relay::run() {
    std::vector<PollFd> fds;
    std::vector<uint8_t> buf(4096), data;
    while (!m_stop) {
        fds.clear();
        PollFd l{};
        l.fd = m->listener;
        l.events = POLLIN;
        fds.push_back(l);
        for (const Impl::Conn& c : m->conns) {
            PollFd p{};
            p.fd = c.sock;
            p.events = short(POLLIN | (c.out.empty() ? 0 : POLLOUT));
            fds.push_back(p);
        }
        pollFds(fds.data(), fds.size(), 5);
        // New consoles, in the order they connect.
        for (;;) {
            // Close-on-exec from the start: Play may be starting a console on another thread.
#if defined(SOCK_CLOEXEC) && defined(__linux__)
            Sock s = accept4(m->listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
            Sock s = accept(m->listener, nullptr, nullptr);
#endif
            if (s == kNoSock) break;
            nonBlocking(s);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            Impl::Conn c;
            c.sock = s;
            {
                std::lock_guard<std::mutex> lock(m->mu);
                c.id = m->hub.connect(m->now());
            }
            m->conns.push_back(std::move(c));
            ++m_accepted;
        }
        const double now = m->now();
        for (size_t i = 0; i < m->conns.size(); ++i) {
            Impl::Conn& c = m->conns[i];
            const short re = i + 1 < fds.size() ? fds[i + 1].revents : short(0);
            if (!(re & (POLLIN | POLLHUP | POLLERR))) continue;
            for (;;) {
                const auto got = recv(c.sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
                if (got > 0) {
                    data.clear();
                    c.decoder.feed(buf.data(), size_t(got), data);
                    if (!data.empty()) {
                        std::lock_guard<std::mutex> lock(m->mu);
                        m->hub.receive(c.id, data.data(), data.size(), now);
                    }
                    continue;
                }
                if (got < 0 && wouldBlock()) break;
                c.dead = true;  // closed, or failed
                break;
            }
        }
        std::vector<std::pair<int, std::vector<uint8_t>>> out;
        std::vector<int> closed;
        {
            std::lock_guard<std::mutex> lock(m->mu);
            for (const Impl::Conn& c : m->conns)
                if (c.dead) m->hub.disconnect(c.id, now);
            m->hub.tick(now);
            out = m->hub.takeOutput();
            closed = m->hub.takeClosed();
        }
        for (auto& [id, bytes] : out)
            for (Impl::Conn& c : m->conns)
                if (c.id == id && !c.dead) encodeSio(bytes.data(), bytes.size(), c.out);
        for (Impl::Conn& c : m->conns) {
            if (std::find(closed.begin(), closed.end(), c.id) != closed.end()) c.dead = true;
            while (!c.dead && !c.out.empty()) {
                const auto put = ::send(c.sock, reinterpret_cast<const char*>(c.out.data()), int(c.out.size()), sendFlags());
                if (put > 0) {
                    c.out.erase(c.out.begin(), c.out.begin() + put);
                    continue;
                }
                if (put < 0 && wouldBlock()) break;
                c.dead = true;
            }
            // A console that stops reading must not grow this without bound.
            if (c.out.size() > (4u << 20)) c.dead = true;
        }
        for (size_t i = 0; i < m->conns.size();) {
            if (!m->conns[i].dead) {
                ++i;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m->mu);
                m->hub.disconnect(m->conns[i].id, now);
            }
            closeSock(m->conns[i].sock);
            m->conns.erase(m->conns.begin() + std::ptrdiff_t(i));
        }
    }
}

std::vector<Connection> Relay::connections() const {
    std::lock_guard<std::mutex> lock(m->mu);
    std::vector<Connection> v;
    for (const auto& [id, c] : m->hub.connections()) v.push_back(c);
    return v;
}

std::vector<Packet> Relay::packets(uint64_t after) const {
    std::lock_guard<std::mutex> lock(m->mu);
    std::vector<Packet> v;
    for (const Packet& p : m->hub.packets())
        if (p.id > after) v.push_back(p);
    return v;
}

double Relay::seconds() const { return m->now(); }

uint64_t Relay::packetCount() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->hub.packetCount();
}

// ---- the Network view's words ----

bool PacketFilter::shows(const Packet& p) const {
    if (p.type >= 1 && p.type <= 31 && !(types & (1u << p.type))) return false;
    if (slot < 0) return true;
    if (p.from == slot) return true;
    return std::find(p.to.begin(), p.to.end(), uint8_t(slot)) != p.to.end();
}

std::string describeType(const Packet& p) {
    if (!p.handOff()) return packetTypeName(p.type);
    std::string s = "Authority slot " + std::to_string(p.authority);
    if (p.previous != kNoSlot) s += " (was " + std::to_string(p.previous) + ")";
    return s;
}

std::string describeEnd(uint8_t slot, int conn) {
    if (slot == kRelaySlot) return "relay";
    if (slot == kNoSlot) return "conn " + std::to_string(conn);
    return "slot " + std::to_string(slot);
}

std::string describeTo(const std::vector<uint8_t>& to) {
    if (to.empty()) return "nobody";
    if (to.size() == 1 && to[0] == kRelaySlot) return "relay";
    std::string s = to.size() == 1 ? "slot " : "slots ";
    for (size_t i = 0; i < to.size(); ++i) {
        if (i) s += ", ";
        s += to[i] == kRelaySlot ? std::string("relay") : to[i] == kNoSlot ? std::string("?") : std::to_string(to[i]);
    }
    return s;
}

}  // namespace editor::relay
