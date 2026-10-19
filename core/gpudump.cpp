#include "gpudump.hh"

#include <cstring>
#include <fstream>
#include <iterator>

namespace gpudump {

static const char kMagic[16] = {'P', 'S', 'X', 'G', 'P', 'U', 'D', 'U', 'M', 'P', 'v', '1', 'r', '1', 0, 0};

static std::string packetString(const Dump& d, const Packet& p) {
    std::string s(reinterpret_cast<const char*>(d.words.data() + p.offset), size_t(p.length) * 4);
    size_t end = s.find('\0');
    if (end != std::string::npos) s.resize(end);
    return s;
}

bool parse(const std::vector<uint8_t>& bytes, Dump& out, std::string* error) {
    out = Dump();
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic, 16) != 0) {
        if (error) *error = "not a GPU dump (no PSXGPUDUMPv1r1 magic)";
        return false;
    }
    out.words.resize(bytes.size() / 4);
    for (size_t i = 0; i < out.words.size(); ++i)
        out.words[i] = uint32_t(bytes[i * 4]) | uint32_t(bytes[i * 4 + 1]) << 8 | uint32_t(bytes[i * 4 + 2]) << 16 |
                       uint32_t(bytes[i * 4 + 3]) << 24;
    size_t at = 4;
    while (at < out.words.size()) {
        const uint32_t header = out.words[at++];
        Packet p;
        p.type = uint8_t(header >> 24);
        p.length = header & 0xffffff;
        p.offset = uint32_t(at);
        if (p.length > out.words.size() - at) {
            out.truncated = true;
            break;
        }
        at += p.length;
        out.packets.push_back(p);
        switch (p.type) {
            case GPUVersion:
                if (p.length) out.version = out.words[p.offset];
                break;
            case GameID: out.gameId = packetString(out, p); break;
            case VideoFormat: out.videoFormat = packetString(out, p); break;
            case Comment: out.comment = packetString(out, p); break;
            default: break;
        }
    }
    return true;
}

bool load(const std::filesystem::path& file, Dump& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open the file";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse(bytes, out, error);
}

bool save(const std::filesystem::path& file, const Dump& dump, std::string* error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (uint32_t w : dump.words) {
        const char b[4] = {char(w & 0xff), char(w >> 8 & 0xff), char(w >> 16 & 0xff), char(w >> 24 & 0xff)};
        out.write(b, 4);
    }
    if (!out) {
        if (error) *error = "cannot write the file";
        return false;
    }
    return true;
}

int DrawState::displayWidth() const {
    if (displayMode & 0x40) return 368;
    static const int widths[4] = {256, 320, 512, 640};
    return widths[displayMode & 3];
}

int DrawState::displayHeight() const {
    int h = int((vRange >> 10) & 0x3ff) - int(vRange & 0x3ff);
    if (h <= 0) h = 240;
    if ((displayMode & 0x24) == 0x24) h *= 2;  // interlaced 480-line mode
    return h;
}

std::string typeName(const Command& c) {
    std::string s;
    switch (c.kind) {
        case Kind::Polygon:
            s = c.quad() ? "quad" : "tri";
            s += c.gouraud() ? "-gouraud" : "-flat";
            if (c.textured()) s += c.raw() ? "-raw" : "-tex";
            if (c.semiTransparent()) s += "-semi";
            return s;
        case Kind::Rect: {
            static const char* sizes[4] = {"rect", "rect1", "rect8", "rect16"};
            s = sizes[c.op >> 27 & 3];
            if (c.textured()) s += c.raw() ? "-raw" : "-tex";
            if (c.semiTransparent()) s += "-semi";
            return s;
        }
        case Kind::Line:
            s = (c.op >> 27 & 1) ? "polyline" : "line";
            if (c.gouraud()) s += "-gouraud";
            if (c.semiTransparent()) s += "-semi";
            return s;
        case Kind::Fill: return "fill";
        case Kind::CopyVram: return "copy-vram";
        case Kind::CopyIn: return "copy-in";
        case Kind::CopyOut: return "copy-out";
        case Kind::DrawMode: return "draw-mode";
        case Kind::TexWindow: return "tex-window";
        case Kind::AreaTopLeft: return "area-top-left";
        case Kind::AreaBottomRight: return "area-bottom-right";
        case Kind::DrawOffset: return "draw-offset";
        case Kind::MaskBits: return "mask-bits";
        case Kind::ClearCache: return "clear-cache";
        case Kind::Irq: return "irq";
        case Kind::Gp1: return "gp1";
        case Kind::VSync: return "vsync";
        case Kind::Nop: break;
    }
    return "nop";
}

namespace {

int signExtend11(uint32_t v) { return int(int32_t(v << 21) >> 21); }

Vertex vertexAt(uint32_t w) {
    Vertex v;
    v.x = int16_t(signExtend11(w & 0x7ff));
    v.y = int16_t(signExtend11((w >> 16) & 0x7ff));
    return v;
}

void colourOf(Vertex& v, uint32_t w) {
    v.r = uint8_t(w);
    v.g = uint8_t(w >> 8);
    v.b = uint8_t(w >> 16);
}

// Builds one command from its words, GP0 or GP1 in order, keeping the draw and display state.
class Decoder {
public:
    explicit Decoder(CommandList& out) : m_out(out) {}

    void gp0(uint32_t word, uint32_t at) {
        if (m_words.empty()) m_first = at;
        m_words.push_back(word);
        m_last = at;
        if (complete()) emit();
    }

    void gp1(uint32_t word, uint32_t at) {
        const uint32_t cmd = (word >> 24) & 0x3f;
        if (cmd == 0x00) {  // reset: state cleared, command buffer dropped
            m_words.clear();
            m_state = DrawState();
            m_state.hRange = 0x200 | 0xc00u << 12;  // GP1(00h) defaults (psx-spx GP1 list)
            m_state.vRange = 0x010 | 0x100u << 10;
        } else if (cmd == 0x01) {
            m_words.clear();
        } else if (cmd == 0x05) {
            m_state.displayStart = word & 0x7ffff;
        } else if (cmd == 0x06) {
            m_state.hRange = word & 0xffffff;
        } else if (cmd == 0x07) {
            m_state.vRange = word & 0xfffff;
        } else if (cmd == 0x08) {
            m_state.displayMode = word & 0xff;
        }
        Command c;
        c.kind = Kind::Gp1;
        c.op = word;
        c.words = {word};
        c.firstWord = c.lastWord = at;
        push(std::move(c));
    }

    void vsync(uint32_t at) {
        Command c;
        c.kind = Kind::VSync;
        c.firstWord = c.lastWord = at;
        push(std::move(c));
        if (m_live) ++m_frame;
    }

    void setLive() { m_live = true; }
    bool pending() const { return !m_words.empty(); }
    int frames() const { return m_frame; }

private:
    // Whether m_words holds the whole command.
    bool complete() const {
        const uint32_t op = m_words[0] >> 24;
        const size_t n = m_words.size();
        switch (op >> 5) {
            case 0:
                if (op == 0x02) return n >= 3;
                return true;
            case 1: {  // polygon
                const bool quad = op & 0x08, tex = op & 0x04, gouraud = op & 0x10;
                const size_t verts = quad ? 4 : 3;
                const size_t per = 1 + (tex ? 1 : 0) + (gouraud ? 1 : 0);
                return n >= 1 + verts * per - (gouraud ? 1 : 0);
            }
            case 2: {  // line
                const bool poly = op & 0x08, gouraud = op & 0x10;
                if (!poly) return n >= (gouraud ? 4u : 3u);
                // command (colour 0), vertex 0, then [colour,] vertex per further vertex. From the
                // third vertex on, the terminator may take the place of a vertex's first word.
                const size_t last = n - 1;
                const bool vertexStart = !gouraud || last % 2 == 0;
                return last >= size_t(gouraud ? 4 : 3) && vertexStart && (m_words[last] & 0xf000f000) == 0x50005000;
            }
            case 3: {  // rectangle
                const bool tex = op & 0x04;
                const bool variable = ((op >> 3) & 3) == 0;
                return n >= size_t(2 + (tex ? 1 : 0) + (variable ? 1 : 0));
            }
            case 4: return n >= 4;
            case 5: {
                if (n < 3) return false;
                const uint32_t w = ((m_words[2] & 0xffff) - 1) % 0x400 + 1, h = ((m_words[2] >> 16) - 1) % 0x200 + 1;
                return n >= 3 + (w * h + 1) / 2;
            }
            case 6: return n >= 3;
            default: return true;  // E0..FF: one word
        }
    }

    void emit() {
        Command c;
        c.op = m_words[0];
        c.words = std::move(m_words);
        m_words.clear();
        c.firstWord = m_first;
        c.lastWord = m_last;
        const uint32_t op = c.op >> 24;
        const std::vector<uint32_t>& w = c.words;
        switch (op >> 5) {
            case 0:
                if (op == 0x02) {
                    c.kind = Kind::Fill;
                    c.dstX = int(w[1] & 0x3f0);
                    c.dstY = int((w[1] >> 16) & 0x1ff);
                    c.width = int(((w[2] & 0x3ff) + 0xf) & ~0xfu);
                    c.height = int((w[2] >> 16) & 0x1ff);
                    Vertex v;
                    colourOf(v, w[0]);
                    c.vertices.push_back(v);
                } else if (op == 0x01) {
                    c.kind = Kind::ClearCache;
                } else if (op == 0x1f) {
                    c.kind = Kind::Irq;
                }
                break;
            case 1: {
                c.kind = Kind::Polygon;
                const bool quad = op & 0x08, tex = op & 0x04, gouraud = op & 0x10;
                size_t at = 1;
                for (int i = 0; i < (quad ? 4 : 3); ++i) {
                    uint32_t colour = w[0];
                    if (gouraud && i > 0) colour = w[at++];
                    Vertex v = vertexAt(w[at++]);
                    colourOf(v, colour);
                    if (tex) {
                        const uint32_t t = w[at++];
                        v.u = uint8_t(t);
                        v.v = uint8_t(t >> 8);
                        if (i == 0) c.clut = uint16_t(t >> 16);
                        if (i == 1) {
                            // The page attribute sets E1 bits 0-8 and 11, and stays.
                            const uint32_t page = t >> 16;
                            m_state.texpage = (m_state.texpage & ~0x9ffu) | (page & 0x9ff);
                        }
                    }
                    c.vertices.push_back(v);
                }
                break;
            }
            case 2: {
                c.kind = Kind::Line;
                const bool gouraud = op & 0x10;
                size_t at = 1;
                uint32_t colour = w[0];
                for (int i = 0; at < w.size(); ++i) {
                    if (gouraud && i > 0) {
                        if ((w[at] & 0xf000f000) == 0x50005000 && i >= 2) break;
                        colour = w[at++];
                        if (at >= w.size()) break;
                    } else if (i >= 2 && (w[at] & 0xf000f000) == 0x50005000) {
                        break;
                    }
                    Vertex v = vertexAt(w[at++]);
                    colourOf(v, colour);
                    c.vertices.push_back(v);
                }
                break;
            }
            case 3: {
                c.kind = Kind::Rect;
                const bool tex = op & 0x04;
                Vertex v = vertexAt(w[1]);
                colourOf(v, w[0]);
                size_t at = 2;
                if (tex) {
                    v.u = uint8_t(w[at]);
                    v.v = uint8_t(w[at] >> 8);
                    c.clut = uint16_t(w[at] >> 16);
                    ++at;
                }
                switch ((op >> 3) & 3) {
                    case 0:
                        c.width = int(w[at] & 0x3ff);
                        c.height = int((w[at] >> 16) & 0x1ff);
                        break;
                    case 1: c.width = c.height = 1; break;
                    case 2: c.width = c.height = 8; break;
                    case 3: c.width = c.height = 16; break;
                }
                c.vertices.push_back(v);
                break;
            }
            case 4:
                c.kind = Kind::CopyVram;
                c.srcX = int(w[1] & 0x3ff);
                c.srcY = int((w[1] >> 16) & 0x1ff);
                c.dstX = int(w[2] & 0x3ff);
                c.dstY = int((w[2] >> 16) & 0x1ff);
                c.width = int(((w[3] & 0xffff) - 1) % 0x400 + 1);
                c.height = int(((w[3] >> 16) - 1) % 0x200 + 1);
                break;
            case 5:
                c.kind = Kind::CopyIn;
                c.dstX = int(w[1] & 0x3ff);
                c.dstY = int((w[1] >> 16) & 0x1ff);
                c.width = int(((w[2] & 0xffff) - 1) % 0x400 + 1);
                c.height = int(((w[2] >> 16) - 1) % 0x200 + 1);
                break;
            case 6:
                c.kind = Kind::CopyOut;
                c.srcX = int(w[1] & 0x3ff);
                c.srcY = int((w[1] >> 16) & 0x1ff);
                c.width = int(((w[2] & 0xffff) - 1) % 0x400 + 1);
                c.height = int(((w[2] >> 16) - 1) % 0x200 + 1);
                break;
            default:
                switch (op) {
                    case 0xe1:
                        c.kind = Kind::DrawMode;
                        m_state.texpage = w[0] & 0x3fff;
                        break;
                    case 0xe2:
                        c.kind = Kind::TexWindow;
                        m_state.texWindow = w[0] & 0xfffff;
                        break;
                    case 0xe3:
                        c.kind = Kind::AreaTopLeft;
                        m_state.areaX1 = int(w[0] & 0x3ff);
                        m_state.areaY1 = int((w[0] >> 10) & 0x1ff);
                        break;
                    case 0xe4:
                        c.kind = Kind::AreaBottomRight;
                        m_state.areaX2 = int(w[0] & 0x3ff);
                        m_state.areaY2 = int((w[0] >> 10) & 0x1ff);
                        break;
                    case 0xe5:
                        c.kind = Kind::DrawOffset;
                        m_state.offsetX = signExtend11(w[0] & 0x7ff);
                        m_state.offsetY = signExtend11((w[0] >> 11) & 0x7ff);
                        break;
                    case 0xe6:
                        c.kind = Kind::MaskBits;
                        m_state.setMask = w[0] & 1;
                        m_state.checkMask = w[0] & 2;
                        break;
                    default: break;
                }
        }
        push(std::move(c));
    }

    void push(Command&& c) {
        c.state = m_state;
        c.live = m_live;
        c.frame = m_frame;
        if (c.live && c.draws()) m_out.drawn.push_back(int(m_out.commands.size()));
        m_out.commands.push_back(std::move(c));
    }

    CommandList& m_out;
    DrawState m_state;
    std::vector<uint32_t> m_words;
    uint32_t m_first = 0, m_last = 0;
    bool m_live = false;
    int m_frame = 0;
};

}  // namespace

CommandList decode(const Dump& dump) {
    CommandList out;
    Decoder d(out);
    for (const Packet& p : dump.packets) {
        switch (p.type) {
            case GP0:
                for (uint32_t i = 0; i < p.length; ++i) d.gp0(dump.words[p.offset + i], p.offset + i);
                break;
            case GP1:
                for (uint32_t i = 0; i < p.length; ++i) d.gp1(dump.words[p.offset + i], p.offset + i);
                break;
            case VSync: d.vsync(p.offset - 1); break;
            case TraceBegin: d.setLive(); break;
            default: break;
        }
    }
    out.frames = d.frames();
    out.incomplete = d.pending();
    return out;
}

}  // namespace gpudump
