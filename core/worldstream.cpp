#include "worldstream.hh"

#include <algorithm>
#include <climits>

namespace splash::stream {

size_t Plan::pieceCount() const {
    size_t n = 0;
    for (const Region& r : regions) n += r.pieces.size();
    return n;
}

int64_t distSq(const Box& b, int64_t x, int64_t z) {
    int64_t dx = x < b.minX ? b.minX - x : (x > b.maxX ? x - b.maxX : 0);
    int64_t dz = z < b.minZ ? b.minZ - z : (z > b.maxZ ? z - b.maxZ : 0);
    return dx * dx + dz * dz;
}

namespace {

// Regions on a dense grid of `cell`-sized squares from the world's min
// corner, row by row, so lookups are array reads and the order is fixed.
struct Grid {
    int64_t cell = 0;
    int64_t cols = 0, rows = 0;
    std::vector<int> cellRegion;  // cols * rows, -1 = empty
    std::vector<Region> regions;  // row order
    uint32_t maxBytes = 0;
    int slots = 0;
    uint64_t drawTris = 0;  // most triangles within the draw distance of one camera position
};

void grow(Box& b, const Box& t) {
    b.minX = std::min(b.minX, t.minX);
    b.minZ = std::min(b.minZ, t.minZ);
    b.maxX = std::max(b.maxX, t.maxX);
    b.maxZ = std::max(b.maxZ, t.maxZ);
}

Grid partition(const std::vector<Candidate>& in, const Box& all, int64_t cell) {
    Grid g;
    g.cell = cell;
    g.cols = (int64_t(all.maxX) - all.minX) / cell + 1;
    g.rows = (int64_t(all.maxZ) - all.minZ) / cell + 1;
    const size_t cells = size_t(g.cols * g.rows);
    std::vector<Region> byCell(cells);
    std::vector<int> stampCand(cells, -1), stampPiece(cells, 0);
    std::vector<char> used(cells, 0);
    auto cellOf = [&](int64_t x, int64_t z) {
        int64_t cx = std::clamp<int64_t>((x - all.minX) / cell, 0, g.cols - 1);
        int64_t cz = std::clamp<int64_t>((z - all.minZ) / cell, 0, g.rows - 1);
        return size_t(cz * g.cols + cx);
    };
    for (size_t ci = 0; ci < in.size(); ci++) {
        const Candidate& c = in[ci];
        if (c.tris.empty()) continue;
        const Box& b = c.bounds;
        const bool whole = int64_t(b.maxX) - b.minX <= cell && int64_t(b.maxZ) - b.minZ <= cell;
        auto add = [&](size_t k, int t) {
            Region& r = byCell[k];
            if (!used[k]) {
                used[k] = 1;
                r.bounds = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
            }
            if (stampCand[k] != int(ci)) {
                stampCand[k] = int(ci);
                stampPiece[k] = int(r.pieces.size());
                r.pieces.push_back(Piece{int(ci), {}});
            }
            r.pieces[size_t(stampPiece[k])].tris.push_back(t);
            grow(r.bounds, c.tris[size_t(t)]);
            r.triBytes += kBytesPerTri;
        };
        if (whole) {
            const size_t k = cellOf((int64_t(b.minX) + b.maxX) / 2, (int64_t(b.minZ) + b.maxZ) / 2);
            for (size_t t = 0; t < c.tris.size(); t++) add(k, int(t));
        } else {
            for (size_t t = 0; t < c.tris.size(); t++) {
                const Box& tb = c.tris[t];
                add(cellOf((int64_t(tb.minX) + tb.maxX) / 2, (int64_t(tb.minZ) + tb.maxZ) / 2), int(t));
            }
        }
    }
    g.cellRegion.assign(cells, -1);
    for (size_t k = 0; k < cells; k++) {
        if (!used[k]) continue;
        Region& r = byCell[k];
        r.byteSize = (r.triBytes + kSector - 1) / kSector * kSector;
        g.maxBytes = std::max(g.maxBytes, r.byteSize);
        g.cellRegion[k] = int(g.regions.size());
        g.regions.push_back(std::move(r));
    }
    return g;
}

// Slots: the most regions within the unload radius of any sampled camera
// position, plus one for the read in flight.
void size(Grid& g, int64_t unload, int64_t draw, const Box& all) {
    const int64_t cell = g.cell;
    const int64_t spanMax = std::max(int64_t(all.maxX) - all.minX, int64_t(all.maxZ) - all.minZ);
    const int64_t step = std::max({int64_t(1), cell / 2, spanMax / 48});
    const int64_t reach = unload / cell + 2;  // a region's bounds stay near its own cell
    const int64_t unloadSq = unload * unload, drawSq = draw * draw;
    const bool scanAll = (2 * reach + 1) * (2 * reach + 1) > int64_t(g.regions.size());
    int worst = 0;
    for (int64_t z = all.minZ; z <= int64_t(all.maxZ) + step - 1; z += step)
        for (int64_t x = all.minX; x <= int64_t(all.maxX) + step - 1; x += step) {
            int count = 0;
            uint64_t tris = 0;
            auto visit = [&](const Region& r) {
                const int64_t d = distSq(r.bounds, x, z);
                if (d <= unloadSq) count++;
                if (d <= drawSq) tris += r.triBytes / kBytesPerTri;
            };
            if (scanAll) {
                for (const Region& r : g.regions) visit(r);
            } else {
                const int64_t cx = (std::min(x, int64_t(all.maxX)) - all.minX) / cell;
                const int64_t cz = (std::min(z, int64_t(all.maxZ)) - all.minZ) / cell;
                for (int64_t zz = std::max<int64_t>(0, cz - reach); zz <= std::min(g.rows - 1, cz + reach); zz++)
                    for (int64_t xx = std::max<int64_t>(0, cx - reach); xx <= std::min(g.cols - 1, cx + reach); xx++) {
                        const int ri = g.cellRegion[size_t(zz * g.cols + xx)];
                        if (ri >= 0) visit(g.regions[size_t(ri)]);
                    }
            }
            worst = std::max(worst, count);
            g.drawTris = std::max(g.drawTris, tris);
        }
    g.slots = std::max(1, worst) + 1;
}

}  // namespace

Plan plan(const std::vector<Candidate>& in, int64_t drawDistance, int64_t cellOverride) {
    Plan p;
    Box all{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    bool any = false;
    for (const Candidate& c : in) {
        if (c.tris.empty()) continue;
        grow(all, c.bounds);
        for (const Box& t : c.tris) grow(all, t);
        p.streamedTriBytes += uint64_t(c.tris.size()) * kBytesPerTri;
        any = true;
    }
    if (!any) return p;

    // A quarter past the draw distance covers the camera moving while the
    // disc read runs.
    const int64_t draw = std::clamp<int64_t>(drawDistance, 1, INT32_MAX / 4);
    const int64_t load = draw * 5 / 4;
    const int64_t unload = load * 3 / 2;
    p.loadRadius = int32_t(load);
    p.unloadRadius = int32_t(unload);
    const int64_t span = std::max<int64_t>(1, std::max(int64_t(all.maxX) - all.minX, int64_t(all.maxZ) - all.minZ));

    Grid best;
    if (cellOverride > 0) {
        best = partition(in, all, std::max<int64_t>(cellOverride, span / 1024 + 1));
        size(best, unload, draw, all);
    } else {
        // The pool is slotCount x the largest region, so take the grid with the
        // least of that among those whose regions stay small and whose slots
        // fit the engine's cap.
        int bestRank = INT_MAX;
        uint64_t bestCost = UINT64_MAX;
        int64_t last = -1;
        for (int k = 1; k <= 96; k++) {
            const int64_t c = std::max<int64_t>(1, (span + k - 1) / k);
            if (c == last) continue;
            last = c;
            Grid g = partition(in, all, c);
            size(g, unload, draw, all);
            const int rank = (g.slots <= kMaxSlots ? 0 : 2) + (g.maxBytes <= kTargetRegionBytes ? 0 : 1);
            const uint64_t cost = uint64_t(std::min(g.slots, kMaxSlots)) * g.maxBytes;
            const size_t regionCount = g.regions.size();
            if (rank < bestRank || (rank == bestRank && cost < bestCost)) {
                bestRank = rank;
                bestCost = cost;
                best = std::move(g);
            }
            // Smaller cells only put more regions in reach: once over the cap,
            // with a grid under it already found, nothing later can win.
            if (bestRank < 2 && rank >= 2) break;
            if (regionCount > 4096) break;
        }
    }

    p.cell = best.cell;
    uint32_t sector = 0;
    for (Region& r : best.regions) {
        r.firstSector = sector;
        sector += r.byteSize / kSector;
        p.regions.push_back(std::move(r));
    }
    p.slotBytes = best.maxBytes;
    p.slotsWanted = best.slots;
    p.drawTris = best.drawTris;
    p.slotCount = std::min(best.slots, kMaxSlots);
    return p;
}

}  // namespace splash::stream
