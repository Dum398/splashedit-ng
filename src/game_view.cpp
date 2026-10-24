#include "game_view.h"

#include <atomic>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "gl.h"

namespace {

// Mirrors ShmDisplayHeader in pcsx-redux's src/core/shmdisplay.h.
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t vramOffset;
    uint32_t sequence;
    uint32_t frame;
    int32_t displayX, displayY, displayWidth, displayHeight;
    uint32_t displayDepth24;
    uint32_t displayEnabled;
    uint32_t hostInput;
    uint32_t hostPads[2];
};

constexpr uint32_t kMagic = 0x44535850;  // "PXSD"
constexpr size_t kVramSize = 1024 * 512 * 2;

// Apple's libc++ has no std::atomic_ref yet.
#ifdef _MSC_VER
uint32_t load(uint32_t& v, std::memory_order) {
    uint32_t x = *(volatile uint32_t*)&v;
    std::atomic_thread_fence(std::memory_order_acquire);
    return x;
}
void store(uint32_t& v, uint32_t x, std::memory_order) {
    std::atomic_thread_fence(std::memory_order_release);
    *(volatile uint32_t*)&v = x;
}
#else
uint32_t load(uint32_t& v, std::memory_order) { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
void store(uint32_t& v, uint32_t x, std::memory_order) { __atomic_store_n(&v, x, __ATOMIC_RELEASE); }
#endif

}  // namespace

GameView::~GameView() {
    detach();
    if (m_tex) glDeleteTextures(1, &m_tex);
}

bool GameView::attach(int64_t pid) {
    if (m_mem) return true;
    std::string name = "pcsx-redux-display-" + std::to_string(pid);
#ifdef _WIN32
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!h) return false;
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) {
        CloseHandle(h);
        return false;
    }
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(p, &info, sizeof info);
    m_handle = h;
    m_size = info.RegionSize;
#else
    // Same name redux passes to shm_open, no leading slash, so both sides agree on macOS.
    int fd = shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    void* p = mmap(nullptr, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    m_size = (size_t)st.st_size;
#endif
    m_mem = (uint8_t*)p;
    Header* hd = (Header*)m_mem;
    // redux sizes the block before it writes the header, so a block caught
    // in between reads as not there yet.
    if (m_size < sizeof(Header) || load(hd->magic, std::memory_order_acquire) != kMagic ||
        m_size < hd->vramOffset + kVramSize) {
        detach();
        return false;
    }
    m_frames = 0;
    m_lastFrame = 0;
    return true;
}

void GameView::detach() {
    if (!m_mem) return;
#ifdef _WIN32
    UnmapViewOfFile(m_mem);
    CloseHandle((HANDLE)m_handle);
    m_handle = nullptr;
#else
    munmap(m_mem, m_size);
#endif
    m_mem = nullptr;
    m_size = 0;
    m_frames = 0;
}

void GameView::update() {
    if (!m_mem) return;
    Header* hd = (Header*)m_mem;
    Header h;
    m_vram.resize(kVramSize);
    // The sequence is odd while redux writes; retry until a copy lands between writes.
    bool got = false;
    for (int tries = 0; tries < 8 && !got; ++tries) {
        uint32_t s1 = load(hd->sequence, std::memory_order_acquire);
        if (s1 & 1) continue;
        std::memcpy(&h, hd, sizeof h);
        std::memcpy(m_vram.data(), m_mem + h.vramOffset, kVramSize);
        std::atomic_thread_fence(std::memory_order_acquire);
        got = load(hd->sequence, std::memory_order_relaxed) == s1;
    }
    const bool fresh = got && h.frame != m_lastFrame;
    // A new look redraws the last frame, as a paused game sends no new one.
    if (!fresh && !(m_relook && m_frames && m_fw > 0)) return;
    if (fresh && !buildFrame(&h)) return;
    m_relook = false;

    // m_rgba stays the frame as redux shows it; the look goes on a copy.
    int th = m_fh;
    std::vector<uint8_t> looked;
    if (m_look != editor::tv::Look::RGB) looked = editor::tv::applyLook(m_rgba.data(), m_fw, m_fh, m_look, m_scanlines, &th);
    const uint8_t* pixels = looked.empty() ? m_rgba.data() : looked.data();
    const int w = m_fw;
    if (!m_tex) glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (w != m_w || th != m_h) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        m_w = w;
        m_h = th;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, th, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (fresh) ++m_frames;
}

// The display area of the copied VRAM into m_rgba. False for a size redux
// would not show.
bool GameView::buildFrame(const void* header) {
    const Header& h = *(const Header*)header;
    m_lastFrame = h.frame;
    int w = h.displayWidth, ht = h.displayHeight;
    if (w <= 0 || ht <= 0 || w > 1024 || ht > 512) return false;
    m_fw = w;
    m_fh = ht;
    m_rgba.assign((size_t)w * ht * 4, 0);
    if (h.displayEnabled) {
        for (int y = 0; y < ht; ++y) {
            const uint8_t* row = m_vram.data() + ((h.displayY + y) & 511) * 2048;
            uint8_t* out = m_rgba.data() + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x, out += 4) {
                if (h.displayDepth24) {
                    for (int c = 0; c < 3; ++c) out[c] = row[(h.displayX * 2 + x * 3 + c) & 2047];
                } else {
                    int o = ((h.displayX + x) & 1023) * 2;
                    uint16_t v = row[o] | row[o + 1] << 8;
                    out[0] = (uint8_t)((v & 31) << 3 | (v & 31) >> 2);
                    out[1] = (uint8_t)((v >> 5 & 31) << 3 | (v >> 5 & 31) >> 2);
                    out[2] = (uint8_t)((v >> 10 & 31) << 3 | (v >> 10 & 31) >> 2);
                }
                out[3] = 255;
            }
        }
    }
    return true;
}

void GameView::setPads(uint16_t pad0, uint16_t pad1) {
    if (!m_mem) return;
    Header* hd = (Header*)m_mem;
    store(hd->hostPads[0], pad0, std::memory_order_relaxed);
    store(hd->hostPads[1], pad1, std::memory_order_relaxed);
    store(hd->hostInput, 1, std::memory_order_release);
}
