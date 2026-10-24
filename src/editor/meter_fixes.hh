// Fixes for a budget meter that went amber or red, found in the scene itself:
// textures that can drop to a lower bit depth (VRAM, and RAM through the
// .vram file), render buffers sized above what the scene needs (RAM), and
// the meshes that put the most triangles in view (Triangles). Every saving
// shown is measured, not guessed: the scene is exported dry with and without
// the fix, so it is the meter's change once the fix is applied. The one-click
// fixes go through the document's undo history. No UI.
#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "editor/document.hh"
#include "scene.hh"
#include "splashpack.hh"

namespace editor {

enum class FixMeter { Vram = 0, Ram = 1, Tris = 2 };
constexpr int kFixMeters = 3;

// What a meter reads (Budget::used) for an export's stats.
size_t meterUsed(FixMeter m, const splash::ExportStats& s);
const char* meterName(FixMeter m);  // "vram", "ram", "tris"

struct MeterFix {
    enum class Kind {
        TextureDepth,        // set the meshes in `objects` to bit depth `to`
        ResetOrderingTable,  // clear the scene's ordering table override (the export sizes it)
        ResetBumpAllocator,  // clear the scene's bump allocator override
        SimplifyMesh,        // listed only: simplify `mesh` (its import settings' Triangles)
    };
    Kind kind = Kind::TextureDepth;
    std::vector<ObjectPath> objects;    // the objects the fix edits (SimplifyMesh: the mesh's instances)
    std::vector<std::string> textures;  // TextureDepth: the textures those meshes share, first is the largest
    splash::BitDepth from = splash::BitDepth::Bpp8, to = splash::BitDepth::Bpp4;
    int colors = 0;  // TextureDepth: most distinct 15-bit colours in one of `textures`
    std::string mesh;                   // SimplifyMesh: project-relative .mesh
    int triangles = 0, simplified = 0;  // SimplifyMesh: per instance, now and simplified to half
    std::string title, detail;
    // Meter readings before and after, indexed by FixMeter. TextureDepth and
    // the resets: from dry-run exports of the scene without and with the fix.
    // SimplifyMesh: before from the export, after = before minus the
    // instances' triangles that simplifying to half removes (not exported:
    // that fix rewrites the model's files, outside the undo history).
    std::array<size_t, kFixMeters> before{}, after{};
    long long saving(FixMeter m) const { return (long long)before[int(m)] - (long long)after[int(m)]; }
    bool oneClick() const { return kind != Kind::SimplifyMesh; }
};

// Applies `fix` to a scene (the dry runs do this to a copy).
void applyFix(splash::Scene& scene, const MeterFix& fix);
// Applies `fix` to the document as one undo step. False when nothing changed.
bool applyFix(Document& doc, const MeterFix& fix);

// The fixes for `scene` (as the document holds it, before resolving
// prefabs), each measured: those that lower no meter are left out. Ordered
// by kind of meter they serve, biggest saving first. At most `maxTextureTries`
// texture-depth candidates are exported (the biggest first). `stop` is
// polled between exports; `progress(done, total)` is called after each.
struct MeterFixResult {
    bool ok = false;               // the scene as it is exported
    splash::ExportStats base;      // its stats
    std::vector<MeterFix> fixes;
    std::vector<std::string> errors;
};
MeterFixResult findMeterFixes(const splash::Scene& scene, const std::filesystem::path& projectRoot,
                              const splash::VramSettings& vram, const std::function<bool()>& stop = {},
                              const std::function<void(int, int)>& progress = {}, int maxTextureTries = 8);

// The fixes that serve meter `m`: those lowering it, biggest saving first.
std::vector<const MeterFix*> fixesFor(const MeterFixResult& r, FixMeter m);

// Runs findMeterFixes on a worker thread for a document revision; a new
// revision restarts it.
class MeterFixFinder {
public:
    MeterFixFinder() = default;
    ~MeterFixFinder();
    MeterFixFinder(const MeterFixFinder&) = delete;
    MeterFixFinder& operator=(const MeterFixFinder&) = delete;
    // Starts a search for the document as it is, unless one for this
    // revision has run or is running.
    void request(const Document& doc);
    struct State {
        unsigned revision = 0;
        bool started = false, done = false;
        int measured = 0, total = 0;
        MeterFixResult result;
    };
    State state() const;
    void wait();

private:
    void join();
    mutable std::mutex m_mu;
    State m_state;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

}  // namespace editor
