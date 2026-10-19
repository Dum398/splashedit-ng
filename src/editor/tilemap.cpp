#include "editor/tilemap.hh"

#include <algorithm>
#include <cstdlib>

#include "editor/document.hh"

namespace editor {

std::array<float, 3> tileBrushTint(int i) {
    static const std::array<float, 3> tints[] = {{0.36f, 0.80f, 0.45f}, {0.90f, 0.42f, 0.38f}, {0.36f, 0.62f, 0.95f},
                                                 {0.95f, 0.76f, 0.30f}, {0.72f, 0.48f, 0.92f}, {0.30f, 0.82f, 0.82f},
                                                 {0.95f, 0.55f, 0.80f}, {0.70f, 0.70f, 0.70f}};
    const int n = int(sizeof tints / sizeof tints[0]);
    return tints[((i % n) + n) % n];
}

bool addTilemap(Document& doc, const std::string& tileset, int cells) {
    if (doc.scene().tilemap) return false;
    return doc.editTilemap([&](std::optional<splash::Tilemap>& t) {
        splash::Tilemap m;
        m.tileset = tileset;
        m.resize(20, 15);
        splash::TileBrush floor;
        floor.color = tileBrushTint(0);
        splash::TileBrush wall;
        wall.name = "wall";
        wall.cell = cells == 1 ? 0 : 1;
        wall.walkable = false;
        wall.color = tileBrushTint(1);
        m.brushes = {floor, wall};
        t = std::move(m);
    });
}

bool removeTilemap(Document& doc) {
    return doc.editTilemap([](std::optional<splash::Tilemap>& t) { t.reset(); });
}

bool editTilemap(Document& doc, const std::function<void(splash::Tilemap&)>& fn, const std::string& mergeKey) {
    if (!doc.scene().tilemap) return false;
    return doc.editTilemap([&](std::optional<splash::Tilemap>& t) { fn(*t); }, mergeKey);
}

bool resizeTilemap(Document& doc, int width, int height, const std::string& mergeKey) {
    return editTilemap(doc, [&](splash::Tilemap& t) { t.resize(width, height); }, mergeKey);
}

std::string uniqueTileBrushName(const splash::Tilemap& t, const std::string& base) {
    const std::string b = base.empty() ? "brush" : base;
    auto taken = [&](const std::string& n) {
        return std::any_of(t.brushes.begin(), t.brushes.end(), [&](const splash::TileBrush& x) { return x.name == n; });
    };
    if (!taken(b)) return b;
    for (int k = 2;; k++)
        if (const std::string n = b + " (" + std::to_string(k) + ")"; !taken(n)) return n;
}

int addTileBrush(Document& doc, int cells) {
    int at = -1;
    editTilemap(doc, [&](splash::Tilemap& t) {
        splash::TileBrush b;
        b.name = uniqueTileBrushName(t, "brush");
        const int next = t.brushes.empty() ? 0 : std::max(t.brushes.back().cell, -1) + 1;
        b.cell = cells > 0 && next >= cells ? 0 : next;
        b.color = tileBrushTint(int(t.brushes.size()));
        t.brushes.push_back(b);
        at = int(t.brushes.size()) - 1;
    });
    return at;
}

bool deleteTileBrush(Document& doc, int brush) {
    const std::optional<splash::Tilemap>& tm = doc.scene().tilemap;
    if (!tm || brush < 0 || brush >= int(tm->brushes.size())) return false;
    return editTilemap(doc, [&](splash::Tilemap& t) {
        t.brushes.erase(t.brushes.begin() + brush);
        for (int& c : t.cells)
            if (c == brush) c = -1;
            else if (c > brush) c--;
    });
}

std::vector<std::array<int, 2>> tileLine(int x0, int y0, int x1, int y1) {
    std::vector<std::array<int, 2>> out;
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        out.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
    return out;
}

bool paintTiles(Document& doc, const std::vector<std::array<int, 2>>& cells, int brush, const std::string& strokeKey) {
    return editTilemap(
        doc,
        [&](splash::Tilemap& t) {
            for (const std::array<int, 2>& c : cells)
                if (c[0] >= 0 && c[1] >= 0 && c[0] < t.width && c[1] < t.height) t.cells[size_t(c[1] * t.width + c[0])] = brush;
        },
        strokeKey);
}

int fillTiles(Document& doc, int x0, int y0, int x1, int y1, int brush) {
    int changed = 0;
    editTilemap(doc, [&](splash::Tilemap& t) {
        const int ax = std::max(std::min(x0, x1), 0), bx = std::min(std::max(x0, x1), t.width - 1);
        const int ay = std::max(std::min(y0, y1), 0), by = std::min(std::max(y0, y1), t.height - 1);
        for (int y = ay; y <= by; y++)
            for (int x = ax; x <= bx; x++) {
                int& c = t.cells[size_t(y * t.width + x)];
                if (c != brush) c = brush, changed++;
            }
    });
    return changed;
}

int floodTiles(Document& doc, int x, int y, int brush) {
    const std::optional<splash::Tilemap>& tm = doc.scene().tilemap;
    if (!tm || x < 0 || y < 0 || x >= tm->width || y >= tm->height || size_t(y * tm->width + x) >= tm->cells.size() ||
        tm->at(x, y) == brush)
        return 0;
    int changed = 0;
    editTilemap(doc, [&](splash::Tilemap& t) {
        const int from = t.at(x, y);
        // Iterative (a 256 x 256 map is 65536 cells): each cell is set as it is
        // pushed, so none is pushed twice.
        std::vector<std::array<int, 2>> todo{{x, y}};
        t.cells[size_t(y * t.width + x)] = brush;
        changed = 1;
        static const int step[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        while (!todo.empty()) {
            const std::array<int, 2> c = todo.back();
            todo.pop_back();
            for (const auto& s : step) {
                const int nx = c[0] + s[0], ny = c[1] + s[1];
                if (nx < 0 || ny < 0 || nx >= t.width || ny >= t.height || size_t(ny * t.width + nx) >= t.cells.size()) continue;
                int& v = t.cells[size_t(ny * t.width + nx)];
                if (v != from) continue;
                v = brush;
                changed++;
                todo.push_back({nx, ny});
            }
        }
    });
    return changed;
}

}  // namespace editor
