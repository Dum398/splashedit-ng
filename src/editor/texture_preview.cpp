#include "editor/texture_preview.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <unordered_set>

namespace editor {

namespace {

uint8_t byte(float c) { return uint8_t(std::lround(std::clamp(c, 0.f, 1.f) * 255.f)); }

// Image (bottom-up floats) to RGBA8 top-down.
std::vector<uint8_t> rgbaTopDown(const splash::Image& img, bool keepAlpha) {
    std::vector<uint8_t> out(size_t(img.width) * img.height * 4);
    for (int row = 0; row < img.height; row++) {
        const splash::Image::Px* src = &img.pixels[size_t(img.height - 1 - row) * img.width];
        uint8_t* dst = &out[size_t(row) * img.width * 4];
        for (int x = 0; x < img.width; x++) {
            dst[x * 4 + 0] = byte(src[x].r);
            dst[x * 4 + 1] = byte(src[x].g);
            dst[x * 4 + 2] = byte(src[x].b);
            dst[x * 4 + 3] = keepAlpha ? byte(src[x].a) : 255;
        }
    }
    return out;
}

// The exporter's cutout threshold (texture.cpp kCutoutAlpha).
bool drawn(const splash::Image::Px& p, bool cutout) { return !(cutout && p.a < 0.5f); }

}  // namespace

TexturePreview makeTexturePreview(const splash::Image& img, const TexturePreviewKey& key) {
    TexturePreview p;
    p.key = key;
    p.width = img.width;
    p.height = img.height;
    const int per = 16 / int(key.depth);
    if (img.width <= 0 || img.height <= 0) {
        p.error = "empty image";
        return p;
    }
    if (img.width % per) {
        // convertTexture packs width / per words a row and drops the rest.
        p.error = "width " + std::to_string(img.width) + " is not a multiple of " + std::to_string(per) + " at " +
                  std::to_string(int(key.depth)) + " bpp";
        return p;
    }
    try {
        // The export path: the same call the splashpack writer makes.
        const bool shared = !key.palette.empty() && key.depth != splash::BitDepth::Bpp16;
        splash::PsxTexture t = shared ? splash::convertTexture(img, key.depth, key.cutout, key.palette)
                                      : splash::convertTexture(img, key.depth, key.cutout);
        splash::Image out = splash::decodeTexture(t);
        splash::TextureError e = splash::measureTexture(img, t);
        p.deltaE = e.deltaE;
        p.deltaEBlur = e.deltaEBlur;
        p.psnr = e.psnr;
        p.paletteUsed = e.colorsUsed;
        p.paletteSize = t.hasPalette ? int(t.palette.size()) : 0;
        p.vramBytes = int((t.imageData.size() + t.palette.size()) * sizeof(uint16_t));
        p.source = rgbaTopDown(img, key.cutout);
        p.ps1 = rgbaTopDown(out, true);

        std::unordered_set<uint32_t> src, dst;
        for (size_t i = 0; i < img.pixels.size(); i++) {
            const splash::Image::Px& s = img.pixels[i];
            if (!drawn(s, key.cutout)) continue;
            auto c5 = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * 31.f)); };
            src.insert(c5(s.r) | c5(s.g) << 5 | c5(s.b) << 10);
            const splash::Image::Px& o = out.pixels[i];
            dst.insert(uint32_t(byte(o.r)) | uint32_t(byte(o.g)) << 8 | uint32_t(byte(o.b)) << 16);
        }
        p.sourceColors = int(src.size());
        p.ps1Colors = int(dst.size());
    } catch (const std::exception& ex) {
        p.error = ex.what();
    }
    return p;
}

TexturePreview makeTexturePreview(const TexturePreviewKey& key) {
    auto t0 = std::chrono::steady_clock::now();
    TexturePreview p;
    try {
        p = makeTexturePreview(splash::loadImage(key.file), key);
    } catch (const std::exception& ex) {
        p = TexturePreview();
        p.key = key;
        p.error = ex.what();
    }
    p.convertMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return p;
}

bool saveTexturePreviewPng(const TexturePreview& p, const std::filesystem::path& file, bool source) {
    const std::vector<uint8_t>& px = source ? p.source : p.ps1;
    if (!p.error.empty() || px.size() != size_t(p.width) * p.height * 4) return false;
    splash::Image img;
    img.width = p.width;
    img.height = p.height;
    img.pixels.resize(size_t(p.width) * p.height);
    for (int row = 0; row < p.height; row++)
        for (int x = 0; x < p.width; x++) {
            const uint8_t* s = &px[(size_t(row) * p.width + x) * 4];
            img.pixels[size_t(p.height - 1 - row) * p.width + x] = {s[0] / 255.f, s[1] / 255.f, s[2] / 255.f, s[3] / 255.f};
        }
    try {
        splash::savePng(img, file);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// ---- TexturePreviewer

namespace {
constexpr size_t kCacheSize = 8;  // flicking between bit depths stays instant
}

TexturePreviewer::TexturePreviewer() : m_thread([this] { run(); }) {}

TexturePreviewer::~TexturePreviewer() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void TexturePreviewer::request(const TexturePreviewKey& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_wanted && *m_wanted == key) return;
    m_wanted = key;
    // A cached result answers at once.
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
        if ((*it)->key == key) {
            auto hit = *it;
            m_cache.erase(it);
            m_cache.push_front(hit);
            m_latest = hit;
            m_generation++;
            m_pending = false;
            m_cv.notify_all();
            return;
        }
    m_pending = true;
    m_cv.notify_all();
}

void TexturePreviewer::invalidate(const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(std::remove_if(m_cache.begin(), m_cache.end(), [&](const auto& p) { return p->key.file == file; }),
                  m_cache.end());
    if (m_wanted && m_wanted->file == file) {
        m_pending = true;
        m_cv.notify_all();
    }
}

std::shared_ptr<const TexturePreview> TexturePreviewer::latest() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latest;
}

bool TexturePreviewer::busy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending || m_working;
}

unsigned TexturePreviewer::generation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

void TexturePreviewer::wait() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [&] { return m_quit || (!m_pending && !m_working); });
}

void TexturePreviewer::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [&] { return m_quit || m_pending; });
        if (m_quit) return;
        TexturePreviewKey key = *m_wanted;
        m_pending = false;
        m_working = true;
        lock.unlock();
        auto result = std::make_shared<const TexturePreview>(makeTexturePreview(key));
        lock.lock();
        m_working = false;
        m_cache.push_front(result);
        if (m_cache.size() > kCacheSize) m_cache.pop_back();
        // If a newer request came in meanwhile this one still shows until
        // that finishes: an older result beats an empty pane.
        m_latest = result;
        m_generation++;
        m_cv.notify_all();
    }
}

}  // namespace editor
