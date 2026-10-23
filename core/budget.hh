// The console's memory budgets, for the editor's meters, from what an export
// actually produces (ExportStats).
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

#include "splashpack.hh"

namespace splash {

struct Budget {
    size_t used = 0, capacity = 1;
    float fraction() const { return float(double(used) / double(capacity)); }
};

constexpr size_t kVramCapacity = 1024 * 512 * 2;
constexpr size_t kSpuRamCapacity = 512 * 1024;
// Heap of the disc engine SplashEdit bundles, psxsplash d2c232c built with
// LOADER=cdrom NOPARSER=1 and every feature: __stack_start - __heap_start in
// psxsplash.elf (mipsel-none-elf-nm). The disc engine has the least heap of
// those a game ships with; leaving the Lua parser out gave it 0x7000 bytes
// (__heap_start 0x800a5c70 with the parser). The stack grows down into the
// same space.
constexpr size_t kEngineHeapBytes = 0x80200000 - 0x8009ec70;

// psxsplash-docs components/objects.md: "roughly 1000-2000 visible triangles
// per frame at 30fps". The top of that range is the meter's capacity.
constexpr size_t kVisibleTriangleGuide = 2000;

// The meters' colour bands, shared by the meter widget and the scripted
// status bar lines: under 75% good, under 90% warn, else bad.
enum class BudgetBand { Good, Warn, Bad };
inline BudgetBand budgetBandOf(float fraction) {
    return fraction < 0.75f ? BudgetBand::Good : fraction < 0.9f ? BudgetBand::Warn : BudgetBand::Bad;
}
inline const char* budgetBand(float fraction) {
    switch (budgetBandOf(fraction)) {
        case BudgetBand::Good: return "good";
        case BudgetBand::Warn: return "warn";
        default: return "bad";
    }
}

inline Budget vramBudget(const ExportStats& s) { return {s.vramBytes(), kVramCapacity}; }
inline Budget spuBudget(const ExportStats& s) { return {s.spuEnd, kSpuRamCapacity}; }
inline Budget triangleBudget(const ExportStats& s) { return {size_t(s.trianglesInView), kVisibleTriangleGuide}; }
// What psxsplash allocates for each game object when a scene loads, none of
// it in the splashpack. Measured on psxsplash 669fb6b in pcsx-redux, printing
// the heap break (psyqo_heap_end) and lua_gc(LUA_GCCOUNT) through
// SceneManager::InitializeScene, on streamed scenes of 276, 398, 545 and 1257
// game objects:
// - initializeAgentStates sizes m_agentStates to every object plus the
//   player, agent or not: the heap grew 224.4-224.9 bytes per object there.
// - Lua::RegisterGameObject makes two tables per object (96 bytes each
//   between registry resizes, 276 vs 398 objects) and two registry entries
//   keyed on the object. The registry's hash part doubles as it fills, about
//   23 KB per doubling at 1024 -> 2048 entries; while it resizes the old part
//   is still live, hence the 3/2.
// - Everything else that load allocates (the Lua state with the engine's
//   bindings and the scripts, the parsed object lists) left 50-54 KB unexplained
//   on those scenes; kEngineLoadBytes is that with margin.
// The 1257-object scene ran Lua out of memory where the old meter read 1.03 MB.
constexpr size_t kAgentStateBytes = 224;
constexpr size_t kLuaObjectBytes = 96;
constexpr size_t kLuaRegistryNodeBytes = 24;
constexpr size_t kEngineLoadBytes = 64 * 1024;

inline size_t luaRegistryPeakBytes(size_t objects) {
    size_t nodes = 1;
    while (nodes < 2 * objects) nodes *= 2;
    return nodes * kLuaRegistryNodeBytes * 3 / 2;
}

// The per-object part, which merging objects shrinks.
inline size_t objectLoadBytes(const ExportStats& s) {
    const size_t n = size_t(std::max(s.gameObjects, 0));
    return (n + 1) * kAgentStateBytes + n * kLuaObjectBytes + luaRegistryPeakBytes(n);
}

// Renderer::Configure allocates two ordering tables and two bump allocators
// on the heap, sized from the splashpack (ExportStats::rendererBytes), and the
// frustum culling output, one entry per BVH triangle reference. LoadScene
// reads the .vram and .spu files into the heap one at a time and frees each
// before the .splashpack, which stays resident, so the peak is the largest of
// the three. A streamed scene adds its slots (WorldStreamer::init). Then come
// the per-object allocations above and the rest of what loading makes.
// The localised strings the scene carries stay on the Lua heap
// (stringsLuaHeapBytes, measured in Redux).
inline Budget ramBudget(const ExportStats& s) {
    return {s.rendererBytes() + s.visibleListBytes() + s.streamPoolBytes() +
                std::max({s.splashpackBytes, s.vramFileBytes, s.spuFileBytes}) + objectLoadBytes(s) + kEngineLoadBytes +
                s.stringsLuaBytes,
            kEngineHeapBytes};
}

// True when the scene does not fit and would without its per-object cost:
// the object count, not the geometry, is what to cut.
inline bool objectsBindRam(const ExportStats& s) {
    const Budget b = ramBudget(s);
    return b.used > b.capacity && b.used - objectLoadBytes(s) <= b.capacity;
}

// The problem and its fix, for the inspector and the export's warnings.
inline std::string objectRamWarning(const ExportStats& s) {
    const Budget b = ramBudget(s);
    std::string cut = s.streamSplitObjects ? " (" + std::to_string(s.streamSplitObjects) + " of them pieces streaming cut from big meshes)" : "";
    return std::to_string(s.gameObjects) + " objects" + cut + " take " + std::to_string(objectLoadBytes(s) / 1024) +
           " KB of RAM when the scene loads, about 0.4 KB each however small, and the scene needs " +
           std::to_string(b.used / 1024) + " of " + std::to_string(b.capacity / 1024) +
           " KB. Merge props that never move and have no script into one mesh per area.";
}

}  // namespace splash
