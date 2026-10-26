// What a texture will look like on the console, next to its source, for the
// inspector's side-by-side check. The PS1 image comes from the export path
// itself: splash::loadImage + splash::convertTexture (what the splashpack
// writer calls), decoded back to colours with splash::decodeTexture. No UI.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "scene.hh"
#include "texture.hh"

namespace editor {

struct TexturePreviewKey {
    std::filesystem::path file;  // absolute (resolved) image file
    splash::BitDepth depth = splash::BitDepth::Bpp8;
    bool cutout = false;  // UI images: texels under half alpha are transparent
    // The palette group's shared CLUT (splash::paletteGroup) to convert onto
    // at 4 or 8 bpp; empty = the texture's own palette.
    std::vector<uint16_t> palette = {};
    bool operator==(const TexturePreviewKey& o) const {
        return file == o.file && depth == o.depth && cutout == o.cutout && palette == o.palette;
    }
    bool operator!=(const TexturePreviewKey& o) const { return !(*this == o); }
};

struct TexturePreview {
    TexturePreviewKey key;
    std::string error;  // set when the image could not be loaded or converted; nothing else is valid
    int width = 0, height = 0;
    // RGBA8, rows top-down (row 0 = top of the image, as drawn on screen).
    std::vector<uint8_t> source;
    std::vector<uint8_t> ps1;
    int sourceColors = 0;  // distinct 15-bit colours among the drawn source texels
    int ps1Colors = 0;     // distinct colours in the PS1 image (drawn texels)
    int paletteSize = 0;   // CLUT entries the export writes (with padding; the shared CLUT's in a group); 0 at 16 bpp
    int paletteUsed = 0;   // CLUT entries the texels reference; 0 at 16 bpp
    double deltaE = 0;     // mean OKLab distance x100 per drawn texel (splash::measureTexture)
    double deltaEBlur = 0;
    double psnr = 0;
    int vramBytes = 0;  // image + CLUT
    double convertMs = 0;  // load + convert + decode + measure, wall time
};

// Loads `key.file` and converts it the way export does. Never throws: a
// failure comes back in `error`.
TexturePreview makeTexturePreview(const TexturePreviewKey& key);
// Same from an image already in memory (`key.file` is only recorded).
TexturePreview makeTexturePreview(const splash::Image& img, const TexturePreviewKey& key);

// Writes the PS1 image (or, with `source`, the source) as a PNG. False on failure.
bool saveTexturePreviewPng(const TexturePreview& p, const std::filesystem::path& file, bool source = false);

// Converts on a worker thread, newest request wins, so the UI never waits on
// a k-means: 8 bpp on a 256 x 256 photo takes far longer than a frame.
class TexturePreviewer {
public:
    TexturePreviewer();
    ~TexturePreviewer();
    TexturePreviewer(const TexturePreviewer&) = delete;
    TexturePreviewer& operator=(const TexturePreviewer&) = delete;

    // Asks for `key`. Cheap when it is already shown or being worked on.
    void request(const TexturePreviewKey& key);
    // Drops the cached result for `file` (it changed on disk) and recomputes
    // the current key if it is that file.
    void invalidate(const std::filesystem::path& file);
    // The newest finished result (possibly for an older key), or null.
    std::shared_ptr<const TexturePreview> latest() const;
    // True while the requested key has no finished result yet.
    bool busy() const;
    // Bumped every time a result lands, so the UI knows to re-upload.
    unsigned generation() const;
    // Blocks until the requested key is done (tests, screenshot runs).
    void wait() const;

private:
    void run();
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::optional<TexturePreviewKey> m_wanted;  // last request
    bool m_pending = false;                     // m_wanted not yet started
    bool m_working = false;
    bool m_quit = false;
    unsigned m_generation = 0;
    std::shared_ptr<const TexturePreview> m_latest;
    std::deque<std::shared_ptr<const TexturePreview>> m_cache;  // most recent first
    std::thread m_thread;
};

}  // namespace editor
