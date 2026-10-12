// The console's memory budgets, for the editor's meters, from what an export
// actually produces (ExportStats).
#pragma once

#include <algorithm>
#include <cstddef>

#include "splashpack.hh"

namespace splash {

struct Budget {
    size_t used = 0, capacity = 1;
    float fraction() const { return float(double(used) / double(capacity)); }
};

constexpr size_t kVramCapacity = 1024 * 512 * 2;
constexpr size_t kSpuRamCapacity = 512 * 1024;
// Heap of psxsplash 8283c6b (__stack_start - __heap_start in psxsplash.elf,
// default build). The stack grows down into the same space.
constexpr size_t kEngineHeapBytes = 0x80200000 - 0x8009f600;

inline Budget vramBudget(const ExportStats& s) { return {s.vramBytes(), kVramCapacity}; }
inline Budget spuBudget(const ExportStats& s) { return {s.spuEnd, kSpuRamCapacity}; }
// Renderer::Configure allocates two ordering tables and two bump allocators
// on the heap, sized from the splashpack (ExportStats::rendererBytes), and the
// frustum culling output, one entry per BVH triangle reference. LoadScene
// reads the .vram and .spu files into the heap one at a time and frees each
// before the .splashpack, which stays resident, so the peak is the largest of
// the three. A streamed scene adds its slots (WorldStreamer::init). Lua's own
// allocations are not counted.
inline Budget ramBudget(const ExportStats& s) {
    return {s.rendererBytes() + s.visibleListBytes() + s.streamPoolBytes() +
                std::max({s.splashpackBytes, s.vramFileBytes, s.spuFileBytes}),
            kEngineHeapBytes};
}

}  // namespace splash
