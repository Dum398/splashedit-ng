// Headless tests for the editor's document model: the undo history and the
// viewport's ray picker and gizmo maths. No SDL, no GL. Run through CTest (`ctest`).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <set>
#include <string>
#include <system_error>
#ifndef _WIN32
#include <csignal>
#include <cstdlib>
#include <sys/types.h>
#endif

#include "editor/asset_browser.hh"
#include "editor/catalog.hh"
#include "editor/document.hh"
#include "editor/hardware.hh"
#include "editor/gizmo.hh"
#include "editor/live_export.hh"
#include "editor/lua_editor.hh"
#include "editor/memcard.hh"
#include "editor/model_import.hh"
#include "editor/pick.hh"
#include "editor/play.hh"
#include "editor/project.hh"
#include "editor/psxmon.hh"
#include "editor/serial.hh"
#include "editor/texture_preview.hh"
#include "editor/timeline.hh"
#include "editor/unirom.hh"
#include "blend.hh"
#include "editor/ui_canvas.hh"
#include "budget.hh"
#include "navregion.hh"
#include "splashpack.hh"
#include "texture.hh"
#include "worldstream.hh"

namespace {

int g_failures = 0;
int g_checks = 0;
std::filesystem::path g_outDir;  // next to the executable, inside the build tree

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

splash::Object named(const char* name, float x = 0) {
    splash::Object o;
    o.name = name;
    o.transform.position = {x, 0, 0};
    return o;
}

// Scene: A, B (with child B1), C.
splash::Scene sample() {
    splash::Scene s;
    s.objects.push_back(named("A", 1));
    splash::Object b = named("B", 2);
    b.children.push_back(named("B1", 3));
    s.objects.push_back(b);
    s.objects.push_back(named("C", 4));
    return s;
}

void setX(editor::Document& d, const editor::ObjectPath& p, float x, const std::string& key) {
    d.edit(p, [&](splash::Object& o) { o.transform.position.x = x; }, key);
}

void testApplyUndoRedo() {
    editor::Document d;
    d.reset(sample());
    CHECK(!d.dirty());
    CHECK(!d.canUndo() && !d.canRedo());
    unsigned rev = d.revision();
    setX(d, {0}, 10, "");
    CHECK(d.object({0})->transform.position.x == 10);
    CHECK(d.revision() != rev);
    CHECK(d.dirty());
    CHECK(d.canUndo());
    // The edit leaves children alone.
    setX(d, {1}, 20, "");
    CHECK(d.object({1, 0}) && d.object({1, 0})->name == "B1");
    rev = d.revision();
    CHECK(d.undo());
    CHECK(d.revision() != rev);
    CHECK(d.object({1})->transform.position.x == 2);
    CHECK(d.object({1, 0}) && d.object({1, 0})->name == "B1");
    CHECK(d.undo());
    CHECK(d.object({0})->transform.position.x == 1);
    CHECK(!d.dirty());
    CHECK(!d.undo());
    CHECK(d.redo());
    CHECK(d.object({0})->transform.position.x == 10);
    CHECK(d.dirty());
    // A new edit drops what could be redone.
    setX(d, {2}, 40, "");
    CHECK(!d.canRedo());
    CHECK(d.historySize() == 2);
}

void testMerge() {
    editor::Document d;
    d.reset(sample());
    // A drag: many updates with one key, one undo step.
    for (int i = 1; i <= 5; ++i) setX(d, {0}, 1.0f + static_cast<float>(i), "drag");
    CHECK(d.historySize() == 1);
    CHECK(d.object({0})->transform.position.x == 6);
    d.endMerge();
    // A second drag on the same field is its own step.
    for (int i = 1; i <= 3; ++i) setX(d, {0}, 6.0f + static_cast<float>(i), "drag");
    CHECK(d.historySize() == 2);
    CHECK(d.undo());
    CHECK(d.object({0})->transform.position.x == 6);
    CHECK(d.undo());
    CHECK(d.object({0})->transform.position.x == 1);
    CHECK(!d.dirty());
    CHECK(d.redo() && d.redo());
    CHECK(d.object({0})->transform.position.x == 9);

    // Different object or different key: no merge, even while a merge is open.
    editor::Document e;
    e.reset(sample());
    setX(e, {0}, 5, "drag");
    setX(e, {2}, 5, "drag");
    CHECK(e.historySize() == 2);
    setX(e, {2}, 6, "other");
    CHECK(e.historySize() == 3);
    // An edit without a key never merges.
    setX(e, {2}, 7, "");
    setX(e, {2}, 8, "");
    CHECK(e.historySize() == 5);
    // Undo closes the merge: the next keyed edit is a new step.
    setX(e, {1}, 1, "k");
    CHECK(e.undo());
    setX(e, {1}, 2, "k");
    setX(e, {1}, 3, "k");
    CHECK(e.historySize() == 6);
    CHECK(e.undo());
    CHECK(e.object({1})->transform.position.x == 2);
}

// Scene settings edit through the same history as objects.
void testSettings() {
    editor::Document d;
    d.reset(sample());
    unsigned rev = d.revision();
    for (int v : {300, 400, 500}) d.editSettings([&](splash::SceneSettings& s) { s.orderingTableSize = v; }, "ot");
    CHECK(d.historySize() == 1);
    CHECK(d.revision() > rev);
    CHECK(d.dirty());
    d.endMerge();
    d.editSettings([](splash::SceneSettings& s) { s.orderingTableSize = 0; });
    CHECK(d.scene().settings.orderingTableSize == 0);
    CHECK(d.undo());
    CHECK(d.scene().settings.orderingTableSize == 500);
    CHECK(d.undo());
    CHECK(d.scene().settings.orderingTableSize == 0);
    CHECK(!d.dirty());
    CHECK(d.redo());
    CHECK(d.scene().settings.orderingTableSize == 500);
}

// The Memory card section's helpers: the filename preview, each problem's
// fix, and the icon decoded through the export's shared palette.
void testMemCard() {
    splash::MemCardSettings mc;
    CHECK(editor::memCardFilename(mc, "SAVE1") == "BASLUS-00000SAVE1");
    CHECK(editor::memCardKeyRoom(mc) == 8);
    CHECK(editor::memCardFilename(mc, "CHECKPOINT_42") == "BASLUS-00000CHECKPOI");
    CHECK(editor::memCardFilename(mc, "CHECKPOINT_42").size() == splash::kMemCardFilenameMax);
    mc.product = "LONGPRODUCTCODE12345";
    CHECK(editor::memCardKeyRoom(mc) == 0);
    CHECK(editor::memCardFilename(mc, "SAVE1") == "BALONGPRODUCTCODE123");

    // Off: no problems at all, whatever the values.
    mc = {};
    mc.region = "X";
    CHECK(editor::memCardProblems(mc).empty());

    auto only = [](const splash::MemCardSettings& m) {
        std::vector<editor::MemCardProblem> ps = editor::memCardProblems(m);
        return ps.size() == 1 ? ps[0] : editor::MemCardProblem{true, "?", "?", editor::MemCardFix::None, nullptr};
    };
    auto fixes = [&](splash::MemCardSettings m, editor::MemCardFix fix, bool error) {
        editor::MemCardProblem p = only(m);
        bool ok = p.fix == fix && p.error == error && p.fixLabel && !p.title.empty() && !p.body.empty();
        ok = ok && editor::applyMemCardFix(m, p.fix, "my_level-1") && editor::memCardProblems(m).empty();
        if (!ok) std::fprintf(stderr, "memcard: %s | %s\n", p.title.c_str(), p.body.c_str());
        return ok;
    };
    splash::MemCardSettings on;
    on.enabled = true;
    on.icons = {"textures/icon.png"};
    CHECK(editor::memCardProblems(on).empty());
    splash::MemCardSettings m = on;
    m.region = "B";
    CHECK(fixes(m, editor::MemCardFix::RegionBA, true));
    m = on, m.region = "BX";
    CHECK(fixes(m, editor::MemCardFix::RegionBA, false));
    m = on, m.product = "";
    CHECK(fixes(m, editor::MemCardFix::ProductDefault, true));
    m = on, m.product = "SLUS-0000012";
    CHECK(fixes(m, editor::MemCardFix::ProductCut, true));
    m = on, m.product = "SLUS\x01" "000";
    CHECK(fixes(m, editor::MemCardFix::ProductAscii, true));
    m = on, m.title = std::string(40, 'A');
    CHECK(fixes(m, editor::MemCardFix::TitleCut, true));
    m = on, m.title = "CAF\xc3\xa9";
    CHECK(fixes(m, editor::MemCardFix::TitleAscii, true));
    m = on, m.title = "";
    CHECK(fixes(m, editor::MemCardFix::TitleScene, false));
    applyMemCardFix(m, editor::MemCardFix::TitleScene, "my_level-1");
    CHECK(m.title == "my level1");
    m = on, m.title = "Bob's save - 1";
    CHECK(fixes(m, editor::MemCardFix::TitleDropMarks, false));
    m = on, m.icons = {"a.png", "b.png", "c.png", "d.png"};
    CHECK(fixes(m, editor::MemCardFix::IconsFirst3, true));
    m = on, m.icons.clear();
    CHECK(only(m).fix == editor::MemCardFix::AddIcon && !only(m).error);
    CHECK(!editor::applyMemCardFix(m, editor::MemCardFix::AddIcon, "x"));
    CHECK(editor::memCardTitleFrom("") == "SAVE");
    CHECK(editor::memCardTitleFrom(std::string(50, 'z')).size() == splash::kMemCardTitleMax);

    // Icons: a 16x16 frame with a transparent corner and 20 colours, a
    // 16x16 one-colour frame, a 8x8 frame and a missing one.
    std::error_code ec;
    const std::filesystem::path root = g_outDir / "memcard";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "textures", ec);
    auto image = [](int w, int h, auto px) {
        splash::Image img;
        img.width = w, img.height = h;
        img.pixels.resize(size_t(w) * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) img.pixels[size_t(y) * w + x] = px(x, h - 1 - y);  // top row first
        return img;
    };
    splash::savePng(image(16, 16,
                          [](int x, int y) {
                              if (x == 0 && y == 0) return splash::Image::Px{0, 0, 0, 0};
                              return splash::Image::Px{float(x % 20) / 19.f, float(y) / 15.f, 0.5f, 1};
                          }),
                    root / "textures/a.png");
    splash::savePng(image(16, 16, [](int, int) { return splash::Image::Px{1, 0, 0, 1}; }), root / "textures/b.png");
    splash::savePng(image(8, 8, [](int, int) { return splash::Image::Px{0, 1, 0, 1}; }), root / "textures/small.png");
    editor::MemCardIcon ic = editor::memCardIcon({"textures/a.png", "textures/b.png", "textures/small.png", "textures/none.png"}, root);
    CHECK(ic.frames.size() == 3);  // the export reads 3 at most
    CHECK(ic.frames.size() == 3 && ic.frames[0].error.empty() && ic.frames[0].rgba.size() == 256);
    CHECK(ic.frames.size() == 3 && ic.frames[1].error.empty() && ic.frames[1].rgba.size() == 256);
    CHECK(ic.frames.size() == 3 && ic.frames[2].error == "8x8, not 16x16" && ic.frames[2].rgba.empty() && ic.frames[2].width == 8);
    CHECK(ic.paletteColors == 15);
    CHECK(ic.colors > 16);
    if (ic.frames.size() == 3 && ic.frames[0].rgba.size() == 256 && ic.frames[1].rgba.size() == 256) {
        CHECK(ic.frames[0].rgba[0] == 0);                         // the transparent top-left texel
        CHECK((ic.frames[0].rgba[1] >> 24) == 0xff);
        CHECK(ic.frames[1].rgba[0] == 0xff0000ffu);               // pure red, top row
        std::unordered_set<uint32_t> shown;
        for (const auto& f : ic.frames)
            for (uint32_t c : f.rgba)
                if (c) shown.insert(c);
        CHECK(shown.size() <= 15);                                 // one palette, entry 0 transparent
    }
    ic = editor::memCardIcon({"textures/none.png"}, root);
    CHECK(ic.frames.size() == 1 && ic.frames[0].error == "not found" && ic.colors == 0);

    // One edit, one undo step.
    editor::Document d;
    d.reset(sample());
    d.editSettings([](splash::SceneSettings& s) { s.memCard.enabled = true; });
    d.editSettings([](splash::SceneSettings& s) { s.memCard.title = "MY GAME"; });
    CHECK(d.historySize() == 2);
    CHECK(d.undo() && d.scene().settings.memCard.title == "PSXSPLASH SAVE" && d.scene().settings.memCard.enabled);
}

void testRenderPeak() {
    editor::RenderPeak p;
    CHECK(editor::parseRenderPeak("psxsplash: render peak depth 258 of 988, bump 11808 of 51656", &p));
    CHECK(p.depth == 258 && p.orderingTable == 988 && p.bump == 11808 && p.bumpSize == 51656);
    CHECK(editor::parseRenderPeak("[tty] psxsplash: render peak depth 3 of 256, bump 0 of 4000", &p) && p.bumpSize == 4000);
    CHECK(!editor::parseRenderPeak("psxsplash: render peak depth", &p));
    CHECK(!editor::parseRenderPeak("Loading scene 0", &p));
}

void testDirtyAndSave() {
    std::error_code ec;
    std::filesystem::path dir = g_outDir;
    std::filesystem::create_directories(dir, ec);
    editor::Document d;
    d.reset(sample(), dir, "saved");
    setX(d, {0}, 3, "");
    CHECK(d.dirty());
    CHECK(!d.save());
    CHECK(!d.dirty());
    CHECK(std::filesystem::is_regular_file(dir / "saved.scene"));
    setX(d, {0}, 4, "");
    CHECK(d.dirty());
    CHECK(d.undo());
    CHECK(!d.dirty());  // back at the saved state
    CHECK(d.undo());
    CHECK(d.dirty());  // before the saved state
    CHECK(d.redo());
    CHECK(!d.dirty());

    // Merging into the saved step makes the document dirty again.
    editor::Document m;
    m.reset(sample(), dir, "saved2");
    setX(m, {0}, 3, "drag");
    CHECK(!m.save());
    setX(m, {0}, 4, "drag");  // save closed the merge, so this is a new step
    CHECK(m.historySize() == 2);
    CHECK(m.dirty());

    // Reloading what was saved gives the edited value.
    editor::Document r;
    CHECK(!r.load(dir, "saved.scene"));
    CHECK(r.object({0}) && r.object({0})->transform.position.x == 3);
    CHECK(!r.dirty() && !r.canUndo());
}

void testStructure() {
    editor::Document d;
    d.reset(sample());
    d.select(editor::ObjectPath{2});  // C
    CHECK(d.removeObject({0}));       // A
    CHECK(d.scene().objects.size() == 2);
    CHECK(d.objectCount() == 3);
    CHECK(d.selected() && d.selected()->name == "C");  // the selection follows C to index 1
    CHECK(d.undo());
    CHECK(d.scene().objects.size() == 3 && d.object({0})->name == "A");
    CHECK(d.objectCount() == 4);

    // Delete the selected object; undo brings it back with its children.
    d.select(editor::ObjectPath{1});
    CHECK(d.removeObject({1}));
    CHECK(!d.selection());
    CHECK(d.undo());
    CHECK(d.object({1})->name == "B" && d.object({1, 0}) && d.object({1, 0})->name == "B1");

    // Duplicate: inserted after the original, numbered, selected, undoable.
    auto p = d.duplicateObject({1});
    CHECK(p && *p == editor::ObjectPath({2}));
    CHECK(d.object({2})->name == "B (2)" && d.object({2, 0}) && d.object({2, 0})->name == "B1");
    CHECK(d.object({3})->name == "C");
    CHECK(d.selected() == d.object({2}));
    auto q = d.duplicateObject({2});
    CHECK(q && d.object(*q)->name == "B (3)");
    CHECK(d.undo() && d.undo());
    CHECK(d.scene().objects.size() == 3 && d.object({2})->name == "C");
    CHECK(d.redo());
    CHECK(d.object({2})->name == "B (2)");

    // Rename through edit().
    d.edit({0}, [](splash::Object& o) { o.name = "Renamed"; });
    CHECK(d.object({0})->name == "Renamed");
    CHECK(d.undo());
    CHECK(d.object({0})->name == "A");

    // Flatten order paths.
    std::vector<editor::ObjectPath> fp = d.flatPaths();
    std::vector<splash::FlatObject> flats = splash::flatten(d.scene());
    CHECK(fp.size() == flats.size());
    for (size_t i = 0; i < fp.size() && i < flats.size(); ++i) CHECK(d.object(fp[i]) == flats[i].object);
}

// An axis-aligned box as 12 triangles.
void addBox(editor::PickMesh& m, int object, splash::Vec3 c, float h) {
    splash::Vec3 v[8];
    for (int i = 0; i < 8; ++i) v[i] = {c.x + (i & 1 ? h : -h), c.y + (i & 2 ? h : -h), c.z + (i & 4 ? h : -h)};
    const int f[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    size_t first = m.positions.size() / 3;
    for (auto& q : f) {
        m.positions.insert(m.positions.end(), {v[q[0]], v[q[1]], v[q[2]]});
        m.positions.insert(m.positions.end(), {v[q[0]], v[q[2]], v[q[3]]});
    }
    m.ranges.push_back({object, first, 12});
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void testPick() {
    editor::PickMesh m;
    addBox(m, 7, {0, 0, 0}, 1);
    // Hit: straight down the Z axis from z = -5 hits the near face at z = -1.
    auto hit = editor::pickNearest({{0, 0, -5}, {0, 0, 1}}, m);
    CHECK(hit && hit->object == 7 && near(hit->t, 4));
    // Off-centre and oblique still hit.
    hit = editor::pickNearest({{0.9f, -0.9f, -5}, {0, 0, 1}}, m);
    CHECK(hit && hit->object == 7);
    hit = editor::pickNearest({{-4, 3, -4}, {1, -0.75f, 1}}, m);
    CHECK(hit && hit->object == 7);
    // Miss: beside the box, and pointing away from it.
    CHECK(!editor::pickNearest({{1.5f, 0, -5}, {0, 0, 1}}, m));
    CHECK(!editor::pickNearest({{0, 0, -5}, {0, 0, -1}}, m));
    CHECK(!editor::pickNearest({{0, 2, -5}, {0, 0, 1}}, m));
    // From inside the box the far side counts (both faces).
    hit = editor::pickNearest({{0, 0, 0}, {0, 0, 1}}, m);
    CHECK(hit && near(hit->t, 1));

    // Nearest of two: a second box behind the first along the ray, added
    // first and last so the order in the soup cannot decide.
    editor::PickMesh two;
    addBox(two, 2, {0, 0, 6}, 1);
    addBox(two, 1, {0, 0, 0}, 1);
    hit = editor::pickNearest({{0, 0, -5}, {0, 0, 1}}, two);
    CHECK(hit && hit->object == 1 && near(hit->t, 4));
    hit = editor::pickNearest({{0, 0, 12}, {0, 0, -1}}, two);
    CHECK(hit && hit->object == 2 && near(hit->t, 5));
    // A ray that only passes the far box.
    hit = editor::pickNearest({{0, 5, 6}, {0, -1, 0}}, two);
    CHECK(hit && hit->object == 2);

    // Sphere (scene icons).
    auto s = editor::raySphere({{0, 0, -5}, {0, 0, 1}}, {0, 0, 0}, 0.5f);
    CHECK(s && near(*s, 4.5f));
    CHECK(!editor::raySphere({{1, 0, -5}, {0, 0, 1}}, {0, 0, 0}, 0.5f));
}

bool sameQuat(splash::Quat a, splash::Quat b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
bool nearV(splash::Vec3 a, splash::Vec3 b, float eps = 1e-5f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// The pure maths behind the rotate and scale gizmos.
void testGizmoMath() {
    const float pi = editor::kPi;
    auto nearF = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    // Quaternion.AngleAxis(90, up) * forward == right, as in Unity.
    splash::Quat q = editor::quatAxisAngle({0, 1, 0}, pi / 2);
    CHECK(nearV(splash::rotate(q, {0, 0, 1}), {1, 0, 0}));
    // The axis need not be unit length; a zero axis is the identity.
    CHECK(nearV(splash::rotate(editor::quatAxisAngle({0, 0, 5}, pi / 2), {1, 0, 0}), {0, 1, 0}));
    CHECK(sameQuat(editor::quatAxisAngle({0, 0, 0}, 1), splash::Quat{}));
    // a * b applies b first: 90 about Y then 90 about X takes +Z to +X then keeps it.
    splash::Quat x90 = editor::quatAxisAngle({1, 0, 0}, pi / 2);
    CHECK(nearV(splash::rotate(editor::quatMul(x90, q), {0, 0, 1}), {1, 0, 0}));
    CHECK(nearV(splash::rotate(editor::quatMul(q, x90), {0, 1, 0}), {1, 0, 0}));
    splash::Quat n = editor::quatNormalize({0, 0, 0, 2});
    CHECK(sameQuat(n, splash::Quat{}));
    CHECK(sameQuat(editor::quatNormalize({0, 0, 0, 0}), splash::Quat{}));

    // Mouse angle around the centre, y down: right is 0, below is +90.
    CHECK(nearF(editor::screenAngle(100, 100, 150, 100), 0));
    CHECK(nearF(editor::screenAngle(100, 100, 100, 160), pi / 2));
    // Steps go the short way across the +-180 seam, so a drag can pass it.
    CHECK(nearF(editor::angleStep(pi - 0.1f, -pi + 0.1f), 0.2f));
    CHECK(nearF(editor::angleStep(-pi + 0.1f, pi - 0.1f), -0.2f));
    CHECK(nearF(editor::angleStep(0.5f, 0.2f), -0.3f));
    float acc = 0, last = 0;
    for (int i = 1; i <= 40; ++i) {  // a full turn and a bit, in 10 degree steps
        float a = std::remainder(i * pi / 18, 2 * pi);
        acc += editor::angleStep(last, a);
        last = a;
    }
    CHECK(nearF(acc, 40 * pi / 18));

    // Snapping.
    CHECK(editor::snapTo(0.37f, 0.25f) == 0.25f);
    CHECK(editor::snapTo(0.38f, 0.25f) == 0.5f);
    CHECK(editor::snapTo(0.37f, 0) == 0.37f);
    CHECK(nearF(editor::rotateDragDegrees(pi / 4, false), 45));
    CHECK(editor::rotateDragDegrees(23.0f * pi / 180, true) == 30);
    CHECK(editor::rotateDragDegrees(-22.0f * pi / 180, true) == -15);
    CHECK(editor::rotateDragDegrees(7.0f * pi / 180, true) == 0);
    CHECK(nearF(editor::scaleAxis(1, 1.234f, true), 1.2f));
    CHECK(nearF(editor::scaleAxis(1, 1.234f, false), 1.234f));
    CHECK(nearF(editor::scaleAxis(2, 0.5f, false), 1));
}

// A scale gizmo can never reach zero or flip an object.
void testScaleClamp() {
    using editor::kMinScale;
    // Dragged through zero and beyond: held at the minimum, still positive.
    CHECK(editor::scaleAxis(1, 0, false) == kMinScale);
    CHECK(editor::scaleAxis(1, -3, false) == kMinScale);
    CHECK(editor::scaleAxis(1, 0.001f, false) == kMinScale);
    // Snapping a tiny value to 0 does not reach zero either.
    CHECK(editor::scaleAxis(1, 0.04f, true) == kMinScale);
    CHECK(editor::scaleAxis(0.5f, 0.05f, true) == kMinScale);
    // Above the minimum it is left alone.
    CHECK(editor::scaleAxis(1, 0.02f, false) == 0.02f);
    // An object already mirrored stays mirrored and away from zero.
    CHECK(editor::scaleAxis(-2, 0.5f, false) == -1);
    CHECK(editor::scaleAxis(-2, -1, false) == -kMinScale);
    CHECK(editor::scaleAxis(-2, 0, false) == -kMinScale);
    // A zero start recovers to the minimum rather than staying collapsed.
    CHECK(editor::scaleAxis(0, 3, false) == kMinScale);
    CHECK(editor::clampScale(std::nanf(""), 1) == kMinScale);
    // Uniform: per component, each keeping its sign; snap scales the factor.
    splash::Vec3 u = editor::scaleUniform({1, 2, -0.5f}, -1, false);
    CHECK(u.x == kMinScale && u.y == kMinScale && u.z == -kMinScale);
    u = editor::scaleUniform({1, 2, 0.5f}, 1.13f, true);
    CHECK(nearV(u, {1.1f, 2.2f, 0.55f}));
}

// One gizmo drag (many merged edits) undoes to the exact transform before it
// and redoes to the exact transform after it, for rotate and for scale.
void testGizmoUndo() {
    editor::Document d;
    splash::Scene s = sample();
    s.objects[0].transform.rotation = editor::quatAxisAngle({0.3f, 1, 0.2f}, 0.7f);
    s.objects[0].transform.scale = {1.5f, 0.75f, 2};
    d.reset(s);
    const splash::Transform before = d.object({0})->transform;

    // A rotate drag: a run of frames about world Y, as the gizmo edits.
    splash::Quat last;
    for (int f = 1; f <= 8; ++f) {
        float deg = editor::rotateDragDegrees(f * 0.05f, true);
        last = editor::quatNormalize(editor::quatMul(editor::quatAxisAngle({0, 1, 0}, deg * editor::kPi / 180), before.rotation));
        d.edit({0}, [&](splash::Object& o) { o.transform.rotation = last; }, "gizmo.rotate");
    }
    d.endMerge();
    CHECK(d.historySize() == 1);
    CHECK(sameQuat(d.object({0})->transform.rotation, last));
    CHECK(!sameQuat(last, before.rotation));
    CHECK(d.undo());
    CHECK(sameQuat(d.object({0})->transform.rotation, before.rotation));
    CHECK(d.object({0})->transform.scale == before.scale && d.object({0})->transform.position == before.position);
    CHECK(d.redo());
    CHECK(sameQuat(d.object({0})->transform.rotation, last));

    // A scale drag on X, then a uniform one: two drags, two steps.
    const splash::Transform mid = d.object({0})->transform;
    splash::Vec3 sx = mid.scale;
    for (int f = 1; f <= 6; ++f) {
        sx.x = editor::scaleAxis(mid.scale.x, 1 + f * 0.1f, false);
        d.edit({0}, [&](splash::Object& o) { o.transform.scale = sx; }, "gizmo.scale");
    }
    d.endMerge();
    splash::Vec3 su;
    for (int f = 1; f <= 6; ++f) {
        su = editor::scaleUniform(sx, 1 - f * 0.3f, false);  // past zero: clamped
        d.edit({0}, [&](splash::Object& o) { o.transform.scale = su; }, "gizmo.scale");
    }
    d.endMerge();
    CHECK(d.historySize() == 3);
    CHECK(su.x == editor::kMinScale && su.y == editor::kMinScale && su.z == editor::kMinScale);
    CHECK(d.object({0})->transform.scale == su);
    CHECK(d.undo());
    CHECK(d.object({0})->transform.scale == sx);
    CHECK(d.undo());
    CHECK(d.object({0})->transform.scale == mid.scale);
    CHECK(sameQuat(d.object({0})->transform.rotation, last));
    CHECK(d.undo());
    CHECK(sameQuat(d.object({0})->transform.rotation, before.rotation));
    CHECK(d.object({0})->transform.scale == before.scale);
    CHECK(!d.dirty());
    CHECK(d.redo() && d.redo());
    CHECK(d.object({0})->transform.scale == sx);
    CHECK(d.redo());
    CHECK(d.object({0})->transform.scale == su);

    // A rotate drag right after a move drag does not merge into it.
    d.reset(sample());
    setX(d, {0}, 5, "gizmo.move");
    d.edit({0}, [&](splash::Object& o) { o.transform.rotation = last; }, "gizmo.rotate");
    CHECK(d.historySize() == 2);
}

// What the Add object / Add component pickers offer, and how search ranks it.
void testCatalog() {
    using editor::ComponentKind;
    std::vector<std::pair<const char*, const char*>> items;
    for (const editor::ObjectPreset& p : editor::presets()) items.push_back({p.label, p.keywords});
    auto first = [&](const char* q) {
        std::vector<int> r = editor::rank(q, items);
        return r.empty() ? std::string() : std::string(editor::presets()[r[0]].label);
    };
    CHECK(editor::rank("", items).size() == items.size());
    CHECK(editor::rank("   ", items).size() == items.size());
    CHECK(first("") == "Empty");               // empty query keeps the list order
    CHECK(first("lig") == "Point light");      // word prefix inside the label
    CHECK(first("POINT") == "Point light");    // case-insensitive
    CHECK(first("sfx") == "Audio clip");       // keyword
    CHECK(first("tbx") == "Trigger box");      // letters in order
    CHECK(first("Inter") == "Interactable");
    CHECK(editor::rank("zzq", items).empty()); // nothing matches
    // A label prefix outranks a keyword hit: "mesh" is the Mesh preset, not one tagged "model".
    CHECK(first("mesh") == "Mesh");

    // Components: dependencies, cascade on removal, no double add.
    splash::Object o;
    o.name = "Crate";
    CHECK(editor::cannotAdd(o, ComponentKind::Collider) == "Needs a Mesh");
    CHECK(!editor::addComponent(o, ComponentKind::Collider) && !o.collider);
    CHECK(!editor::addComponent(o, ComponentKind::Skin) && !o.skin);
    CHECK(editor::addComponent(o, ComponentKind::Mesh) && o.mesh);
    CHECK(editor::cannotAdd(o, ComponentKind::Mesh) == "Already added");
    CHECK(!editor::addComponent(o, ComponentKind::Mesh));
    CHECK(editor::addComponent(o, ComponentKind::Collider));
    CHECK(o.collider && o.collider->kind == splash::ColliderKind::Static);
    CHECK(editor::addComponent(o, ComponentKind::Skin) && o.skin);
    CHECK(editor::addComponent(o, ComponentKind::Audio) && o.audio && o.audio->clipName == "Crate");
    CHECK(editor::removeComponent(o, ComponentKind::Mesh));
    CHECK(!o.mesh && !o.collider && !o.skin && o.audio);
    CHECK(!editor::removeComponent(o, ComponentKind::Mesh));
    for (const editor::ComponentInfo& c : editor::components()) {
        splash::Object m;
        m.mesh.emplace();
        if (c.kind != ComponentKind::Mesh) CHECK(editor::addComponent(m, c.kind) && editor::hasComponent(m, c.kind));
        CHECK(editor::removeComponent(m, c.kind) && !editor::hasComponent(m, c.kind));
    }

    // New objects take a free Unity-style name among their siblings.
    splash::Scene sc = sample();
    sc.objects.push_back(named("Point light"));
    const editor::ObjectPreset& light = editor::presets()[2];
    CHECK(std::string(light.label) == "Point light");
    splash::Object l2 = editor::makeObject(light, sc.objects);
    CHECK(l2.name == "Point light (2)" && l2.light && !l2.mesh);
    sc.objects.push_back(l2);
    CHECK(editor::uniqueName("Point light", sc.objects) == "Point light (3)");
    CHECK(editor::uniqueName("Empty", sc.objects) == "Empty");

    // Through the document: one undo step each way.
    editor::Document d;
    d.reset(sample());
    CHECK(d.edit({0}, [](splash::Object& ob) { editor::addComponent(ob, ComponentKind::Trigger); }));
    CHECK(d.object({0})->trigger);
    CHECK(d.undo() && !d.object({0})->trigger);
    CHECK(d.insertObject({3}, editor::makeObject(editor::presets()[0], d.scene().objects)));
    CHECK(d.object({3}) && d.object({3})->name == "Empty" && d.selection() == editor::ObjectPath{3});
    CHECK(d.undo() && !d.object({3}));
}

// Export stats: a dry run writes nothing and reports the sizes a real export
// writes; SPU use follows psxsplash's allocator, recomputed here from the .spu.
void testExportStats() {
    namespace fs = std::filesystem;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = src / "examples" / "courtyard";
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    for (const char* clip : {"short11.wav", "sine22.wav"}) {
        splash::Object o = named(clip);
        splash::AudioComponent a;
        a.clip = (fs::path("..") / ".." / "tests" / "audio" / clip).generic_string();
        a.clipName = clip;
        a.sampleRate = 11025;
        o.audio = a;
        scene.objects.push_back(o);
    }
    const fs::path dir = g_outDir / "stats";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::ExportOptions dry;
    dry.dryRun = true;
    splash::ExportResult d = splash::exportSplashpack(scene, project, dir / "scene.splashpack", dry);
    CHECK(d.ok());
    CHECK(fs::is_empty(dir));
    splash::ExportResult r = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(r.ok());
    std::error_code ec;
    CHECK(d.stats.splashpackBytes == fs::file_size(dir / "scene.splashpack", ec));
    CHECK(d.stats.vramFileBytes == fs::file_size(dir / "scene.vram", ec));
    CHECK(d.stats.spuFileBytes == fs::file_size(dir / "scene.spu", ec));
    CHECK(d.stats.framebufferBytes == 320 * 240 * 2 * 2);
    CHECK(d.stats.atlasBytes > 0 && d.stats.triangles > 0);

    // .spu: "SA", u16 count, then per clip u32 size, u16 rate, u8 loop, u8 0, data, align 4.
    std::ifstream in(dir / "scene.spu", std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t end = 0x1010, p = 4;
    int clips = 0;
    while (p + 8 <= b.size()) {
        size_t n = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | size_t(b[p + 3]) << 24;
        end = (end + 15) / 16 * 16 + (n + 15) / 16 * 16;
        p += (8 + n + 3) / 4 * 4;
        clips++;
    }
    CHECK(clips == 2);
    CHECK(d.stats.spuEnd == end);
    CHECK(splash::spuBudget(d.stats).used == end);
    // The visible-triangle list is sized from the header's bvhTriangleRefCount
    // (u16 at offset 34), read back from the written file.
    {
        std::ifstream f(dir / "scene.splashpack", std::ios::binary);
        unsigned char h[36] = {};
        f.read(reinterpret_cast<char*>(h), sizeof h);
        CHECK(f.good());
        uint32_t refs = h[34] | h[35] << 8;
        CHECK(refs > 0);
        CHECK(d.stats.bvhTriangleRefs == refs);
        CHECK(d.stats.visibleListBytes() == 4 * size_t(refs));
    }
    CHECK(splash::ramBudget(d.stats).used == d.stats.rendererBytes() + d.stats.visibleListBytes() +
                                                 std::max({d.stats.splashpackBytes, d.stats.vramFileBytes,
                                                           d.stats.spuFileBytes}) +
                                                 splash::objectLoadBytes(d.stats) + splash::kEngineLoadBytes);

    // Overrides replace the estimate in the pack and in the RAM meter; an
    // ordering table below the minimum is raised to it.
    CHECK(d.stats.orderingTableSize == d.stats.orderingTableNeed && d.stats.bumpAllocatorSize == d.stats.bumpAllocatorNeed);
    scene.settings.orderingTableSize = 10;
    scene.settings.bumpAllocatorSize = 4001;
    splash::ExportResult o = splash::exportSplashpack(scene, project, dir / "scene.splashpack", dry);
    CHECK(o.ok());
    CHECK(o.stats.orderingTableSize == splash::kOtMin);
    CHECK(o.stats.bumpAllocatorSize == 4004);
    CHECK(o.stats.orderingTableNeed == d.stats.orderingTableNeed);
    CHECK(o.stats.rendererBytes() == 2 * (splash::kOtMin + 1) * 4 + 2 * 4004);
    CHECK(o.stats.bvhTriangleRefs == d.stats.bvhTriangleRefs);
    CHECK(splash::ramBudget(o.stats).used == o.stats.rendererBytes() + o.stats.visibleListBytes() +
                                                 std::max({o.stats.splashpackBytes, o.stats.vramFileBytes,
                                                           o.stats.spuFileBytes}) +
                                                 splash::objectLoadBytes(o.stats) + splash::kEngineLoadBytes);
}

// RAM per game object: the engine's agent state and Lua's tables, measured on
// psxsplash (budget.hh). A scene of many small props fails on the object count
// and says so with its fix; the same geometry in few objects fits.
void testObjectRamBudget() {
    splash::ExportStats s;
    s.gameObjects = 0;
    CHECK(splash::objectLoadBytes(s) == splash::kAgentStateBytes + splash::luaRegistryPeakBytes(0));
    s.gameObjects = 545;
    // 1090 registry entries -> 2048 nodes.
    CHECK(splash::luaRegistryPeakBytes(545) == 2048 * splash::kLuaRegistryNodeBytes * 3 / 2);
    CHECK(splash::objectLoadBytes(s) == 546 * splash::kAgentStateBytes + 545 * splash::kLuaObjectBytes +
                                            splash::luaRegistryPeakBytes(545));
    // The measured heap after loading that 545-object scene was 1097504 bytes
    // with 795328 from the other terms: the model must not undercount it.
    s.splashpackBytes = 795328;
    CHECK(splash::ramBudget(s).used >= 1097504);
    CHECK(!splash::objectsBindRam(s));
    // The 1257-object scene that ran Lua out of memory, 1078168 bytes before.
    s.gameObjects = 1257;
    s.splashpackBytes = 1078168;
    CHECK(splash::ramBudget(s).used > splash::kEngineHeapBytes);
    CHECK(splash::objectsBindRam(s));
    const std::string w = splash::objectRamWarning(s);
    CHECK(w.find("1257 objects") != std::string::npos && w.find("Merge props") != std::string::npos);
    // Geometry alone over budget is not the object count's fault.
    s.splashpackBytes = splash::kEngineHeapBytes;
    CHECK(!splash::objectsBindRam(s));
}

// A script on an object with no mesh (the courtyard's Game Logic) ships as a
// game object with no triangles, so the engine still runs it.
void testScriptWithoutMesh() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    const fs::path dir = g_outDir / "meshless-script";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto exportBytes = [&](const splash::Scene& scene, splash::ExportResult& r) {
        r = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
        std::ifstream in(dir / "scene.splashpack", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    splash::ExportResult with;
    std::string a = exportBytes(scene, with);
    CHECK(with.ok());
    CHECK(a.find("scene-wide game logic") != std::string::npos);
    CHECK(a.find("Game Logic") != std::string::npos);
    std::erase_if(scene.objects, [](const splash::Object& o) { return o.name == "Game Logic"; });
    splash::ExportResult without;
    std::string b = exportBytes(scene, without);
    CHECK(without.ok());
    CHECK(b.find("scene-wide game logic") == std::string::npos);
    CHECK(with.stats.triangles == without.stats.triangles);
}

// LiveExport: waits for edits to settle, then exports the current revision.
void testLiveExport() {
    const std::filesystem::path project = std::filesystem::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    editor::Document d;
    CHECK(!d.load(project, "courtyard.scene"));
    editor::LiveExport live(0.3);
    live.update(d, 10.0);
    CHECK(!live.busy());
    live.update(d, 10.2);
    CHECK(!live.busy());
    live.update(d, 10.4);
    CHECK(live.busy());
    live.wait();
    CHECK(live.result() && live.result()->ok());
    CHECK(live.resultRevision() == d.revision());
    const size_t before = live.result()->stats.splashpackBytes;
    CHECK(d.duplicateObject({0}));
    live.update(d, 10.5);
    CHECK(!live.busy());
    live.update(d, 10.9);
    CHECK(live.busy());
    live.wait();
    CHECK(live.resultRevision() == d.revision());
    CHECK(live.result()->stats.splashpackBytes > before);
    live.update(d, 20.0);
    CHECK(!live.busy());  // nothing changed since the last export
}

static void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

void testPlay() {
    namespace fs = std::filesystem;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = src / "examples" / "courtyard";
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    const fs::path dir = g_outDir / "play";
    fs::remove_all(dir);

    // The export lands under the names psxsplash's PCdrv loader opens.
    splash::ExportResult r = editor::exportForPlay(scene, project, dir);
    CHECK(r.ok());
    std::error_code ec;
    CHECK(fs::file_size(dir / "scene_0.splashpack", ec) == r.stats.splashpackBytes && r.stats.splashpackBytes > 0);
    CHECK(fs::file_size(dir / "scene_0.vram", ec) == r.stats.vramFileBytes && r.stats.vramFileBytes > 0);
    CHECK(fs::exists(dir / "scene_0.spu"));

    // A failed export leaves nothing behind to boot.
    splash::Scene bad = scene;
    splash::Object o = named("broken");
    o.mesh = splash::MeshComponent{};
    o.mesh->mesh = "no/such.mesh";  // and no materials: an export error
    bad.objects.push_back(o);
    splash::ExportResult b = editor::exportForPlay(bad, project, dir);
    CHECK(!b.ok());
    CHECK(!fs::exists(dir / "scene_0.splashpack") && !fs::exists(dir / "scene_0.vram") && !fs::exists(dir / "scene_0.spu"));

    // Live edits: a good export replaces the files and bumps the flag the game
    // polls; a bad one leaves both as they were. Play writes the flag at 0.
    auto flag = [&] {
        std::ifstream in(dir / "reload.flag");
        std::string v;
        std::getline(in, v);
        return v;
    };
    CHECK(editor::exportForPlay(scene, project, dir).ok() && !fs::exists(dir / "reload.flag"));
    CHECK(editor::writeReloadFlag(dir, 0) && flag() == "0");
    splash::Scene moved = scene;
    moved.objects.push_back(named("added during play"));
    splash::ExportResult lr = editor::reloadForPlay(moved, project, dir, 3);
    CHECK(lr.ok() && flag() == "3");
    CHECK(fs::file_size(dir / "scene_0.splashpack", ec) == lr.stats.splashpackBytes);
    const auto packTime = fs::last_write_time(dir / "scene_0.splashpack", ec);
    CHECK(!editor::reloadForPlay(bad, project, dir, 4).ok());
    CHECK(flag() == "3" && fs::exists(dir / "scene_0.vram") && fs::last_write_time(dir / "scene_0.splashpack", ec) == packTime);

    // Settings: round trip (non-ASCII path), missing file, unknown keys.
    const fs::path cfg = g_outDir / "play" / "play.cfg";
    editor::PlayTools t;
    t.redux = fs::path(u8"/opt/rédux/pcsx-redux");
    t.psxsplash = "/x/psxsplash.ps-exe";
    t.disc = "/x/psxsplash-cdrom.ps-exe";
    t.port = "tcp:localhost:6699";
    CHECK(editor::savePlayTools(cfg, t));
    { std::ofstream(cfg, std::ios::app) << "colour=blue\nnot a pair\n"; }
    editor::PlayTools l = editor::loadPlayTools(cfg);
    CHECK(l.redux == t.redux && l.psxsplash == t.psxsplash && l.bios.empty() && l.disc == t.disc && l.port == t.port);
    editor::PlayTools noPort = l;
    noPort.port.clear();
    CHECK(editor::missingHardwareTools(noPort).size() == 2);  // the build is not on disk either
    editor::PlayTools none = editor::loadPlayTools(dir / "absent.cfg");
    CHECK(none.redux.empty() && none.psxsplash.empty());

    // Environment fills only what the settings left empty.
    setEnv("SPLASHEDIT_PSXSPLASH", "/env/psxsplash.ps-exe");
    setEnv("SPLASHEDIT_REDUX", "/env/pcsx-redux");
    editor::PlayTools onDisk = l;
    onDisk.redux = cfg;  // an existing file stands in for the programs
    onDisk.psxsplash = cfg;
    editor::PlayTools e = editor::withDefaults(onDisk);
    CHECK(e.redux == cfg && e.psxsplash == cfg);
    // A saved path that is gone falls through to what is there now.
    editor::PlayTools gone = editor::withDefaults(l);
    CHECK(gone.redux == fs::path("/env/pcsx-redux") && gone.psxsplash == fs::path("/env/psxsplash.ps-exe"));
    editor::PlayTools e2 = editor::withDefaults({});
    CHECK(e2.redux == fs::path("/env/pcsx-redux") && e2.psxsplash == fs::path("/env/psxsplash.ps-exe"));

    // A packaged editor carries redux/ and engine/ beside itself; the
    // environment still wins over them, and they win over PATH.
    fs::path bundle = dir / "bundle";
    fs::create_directories(bundle / "redux");
    fs::create_directories(bundle / "engine");
    std::ofstream(bundle / "redux" / "pcsx-redux") << "x";
    std::ofstream(bundle / "engine" / "psxsplash.ps-exe") << "x";
    CHECK(!editor::missingDiscTools(editor::withDefaults({}, bundle)).empty());
    std::ofstream(bundle / "engine" / "psxsplash-cdrom.ps-exe") << "x";
    setEnv("SPLASHEDIT_DISC_ENGINE", "/env/psxsplash-cdrom.ps-exe");
    editor::PlayTools envWins = editor::withDefaults({}, bundle);
    CHECK(envWins.redux == fs::path("/env/pcsx-redux") && envWins.psxsplash == fs::path("/env/psxsplash.ps-exe") &&
          envWins.disc == fs::path("/env/psxsplash-cdrom.ps-exe"));
    setEnv("SPLASHEDIT_PSXSPLASH", "");
    setEnv("SPLASHEDIT_REDUX", "");
    setEnv("SPLASHEDIT_DISC_ENGINE", "");
    editor::PlayTools fromBundle = editor::withDefaults({}, bundle);
    CHECK(fromBundle.redux == bundle / "redux" / "pcsx-redux" &&
          fromBundle.psxsplash == bundle / "engine" / "psxsplash.ps-exe" &&
          fromBundle.disc == bundle / "engine" / "psxsplash-cdrom.ps-exe");
    CHECK(editor::missingTools(fromBundle).empty());
    CHECK(editor::missingDiscTools(fromBundle).empty());
    editor::PlayTools empty = editor::withDefaults({}, dir / "nothing-here");
    CHECK(empty.psxsplash.empty());
    // A stale saved build loses to the packaged one.
    editor::PlayTools stale;
    stale.psxsplash = dir / "old-build" / "psxsplash.ps-exe";
    CHECK(editor::withDefaults(stale, bundle).psxsplash == bundle / "engine" / "psxsplash.ps-exe");

    // Windows: the pcsx-redux.exe launcher hands off to pcsx-redux.main, so
    // Play runs .main directly when it is there.
    fs::path win = dir / "winbundle";
    fs::create_directories(win / "redux");
    std::ofstream(win / "redux" / "pcsx-redux.exe") << "x";
    CHECK(editor::withDefaults({}, win).redux == win / "redux" / "pcsx-redux.exe");
    std::ofstream(win / "redux" / "pcsx-redux.main") << "x";
    CHECK(editor::withDefaults({}, win).redux == win / "redux" / "pcsx-redux.main");
    editor::PlayTools picked;
    picked.redux = win / "redux" / "pcsx-redux.exe";
    CHECK(editor::withDefaults(picked).redux == win / "redux" / "pcsx-redux.main");

    // What is missing, and the command once nothing is.
    CHECK(editor::missingTools({}).size() == 2);
    editor::PlayTools real;
    real.redux = dir / "play.cfg";  // any existing file stands in for the programs here
    real.psxsplash = dir / "play.cfg";
    CHECK(editor::missingTools(real).empty());
    real.bios = dir / "absent.bin";
    CHECK(editor::missingTools(real).size() == 1);
    real.bios.clear();
    std::vector<std::string> cmd = editor::reduxCommand(t, "/b");
    std::vector<std::string> want = {(const char*)u8"/opt/rédux/pcsx-redux", "-run", "-fastboot", "-no-ui",
                                     "-shmdisplay", "-loadexe", "/x/psxsplash.ps-exe", "-pcdrv", "-pcdrvbase", "/b"};
    CHECK(cmd == want);
    t.bios = "/x/openbios.bin";
    cmd = editor::reduxCommand(t, "/b");
    CHECK(cmd.size() == want.size() + 2 && cmd[5] == "-bios" && cmd[6] == "/x/openbios.bin");
}

// File menu: a new project on disk, Save As inside it, and the recent list.
void testProject() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "project";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "My Game";

    editor::NewProject made = editor::createProject(proj);
    CHECK(made.error.empty());
    CHECK(made.scene == fs::path("My Game.scene"));
    CHECK(fs::is_directory(proj / "meshes") && fs::is_directory(proj / "textures") && fs::is_directory(proj / "scripts"));
    CHECK(fs::is_regular_file(proj / made.scene));
    CHECK(editor::firstScene(proj) == made.scene);

    // The new scene loads, empty, and is not dirty.
    editor::Document doc;
    CHECK(!doc.load(proj, made.scene));
    CHECK(doc.objectCount() == 0 && !doc.dirty());

    // A folder that already holds a scene is never overwritten.
    CHECK(doc.insertObject({0}, named("Crate")));
    CHECK(!doc.save());
    editor::NewProject again = editor::createProject(proj);
    CHECK(!again.error.empty() && again.scene.empty());
    editor::Document reread;
    CHECK(!reread.load(proj, made.scene) && reread.objectCount() == 1);

    // Save As: inside the project only, with .scene added when missing.
    CHECK(editor::sceneInProject(proj, proj / "level2") == fs::path("level2.scene"));
    CHECK(editor::sceneInProject(proj, proj / "levels" / "a.scene") == fs::path("levels") / "a.scene");
    CHECK(!editor::sceneInProject(proj, dir / "outside.scene"));
    CHECK(!editor::sceneInProject(proj, proj / ".." / "escape.scene"));
    CHECK(!editor::sceneInProject({}, proj / "a.scene"));
    CHECK(!doc.saveAs("level2.scene"));
    CHECK(doc.sceneStem() == "level2" && doc.scenePath() == proj / "level2.scene" && !doc.dirty());
    CHECK(fs::is_regular_file(proj / "level2.scene"));
    editor::Document level2;
    CHECK(!level2.load(proj, "level2.scene") && level2.objectCount() == 1);
    // A failed Save As keeps the document on its old file.
    fs::create_directories(proj / "blocked.scene", ec);
    CHECK(doc.saveAs("blocked.scene").has_value());
    CHECK(doc.sceneStem() == "level2");

    // Recent scenes: newest first, no duplicates, capped, persisted.
    fs::path cfg = dir / "recent.cfg";
    {
        editor::RecentScenes r(cfg);
        r.load();
        CHECK(r.items().empty());
        for (int i = 0; i < 10; ++i) r.add(proj / ("s" + std::to_string(i) + ".scene"));
        CHECK(r.items().size() == editor::RecentScenes::kMax);
        CHECK(r.items().front() == proj / "s9.scene");
        r.add(proj / "s5.scene");
        CHECK(r.items().front() == proj / "s5.scene" && r.items().size() == editor::RecentScenes::kMax);
        CHECK(std::count(r.items().begin(), r.items().end(), proj / "s5.scene") == 1);
        CHECK(r.items().back() == proj / "s2.scene");
        r.remove(proj / "s9.scene");
        CHECK(r.items().size() == editor::RecentScenes::kMax - 1);
    }
    editor::RecentScenes back(cfg);
    back.load();
    CHECK(back.items().size() == editor::RecentScenes::kMax - 1 && back.items().front() == proj / "s5.scene");
    CHECK(std::find(back.items().begin(), back.items().end(), proj / "s9.scene") == back.items().end());
}

// New script: the file name from the object's or scene's name, numbered
// rather than overwriting, and a template that compiles for the PS1.
void testNewScript() {
    namespace fs = std::filesystem;
    std::error_code ec;
    CHECK(editor::scriptFileStem("Game Logic") == "game_logic");
    CHECK(editor::scriptFileStem("  Door (2)!") == "door_2");
    CHECK(editor::scriptFileStem("Crate--Big__one") == "crate_big_one");
    CHECK(editor::scriptFileStem("\xc3\xa9t\xc3\xa9") == "t");
    CHECK(editor::scriptFileStem("***") == "script");
    CHECK(editor::scriptFileStem("") == "script");

    const std::string object = editor::scriptTemplate("Door", false), scene = editor::scriptTemplate("courtyard", true);
    CHECK(object.find("function onCreate(self)") != std::string::npos);
    CHECK(scene.find("function onSceneCreationStart()") != std::string::npos);
    CHECK(scene.find("function onSceneCreationEnd()") != std::string::npos);
    CHECK(editor::lua::check(object).ok() && editor::lua::check(scene).ok());
    CHECK(editor::lua::check(editor::scriptTemplate("two\nlines", false)).ok());

    fs::path proj = g_outDir / "newscript";
    fs::remove_all(proj, ec);
    fs::create_directories(proj, ec);
    editor::NewScript a = editor::createScript(proj, "Game Logic", false);
    CHECK(a.error.empty() && a.path == "scripts/game_logic.lua");
    std::ifstream in(proj / "scripts" / "game_logic.lua", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == editor::scriptTemplate("Game Logic", false));
    // Taken: numbered, the first left alone.
    std::ofstream(proj / "scripts" / "game_logic.lua", std::ios::binary) << "mine";
    editor::NewScript b = editor::createScript(proj, "Game Logic", false);
    CHECK(b.error.empty() && b.path == "scripts/game_logic_2.lua");
    std::ifstream mine(proj / "scripts" / "game_logic.lua", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(mine)), {}) == "mine");
    CHECK(editor::createScript(proj, "Game Logic", true).path == "scripts/game_logic_3.lua");
    CHECK(!editor::createScript({}, "x", false).error.empty());
    // A Trigger's script: its own file, the trigger callbacks.
    const std::string trig = editor::scriptTemplate("Door", editor::ScriptKind::Trigger);
    CHECK(trig.find("function onTriggerEnter(index)") != std::string::npos && trig.find("function onTriggerExit(index)") != std::string::npos);
    // The template and the editor's API agree, with the index psxsplash
    // passes a Trigger's script (Lua::OnTriggerEnterScript pushes it).
    for (const char* cb : {"onTriggerEnter", "onTriggerExit"}) {
        const editor::lua::ApiSymbol* api = editor::lua::findApi("", cb);
        CHECK(api && api->kind == editor::lua::ApiKind::Callback && std::string(api->signature) == std::string("function ") + cb + "(index)");
        CHECK(api && trig.find(api->signature) != std::string::npos);
    }
    CHECK(editor::lua::check(trig).ok());
    editor::NewScript t = editor::createScript(proj, "Game Logic", editor::ScriptKind::Trigger);
    CHECK(t.error.empty() && t.path == "scripts/game_logic_trigger.lua");
    std::ifstream tin(proj / "scripts" / "game_logic_trigger.lua", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(tin)), {}) == editor::scriptTemplate("Game Logic", editor::ScriptKind::Trigger));
}

void testAdoptAsset() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "adopt";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game", outside = dir / "elsewhere";
    fs::create_directories(proj / "textures", ec);
    fs::create_directories(outside, ec);
    auto write = [](const fs::path& p, const std::string& bytes) { std::ofstream(p, std::ios::binary) << bytes; };
    auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };

    // Inside the project: used where it is, never copied.
    write(proj / "textures" / "rock.png", "rock");
    editor::AdoptedAsset in = editor::adoptAsset(proj, proj / "textures" / "rock.png", "textures");
    CHECK(in.error.empty() && in.path == "textures/rock.png" && !in.copied);

    // Outside: copied into the folder, created if missing.
    write(outside / "door.lua", "print(1)");
    editor::AdoptedAsset lua = editor::adoptAsset(proj, outside / "door.lua", "scripts");
    CHECK(lua.error.empty() && lua.path == "scripts/door.lua" && lua.copied);
    CHECK(read(proj / "scripts" / "door.lua") == "print(1)");
    // The same file again reuses the copy.
    editor::AdoptedAsset again = editor::adoptAsset(proj, outside / "door.lua", "scripts");
    CHECK(again.path == "scripts/door.lua" && !again.copied);
    // A different file of the same name is numbered, never overwrites.
    fs::create_directories(outside / "b", ec);
    write(outside / "b" / "door.lua", "print(2)");
    editor::AdoptedAsset other = editor::adoptAsset(proj, outside / "b" / "door.lua", "scripts");
    CHECK(other.path == "scripts/door_2.lua" && other.copied);
    CHECK(read(proj / "scripts" / "door.lua") == "print(1)" && read(proj / "scripts" / "door_2.lua") == "print(2)");

    // Failures carry a reason and no path.
    editor::AdoptedAsset missing = editor::adoptAsset(proj, outside / "nope.wav", "audio");
    CHECK(!missing.error.empty() && missing.path.empty());
    editor::AdoptedAsset noProject = editor::adoptAsset({}, outside / "door.lua", "scripts");
    CHECK(!noProject.error.empty() && noProject.path.empty());

    // Every file field is checked on disk: script, trigger script, audio clip,
    // skin clips. Picking a file refreshes its entry.
    write(proj / "scripts" / "zone.lua", "");
    fs::create_directories(proj / "audio", ec);
    write(proj / "audio" / "bell.wav", "RIFF");
    splash::Scene sc;
    splash::Object ob = named("Thing");
    ob.trigger.emplace();
    ob.trigger->lua = "scripts/zone.lua";
    ob.audio.emplace();
    ob.audio->clip = "audio/bell.wav";
    ob.skin.emplace();
    ob.skin->clips = {"anims/walk.anim"};
    sc.objects.push_back(ob);
    editor::Document doc;
    doc.reset(sc, proj, "game");
    CHECK(doc.fileExists("scripts/zone.lua") && doc.fileExists("audio/bell.wav"));
    CHECK(!doc.fileExists("anims/walk.anim") && !doc.fileExists("scripts/new.lua"));
    fs::create_directories(proj / "anims", ec);
    write(proj / "anims" / "walk.anim", "x");
    write(proj / "scripts" / "new.lua", "");
    doc.refreshAssets({"anims/walk.anim", "scripts/new.lua"});
    CHECK(doc.fileExists("anims/walk.anim") && doc.fileExists("scripts/new.lua"));
}

// Dropping a model: the file's node tree becomes objects in one undo step,
// the file is adopted into models/, and composing the objects' transforms
// over the per-node meshes lands every vertex where the baked mesh has it.
void testPlaceModel() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "placemodel";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game", outside = dir / "downloads";
    fs::create_directories(proj, ec);
    fs::create_directories(outside, ec);
    // Body (moved 1 along X) holding Arm (moved, turned 90 degrees about Y),
    // each drawing the triangle (0,0,0) (1,0,0) (0,1,0).
    std::ofstream(outside / "robot.gltf") << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Body","mesh":0,"translation":[1,0,0],"children":[1]},
 {"name":"Arm","mesh":0,"translation":[0.5,1,2],"rotation":[0,0.70710678,0,0.70710678]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
"bufferViews":[{"buffer":0,"byteLength":36}],
"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    std::ofstream(outside / "tri.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    splash::Scene sc;
    splash::Object a = named("A");
    a.mesh.emplace();
    sc.objects.push_back(a);
    editor::Document doc;
    doc.reset(sc, proj, "game");

    // On the viewport: a new top-level object at the drop point.
    editor::PlacedModel p = editor::placeModel(doc, outside / "robot.gltf", std::nullopt, {1}, {4, 5, 6});
    CHECK(p.error.empty() && p.path == editor::ObjectPath{1});
    CHECK(doc.historySize() == 1 && doc.scene().objects.size() == 2);
    const splash::Object* root = doc.object({1});
    CHECK(root && root->name == "robot" && !root->mesh && root->children.size() == 1);
    CHECK(root && near(root->transform.position.x, 4) && near(root->transform.position.y, 5) && near(root->transform.position.z, 6));
    const splash::Object* body = doc.object({1, 0});
    const splash::Object* arm = doc.object({1, 0, 0});
    CHECK(body && body->name == "Body" && body->mesh && body->mesh->mesh == "models/robot/Body.mesh" && body->mesh->materials.size() == 1);
    CHECK(body && near(body->transform.position.x, -1));
    CHECK(arm && arm->name == "Arm" && arm->mesh && arm->mesh->mesh == "models/robot/Arm.mesh" && arm->children.empty());
    CHECK(arm && near(arm->transform.position.x, -0.5f) && near(arm->transform.position.y, 1) && near(arm->transform.position.z, 2));
    CHECK(arm && near(arm->transform.rotation.y, -0.70710678f) && near(arm->transform.rotation.w, 0.70710678f));
    CHECK(fs::is_regular_file(proj / "models" / "robot.gltf"));  // adopted
    const editor::MeshInfo* mi = doc.mesh("models/robot/Arm.mesh");
    CHECK(mi && mi->status == editor::AssetStatus::Ok && mi->triangles == 1);
    // The editor's own composition over the node meshes gives the baked mesh, placed.
    {
        splash::Mesh baked = splash::loadMesh(proj / "models" / "robot.mesh");
        std::vector<splash::Vec3> got;
        splash::Mat34 rootWorld;
        for (const splash::FlatObject& fo : splash::flatten(doc.scene())) {
            if (fo.object == root) rootWorld = fo.localToWorld;
            if (!fo.object->mesh || fo.object == doc.object({0})) continue;
            for (const splash::Vec3& v : splash::loadMesh(proj / fo.object->mesh->mesh).positions) got.push_back(fo.localToWorld.point(v));
        }
        bool same = got.size() == baked.positions.size() && got.size() == 6;
        for (size_t i = 0; same && i < got.size(); ++i) {
            splash::Vec3 w = rootWorld.point(baked.positions[i]);
            same = std::fabs(w.x - got[i].x) < 1e-4f && std::fabs(w.y - got[i].y) < 1e-4f && std::fabs(w.z - got[i].z) < 1e-4f;
        }
        CHECK(same);
    }
    // One undo takes the whole tree away, redo brings it back.
    CHECK(doc.undo());
    CHECK(doc.scene().objects.size() == 1 && !doc.canUndo());
    CHECK(doc.redo());
    CHECK(doc.object({1, 0, 0}) && doc.object({1, 0, 0})->name == "Arm");

    // On a scene tree row: a child of that row's object, at its origin.
    p = editor::placeModel(doc, outside / "robot.gltf", std::nullopt, {0, 0}, {0, 0, 0});
    CHECK(p.error.empty() && doc.historySize() == 2);
    CHECK(doc.object({0, 0}) && doc.object({0, 0})->name == "robot" && doc.object({0, 0, 0, 0}) &&
          doc.object({0, 0, 0, 0})->name == "Arm");
    CHECK(doc.undo() && doc.object({0})->children.empty());

    // A second drop at the top level is named apart from the first.
    p = editor::placeModel(doc, outside / "robot.gltf", std::nullopt, {2}, {0, 0, 0});
    CHECK(doc.object({2}) && doc.object({2})->name == "robot (2)");
    CHECK(doc.undo());

    // On the Model field: the object's model is replaced by the baked mesh.
    p = editor::placeModel(doc, outside / "robot.gltf", editor::ObjectPath{0}, {}, {});
    CHECK(p.error.empty() && doc.object({0})->mesh->mesh == "models/robot.mesh" && doc.object({0})->children.empty());
    CHECK(doc.undo() && doc.object({0})->mesh->mesh.empty());

    // A model of one node is one object drawing the baked mesh.
    p = editor::placeModel(doc, outside / "tri.obj", std::nullopt, {2}, {0, 0, 0});
    CHECK(p.error.empty() && doc.object({2}) && doc.object({2})->mesh && doc.object({2})->mesh->mesh == "models/tri.mesh" &&
          doc.object({2})->children.empty());

    // A file that does not import changes nothing.
    std::ofstream(outside / "junk.glb") << "junk";
    size_t before = doc.historySize();
    p = editor::placeModel(doc, outside / "junk.glb", std::nullopt, {3}, {0, 0, 0});
    CHECK(!p.error.empty() && doc.historySize() == before && !fs::exists(proj / "models" / "junk.glb"));
}

// Dropping a skinned model (tests/skinimport/rig.gltf): the object drawing the
// skinned mesh gets a skin component playing the model's clips, the scene
// exports with it, and a reimport keeps a clip added by hand.
void testPlaceSkinnedModel() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "placeskinned";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game";
    fs::create_directories(proj, ec);
    const fs::path rig = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skinimport" / "rig.gltf";
    editor::Document doc;
    doc.reset(splash::Scene{}, proj, "game");

    editor::PlacedModel p = editor::placeModel(doc, rig, std::nullopt, {0}, {0, 0, 0});
    CHECK(p.error.empty() && p.model.clips.size() == 2);
    const splash::Object* root = doc.object({0});
    CHECK(root && root->name == "rig" && root->children.size() == 1 && !root->skin);
    // Armature holding Upper (holding Lower) and Body, the skinned mesh.
    const splash::Object* arm = doc.object({0, 0});
    CHECK(arm && arm->name == "Armature" && arm->children.size() == 2);
    const splash::Object* body = doc.object({0, 0, 1});
    CHECK(body && body->name == "Body" && body->mesh && body->mesh->mesh == "models/rig/Body.mesh");
    CHECK(body && body->skin &&
          body->skin->clips == std::vector<std::string>({"models/rig/Wave.anim", "models/rig/Nod_Big.anim"}));
    const splash::Object* upper = doc.object({0, 0, 0});
    CHECK(upper && upper->name == "Upper" && !upper->mesh && !upper->skin);
    CHECK(doc.fileExists("models/rig/Wave.anim") && doc.fileExists("models/rig/Nod_Big.anim"));
    bool warned = false;
    for (const std::string& w : p.model.warnings) warned |= w.find("the heaviest joint wins") != std::string::npos;
    CHECK(warned);

    // The scene exports with the skin: one skinned mesh, no errors.
    splash::ExportResult r = splash::exportSplashpack(doc.scene(), proj, dir / "scene.splashpack");
    CHECK(r.ok());
    for (const std::string& e : r.errors) std::fprintf(stderr, "  export error: %s\n", e.c_str());

    // Merged: the root object draws the baked mesh and holds the skin.
    splash::ImportSettings merged;
    merged.mergeMeshes = true;
    splash::Object m = editor::modelObject(p.model, "rig", merged);
    CHECK(m.mesh && m.mesh->mesh == "models/rig.mesh" && m.skin && m.skin->clips.size() == 2);

    // A clip added by hand stays through a reimport; the model's are rewritten.
    doc.edit({0, 0, 1}, [](splash::Object& o) {
        if (o.skin) o.skin->clips.push_back("anims/extra.anim");
    });
    editor::Reimported re = editor::reimportModel(doc, proj / "models" / "rig.gltf");
    CHECK(re.error.empty());
    body = doc.object({0, 0, 1});
    CHECK(body && body->skin && body->skin->clips.size() == 3 && body->skin->clips[2] == "anims/extra.anim");
    // A skin taken off by hand stays off.
    doc.edit({0, 0, 1}, [](splash::Object& o) { o.skin.reset(); });
    re = editor::reimportModel(doc, proj / "models" / "rig.gltf");
    body = doc.object({0, 0, 1});
    CHECK(re.error.empty() && body && !body->skin);

    // Merge meshes turned on: the instance becomes one object with the baked
    // mesh and a skin; off again: the node objects, the skin back on Body.
    splash::ImportSettings st = splash::loadImportSettings(proj / "models" / "rig.gltf");
    st.mergeMeshes = true;
    splash::saveImportSettings(proj / "models" / "rig.gltf", st);
    re = editor::reimportModel(doc, proj / "models" / "rig.gltf", false);
    root = doc.object({0});
    CHECK(re.error.empty() && root && root->children.empty() && root->mesh && root->mesh->mesh == "models/rig.mesh" &&
          root->skin && root->skin->clips.size() == 2);
    st.mergeMeshes = false;
    splash::saveImportSettings(proj / "models" / "rig.gltf", st);
    re = editor::reimportModel(doc, proj / "models" / "rig.gltf", true);
    root = doc.object({0});
    body = doc.object({0, 0, 1});
    CHECK(re.error.empty() && root && !root->mesh && !root->skin && body && body->skin && body->skin->clips.size() == 2);
}

// A robot like testPlaceModel's: Body (moved 1 along X) holding Arm, each
// drawing one triangle, with a material of the given colour.
std::string robotGltf(float red, bool twoTriangles = false) {
    const char* buf = twoTriangles ? "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAIA/AAAAAAAAgD8AAIA/"
                                   : "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA";
    const int n = twoTriangles ? 6 : 3;
    return std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Body","mesh":0,"translation":[1,0,0],"children":[1]},
 {"name":"Arm","mesh":0,"translation":[0.5,1,2],"rotation":[0,0.70710678,0,0.70710678]}],
"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[)") +
           std::to_string(red) + R"(,1,1,1]}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":)" + std::to_string(n) +
           R"(,"type":"VEC3","min":[0,0,0],"max":[1,1,)" + (twoTriangles ? "1" : "0") + R"(]}],
"bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(n * 12) + R"(}],
"buffers":[{"byteLength":)" + std::to_string(n * 12) + R"(,"uri":"data:application/octet-stream;base64,)" + buf + R"("}]})";
}

void testImportSettings() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "importsettings";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game", outside = dir / "downloads";
    fs::create_directories(proj / "models", ec);
    fs::create_directories(outside, ec);

    // The sidecar: defaults without one, every field and an unknown key kept.
    fs::path model = proj / "models" / "crate.fbx";
    CHECK(splash::importSettingsPath(model) == proj / "models" / "crate.fbx.import.json");
    splash::ImportSettings d = splash::loadImportSettings(model);
    CHECK(d.scale == 1 && d.up == splash::UpAxis::File && d.normals == splash::Normals::Import && d.smoothingAngle == 60 &&
          !d.mergeMeshes && d.bitDepth == splash::MeshComponent{}.bitDepth && d.lighting == splash::MeshComponent{}.dynamicLighting &&
          d.source.empty());
    std::ofstream(splash::importSettingsPath(model)) << R"({"scale": 0.5, "up": "z", "later": [1, 2]})";
    splash::ImportSettings s = splash::loadImportSettings(model);
    CHECK(s.scale == 0.5f && s.up == splash::UpAxis::ZUp && s.extra.size() == 1 && s.extra[0].first == "later");
    s.normals = splash::Normals::Recompute;
    s.smoothingAngle = 30;
    s.mergeMeshes = true;
    s.bitDepth = splash::BitDepth::Bpp4;
    s.lighting = splash::DynamicLighting::Off;
    s.source = "/somewhere/crate.fbx";
    splash::saveImportSettings(model, s);
    splash::ImportSettings back = splash::loadImportSettings(model);
    CHECK(back.scale == 0.5f && back.up == splash::UpAxis::ZUp && back.normals == splash::Normals::Recompute &&
          back.smoothingAngle == 30 && back.mergeMeshes && back.bitDepth == splash::BitDepth::Bpp4 &&
          back.lighting == splash::DynamicLighting::Off && back.source == "/somewhere/crate.fbx");
    CHECK(back.extra.size() == 1 && back.extra[0].first == "later" && back.extra[0].second == "[1,2]");
    std::ofstream(splash::importSettingsPath(model)) << R"({"up": "sideways"})";
    bool threw = false;
    try {
        splash::loadImportSettings(model);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("crate.fbx.import.json") != std::string::npos;
    }
    CHECK(threw);
    fs::remove(splash::importSettingsPath(model), ec);

    // modelObject: the settings' bit depth and lighting on every mesh
    // component; merged, one object drawing the baked mesh.
    splash::ImportedModel two;
    two.mesh = "models/two.mesh";
    two.materials.resize(1);
    two.nodes.resize(2);
    two.nodes[0].name = "P";
    two.nodes[0].mesh = "models/two/P.mesh";
    two.nodes[0].materials.resize(1);
    two.nodes[1].name = "Q";
    two.nodes[1].parent = 0;
    two.nodes[1].mesh = "models/two/Q.mesh";
    two.nodes[1].materials.resize(1);
    splash::ImportSettings ms;
    ms.bitDepth = splash::BitDepth::Bpp16;
    ms.lighting = splash::DynamicLighting::Smooth;
    splash::Object tree = editor::modelObject(two, "two", ms);
    CHECK(!tree.mesh && tree.children.size() == 1 && tree.children[0].children.size() == 1);
    CHECK(tree.children.size() == 1 && tree.children[0].mesh && tree.children[0].mesh->bitDepth == splash::BitDepth::Bpp16 &&
          tree.children[0].mesh->dynamicLighting == splash::DynamicLighting::Smooth);
    CHECK(tree.children.size() == 1 && tree.children[0].children.size() == 1 && tree.children[0].children[0].mesh &&
          tree.children[0].children[0].mesh->bitDepth == splash::BitDepth::Bpp16);
    ms.mergeMeshes = true;
    splash::Object merged = editor::modelObject(two, "two", ms);
    CHECK(merged.children.empty() && merged.mesh && merged.mesh->mesh == "models/two.mesh" &&
          merged.mesh->bitDepth == splash::BitDepth::Bpp16 && merged.mesh->dynamicLighting == splash::DynamicLighting::Smooth);

    // placeModel writes the sidecar: defaults, and where the file came from.
    std::ofstream(outside / "robot.gltf") << robotGltf(0.5f);
    editor::Document doc;
    doc.reset(splash::Scene{}, proj, "game");
    editor::PlacedModel p = editor::placeModel(doc, outside / "robot.gltf", std::nullopt, {0}, {0, 0, 0});
    const fs::path robot = proj / "models" / "robot.gltf";
    CHECK(p.error.empty() && fs::is_regular_file(splash::importSettingsPath(robot)));
    splash::ImportSettings rs = splash::loadImportSettings(robot);
    CHECK(rs.scale == 1 && !rs.mergeMeshes && rs.source == std::string(reinterpret_cast<const char*>(fs::absolute(outside / "robot.gltf").u8string().c_str())));
    // A model already in the project has no source.
    fs::create_directories(proj / "art", ec);
    std::ofstream(proj / "art" / "tri.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    p = editor::placeModel(doc, proj / "art" / "tri.obj", std::nullopt, {1}, {0, 0, 0});
    CHECK(p.error.empty() && fs::is_regular_file(splash::importSettingsPath(proj / "art" / "tri.obj")) &&
          splash::loadImportSettings(proj / "art" / "tri.obj").source.empty());
    // The mesh paths lead back to the model.
    CHECK(editor::modelOfMesh(proj, "models/robot.mesh") == robot);
    CHECK(editor::modelOfMesh(proj, "models/robot/Arm.mesh") == robot);
    CHECK(editor::modelOfMesh(proj, "meshes/robot.mesh").empty() && editor::modelOfMesh(proj, "models/other.mesh").empty());

    // Reimport with scale 2: same paths, node translations doubled, the baked
    // mesh doubled on disk, one undo step for the scene change.
    const splash::Object* body = doc.object({0, 0});
    const splash::Object* arm = doc.object({0, 0, 0});
    CHECK(body && arm && near(body->transform.position.x, -1) && near(arm->transform.position.y, 1));
    CHECK(body && body->mesh && body->mesh->materials.size() == 1 && near(body->mesh->materials[0].color[0], 0.5f));
    const float bakedY = splash::loadMesh(proj / "models" / "robot.mesh").positions[5].y;
    rs.scale = 2;
    splash::saveImportSettings(robot, rs);
    size_t steps = doc.historySize();
    unsigned assets = doc.assetsId();
    editor::Reimported re = editor::reimportModel(doc, robot);
    body = doc.object({0, 0});
    arm = doc.object({0, 0, 0});
    CHECK(re.error.empty() && re.objects == 1 && doc.historySize() == steps + 1 && doc.assetsId() != assets);
    CHECK(body && body->mesh && body->mesh->mesh == "models/robot/Body.mesh" && near(body->transform.position.x, -2));
    CHECK(arm && arm->mesh && arm->mesh->mesh == "models/robot/Arm.mesh" && near(arm->transform.position.x, -1) &&
          near(arm->transform.position.y, 2) && near(arm->transform.position.z, 4));
    CHECK(near(splash::loadMesh(proj / "models" / "robot.mesh").positions[5].y, 2 * bakedY));
    CHECK(near(splash::loadMesh(proj / "models" / "robot" / "Arm.mesh").positions[2].y, 2));
    CHECK(doc.undo() && doc.object({0, 0}) && near(doc.object({0, 0})->transform.position.x, -1));
    CHECK(doc.redo());

    // Merge meshes turned on: the instance becomes one object drawing the
    // baked mesh with the settings' bit depth; off again, the tree is back.
    rs.mergeMeshes = true;
    rs.bitDepth = splash::BitDepth::Bpp4;
    splash::saveImportSettings(robot, rs);
    steps = doc.historySize();
    re = editor::reimportModel(doc, robot, false);
    const splash::Object* top = doc.object({0});
    CHECK(re.error.empty() && doc.historySize() == steps + 1);
    CHECK(top && top->name == "robot" && top->children.empty() && top->mesh && top->mesh->mesh == "models/robot.mesh" &&
          top->mesh->bitDepth == splash::BitDepth::Bpp4);
    rs.mergeMeshes = false;
    splash::saveImportSettings(robot, rs);
    re = editor::reimportModel(doc, robot, true);
    top = doc.object({0});
    CHECK(top && !top->mesh && top->children.size() == 1 && doc.object({0, 0, 0}) && doc.object({0, 0, 0})->name == "Arm" &&
          doc.object({0, 0, 0})->mesh->bitDepth == splash::BitDepth::Bpp4 && near(doc.object({0, 0, 0})->transform.position.y, 2));
    // Without wasMerged nothing is restructured, even when the setting differs.
    rs.mergeMeshes = true;
    splash::saveImportSettings(robot, rs);
    re = editor::reimportModel(doc, robot);
    CHECK(re.error.empty() && doc.object({0}) && !doc.object({0})->mesh && doc.object({0})->children.size() == 1);
    rs.mergeMeshes = false;
    splash::saveImportSettings(robot, rs);

    // Recomputed normals are written to the same paths and reach the views.
    rs.normals = splash::Normals::Recompute;
    splash::saveImportSettings(robot, rs);
    assets = doc.assetsId();
    re = editor::reimportModel(doc, robot);
    CHECK(re.error.empty() && doc.assetsId() != assets && doc.object({0, 0})->mesh->mesh == "models/robot/Body.mesh");

    // The watcher: the first poll only notes; a touched model is reported once.
    editor::ModelWatcher watch;
    CHECK(watch.poll(proj).empty());
    CHECK(watch.poll(proj).empty());
    fs::last_write_time(robot, fs::last_write_time(robot) + std::chrono::seconds(5));
    std::vector<fs::path> hits = watch.poll(proj);
    CHECK(hits.size() == 1 && hits[0] == robot);
    CHECK(watch.poll(proj).empty());
    // The source changes outside the project (a second triangle, a new
    // colour): copied over the model, which is reported; reimported, the
    // references stay and the triangle count and materials follow.
    const fs::path src = outside / "robot.gltf";
    std::ofstream(src) << robotGltf(0.25f, true);
    fs::last_write_time(src, fs::last_write_time(src) + std::chrono::seconds(10));
    hits = watch.poll(proj);
    CHECK(hits.size() == 1 && hits[0] == robot);
    {
        std::ifstream a(robot, std::ios::binary), b(src, std::ios::binary);
        std::string sa((std::istreambuf_iterator<char>(a)), {}), sb((std::istreambuf_iterator<char>(b)), {});
        CHECK(sa == sb);
    }
    CHECK(watch.poll(proj).empty());
    for (const fs::path& h : hits) re = editor::reimportModel(doc, h);
    body = doc.object({0, 0});
    CHECK(re.error.empty() && re.model.triangles == 4);
    CHECK(body && body->mesh && body->mesh->mesh == "models/robot/Body.mesh" && body->mesh->materials.size() == 1 &&
          near(body->mesh->materials[0].color[0], 0.25f));
    const editor::MeshInfo* mi = doc.mesh("models/robot/Body.mesh");
    CHECK(mi && mi->status == editor::AssetStatus::Ok && mi->triangles == 2);
    // Saving the sidecar alone is not a change of the model.
    splash::saveImportSettings(robot, rs);
    CHECK(watch.poll(proj).empty());
}

}  // namespace

// Reparenting keeps the object where it is in the world, follows the paths
// of everything else, and undoes in one step.
void testMove() {
    auto worldOf = [](const editor::Document& d, const std::string& name) {
        for (const splash::FlatObject& fo : splash::flatten(d.scene()))
            if (fo.object->name == name) return fo;
        return splash::FlatObject{};
    };
    auto same = [](splash::Vec3 a, splash::Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); };
    editor::Document d;
    splash::Scene s = sample();
    // B: moved, turned 90 degrees around Y and scaled 2; C sits elsewhere.
    s.objects[1].transform.position = {10, 0, 0};
    s.objects[1].transform.rotation = {0, 0.70710678f, 0, 0.70710678f};
    s.objects[1].transform.scale = {2, 2, 2};
    s.objects[1].children[0].transform.position = {1, 2, 3};
    s.objects[2].transform.position = {5, 1, -2};
    d.reset(s);
    const splash::FlatObject c0 = worldOf(d, "C");
    const splash::FlatObject b10 = worldOf(d, "B1");

    // C into B: becomes B's second child, same world position.
    d.select(editor::ObjectPath{0});
    d.pin(editor::ObjectPath{0});
    auto to = d.moveObject({2}, {1}, 1);
    CHECK(to && *to == editor::ObjectPath({1, 1}));
    CHECK(d.scene().objects.size() == 2 && d.object({1, 1}) && d.object({1, 1})->name == "C");
    CHECK(same(worldOf(d, "C").worldPosition, c0.worldPosition));
    CHECK(same(worldOf(d, "C").lossyScale, c0.lossyScale));
    {
        // Same rotation (q and -q are the same turn).
        splash::Quat a = worldOf(d, "C").worldRotation, b = c0.worldRotation;
        CHECK(near(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w), 1));
    }
    CHECK(d.object({1, 1})->transform.scale == splash::Vec3({0.5f, 0.5f, 0.5f}));
    CHECK(d.selected() && d.selected()->name == "C");
    CHECK(d.pinned() && d.object(*d.pinned())->name == "A");  // the pin stays on A
    CHECK(d.undo());
    CHECK(d.object({2}) && d.object({2})->name == "C" && d.object({1})->children.size() == 1);
    CHECK(d.object({2})->transform.position == splash::Vec3({5, 1, -2}));
    CHECK(d.redo());
    CHECK(d.object({1, 1})->name == "C");
    CHECK(d.undo());

    // B1 out to the root, before A: world position kept, B loses its child.
    auto out = d.moveObject({1, 0}, {}, 0);
    CHECK(out && *out == editor::ObjectPath({0}));
    CHECK(d.object({0})->name == "B1" && d.object({2})->name == "B" && d.object({2})->children.empty());
    CHECK(same(worldOf(d, "B1").worldPosition, b10.worldPosition));
    CHECK(same(d.object({0})->transform.position, b10.worldPosition));
    CHECK(d.pinned() && d.object(*d.pinned())->name == "A");  // A moved to index 1, the pin followed
    CHECK(d.undo());
    CHECK(d.object({1, 0}) && d.object({1, 0})->name == "B1");
    CHECK(d.object({1, 0})->transform.position == splash::Vec3({1, 2, 3}));

    // Reorder among siblings: A after C (index counts A still in place).
    auto re = d.moveObject({0}, {}, 3);
    CHECK(re && *re == editor::ObjectPath({2}));
    CHECK(d.object({0})->name == "B" && d.object({2})->name == "A");
    CHECK(d.pinned() && *d.pinned() == editor::ObjectPath({2}));  // the pinned object itself moved
    CHECK(d.undo());

    // Refused: into itself, into its own child, no-op, bad paths.
    size_t steps = d.historySize();
    CHECK(!d.moveObject({1}, {1}, 0));
    CHECK(!d.moveObject({1}, {1, 0}, 0));
    CHECK(!d.moveObject({1}, {}, 1));
    CHECK(!d.moveObject({1}, {}, 2));  // after itself = where it is
    CHECK(!d.moveObject({9}, {}, 0));
    CHECK(!d.moveObject({0}, {7}, 0));
    CHECK(d.historySize() == steps);

    // Expand / collapse all.
    d.setAllExpanded(false);
    CHECK(!d.expanded({1}) && d.expanded({0}) && d.expanded({}));
    d.setAllExpanded(true);
    CHECK(d.expanded({1}));

    // Removing the pinned object clears the pin.
    d.pin(editor::ObjectPath{1});
    CHECK(d.removeObject({1}));
    CHECK(!d.pinned());
}

// ---- Run on hardware: the Unirom upload and PCdrv host against a simulated console.

struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint8_t> q;
    bool closed = false;
};

class PipeEnd : public editor::Link {
  public:
    PipeEnd(Pipe& in, Pipe& out) : in_(in), out_(out) {}
    bool write(const void* d, size_t n) override {
        std::lock_guard<std::mutex> lock(out_.m);
        if (out_.closed) return false;
        const uint8_t* p = static_cast<const uint8_t*>(d);
        out_.q.insert(out_.q.end(), p, p + n);
        out_.cv.notify_all();
        return true;
    }
    int read(void* d, size_t n, int timeoutMs) override {
        std::unique_lock<std::mutex> lock(in_.m);
        in_.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !in_.q.empty() || in_.closed; });
        if (in_.q.empty()) return in_.closed ? -1 : 0;
        size_t k = std::min(n, in_.q.size());
        std::copy(in_.q.begin(), in_.q.begin() + k, static_cast<uint8_t*>(d));
        in_.q.erase(in_.q.begin(), in_.q.begin() + k);
        return int(k);
    }
    void close() {
        std::lock_guard<std::mutex> lock(out_.m);
        out_.closed = true;
        out_.cv.notify_all();
    }

  private:
    Pipe& in_;
    Pipe& out_;
};

// The console's half of the protocol, as Unirom (sio.c, kdebug.c) speaks it.
struct FakeConsole {
    PipeEnd& link;
    int offer;  // protocol version Unirom offers: 1 (none), 2 or 3
    std::vector<std::string> problems;
    std::vector<uint8_t> program;
    uint32_t dest = 0, entry = 0;

    void fail(const std::string& s) { problems.push_back(s); }
    bool get(void* p, size_t n) {
        uint8_t* o = static_cast<uint8_t*>(p);
        while (n) {
            int r = link.read(o, n, 3000);
            if (r <= 0) {
                fail("console: host went quiet");
                return false;
            }
            o += r;
            n -= size_t(r);
        }
        return true;
    }
    uint32_t u32() {
        uint8_t b[4] = {};
        get(b, 4);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
    void put(const void* p, size_t n) { link.write(p, n); }
    void say(const char* s) { put(s, std::strlen(s)); }
    void put32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        put(b, 4);
    }
    // The shell matches the last four bytes it read against its commands and
    // echoes a match, so anything else in front (a psxmon PING) is skipped.
    std::string command() {
        char c[5] = {};
        for (int have = 0;;) {
            uint8_t b;
            if (!get(&b, 1)) return {};
            std::memmove(c, c + 1, 3);
            c[3] = char(b);
            if (++have < 4) continue;
            for (const char* known : {"SEXE", "UPV2", "UPV3"})
                if (!std::memcmp(c, known, 4)) {
                    put(c, 4);
                    return c;
                }
        }
    }
    static uint32_t sum(const uint8_t* p, size_t n) {
        uint32_t s = 0;
        for (size_t i = 0; i < n; ++i) s += p[i];
        return s;
    }

    void shell() {
        std::string cmd = command();
        if (cmd != "SEXE") return fail("console: expected SEXE, got " + cmd);
        int version = 1;
        if (offer >= 2) {
            say(offer == 3 ? "OKV3" : "OKV2");
            std::string up = command();
            if (up != (offer == 3 ? "UPV3" : "UPV2")) return fail("console: upgrade answered with " + up);
            version = offer;
        }
        say("OKAY");
        std::vector<uint8_t> header(2048);
        if (!get(header.data(), header.size())) return;
        if (std::memcmp(header.data(), "PS-X EXE", 8)) fail("console: SEXE header is not the EXE header");
        entry = u32();
        dest = u32();
        const uint32_t len = u32(), check = u32();
        if (len % 2048) fail("console: SEXE length not padded");
        program.assign(len, 0);
        bool corrupted = false;
        for (uint32_t at = 0; at < len;) {
            if (!get(program.data() + at, 2048)) return;
            if (version >= 2) {
                say("CHEK");
                const uint32_t host = u32();
                if (host != sum(program.data() + at, 2048)) fail("console: wrong chunk checksum");
                if (!corrupted) {  // pretend the first chunk arrived damaged
                    corrupted = true;
                    say("ERR!");
                    continue;
                }
                say("MORE");
            }
            at += 2048;
        }
        uint32_t whole = 0;
        if (version == 3) {
            whole = 5381;
            for (uint8_t b : program) whole = ((whole << 5) + whole) ^ b;
        } else {
            whole = sum(program.data(), program.size());
        }
        if (whole != check) fail("console: wrong SEXE checksum");
    }

    // psxsplash's side of a file call (pcdrv_handler.hh): escape, call number, presence OKAY.
    bool okay() {  // sio_check_okay: stops reading at the first byte that does not match
        for (const char* w = "OKAY"; *w; ++w) {
            uint8_t c = 0;
            if (!get(&c, 1) || c != uint8_t(*w)) return false;
        }
        return true;
    }
    bool call(uint32_t op) {
        uint8_t esc[2] = {0, 'p'};
        put(esc, 2);
        put32(op);
        if (okay()) return true;
        fail("console: no presence ack");
        return false;
    }
    bool init() {
        if (!call(0x101)) return false;
        uint8_t z = 1;
        get(&z, 1);
        return z == 0;
    }
    bool open(const char* name, uint32_t* h) {
        if (!call(0x103)) return false;
        put(name, std::strlen(name) + 1);
        put32(0);
        if (!okay()) return false;
        *h = u32();
        return true;
    }
    std::vector<uint8_t> read(uint32_t h, uint32_t len) {
        if (!call(0x105)) return {};
        put32(h), put32(len), put32(0x80100000);
        if (!okay()) {
            fail("console: read refused");
            return {};
        }
        const uint32_t n = u32(), check = u32();
        std::vector<uint8_t> mem(n);
        get(mem.data(), n);
        if (check != sum(mem.data(), n)) fail("console: read checksum");
        return mem;
    }
    bool three(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t* out) {
        if (!call(op)) return false;
        put32(a), put32(b), put32(c);
        if (!okay()) return false;
        *out = u32();
        return true;
    }
};

void runUnirom(int offer) {
    namespace fs = std::filesystem;
    const fs::path base = g_outDir / ("unirom" + std::to_string(offer));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    std::vector<uint8_t> scene(3000);
    for (size_t i = 0; i < scene.size(); ++i) scene[i] = uint8_t(i * 7 + 1);
    {
        std::ofstream(base / "scene_0.splashpack", std::ios::binary).write(reinterpret_cast<const char*>(scene.data()), scene.size());
    }
    // A program of a bit over two chunks.
    std::vector<uint8_t> exe(0x800 + 5000);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    const uint32_t pc = 0x80010000, addr = 0x80010000;
    std::memcpy(&exe[0x10], &pc, 4);
    std::memcpy(&exe[0x18], &addr, 4);
    for (size_t i = 0x800; i < exe.size(); ++i) exe[i] = uint8_t(i * 13);

    Pipe toConsole, toHost;
    PipeEnd hostEnd(toHost, toConsole), consoleEnd(toConsole, toHost);
    FakeConsole con{consoleEnd, offer, {}, {}, 0, 0};
    std::vector<uint8_t> readBack, tail;
    uint32_t seekPos = 0, closed = 99;
    bool inited = false, escapeRefused = false, missingRefused = false, unsupportedRefused = false;
    std::thread console([&] {
        con.shell();
        con.say("psxsplash: boot\r\n");
        uint32_t h = 0, n = 0;
        inited = con.init();
        if (con.open("scene_0.splashpack", &h)) {
            readBack = con.read(h, 4096);  // more than the file holds
            con.three(0x107, h, 2990, 0, &seekPos);
            tail = con.read(h, 64);
            con.three(0x104, h, 0, 0, &closed);
        }
        escapeRefused = !con.open("../outside", &n);
        missingRefused = !con.open("missing.bin", &n);
        if (con.call(0x106)) {  // write: not served, the host answers NOPE
            con.put32(h), con.put32(4), con.put32(0x80100000);
            unsupportedRefused = !con.okay();
        }
        con.say("done\n");
        consoleEnd.close();
    });

    std::string err;
    std::vector<int> progress;
    CHECK(editor::uniromUpload(hostEnd, exe, &err, [&](int p) { progress.push_back(p); }));
    if (!err.empty()) std::fprintf(stderr, "upload: %s\n", err.c_str());
    std::vector<std::string> lines, events;
    std::atomic<bool> cancel{false};
    editor::PcdrvHost host(base);
    const bool clean = host.serve(hostEnd, cancel, [&](const std::string& l) { lines.push_back(l); },
                                  [&](const std::string& e) { events.push_back(e); }, &err);
    console.join();

    CHECK(!clean && err == "The connection closed.");  // the console hung up
    for (const std::string& p : con.problems) std::fprintf(stderr, "offer %d: %s\n", offer, p.c_str());
    CHECK(con.problems.empty());
    CHECK(con.dest == addr && con.entry == pc);
    CHECK(con.program.size() == 6144 && std::equal(exe.begin() + 0x800, exe.end(), con.program.begin()));
    CHECK(!progress.empty() && progress.back() == 100);
    CHECK(std::find(lines.begin(), lines.end(), "psxsplash: boot") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "done") != lines.end());
    CHECK(inited);
    CHECK(readBack.size() == 3000 && std::equal(scene.begin(), scene.end(), readBack.begin()));
    CHECK(seekPos == 2990 && tail.size() == 10 && std::equal(scene.end() - 10, scene.end(), tail.begin()));
    CHECK(closed != 99);
    CHECK(escapeRefused && missingRefused && unsupportedRefused);
    CHECK(!fs::exists(base.parent_path() / "outside"));
}

// psxmon's half (nugget monitor/PROTOCOL.md): frames in, ACK/PONG/ERROR out,
// then RUN hands the link to the program, here the same psxsplash file calls.
struct FakeMonitor {
    FakeConsole con;
    std::map<uint32_t, uint8_t> mem{};
    uint32_t pc = 0, gp = 0, sp = 0;
    int pings = 0;
    bool refuseFirstLoad = false;
    uint16_t caps = 0x0007;

    bool frame(uint16_t* type, std::vector<uint16_t>* words) {
        uint8_t b = 1;
        while (b != 0)
            if (!con.get(&b, 1)) return false;
        uint8_t h[6];
        if (!con.get(h, 6)) return false;
        if (h[0] != 0xaa || h[1] != 0x55) {
            con.fail("monitor: bad sync");
            return false;
        }
        *type = uint16_t(h[2] | h[3] << 8);
        const uint16_t len = uint16_t(h[4] | h[5] << 8);
        std::vector<uint8_t> raw(2 * size_t(len) + 4);
        if (!con.get(raw.data(), raw.size())) return false;
        std::vector<uint16_t> all{*type, len};
        for (size_t i = 0; i < len; ++i) all.push_back(uint16_t(raw[2 * i] | raw[2 * i + 1] << 8));
        const size_t c = 2 * size_t(len);
        const uint32_t ck = uint32_t(raw[c] | raw[c + 1] << 8) | uint32_t(raw[c + 2] | raw[c + 3] << 8) << 16;
        if (ck != editor::psxmon::fletcher(all.data(), all.size())) con.fail("monitor: bad checksum");
        words->assign(all.begin() + 2, all.end());
        return true;
    }
    void reply(uint16_t type, const std::vector<uint16_t>& words = {}) {
        const std::vector<uint8_t> f = editor::psxmon::encode(type, words);
        con.put(f.data(), f.size());
    }
    // Serves commands until RUN. `afterRun` is written in the same burst as RUN's ACK.
    bool session(const std::string& afterRun) {
        for (;;) {
            uint16_t type;
            std::vector<uint16_t> w;
            if (!frame(&type, &w)) return false;
            auto u32 = [&](size_t i) { return uint32_t(w[i]) | uint32_t(w[i + 1]) << 16; };
            if (type == editor::psxmon::Ping) {
                ++pings;
                reply(editor::psxmon::Pong, {2, caps, 0x1234, 0x5678});
            } else if (type == editor::psxmon::Load) {
                if (refuseFirstLoad) {
                    refuseFirstLoad = false;
                    reply(editor::psxmon::Error, {3});
                    return false;
                }
                const uint32_t addr = u32(0), n = u32(2);
                if (w.size() != 4 + (n + 1) / 2) con.fail("monitor: LOAD length");
                for (uint32_t i = 0; i < n; ++i) mem[addr + i] = uint8_t(w[4 + i / 2] >> (i & 1 ? 8 : 0));
                reply(editor::psxmon::Ack);
            } else if (type == editor::psxmon::Run) {
                pc = u32(0), gp = u32(2), sp = u32(4);
                std::vector<uint8_t> burst = editor::psxmon::encode(editor::psxmon::Ack, {});
                burst.insert(burst.end(), afterRun.begin(), afterRun.end());
                con.put(burst.data(), burst.size());
                return true;
            } else {
                reply(editor::psxmon::Error, {1});
            }
        }
    }

    // The running program: a `break` stops it, and the host works on it from
    // HALTED until CONT. Returns false if the host never resumes it.
    uint32_t regs[38] = {};
    uint32_t lastEpc = 0;
    std::vector<std::pair<uint32_t, uint32_t>> resumedAt{};  // (epc, pc on CONT)
    bool brk(uint32_t code1, uint32_t code2, bool final = false) {
        lastEpc += 0x40;
        const uint32_t insn = code1 << 16 | code2 << 6 | 0x0d;
        std::vector<uint16_t> st{1};
        for (uint32_t v : {lastEpc, insn, 0u}) st.push_back(uint16_t(v)), st.push_back(uint16_t(v >> 16));
        reply(editor::psxmon::Stopped, st);
        if (final) {  // exit: the host reads a0 and is done; nothing resumes it
            uint16_t type;
            std::vector<uint16_t> w;
            if (!frame(&type, &w) || type != editor::psxmon::GetRegs) return false;
            std::vector<uint16_t> r;
            for (uint32_t v : regs) r.push_back(uint16_t(v)), r.push_back(uint16_t(v >> 16));
            reply(editor::psxmon::Regs, r);
            return true;
        }
        for (;;) {
            uint16_t type;
            std::vector<uint16_t> w;
            if (!frame(&type, &w)) return false;
            auto u32 = [&](size_t i) { return i + 1 < w.size() ? uint32_t(w[i]) | uint32_t(w[i + 1]) << 16 : 0u; };
            if (type == editor::psxmon::GetRegs) {
                std::vector<uint16_t> r;
                for (uint32_t v : regs) r.push_back(uint16_t(v)), r.push_back(uint16_t(v >> 16));
                reply(editor::psxmon::Regs, r);
            } else if (type == editor::psxmon::ReadMem) {
                const uint32_t addr = u32(0), len = u32(2);
                for (uint32_t at = 0; at < len; at += 100) {  // several DATA frames
                    const uint32_t n = std::min(100u, len - at);
                    std::vector<uint16_t> d{uint16_t(n), 0};
                    for (uint32_t i = 0; i < n; i += 2)
                        d.push_back(uint16_t(mem[addr + at + i] | mem[addr + at + i + 1] << 8));
                    reply(editor::psxmon::Data, d);
                }
            } else if (type == editor::psxmon::WriteMem) {
                const uint32_t addr = u32(0), n = u32(2);
                for (uint32_t i = 0; i < n; ++i) mem[addr + i] = uint8_t(w[4 + i / 2] >> (i & 1 ? 8 : 0));
                reply(editor::psxmon::Ack);
            } else if (type == editor::psxmon::SetReg) {
                if (!w.empty() && w[0] < 38) regs[w[0]] = u32(1);
                reply(editor::psxmon::Ack);
            } else if (type == editor::psxmon::Cont) {
                resumedAt.emplace_back(lastEpc, regs[37]);
                reply(editor::psxmon::Ack);
                return true;
            } else {
                con.fail("monitor: unexpected command while halted");
                reply(editor::psxmon::Error, {1});
            }
        }
    }
    // pcdrv.h's calls; each returns what the wrapper would.
    int32_t viaV1() { return regs[2] == 0 ? int32_t(regs[3]) : -1; }
    void setName(const char* name) {
        for (size_t i = 0; i <= std::strlen(name); ++i) mem[0x80100000 + i] = uint8_t(name[i]);
        for (size_t i = std::strlen(name) + 1; i < 300; ++i) mem[0x80100000 + i] = 0xee;  // junk after the NUL
    }
    int32_t pcInit() { return brk(0, 0x101) ? int32_t(regs[2]) : -99; }
    int32_t pcOpen(const char* name) {
        setName(name);
        regs[4] = regs[5] = 0x80100000, regs[6] = 0;
        return brk(0, 0x103) ? viaV1() : -99;
    }
    int32_t pcRead(uint32_t fd, uint32_t len, uint32_t buf) {
        regs[4] = 0, regs[5] = fd, regs[6] = len, regs[7] = buf;
        return brk(0, 0x105) ? viaV1() : -99;
    }
    int32_t pcSeek(uint32_t fd, int32_t off, uint32_t whence) {
        regs[4] = regs[5] = fd, regs[6] = uint32_t(off), regs[7] = whence;
        return brk(0, 0x107) ? viaV1() : -99;
    }
    int32_t pcClose(uint32_t fd) {
        regs[4] = regs[5] = fd;
        return brk(0, 0x104) ? int32_t(regs[2]) : -99;
    }
};

void testPsxmon() {
    namespace fs = std::filesystem;
    // A PING and a LOAD byte for byte as a separate host implementation encodes them.
    const std::vector<uint8_t> ping = editor::psxmon::encode(editor::psxmon::Ping, {});
    const std::vector<uint8_t> load = editor::psxmon::encode(editor::psxmon::Load, {0x10, 0x8001, 3, 0, 0x0201, 0x03});
    CHECK(ping == std::vector<uint8_t>({0x00, 0xaa, 0x55, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00}));
    CHECK(load == std::vector<uint8_t>({0x00, 0xaa, 0x55, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x01, 0x80, 0x03, 0x00,
                                        0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x26, 0x82, 0xe2, 0x84}));

    const fs::path base = g_outDir / "psxmon";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    std::vector<uint8_t> scene(2500);
    for (size_t i = 0; i < scene.size(); ++i) scene[i] = uint8_t(i * 5 + 3);
    std::ofstream(base / "scene_0.splashpack", std::ios::binary).write(reinterpret_cast<const char*>(scene.data()), scene.size());
    // A program over two LOAD frames, odd-sized, with a stack in the header.
    std::vector<uint8_t> exe(0x800 + 9001);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    const uint32_t pc = 0x80010000, gp = 0x8001f000, addr = 0x80010000, tsize = 9001, sbase = 0x801fff00, ssize = 0xf0;
    std::memcpy(&exe[0x10], &pc, 4);
    std::memcpy(&exe[0x14], &gp, 4);
    std::memcpy(&exe[0x18], &addr, 4);
    std::memcpy(&exe[0x1c], &tsize, 4);
    std::memcpy(&exe[0x30], &sbase, 4);
    std::memcpy(&exe[0x34], &ssize, 4);
    for (size_t i = 0x800; i < exe.size(); ++i) exe[i] = uint8_t(i * 11 + 1);

    Pipe toConsole, toHost;
    PipeEnd hostEnd(toHost, toConsole), consoleEnd(toConsole, toHost);
    FakeMonitor mon{FakeConsole{consoleEnd, 1, {}, {}, 0, 0}};
    std::vector<uint8_t> readBack;
    int32_t init = -9, h = -9, got = -9, pos = -9, tailGot = -9, closed = -9, missing = -9, outside = -9;
    std::thread console([&] {
        // The program prints straight after RUN's ACK, in the same burst.
        if (mon.session("psxsplash: boot\r\n")) {
            init = mon.pcInit();
            h = mon.pcOpen("scene_0.splashpack");
            got = mon.pcRead(uint32_t(h), 4096, 0x80120000);  // more than the file holds
            for (int32_t i = 0; i < got; ++i) readBack.push_back(mon.mem[0x80120000 + uint32_t(i)]);
            pos = mon.pcSeek(uint32_t(h), 2490, 0);
            tailGot = mon.pcRead(uint32_t(h), 64, 0x80130000);
            closed = mon.pcClose(uint32_t(h));
            missing = mon.pcOpen("missing.bin");
            outside = mon.pcOpen("../outside");
            mon.con.say("done\n");
            mon.regs[4] = 7;
            mon.brk(4, 0, true);  // exit(7): the host reads a0 and stops serving
        }
        consoleEnd.close();
    });
    std::string err;
    std::vector<int> progress;
    uint16_t caps = 0;
    CHECK(editor::psxmonPresent(hostEnd, 2000, &caps) && caps == 0x0007);
    CHECK(editor::psxmonUpload(hostEnd, exe, &err, [&](int p) { progress.push_back(p); }));
    if (!err.empty()) std::fprintf(stderr, "psxmon upload: %s\n", err.c_str());
    std::vector<std::string> lines, events;
    std::atomic<bool> cancel{false};
    editor::PcdrvHost host(base);
    const bool exited = editor::psxmonServe(hostEnd, host, cancel, [&](const std::string& l) { lines.push_back(l); },
                                            [&](const std::string& e) { events.push_back(e); }, &err);
    if (!err.empty()) std::fprintf(stderr, "psxmon serve: %s\n", err.c_str());
    consoleEnd.close();
    console.join();
    for (const std::string& p : mon.con.problems) std::fprintf(stderr, "psxmon: %s\n", p.c_str());
    CHECK(mon.con.problems.empty());
    CHECK(mon.pings >= 1);
    CHECK(mon.pc == pc && mon.gp == gp && mon.sp == sbase + ssize);
    bool same = mon.mem.count(addr + tsize) == 0 && mon.mem.count(addr - 1) == 0;
    for (uint32_t i = 0; same && i < tsize; ++i) same = mon.mem[addr + i] == exe[0x800 + i];
    CHECK(same);
    CHECK(progress.size() == 2 && progress.back() == 100);
    CHECK(std::find(lines.begin(), lines.end(), "psxsplash: boot") != lines.end());
    CHECK(init == 0 && h >= 3);
    CHECK(got == 2500 && readBack.size() == 2500 && std::equal(scene.begin(), scene.end(), readBack.begin()));
    CHECK(pos == 2490 && tailGot == 10 && mon.mem[0x80130009] == scene[2499] && mon.mem.count(0x8013000a) == 0);
    CHECK(closed == 0 && missing == -1 && outside == -1);
    CHECK(std::find(lines.begin(), lines.end(), "done") != lines.end());
    CHECK(exited && std::find(lines.begin(), lines.end(), "The program exited with code 7.") != lines.end());
    // Every call resumed past its break, and the exit was not resumed at all.
    bool past = mon.resumedAt.size() == 8;
    for (auto& [epc, next] : mon.resumedAt) past = past && next == epc + 4;
    CHECK(past);
    CHECK(!fs::exists(base.parent_path() / "outside"));

    // A refused LOAD stops the upload with the monitor's error code.
    {
        Pipe a, b;
        PipeEnd h(a, b), c(b, a);
        FakeMonitor m{FakeConsole{c, 1, {}, {}, 0, 0}};
        m.refuseFirstLoad = true;
        std::thread t([&] {
            m.session("");
            c.close();
        });
        CHECK(!editor::psxmonUpload(h, exe, &err) && err.find("refused LOAD") != std::string::npos &&
              err.find("error 3") != std::string::npos);
        t.join();
    }
    // A psxsplash from before break calls under psxmon: it tries its SIO1
    // protocol, and the host says why nothing loads.
    {
        Pipe a, b;
        PipeEnd h(a, b), c(b, a);
        FakeMonitor m{FakeConsole{c, 1, {}, {}, 0, 0}};
        std::thread t([&] {
            const uint8_t escape[] = {0, 'p', 0x01, 0x01, 0, 0};
            m.con.put(escape, sizeof escape);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            c.close();
        });
        std::vector<std::string> seen;
        std::atomic<bool> stop{false};
        editor::PcdrvHost files(base);
        editor::psxmonServe(h, files, stop, [&](const std::string& l) { seen.push_back(l); }, {}, &err);
        t.join();
        CHECK(std::count_if(seen.begin(), seen.end(),
                            [](const std::string& l) { return l.find("over SIO1") != std::string::npos; }) == 1);
    }
    // Nothing answers the PING: not present, so Unirom gets its turn.
    {
        Pipe a, b;
        PipeEnd h(a, b);
        CHECK(!editor::psxmonPresent(h, 600));
    }
}

// HardwareRun: the editor's session, over a pipe to the same fake console.
class Borrowed : public editor::Link {
  public:
    explicit Borrowed(editor::Link& l) : l_(l) {}
    bool write(const void* d, size_t n) override { return l_.write(d, n); }
    int read(void* d, size_t n, int t) override { return l_.read(d, n, t); }

  private:
    editor::Link& l_;
};

// HardwareRun picks the file protocol from the monitor's caps: break calls
// with the slot, psxsplash's SIO1 protocol without it.
void runHardwarePsxmon(uint16_t caps) {
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "hwrun-psxmon";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(dir / "scene_0.splashpack", std::ios::binary) << "scene";
    std::vector<uint8_t> exe(0x800 + 64);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    const fs::path exePath = dir / "engine.ps-exe";
    std::ofstream(exePath, std::ios::binary).write(reinterpret_cast<const char*>(exe.data()), exe.size());

    Pipe toConsole, toHost;
    PipeEnd hostEnd(toHost, toConsole), consoleEnd(toConsole, toHost);
    FakeMonitor mon{FakeConsole{consoleEnd, 1, {}, {}, 0, 0}};
    mon.caps = caps;
    int32_t h = -9;
    std::thread console([&] {
        if (!mon.session("")) return;
        if (caps & editor::psxmon::kCapSlot) {
            h = mon.pcOpen("scene_0.splashpack");
        } else {
            uint32_t u = 0;
            if (mon.con.open("scene_0.splashpack", &u)) h = int32_t(u);
        }
    });
    editor::HardwareRun run;
    run.start([&](std::string*) { return std::make_unique<Borrowed>(hostEnd); }, exePath, dir);
    console.join();
    for (int i = 0; i < 300; ++i) {
        auto l = run.lines();
        if (std::find(l.begin(), l.end(), "[file] open scene_0.splashpack -> 3") != l.end()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto l = run.lines();
    CHECK(h == 3 && std::find(l.begin(), l.end(), "Uploading psxsplash through psxmon...") != l.end());
    CHECK(mon.con.problems.empty());
    run.stop();
    consoleEnd.close();
}

void testHardwareRun() {
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "hwrun";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<uint8_t> exe(0x800 + 100);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    const fs::path exePath = dir / "engine.ps-exe";
    std::ofstream(exePath, std::ios::binary).write(reinterpret_cast<const char*>(exe.data()), exe.size());

    Pipe toConsole, toHost;
    PipeEnd hostEnd(toHost, toConsole), consoleEnd(toConsole, toHost);
    FakeConsole con{consoleEnd, 2, {}, {}, 0, 0};
    std::atomic<bool> printed{false};
    std::thread console([&] {
        con.shell();
        con.say("hello from the console\n");
        printed = true;
    });
    editor::HardwareRun run;
    run.start([&](std::string*) { return std::make_unique<Borrowed>(hostEnd); }, exePath, dir);
    auto waitFor = [&](auto pred) {
        for (int i = 0; i < 300 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pred();
    };
    console.join();
    CHECK(con.problems.empty());
    CHECK(waitFor([&] {
        auto l = run.lines();
        return std::find(l.begin(), l.end(), "hello from the console") != l.end();
    }));
    CHECK(run.phase() == editor::HardwareRun::Phase::Running && run.active() && run.progress() == 100);
    run.stop();
    CHECK(run.phase() == editor::HardwareRun::Phase::Stopped && !run.active());

    // A port that does not open: Failed, with the reason.
    run.start([](std::string* err) {
        *err = "Cannot open COM9";
        return std::unique_ptr<editor::Link>();
    }, exePath, dir);
    CHECK(waitFor([&] { return run.phase() == editor::HardwareRun::Phase::Failed; }));
    CHECK(run.error() == "Cannot open COM9");
    // No build on disk.
    run.start([&](std::string*) { return std::make_unique<Borrowed>(hostEnd); }, dir / "absent.ps-exe", dir);
    CHECK(waitFor([&] { return run.phase() == editor::HardwareRun::Phase::Failed; }));
    CHECK(run.error() == "Cannot read the psxsplash build.");
}

void testUnirom() {
    runUnirom(1);
    runUnirom(2);
    runUnirom(3);

    // Not a PS-X EXE: refused before anything is sent.
    Pipe a, b;
    PipeEnd end(a, b);
    std::string err;
    CHECK(!editor::uniromUpload(end, std::vector<uint8_t>(4096), &err) && err == "Not a PS-X EXE.");
    CHECK(b.q.empty());

    // A console that never answers.
    std::vector<uint8_t> exe(0x1000);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    end.close();
    CHECK(!editor::uniromUpload(end, exe, &err) && !err.empty());

    std::string linkErr;
    CHECK(!editor::openLink("", 115200, &linkErr) && !linkErr.empty());
    CHECK(!editor::openLink("tcp:nonsense", 115200, &linkErr) && linkErr.find("tcp:HOST:PORT") != std::string::npos);
}

// World streaming planner: a mesh wider than a region is cut so each region
// holds one part, a small one lands whole, and the regions are laid out the
// way psxsplash reads them.
void testWorldStreamPlan() {
    namespace st = splash::stream;
    // A: a 40 x 40 grid of tiny triangles over 0..40960 fp12 (10 units square).
    // B: one small prop near a corner.
    std::vector<st::Candidate> in(2);
    in[0].object = 0;
    in[0].bounds = {0, 0, 40960, 40960};
    for (int z = 0; z < 40; z++)
        for (int x = 0; x < 40; x++) in[0].tris.push_back({x * 1024, z * 1024, x * 1024 + 1000, z * 1024 + 1000});
    in[1].object = 1;
    in[1].bounds = {100, 100, 300, 300};
    for (int i = 0; i < 12; i++) in[1].tris.push_back({100, 100, 300, 300});

    st::Plan p = st::plan(in, 4096);
    CHECK(p.regions.size() > 1);
    CHECK(p.loadRadius == 5120 && p.unloadRadius == 7680);
    CHECK(p.slotCount >= 2 && p.slotCount <= st::kMaxSlots);
    std::vector<int> seenA(in[0].tris.size(), 0);
    int partsA = 0, partsB = 0;
    uint32_t sector = 0;
    uint32_t maxBytes = 0;
    for (const st::Region& r : p.regions) {
        CHECK(r.firstSector == sector);
        CHECK(r.byteSize % st::kSector == 0 && r.byteSize >= r.triBytes && r.byteSize - r.triBytes < st::kSector);
        sector += r.byteSize / st::kSector;
        maxBytes = std::max(maxBytes, r.byteSize);
        uint32_t bytes = 0;
        for (const st::Piece& pc : r.pieces) {
            (pc.candidate == 0 ? partsA : partsB)++;
            for (int t : pc.tris) {
                const st::Box& b = in[size_t(pc.candidate)].tris[size_t(t)];
                CHECK(b.minX >= r.bounds.minX && b.maxX <= r.bounds.maxX && b.minZ >= r.bounds.minZ && b.maxZ <= r.bounds.maxZ);
                if (pc.candidate == 0) seenA[size_t(t)]++;
            }
            bytes += uint32_t(pc.tris.size()) * st::kBytesPerTri;
        }
        CHECK(bytes == r.triBytes);
    }
    CHECK(p.slotBytes == maxBytes);
    CHECK(partsA == int(p.regions.size()));  // the grid covers every region
    CHECK(partsB == 1);
    CHECK(std::all_of(seenA.begin(), seenA.end(), [](int n) { return n == 1; }));
    CHECK(p.drawTris > 0 && p.drawTris < in[0].tris.size() + in[1].tris.size());

    // A draw distance covering the whole world: every region is in reach, so
    // the slot count is the region count plus the one being read.
    st::Plan all = st::plan(in, 1 << 20);
    CHECK(all.slotCount == std::min(int(all.regions.size()) + 1, st::kMaxSlots));
    CHECK(st::plan({}, 4096).regions.empty());
}

// Streaming on the courtyard: the scenery streams, the scripted Game Logic
// does not, a .geo file sits next to the pack, and turning it off removes it.
void testWorldStreamExport() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    const fs::path dir = g_outDir / "stream";
    fs::create_directories(dir);
    scene.settings.streamWorld = true;
    splash::ExportResult on = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(on.ok());
    const splash::ExportStats& s = on.stats;
    CHECK(s.streamRegions >= 1 && s.streamedObjects >= 1);
    CHECK(fs::exists(dir / "scene.geo") && fs::file_size(dir / "scene.geo") == s.geoFileBytes);
    CHECK(s.geoFileBytes % 2048 == 0 && s.geoFileBytes >= s.streamedTriBytes);
    CHECK(s.orderingTableSize <= splash::kStreamDrawDistance);
    // The courtyard is far smaller than the draw distance: streaming costs RAM.
    CHECK(std::any_of(s.streamWarnings.begin(), s.streamWarnings.end(),
                      [](const splash::StreamWarning& w) { return w.kind == splash::StreamProblem::NoSaving; }));
    CHECK(splash::ramBudget(s).used >= s.streamPoolBytes());

    scene.settings.streamWorld = false;
    splash::ExportResult off = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(off.ok() && off.stats.streamRegions == 0 && off.stats.streamWarnings.empty());
    CHECK(!fs::exists(dir / "scene.geo"));
    CHECK(off.stats.splashpackBytes > on.stats.splashpackBytes);  // the triangles moved out

    // Settings round-trip through the scene file.
    scene.settings.streamWorld = true;
    splash::saveScene(scene, dir / "s.scene");
    CHECK(splash::loadScene(dir / "s.scene").settings.streamWorld);
}

// A flat square of `size` metres, two triangles facing up.
splash::NavMesh bakePlane(float size, std::vector<std::string>& errors) {
    const float h = size / 2;
    static std::vector<splash::Vec3> pos;
    pos = {{-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h}};
    splash::NavInputObject in{true, false, splash::Mat34::trs({0, 0, 0}, splash::Quat{0, 0, 0, 1}, {1, 1, 1}), &pos,
                              {0, 1, 2, 0, 2, 3}, splash::Bounds({0, 0, 0}, {size, 0, size})};
    return splash::buildNavRegions({in}, splash::NavBuildParams{}, {0, 0, 0}, errors);
}

// Recast indexes compact spans with 24 bits: a 300 m plane at the default
// 0.05 m cells is 36M spans, which used to bake to no regions at all, with no
// error, so the player could not move. The bake now grows the cell size.
void testNavBigWorld() {
    std::vector<std::string> errors;
    splash::NavMesh small = bakePlane(20, errors);
    CHECK(errors.empty() && !small.regions.empty());
    CHECK(small.cellSize == splash::NavBuildParams{}.cellSize);  // small worlds bake as asked
    splash::NavMesh big = bakePlane(300, errors);
    CHECK(errors.empty());
    CHECK(!big.regions.empty());
    CHECK(big.cellSize > splash::NavBuildParams{}.cellSize);
    CHECK(double(big.extentX) / big.cellSize * (double(big.extentZ) / big.cellSize) <= 1.05 * splash::kNavSpanBudget);

    // Export: stats carry the bake, and a player with nothing to stand on warns.
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    const fs::path dir = g_outDir / "nav";
    fs::create_directories(dir);
    splash::ExportResult ok = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(ok.ok() && ok.stats.navWanted && ok.stats.navRegions > 0 && !splash::navCoarsened(ok.stats));
    CHECK(std::none_of(ok.warnings.begin(), ok.warnings.end(),
                       [](const std::string& w) { return w == splash::navEmptyWarning(); }));
    std::function<void(std::vector<splash::Object>&)> strip = [&](std::vector<splash::Object>& objs) {
        for (splash::Object& o : objs) {
            o.collider.reset();
            strip(o.children);
        }
    };
    strip(scene.objects);
    splash::ExportResult none = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(none.ok() && none.stats.navWanted && none.stats.navRegions == 0);
    CHECK(std::any_of(none.warnings.begin(), none.warnings.end(),
                      [](const std::string& w) { return w == splash::navEmptyWarning(); }));
}

// editMany: one undo step over several objects.
namespace lua = editor::lua;

lua::Tok tokAt(const std::vector<std::string>& lines, int line, int col) {
    std::vector<std::vector<lua::Token>> toks = lua::tokenize(lines);
    for (const lua::Token& t : toks[line])
        if (col >= t.col && col < t.col + t.len) return t.kind;
    return lua::Tok::Text;
}

std::vector<std::string> luaLines(const char* text) {
    lua::Buffer b;
    b.setText(text);
    return b.lines();
}

void testLuaTokenize() {
    std::vector<std::string> l = luaLines(
        "local x = Entity.Find(\"Door\") -- find\n"
        "function onUpdate(self, dt)\n"
        "  print(42)\n"
        "  --[[ long\n"
        "  comment ]] y = 0x1F .. 'open\n"
        "  local s = [==[ a\n"
        "  ]==] Input.CROSS = nope.Find\n");
    using T = lua::Tok;
    CHECK(tokAt(l, 0, 0) == T::Keyword);
    CHECK(tokAt(l, 0, 6) == T::Identifier);
    CHECK(tokAt(l, 0, 8) == T::Operator);
    CHECK(tokAt(l, 0, 10) == T::Api);        // Entity
    CHECK(tokAt(l, 0, 17) == T::ApiMember);  // Find
    CHECK(tokAt(l, 0, 22) == T::String);     // "Door"
    CHECK(tokAt(l, 0, 31) == T::Comment);
    CHECK(tokAt(l, 1, 0) == T::Keyword);
    CHECK(tokAt(l, 1, 9) == T::Function);   // onUpdate, defined
    CHECK(tokAt(l, 1, 18) == T::Constant);  // self
    CHECK(tokAt(l, 1, 24) == T::Identifier);
    CHECK(tokAt(l, 2, 2) == T::Api);  // print
    CHECK(tokAt(l, 2, 8) == T::Number);
    CHECK(tokAt(l, 3, 4) == T::Comment);
    CHECK(tokAt(l, 4, 2) == T::Comment);  // the long comment goes on
    CHECK(tokAt(l, 4, 13) == T::Identifier);
    CHECK(tokAt(l, 4, 17) == T::Number);
    CHECK(tokAt(l, 4, 22) == T::Operator);  // ..
    CHECK(tokAt(l, 4, 26) == T::Error);     // unfinished 'open
    CHECK(tokAt(l, 5, 12) == T::String);
    CHECK(tokAt(l, 6, 2) == T::String);  // the long string goes on
    CHECK(tokAt(l, 6, 8) == T::Api);
    CHECK(tokAt(l, 6, 14) == T::ApiMember);
    CHECK(tokAt(l, 6, 27) == T::Identifier);  // nope.Find: not the API
    CHECK(lua::columnOf("\tab", 2) == 5);
    CHECK(lua::byteAt("\tab", 5) == 2);
}

void testLuaBuffer() {
    lua::Buffer b;
    b.setText("if x then\nend");
    uint64_t start = b.revision();
    b.moveTo({0, 9});
    b.newline();
    CHECK(b.line(1) == "    " && b.cursor() == (lua::Pos{1, 4}));
    for (char c : std::string("y = 1")) b.insert(std::string(1, c));
    CHECK(b.line(1) == "    y = 1");
    CHECK(b.text() == "if x then\n    y = 1\nend");
    // "y", " ", "=", " ", "1": a word and the spaces after it are one step each.
    CHECK(b.undo() && b.line(1) == "    y = ");
    CHECK(b.undo() && b.line(1) == "    y ");
    CHECK(b.undo() && b.line(1) == "    ");
    CHECK(b.undo() && b.text() == "if x then\nend" && b.cursor() == (lua::Pos{0, 9}));
    CHECK(b.revision() == start);
    CHECK(!b.undo());
    CHECK(b.redo() && b.redo() && b.line(1) == "    y ");
    CHECK(b.redo() && b.redo() && b.line(1) == "    y = 1" && !b.redo());
    // Backspace merges too, and stops at tab stops in the indentation.
    b.moveTo({1, 9});
    b.backspace();
    b.backspace();
    CHECK(b.line(1) == "    y =");
    CHECK(b.undo() && b.line(1) == "    y = 1");
    b.moveTo({1, 4});
    b.backspace();
    CHECK(b.line(1) == "y = 1");
    b.undo();
    // Selections: typing replaces, Delete removes, multi-line text splits.
    b.select({0, 3}, {0, 4});
    b.insert("ready");
    CHECK(b.line(0) == "if ready then");
    b.select({0, 0}, {1, 4});
    CHECK(b.selectedText() == "if ready then\n    ");
    b.del();
    CHECK(b.line(0) == "y = 1" && b.lineCount() == 2);
    b.insert("a\nb\n");
    CHECK(b.lineCount() == 4 && b.line(2) == "y = 1" && b.cursor() == (lua::Pos{2, 0}));
    CHECK(b.undo() && b.lineCount() == 2);
    // A closing line lines up with its opener on Enter.
    b.setText("function f()\n    x()\n    end");
    b.moveTo({2, 7});
    b.newline();
    CHECK(b.line(2) == "end" && b.line(3) == "" && b.cursor() == (lua::Pos{3, 0}));
    // Comment toggling and indenting a selection.
    b.select({0, 0}, {1, 3});
    b.toggleComment();
    CHECK(b.line(0) == "-- function f()" && b.line(1) == "--     x()");
    b.toggleComment();
    CHECK(b.line(0) == "function f()" && b.line(1) == "    x()");
    b.select({0, 0}, {1, 3});
    b.indent();
    CHECK(b.line(0) == "    function f()" && b.line(1) == "        x()");
    b.outdent();
    CHECK(b.line(0) == "function f()");
    // Find, forward and back, wrapping.
    b.setText("Foo foo\nfoo");
    auto hit = b.find("foo", {0, 1}, true, lua::FindOptions{true});
    CHECK(hit && hit->first == (lua::Pos{0, 4}));
    hit = b.find("foo", {1, 1}, true, lua::FindOptions{true});
    CHECK(hit && hit->first == (lua::Pos{0, 4}));
    hit = b.find("foo", {0, 0}, false, lua::FindOptions{false});
    CHECK(hit && hit->first == (lua::Pos{1, 0}));
    CHECK(b.countMatches("foo", lua::FindOptions{false}) == 3 && b.countMatches("foo", lua::FindOptions{true}) == 2);
    // Motions.
    b.setText("local abc = x\n  y");
    b.moveTo({0, 0});
    b.move(lua::Buffer::Move::WordRight, false);
    CHECK(b.cursor() == (lua::Pos{0, 6}));
    b.move(lua::Buffer::Move::End, true);
    CHECK(b.selectedText() == "abc = x");
    b.move(lua::Buffer::Move::Down, false);
    b.move(lua::Buffer::Move::Home, false);
    CHECK(b.cursor() == (lua::Pos{1, 2}));
}

bool hasItem(const lua::Completions& c, const char* label, lua::ItemKind kind) {
    for (const lua::Completion& i : c.items)
        if (i.label == label && i.kind == kind) return true;
    return false;
}

void testLuaComplete() {
    std::vector<std::string> l = luaLines(
        "local health = 3\n"
        "local function hurt(n) health = health - n end\n"
        "Ent\n"
        "Entity.Fi\n"
        "Input.\n"
        "hea\n"
        "-- Ent\n"
        "function onUp\n"
        "x = \"Ent\n"
        "hurt(");
    using K = lua::ItemKind;
    lua::Completions c = lua::complete(l, {2, 3});
    CHECK(c.prefix == "Ent" && c.from == (lua::Pos{2, 0}));
    CHECK(!c.items.empty() && c.items[0].label == "Entity" && c.items[0].kind == K::ApiTable);
    c = lua::complete(l, {3, 9});
    CHECK(!c.items.empty() && c.items[0].label == "Find" && c.items[0].kind == K::ApiFunction);
    CHECK(!c.items.empty() && c.items[0].api && std::string(c.items[0].api->signature).find("Entity.Find(") == 0);
    CHECK(hasItem(c, "FindByIndex", K::ApiFunction) && !hasItem(c, "GetPlayer", K::ApiFunction));
    c = lua::complete(l, {4, 6});
    CHECK(hasItem(c, "CROSS", K::ApiConstant) && hasItem(c, "IsPressedPlayer1", K::ApiFunction));
    c = lua::complete(l, {5, 3});
    CHECK(!c.items.empty() && c.items[0].label == "health" && c.items[0].kind == K::Local);
    CHECK(!hasItem(c, "hea", K::Local));
    CHECK(hasItem(lua::complete(l, {1, 23}, true), "hurt", K::Function));  // Ctrl+Space, nothing typed
    CHECK(lua::complete(l, {6, 6}).items.empty());  // in a comment
    CHECK(lua::complete(l, {8, 8}).items.empty());  // in a string
    c = lua::complete(l, {7, 13});
    CHECK(!c.items.empty() && c.items[0].label == "onUpdate" && c.items[0].kind == K::Callback);
    CHECK(lua::complete(l, {2, 0}).items.empty());  // nothing typed yet
    // Signature hint inside a call.
    lua::Lookup call = lua::callAt(l, {9, 5});
    CHECK(call.def && call.def->name == "hurt" && call.def->signature == "function hurt(n)" && call.arg == 0);
    call = lua::callAt(luaLines("Entity.SetPosition(self, "), {0, 25});
    CHECK(call.api && std::string(call.api->name) == "SetPosition" && call.arg == 1);
}

void testLuaDefinitions() {
    std::vector<std::string> l = luaLines(
        "local speed = 4\n"                      // 0
        "local function step(self, dt)\n"        // 1
        "  return speed * dt\n"                   // 2
        "end\n"                                   // 3
        "function onUpdate(self, dt)\n"          // 4
        "  local speed = step(self, dt)\n"       // 5
        "  helper(speed)\n"                       // 6
        "  Entity.SetPosition(self, speed)\n"    // 7
        "end\n"                                   // 8
        "function helper(v) print(v) end\n");     // 9
    lua::Lookup r = lua::lookup(l, {5, 17});  // step(
    CHECK(r.def && r.def->pos == (lua::Pos{1, 15}) && r.def->kind == lua::DefKind::LocalFunction);
    CHECK(r.def && r.def->signature == "function step(self, dt)");
    r = lua::lookup(l, {2, 10});  // speed in step: the file-level local
    CHECK(r.def && r.def->pos.line == 0);
    r = lua::lookup(l, {6, 10});  // speed after the shadowing local
    CHECK(r.def && r.def->pos.line == 5);
    r = lua::lookup(l, {6, 3});  // helper, defined below
    CHECK(r.def && r.def->pos == (lua::Pos{9, 9}) && r.def->kind == lua::DefKind::Function);
    r = lua::lookup(l, {7, 12});  // Entity.SetPosition
    CHECK(!r.def && r.api && std::string(r.api->table) == "Entity" && std::string(r.api->name) == "SetPosition");
    CHECK(r.start == (lua::Pos{7, 9}) && r.end == (lua::Pos{7, 20}));
    r = lua::lookup(l, {9, 20});  // print
    CHECK(!r.def && r.api && std::string(r.api->name) == "print");
    r = lua::lookup(l, {3, 1});  // end
    CHECK(!r.def && !r.api);
}

void testLuaCheck() {
    lua::Diagnostic d = lua::check("local a = 1\nlocal b = = 2\nreturn a\n");
    CHECK(!d.ok() && d.line == 1);
    CHECK(d.message.find("unexpected symbol") != std::string::npos);
    d = lua::check("function f()\n  if x then\n    y()\n  end\n");
    CHECK(!d.ok() && d.line == 3 && d.message.find("'end' expected") != std::string::npos);
    CHECK(lua::check("").ok());
    std::ifstream in(std::filesystem::path(SPLASHEDIT_SOURCE_DIR) / "tests/lua/game_script.lua", std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty() && lua::check(src).ok());
    // The script re-checks only when its text changed.
    lua::Script s;
    s.buffer.setText("x = 1");
    CHECK(s.diagnose().ok());
    s.buffer.moveTo({0, 5});
    s.buffer.insert(" +");
    CHECK(!s.diagnose().ok() && s.diagnostic().line == 0);
    s.buffer.undo();
    CHECK(s.diagnose().ok());
}

void testLuaScriptFile() {
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "lua-editor";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "scripts" / "door.lua";
    lua::Script s;
    std::string err;
    CHECK(s.open(file, &err) && s.buffer.text().empty());
    CHECK(s.dirty() && s.title() == "door.lua");  // not on disk yet
    s.buffer.insert("function onCreate(self)\nend\n");
    CHECK(s.save(&err) && !s.dirty());
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    CHECK(slurp(file) == "function onCreate(self)\nend\n");
    s.buffer.moveTo({1, 3});
    s.buffer.insert(" -- done");
    CHECK(s.dirty());
    s.buffer.undo();
    CHECK(!s.dirty());  // back to what is on disk
    s.buffer.redo();
    CHECK(s.dirty() && s.save(&err) && slurp(file) == "function onCreate(self)\nend -- done\n");
    // CRLF files stay CRLF.
    const fs::path crlf = dir / "crlf.lua";
    { std::ofstream(crlf, std::ios::binary) << "a = 1\r\nb = 2\r\n"; }
    lua::Script c;
    CHECK(c.open(crlf, &err) && !c.dirty() && c.buffer.lineCount() == 3 && c.buffer.line(1) == "b = 2");
    c.buffer.moveTo({1, 5});
    c.buffer.insert("0");
    CHECK(c.save(&err) && slurp(crlf) == "a = 1\r\nb = 20\r\n");
}

// Replace and Replace all in the script editor.
void testLuaReplace() {
    lua::Buffer b;
    b.setText("local hp = 3\nHP = hp + hp\nprint(hp)");
    b.moveTo({0, 0});
    // Nothing selected: Replace first selects the next match.
    CHECK(!b.replaceNext("hp", "health", lua::FindOptions{true}));
    CHECK(b.selStart() == (lua::Pos{0, 6}) && b.selectedText() == "hp");
    CHECK(b.replaceNext("hp", "health", lua::FindOptions{true}));
    CHECK(b.line(0) == "local health = 3" && b.selStart() == (lua::Pos{1, 5}));
    CHECK(b.undo() && b.line(0) == "local hp = 3");
    // Replace all: every match, one undo step, the cursor stays.
    b.moveTo({1, 2});
    CHECK(b.replaceAll("hp", "health", lua::FindOptions{true}) == 4);
    CHECK(b.text() == "local health = 3\nHP = health + health\nprint(health)");
    CHECK(b.cursor() == (lua::Pos{1, 2}) && !b.hasSelection());
    CHECK(b.undo() && b.text() == "local hp = 3\nHP = hp + hp\nprint(hp)");
    CHECK(b.redo() && b.text() == "local health = 3\nHP = health + health\nprint(health)");
    CHECK(b.undo());
    // Match case off finds HP too; a replacement holding the needle is not
    // replaced again; nothing to replace is no undo step.
    CHECK(b.replaceAll("hp", "hp_hp", lua::FindOptions{false}) == 5);
    CHECK(b.line(1) == "hp_hp = hp_hp + hp_hp");
    CHECK(b.undo());
    const uint64_t rev = b.revision();
    CHECK(b.replaceAll("zz", "y", lua::FindOptions{false}) == 0 && b.revision() == rev);
    CHECK(b.replaceAll("", "y", lua::FindOptions{false}) == 0);
    // Removing every match; matches do not overlap.
    b.setText("aaa\nkeep\naaaa");
    CHECK(b.replaceAll("aa", "", lua::FindOptions{true}) == 3 && b.text() == "a\nkeep\n");
    CHECK(b.undo() && b.text() == "aaa\nkeep\naaaa" && !b.undo());
    // Replace with match case off accepts a selection differing in case.
    b.setText("Foo foo");
    b.select({0, 0}, {0, 3});
    CHECK(b.replaceNext("foo", "x", lua::FindOptions{false}) && b.text() == "x foo" && b.selectedText() == "foo");
    CHECK(!b.replaceNext("FOO", "y", lua::FindOptions{true}) && b.text() == "x foo");
}

// Find options: regex with groups, a bad pattern, whole word, preserve case,
// in selection.
void testLuaFindOptions() {
    lua::Buffer b;
    lua::FindOptions re;
    re.regex = re.matchCase = true;
    // Regex: per line, $1 groups, one undo step.
    b.setText("hp = 3\nlocal mp = 4\nx = hpmax");
    CHECK(b.countMatches("\\b(h|m)p\\b", re) == 2);
    CHECK(b.countMatches("^\\w+", re) == 3);
    CHECK(b.replaceAll("(\\w+) = (\\d+)", "$2 = $1", re) == 2);
    CHECK(b.text() == "3 = hp\nlocal 4 = mp\nx = hpmax");
    CHECK(b.undo() && b.text() == "hp = 3\nlocal mp = 4\nx = hpmax" && !b.canUndo());
    CHECK(b.replaceAll("p", "$&$&", re) == 3 && b.line(2) == "x = hppmax");
    CHECK(b.undo());
    // Zero-length matches are skipped, not looped on.
    CHECK(b.countMatches("4*", re) == 1);
    // Replace (one) uses the groups of the selected match.
    b.moveTo({0, 0});
    CHECK(!b.replaceNext("(m)(p)", "$2$1", re) && b.selStart() == (lua::Pos{1, 6}));
    CHECK(b.replaceNext("(m)(p)", "$2$1", re) && b.line(1) == "local pm = 4");
    CHECK(b.undo());
    // A bad pattern finds nothing and says why; nothing throws.
    CHECK(!lua::findError("(hp", re).empty() && !lua::findError("[a-", re).empty());
    CHECK(lua::findError("(hp)", re).empty() && lua::findError("(hp", lua::FindOptions{}).empty());
    CHECK(b.countMatches("(hp", re) == 0 && !b.find("(hp", {0, 0}, true, re));
    const uint64_t rev = b.revision();
    CHECK(b.replaceAll("*", "x", re) == 0 && b.revision() == rev);
    // Without regex the same text is literal.
    b.setText("f(hp) f(hp");
    CHECK(b.countMatches("(hp", lua::FindOptions{}) == 2);
    // Case-insensitive regex.
    lua::FindOptions rei;
    rei.regex = true;
    CHECK(b.countMatches("F\\(", rei) == 2 && b.countMatches("F\\(", re) == 0);

    // Whole word: not inside a longer name; plain and regex.
    lua::FindOptions ww;
    ww.wholeWord = true;
    b.setText("hp hpmax max_hp _hp hp.x a.hp(hp)");
    CHECK(b.countMatches("hp", lua::FindOptions{}) == 7 && b.countMatches("hp", ww) == 4);
    CHECK(b.replaceAll("hp", "H", ww) == 4 && b.text() == "H hpmax max_hp _hp H.x a.H(H)");
    CHECK(b.undo());
    ww.regex = true;
    CHECK(b.countMatches("h.", ww) == 4);
    // A rejected match does not hide a whole word right after it.
    b.setText("xhp hp");
    ww.regex = false;
    auto hit = b.find("hp", {0, 0}, true, ww);
    CHECK(hit && hit->first == (lua::Pos{0, 4}));

    // Preserve case: lower, Capitalized and UPPER matches.
    CHECK(lua::preserveCase("health", "hp") == "hp" && lua::preserveCase("Health", "hp") == "Hp");
    CHECK(lua::preserveCase("HEALTH", "hp") == "HP" && lua::preserveCase("heAlth", "hP") == "hP");
    CHECK(lua::preserveCase("health", "HP") == "hp");
    lua::FindOptions pc;
    pc.preserveCase = true;
    b.setText("health = Health + HEALTH");
    CHECK(b.replaceAll("health", "hp", pc) == 3 && b.text() == "hp = Hp + HP");
    CHECK(b.undo() && !b.canUndo());
    b.select({0, 9}, {0, 15});
    CHECK(b.replaceNext("health", "hp", pc) && b.text() == "health = Hp + HEALTH");
    CHECK(b.undo());
    // With regex, after the groups are put in.
    pc.regex = true;
    CHECK(b.replaceAll("(h)ealth", "$1p", pc) == 3 && b.text() == "hp = Hp + HP");

    // In selection: only matches inside the scope, which moves with edits.
    b.setText("hp hp\nhp hp\nhp hp");
    lua::FindOptions in;
    in.inSelection = true;
    b.setFindScope({0, 3}, {1, 5});
    CHECK(b.countMatches("hp", in) == 3 && b.countMatches("hp", lua::FindOptions{}) == 6);
    b.moveTo({2, 0});
    CHECK(b.replaceAll("hp", "health", in) == 3);
    CHECK(b.text() == "hp health\nhealth health\nhp hp");
    CHECK(b.cursor() == (lua::Pos{2, 0}));
    CHECK(b.findScope() && b.findScope()->first == (lua::Pos{0, 3}) && b.findScope()->second == (lua::Pos{1, 13}));
    // Again: the grown scope still holds exactly the replaced ones.
    CHECK(b.countMatches("health", in) == 3 && b.countMatches("hp", in) == 0);
    CHECK(b.undo() && b.text() == "hp hp\nhp hp\nhp hp" && b.findScope()->second == (lua::Pos{1, 5}));
    CHECK(b.redo() && b.undo() && !b.canUndo());
    // Typing before the scope shifts it; find stays inside.
    b.moveTo({0, 0});
    b.insert("xx");
    CHECK(b.findScope()->first == (lua::Pos{0, 5}));
    auto f = b.find("hp", {0, 0}, true, in);
    CHECK(f && f->first == (lua::Pos{0, 5}));
    // Replace (one) outside the scope does nothing; inside it walks the scope.
    b.select({0, 2}, {0, 4});
    CHECK(!b.replaceNext("hp", "Q", in) && b.selStart() == (lua::Pos{0, 5}));
    CHECK(b.replaceNext("hp", "Q", in) && b.line(0) == "xxhp Q" && b.selStart() == (lua::Pos{1, 0}));
    b.clearFindScope();
    CHECK(b.countMatches("hp", in) == 5);
    b.setFindScope({0, 0}, {0, 3});
    b.setText("hp");
    CHECK(!b.findScope());
}

// Typing brackets and quotes: pairs, stepping over, Backspace in a pair.
void testLuaAutoClose() {
    lua::Buffer b;
    auto typeAll = [&](const char* s) {
        for (const char* c = s; *c; ++c) b.type(std::string(1, *c));
    };
    typeAll("print(");
    CHECK(b.text() == "print()" && b.cursor() == (lua::Pos{0, 6}) && b.autoClosers() == 1);
    typeAll("x[1");
    CHECK(b.text() == "print(x[1])");
    typeAll("])");
    CHECK(b.text() == "print(x[1])" && b.cursor() == (lua::Pos{0, 11}) && b.autoClosers() == 0);
    // Quotes pair outside words; a typed closer steps over.
    b.setText("");
    typeAll("s = \"hi\"");
    CHECK(b.text() == "s = \"hi\"" && b.cursor() == (lua::Pos{0, 8}));
    typeAll(" -- don't");
    CHECK(b.text() == "s = \"hi\" -- don't");
    // Not inside a string or a comment.
    b.setText("x = \"a, b\"");
    b.moveTo({0, 7});
    typeAll("'");
    CHECK(b.text() == "x = \"a,' b\"");
    b.setText("-- note ");
    b.moveTo({0, 8});
    typeAll("\"");
    CHECK(b.text() == "-- note \"");
    // Brackets only before a blank, the end or punctuation.
    b.setText("foo");
    b.moveTo({0, 0});
    typeAll("(");
    CHECK(b.text() == "(foo");
    // Backspace between an empty pair deletes both; with text inside, one.
    b.setText("");
    typeAll("f({");
    CHECK(b.text() == "f({})");
    b.backspace();
    CHECK(b.text() == "f()" && b.cursor() == (lua::Pos{0, 2}));
    b.backspace();
    CHECK(b.text() == "f" && b.cursor() == (lua::Pos{0, 1}));
    typeAll("(ab");
    b.backspace();
    CHECK(b.text() == "f(a)");
    typeAll(")");
    CHECK(b.text() == "f(a)" && b.cursor() == (lua::Pos{0, 4}));
    // A closer it did not insert is typed, not stepped over.
    b.setText("g()");
    b.moveTo({0, 2});
    typeAll(")");
    CHECK(b.text() == "g())");
    b.setText("g()");
    b.moveTo({0, 2});
    b.backspace();
    CHECK(b.text() == "g)");
    // Another edit forgets the pairs: an undo, a newline.
    b.setText("");
    typeAll("h(");
    b.newline();
    b.move(lua::Buffer::Move::Down, false);
    b.move(lua::Buffer::Move::Home, false);
    CHECK(b.autoClosers() == 0);
    // A selection on one line gets wrapped and stays selected.
    b.setText("a = b");
    b.select({0, 4}, {0, 5});
    typeAll("(");
    CHECK(b.text() == "a = (b)" && b.selectedText() == "b");
    // The pair is one undo step.
    b.setText("");
    typeAll("[");
    CHECK(b.undo() && b.text().empty());
}

// An open script whose file changes on disk.
void testLuaReload() {
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "lua-reload";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "door.lua";
    auto write = [&](const char* text) {
        { std::ofstream(file, std::ios::binary) << text; }
        // A time of its own, whatever the filesystem's resolution.
        static int bump = 0;
        fs::last_write_time(file, fs::file_time_type::clock::now() + std::chrono::seconds(++bump), ec);
    };
    write("a = 1\nb = 2\nc = 3\n");
    lua::Script s;
    std::string err;
    CHECK(s.open(file, &err) && !s.changedOnDisk());
    s.buffer.moveTo({2, 3});
    // Changed above the cursor: reloaded, the cursor moves with its line.
    write("-- top\na = 1\nb = 2\nc = 3\n");
    CHECK(s.changedOnDisk());
    CHECK(s.reload(&err) && s.buffer.text() == "-- top\na = 1\nb = 2\nc = 3\n");
    CHECK(!s.dirty() && !s.changedOnDisk() && s.buffer.cursor() == (lua::Pos{3, 3}));
    // Undo gets the old text back, unsaved.
    CHECK(s.buffer.undo() && s.buffer.text() == "a = 1\nb = 2\nc = 3\n" && s.dirty());
    CHECK(s.buffer.redo() && !s.dirty());
    // Touched with the same text: no change.
    write("-- top\na = 1\nb = 2\nc = 3\n");
    CHECK(!s.changedOnDisk());
    // Keep mine: the change is not reported again; saving overwrites it.
    s.buffer.moveTo({0, 6});
    s.buffer.insert("!");
    write("other\n");
    CHECK(s.changedOnDisk() && s.dirty());
    s.keepMine();
    CHECK(!s.changedOnDisk() && s.dirty());
    CHECK(s.save(&err) && !s.changedOnDisk());
    // A save of our own is not a change; a missing file is not one either.
    fs::remove(file, ec);
    CHECK(!s.changedOnDisk());
    write("back\r\n");
    CHECK(s.changedOnDisk() && s.reload(&err) && s.buffer.text() == "back\n");
    CHECK(s.save(&err));
    std::ifstream in(file, std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == "back\r\n");
}

// A file deleted under its tab is reported until it is saved again or comes
// back; a script for a file not yet written is not deleted.
void testLuaDeleted() {
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "lua-deleted";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "door.lua";
    { std::ofstream(file, std::ios::binary) << "a = 1\n"; }
    lua::Script s;
    std::string err;
    CHECK(s.open(file, &err) && !s.deletedOnDisk());
    fs::remove(file, ec);
    CHECK(s.deletedOnDisk() && !s.changedOnDisk() && !s.dirty());
    // Save writes it again, the text as it was.
    CHECK(s.save(&err) && !s.deletedOnDisk() && fs::exists(file));
    std::ifstream in(file, std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(in)), {}) == "a = 1\n");
    in.close();
    // Deleted, then written back by another program: no longer deleted.
    fs::remove(file, ec);
    CHECK(s.deletedOnDisk());
    { std::ofstream(file, std::ios::binary) << "a = 1\n"; }
    CHECK(!s.deletedOnDisk());
    // Moved away is the same as deleted.
    fs::rename(file, dir / "moved.lua", ec);
    CHECK(s.deletedOnDisk());
    // A new script's file does not exist until it is saved: not deleted.
    lua::Script fresh;
    CHECK(fresh.open(dir / "new.lua", &err) && !fresh.deletedOnDisk());
    fresh.buffer.insert("x = 1");
    CHECK(fresh.save(&err) && !fresh.deletedOnDisk());
    fs::remove(dir / "new.lua", ec);
    CHECK(fresh.deletedOnDisk());
}

void testEditMany() {
    editor::Document d;
    d.reset(sample());
    CHECK(d.editMany({{0}, {1, 0}, {9}}, [](splash::Object& o) { o.transform.position.y = 5; }) == 2);
    CHECK(d.object({0})->transform.position.y == 5 && d.object({1, 0})->transform.position.y == 5);
    CHECK(d.object({1})->transform.position.y == 0 && d.object({1})->children.size() == 1);
    CHECK(d.undo());
    CHECK(d.object({0})->transform.position.y == 0 && d.object({1, 0})->transform.position.y == 0);
    CHECK(!d.undo());
    CHECK(d.redo() && d.object({1, 0})->transform.position.y == 5);
    CHECK(d.editMany({}, [](splash::Object&) {}) == 0);
}

// Copy, cut and paste of objects as clipboard text.
void testClipboard() {
    editor::Document d;
    d.reset(sample());
    const std::string text = d.copyObject({1});  // B with child B1
    CHECK(!text.empty());
    CHECK(d.copyObject({9}).empty());

    // Paste after the original: numbered, children carried, selected, one undo step.
    size_t hist = d.historySize();
    auto p = d.pasteObjects(text, editor::ObjectPath{1});
    CHECK(p && *p == editor::ObjectPath({2}));
    CHECK(d.historySize() == hist + 1);
    CHECK(d.selection() == p);
    const splash::Object* pasted = d.object({2});
    CHECK(pasted && pasted->name == "B (2)");
    CHECK(pasted && pasted->children.size() == 1 && pasted->children[0].name == "B1");
    CHECK(d.object({3}) && d.object({3})->name == "C");
    CHECK(d.objectCount() == 6);
    // Moved one grid step along X off the original it would cover.
    CHECK(pasted && pasted->transform.position.x == 2.25f);
    if (pasted) {
        splash::Object same = *pasted;
        same.name = "B";
        same.transform.position.x = 2;
        CHECK(splash::writeObjectsText({same}) == text);  // equal to the original but for the name and place
    }
    CHECK(d.undo());
    CHECK(d.scene().objects.size() == 3 && d.object({2})->name == "C" && d.objectCount() == 4);
    CHECK(d.redo() && d.object({2})->name == "B (2)" && d.object({2, 0}));
    CHECK(d.undo());

    // Several objects at the end of the top level: a free name is kept, a
    // taken one numbered, and the whole paste undoes as one step.
    splash::Object z = named("Z", 7);
    z.children.push_back(named("Z1"));
    auto q = d.pasteObjects(splash::writeObjectsText({named("A"), z}), std::nullopt);
    CHECK(q && *q == editor::ObjectPath({4}));
    CHECK(d.scene().objects.size() == 5 && d.object({3})->name == "A (2)" && d.object({4})->name == "Z");
    CHECK(d.object({4, 0}) && d.object({4, 0})->name == "Z1");
    CHECK(d.selection() == q);
    CHECK(d.undo());
    CHECK(d.scene().objects.size() == 3 && d.objectCount() == 4);
    CHECK(!d.canUndo());

    // Into a child level, after B1; asset paths stay as they were.
    splash::Object m = named("B1");
    m.mesh = splash::MeshComponent{};
    m.mesh->mesh = "meshes/crate.mesh";
    m.script = splash::ScriptComponent{};
    m.script->lua = "scripts/crate.lua";
    auto r = d.pasteObjects(splash::writeObjectsText({m}), editor::ObjectPath{1, 0});
    CHECK(r && *r == editor::ObjectPath({1, 1}));
    const splash::Object* mo = d.object({1, 1});
    CHECK(mo && mo->name == "B1 (2)" && mo->mesh && mo->mesh->mesh == "meshes/crate.mesh");
    CHECK(mo && mo->script && mo->script->lua == "scripts/crate.lua");
    CHECK(d.undo());

    // Text that is not ours changes nothing.
    const unsigned rev = d.revision();
    hist = d.historySize();
    d.select(editor::ObjectPath{0});
    const std::string truncated = text.substr(0, text.size() / 2);
    for (const std::string& t : {std::string(), std::string("Crate"), std::string("{}"), std::string("[1, 2]"),
                                 std::string(R"({"splashedit-objects": 2, "objects": [{"name": "X"}]})"),
                                 std::string(R"({"splashedit-objects": "1", "objects": [{"name": "X"}]})"),
                                 std::string(R"({"splashedit-objects": 1, "objects": []})"),
                                 std::string(R"({"splashedit-objects": 1, "objects": [{"nom": "X"}]})"),
                                 std::string(R"({"splashedit-objects": 1, "objects": [{"name": "X"}, 3]})"),
                                 std::string(R"({"format": "splashedit-ng/scene", "objects": [{"name": "X"}]})"),
                                 truncated})
        CHECK(!d.pasteObjects(t, editor::ObjectPath{0}));
    CHECK(d.revision() == rev && d.historySize() == hist);
    CHECK(d.scene().objects.size() == 3 && d.objectCount() == 4);
    CHECK(d.selection() == editor::ObjectPath({0}));

    // Cut: the copy's text, the object gone in one step; undo brings it back.
    d.reset(sample());
    d.select(editor::ObjectPath{1});
    CHECK(d.cutObject({1}) == text);
    CHECK(d.scene().objects.size() == 2 && d.object({1})->name == "C" && !d.selection());
    CHECK(d.historySize() == 1);
    CHECK(d.undo());
    CHECK(d.object({1})->name == "B" && d.object({1, 0}) && d.object({1, 0})->name == "B1");
    CHECK(!d.canUndo());
    CHECK(d.cutObject({9}).empty() && d.historySize() == 1);
    // Cut and paste elsewhere keeps the name when it is free there.
    CHECK(d.redo());
    auto s = d.pasteObjects(text, editor::ObjectPath{1});
    CHECK(s && *s == editor::ObjectPath({2}) && d.object({2})->name == "B" && d.object({2, 0}));
}

// Timeline model: every edit one undo step, keys kept sorted, and sampling
// matching psxsplash's interpolation.cpp.
bool near3(const editor::KeyValue& v, float x, float y, float z) {
    return std::fabs(v[0] - x) < 1e-4f && std::fabs(v[1] - y) < 1e-4f && std::fabs(v[2] - z) < 1e-4f;
}

splash::CutsceneTrack trackOf(splash::TrackType type, std::initializer_list<std::pair<int, float>> keys,
                              splash::Interp interp = splash::Interp::Linear) {
    splash::CutsceneTrack t;
    t.type = type;
    t.target = "A";
    for (auto [f, v] : keys) {
        splash::Keyframe k;
        k.frame = f;
        k.value = {v, 2 * v, 0};
        k.interp = interp;
        t.keyframes.push_back(k);
    }
    return t;
}

void testTimelineSample() {
    using splash::Interp;
    using splash::TrackType;
    // Curves, as psxsplash's applyCurve at 0.12 fixed point.
    CHECK(editor::applyCurve(0.25f, Interp::Linear) == 0.25f);
    CHECK(editor::applyCurve(0.75f, Interp::Step) == 0.0f);
    CHECK(std::fabs(editor::applyCurve(0.5f, Interp::EaseIn) - 0.25f) < 1e-6f);
    CHECK(std::fabs(editor::applyCurve(0.5f, Interp::EaseOut) - 0.75f) < 1e-6f);
    CHECK(std::fabs(editor::applyCurve(0.25f, Interp::EaseInOut) - 0.15625f) < 1e-6f);
    for (Interp m : {Interp::Linear, Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut})
        CHECK(std::fabs(editor::applyCurve(1.0f, m) - 1.0f) < 1e-6f && editor::applyCurve(0.0f, m) == 0.0f);

    // Linear between keys; holds after the last; the first key from frame 0.
    splash::CutsceneTrack lin = trackOf(TrackType::ObjectPosition, {{0, 0}, {10, 10}, {20, 30}});
    CHECK(near3(editor::sample(lin, 5), 5, 10, 0));
    CHECK(near3(editor::sample(lin, 15), 20, 40, 0));
    CHECK(near3(editor::sample(lin, 10), 10, 20, 0));
    CHECK(near3(editor::sample(lin, 7.5f), 7.5f, 15, 0));  // sub-frame
    CHECK(near3(editor::sample(lin, 99), 30, 60, 0));
    CHECK(near3(editor::sample(lin, 0), 0, 0, 0));

    // A segment takes the later key's curve: Step on key 1 holds key 0 up to it.
    splash::CutsceneTrack st = lin;
    st.keyframes[1].interp = Interp::Step;
    CHECK(near3(editor::sample(st, 9), 0, 0, 0));
    CHECK(near3(editor::sample(st, 10), 10, 20, 0));
    CHECK(near3(editor::sample(st, 15), 20, 40, 0));  // key 2 is still linear
    splash::CutsceneTrack ease = lin;
    ease.keyframes[1].interp = Interp::EaseIn;
    CHECK(near3(editor::sample(ease, 5), 2.5f, 5, 0));
    ease.keyframes[1].interp = Interp::EaseOut;
    CHECK(near3(editor::sample(ease, 5), 7.5f, 15, 0));

    // Before a first key past frame 0: blend from the initial state with its curve.
    splash::CutsceneTrack late = trackOf(TrackType::ObjectPosition, {{10, 10}, {20, 20}});
    CHECK(near3(editor::sample(late, 5, {0, 0, 4}), 5, 10, 2));
    CHECK(near3(editor::sample(late, 0, {0, 0, 4}), 0, 0, 4));
    late.keyframes[0].interp = Interp::Step;
    CHECK(near3(editor::sample(late, 9, {1, 1, 1}), 1, 1, 1));
    // Unsorted keys sample as the pack sorts them.
    splash::CutsceneTrack shuffled = trackOf(TrackType::ObjectPosition, {{20, 30}, {0, 0}, {10, 10}});
    CHECK(near3(editor::sample(shuffled, 15), 20, 40, 0));
    // No keys: the target is left as it was.
    CHECK(near3(editor::sample(trackOf(TrackType::ObjectPosition, {}), 3, {7, 8, 9}), 7, 8, 9));

    // On/off tracks hold the last key at or before the whole frame; initial before the first.
    splash::CutsceneTrack act = trackOf(TrackType::ObjectActive, {{5, 1}, {10, 0}});
    CHECK(editor::sample(act, 4.9f, {0, 0, 0})[0] == 0);
    CHECK(editor::sample(act, 5, {0, 0, 0})[0] == 1);
    CHECK(editor::sample(act, 9.9f)[0] == 1);
    CHECK(editor::sample(act, 10)[0] == 0);
    CHECK(editor::sample(trackOf(TrackType::RumbleSmall, {{0, 1}}), 3)[0] == 1);
    // ...and the large motor interpolates.
    CHECK(std::fabs(editor::sample(trackOf(TrackType::RumbleLarge, {{0, 0}, {10, 200}}), 5)[0] - 100) < 1e-4f);
    CHECK(editor::isDiscrete(TrackType::LightEnabled) && !editor::isDiscrete(TrackType::LightIntensity));
}

void testTimelineEdit() {
    using splash::Interp;
    using splash::TrackType;
    splash::Scene sc;
    sc.objects.push_back(named("Crate", 3));
    splash::Cutscene cs;
    cs.name = "intro";
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    CHECK(editor::valid(d, ref) && !editor::valid(d, {editor::SeqKind::Animation, 0}));
    CHECK(editor::seqLength(d, ref) == 90 && *editor::seqName(d, ref) == "intro");

    // Tracks: one undo step each; duplicates, missing targets and camera tracks in animations refused.
    size_t h = d.historySize();
    CHECK(editor::addTrack(d, ref, TrackType::ObjectPosition, "Crate") == 0);
    CHECK(d.historySize() == h + 1 && d.dirty());
    CHECK(editor::addTrack(d, ref, TrackType::ObjectPosition, "Crate") == -1);
    CHECK(editor::addTrack(d, ref, TrackType::ObjectRotation, "") == -1);
    CHECK(editor::addTrack(d, ref, TrackType::CameraPosition, "ignored") == 1);
    CHECK(editor::seqTracks(d, ref)->at(1).target.empty());
    CHECK(editor::trackLabel(editor::seqTracks(d, ref)->at(0)) == "Crate: Position");
    CHECK(editor::trackLabel(editor::seqTracks(d, ref)->at(1)) == "Camera: Position");
    CHECK(d.historySize() == h + 2);
    CHECK(d.undo() && editor::seqTracks(d, ref)->size() == 1);
    for (int i = 0; i < 7; ++i) CHECK(editor::addTrack(d, ref, TrackType::ObjectActive, "o" + std::to_string(i)) == i + 1);
    CHECK(editor::addTrack(d, ref, TrackType::ObjectActive, "ninth") == -1);  // 8 tracks at most
    for (int i = 0; i < 7; ++i) CHECK(editor::removeTrack(d, ref, 1));
    CHECK(!editor::removeTrack(d, ref, 5));
    CHECK(editor::seqTracks(d, ref)->size() == 1);

    // Keys: added sorted; with no value they take the sampled one (the object's
    // world position before any key), so adding one leaves the motion as it was.
    h = d.historySize();
    auto k0 = editor::addKey(d, ref, 0, 30);
    CHECK(k0 && *k0 == (editor::KeyRef{0, 0}));
    CHECK(near3(editor::key(d, ref, *k0)->value, 3, 0, 0));
    CHECK(editor::addKey(d, ref, 0, 0, editor::KeyValue{0, 0, 0}) == (editor::KeyRef{0, 0}));
    CHECK(editor::addKey(d, ref, 0, 60, editor::KeyValue{6, 0, 0}) == (editor::KeyRef{0, 2}));
    CHECK(!editor::addKey(d, ref, 0, 60));  // the frame has a key
    CHECK(!editor::addKey(d, ref, 3, 10));  // no such track
    CHECK(d.historySize() == h + 3);
    auto added = editor::addKey(d, ref, 0, 45);
    CHECK(added && near3(editor::key(d, ref, *added)->value, 4.5f, 0, 0));  // between 3 at 30 and 6 at 60
    CHECK(d.undo() && editor::seqTracks(d, ref)->at(0).keyframes.size() == 3);
    CHECK(editor::addKey(d, ref, 0, 99999, editor::KeyValue{1, 1, 1})->key == 3);
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes.back().frame == editor::kMaxTimelineFrame);
    CHECK(d.undo());

    // Move: re-sorts and reports the new addresses; collisions and range refused.
    auto moved = editor::moveKeys(d, ref, {{0, 0}}, 40);  // 0 -> 40, past the key at 30
    CHECK(moved && moved->size() == 1 && (*moved)[0] == (editor::KeyRef{0, 1}));
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes[0].frame == 30);
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes[1].frame == 40);
    CHECK(!editor::moveKeys(d, ref, {{0, 1}}, 20));  // onto 60
    CHECK(!editor::moveKeys(d, ref, {{0, 0}}, -31));  // below frame 0
    CHECK(editor::moveKeys(d, ref, {{0, 1}, {0, 2}}, 20));  // both move: 40->60 is free while 60 moves
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes[2].frame == 80);
    CHECK(d.undo() && d.undo() && editor::seqTracks(d, ref)->at(0).keyframes[0].frame == 0);

    // A drag merges into one step until endMerge.
    editor::KeyRef drag{0, 1};
    for (int i = 0; i < 5; ++i) {
        auto r = editor::moveKeys(d, ref, {drag}, 1, "drag");
        CHECK(r.has_value());
        if (r) drag = (*r)[0];
    }
    d.endMerge();
    // One step: nothing to redo after it, and one undo takes the whole drag back.
    CHECK(!d.canRedo() && editor::key(d, ref, drag)->frame == 35);
    CHECK(d.undo() && editor::seqTracks(d, ref)->at(0).keyframes[1].frame == 30);

    // Value, curve and length.
    CHECK(editor::setKeyValue(d, ref, {0, 1}, {1, 2, 3}));
    CHECK(!editor::setKeyValue(d, ref, {0, 1}, {1, 2, 3}));  // no change, no step
    CHECK(editor::setKeyInterp(d, ref, {0, 1}, Interp::EaseInOut));
    CHECK(editor::key(d, ref, {0, 1})->interp == Interp::EaseInOut);
    CHECK(editor::setLength(d, ref, 120) && editor::seqLength(d, ref) == 120);
    CHECK(editor::setLength(d, ref, 0) && editor::seqLength(d, ref) == 1);
    CHECK(d.undo() && d.undo() && editor::seqLength(d, ref) == 90);
    CHECK(d.undo() && editor::key(d, ref, {0, 1})->interp == Interp::Linear);
    CHECK(d.undo() && near3(editor::key(d, ref, {0, 1})->value, 3, 0, 0));

    // Delete several at once, one step.
    CHECK(editor::deleteKeys(d, ref, {{0, 0}, {0, 2}, {0, 9}}) == 2);
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes.size() == 1);
    CHECK(editor::seqTracks(d, ref)->at(0).keyframes[0].frame == 30);
    CHECK(d.undo() && editor::seqTracks(d, ref)->at(0).keyframes.size() == 3);
    CHECK(editor::deleteKeys(d, ref, {}) == 0 && d.canRedo());  // refused: the redo is still there

    // Sequences: unique names; animations refuse camera tracks.
    auto anim = editor::addSequence(d, editor::SeqKind::Animation, "spin");
    CHECK(anim && anim->index == 0 && d.scene().animations.size() == 1);
    CHECK(editor::addTrack(d, *anim, TrackType::CameraRotation, "") == -1);
    CHECK(editor::addTrack(d, *anim, TrackType::ObjectRotation, "Crate") == 0);
    auto again = editor::addSequence(d, editor::SeqKind::Cutscene, "intro");
    CHECK(again && *editor::seqName(d, *again) == "intro (2)");
    CHECK(editor::allSequences(d).size() == 3);
    CHECK(d.undo() && d.undo() && d.undo() && d.scene().animations.empty());

    // The edits survive a save and load.
    const std::filesystem::path dir = g_outDir / "timeline";
    std::filesystem::create_directories(dir);
    splash::saveScene(d.scene(), dir / "t.scene");
    splash::Scene back = splash::loadScene(dir / "t.scene");
    CHECK(back.cutscenes.size() == 1 && back.cutscenes[0].tracks.size() == 1);
    CHECK(back.cutscenes[0].tracks[0].keyframes.size() == 3 && back.cutscenes[0].tracks[0].keyframes[2].frame == 60);
}

// The Scene view's preview: a sequence posed at a frame, drawn from a copy;
// the document, its history and the saved scene stay as they were.
// Sequences renamed and deleted, one undo step each; names held to the
// exporter's rule (1..24 UTF-16 units, unique within the kind).
void testTimelineRenameDelete() {
    using editor::SeqKind;
    splash::Scene sc;
    for (const char* n : {"intro", "outro"}) {
        splash::Cutscene c;
        c.name = n;
        sc.cutscenes.push_back(c);
    }
    splash::Animation a;
    a.name = "spin";
    sc.animations.push_back(a);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef intro{SeqKind::Cutscene, 0}, outro{SeqKind::Cutscene, 1}, spin{SeqKind::Animation, 0};
    size_t h = d.historySize();
    CHECK(editor::renameSequence(d, intro, "opening") && *editor::seqName(d, intro) == "opening");
    CHECK(d.historySize() == h + 1);
    CHECK(d.undo() && *editor::seqName(d, intro) == "intro" && d.redo() && *editor::seqName(d, intro) == "opening");
    h = d.historySize();
    // Refused: empty, taken by the same kind, too long, unchanged. No step.
    CHECK(!editor::renameSequence(d, intro, ""));
    CHECK(!editor::renameSequence(d, intro, "outro") && !editor::seqNameProblem(d, intro, "outro").empty());
    CHECK(!editor::renameSequence(d, intro, std::string(25, 'x')));
    CHECK(!editor::renameSequence(d, intro, "opening"));
    CHECK(d.historySize() == h && *editor::seqName(d, intro) == "opening");
    // 24 units is fine; an astral character counts two (String.Substring).
    CHECK(editor::seqNameProblem(d, intro, std::string(24, 'x')).empty());
    CHECK(!editor::seqNameProblem(d, intro, std::string(23, 'x') + "\xF0\x9F\x8E\xAC").empty());
    CHECK(editor::seqNameProblem(d, intro, std::string(22, 'x') + "\xF0\x9F\x8E\xAC").empty());
    CHECK(editor::seqNameProblem(d, intro, std::string(23, 'x') + "\xC3\xA9").empty());
    // Another kind may share the name: the exporter checks each kind on its own.
    CHECK(editor::renameSequence(d, spin, "outro") && *editor::seqName(d, spin) == "outro");

    // Delete: one step, the later ones move down, undo brings it back in place.
    h = d.historySize();
    CHECK(editor::removeSequence(d, intro));
    CHECK(d.historySize() == h + 1 && d.scene().cutscenes.size() == 1 && *editor::seqName(d, intro) == "outro");
    CHECK(!editor::removeSequence(d, {SeqKind::Cutscene, 5}) && d.historySize() == h + 1);
    CHECK(d.undo() && d.scene().cutscenes.size() == 2 && *editor::seqName(d, intro) == "opening" &&
          *editor::seqName(d, outro) == "outro");
    CHECK(editor::removeSequence(d, spin) && d.scene().animations.empty() && d.scene().cutscenes.size() == 2);
}

// A track pointed at another target: one step, keys kept; refused where the
// export could not tell the tracks apart or the track has no target.
// Box selections: keys on several lanes and events move together and
// delete together, one undo step each; refused moves change nothing.
void testTimelineSelection() {
    using splash::TrackType;
    splash::Scene sc;
    splash::Cutscene cs;
    cs.name = "intro";
    cs.tracks.push_back(trackOf(TrackType::ObjectPosition, {{0, 0}, {30, 1}, {60, 2}}));
    cs.tracks.push_back(trackOf(TrackType::ObjectRotation, {{10, 0}, {40, 1}}));
    cs.audioEvents.push_back({});
    cs.audioEvents[0].frame = 20;
    cs.audioEvents[0].clip = "bell";
    cs.skinEvents.push_back({});
    cs.skinEvents[0].frame = 5;
    cs.skinEvents[0].object = "Arm";
    cs.skinEvents[0].clip = "bend";
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    auto frames = [&](int t) {
        std::vector<int> f;
        for (const auto& k : editor::seqTracks(d, ref)->at(size_t(t)).keyframes) f.push_back(k.frame);
        return f;
    };

    // Keys on two lanes and both events, moved +5 as one step.
    editor::Selection sel;
    sel.keys = {{0, 1}, {1, 0}, {1, 1}};
    sel.audio = {0};
    sel.skin = {0};
    size_t h = d.historySize();
    auto moved = editor::moveSelection(d, ref, sel, 5);
    CHECK(moved && d.historySize() == h + 1);
    CHECK(frames(0) == std::vector<int>({0, 35, 60}) && frames(1) == std::vector<int>({15, 45}));
    CHECK(editor::seqAudio(d, ref)->at(0).frame == 25 && editor::seqSkin(d, ref)->at(0).frame == 10);
    CHECK(moved && moved->keys == sel.keys && moved->audio == sel.audio && moved->skin == sel.skin);
    // A drag merges; past frame 0 (the skin event at 10 cannot go -11) or onto a resting key is refused whole.
    CHECK(!editor::moveSelection(d, ref, *moved, -11));
    CHECK(!editor::moveSelection(d, ref, {{{0, 1}}, {}, {}}, 25));  // 35 -> 60, taken
    CHECK(d.historySize() == h + 1 && frames(1) == std::vector<int>({15, 45}));
    editor::Selection cur = *moved;
    for (int i = 0; i < 3; ++i) {
        auto r = editor::moveSelection(d, ref, cur, 1, "box");
        CHECK(r.has_value());
        if (r) cur = *r;
    }
    d.endMerge();
    CHECK(d.historySize() == h + 2 && frames(0) == std::vector<int>({0, 38, 60}));
    CHECK(d.undo() && d.undo() && frames(0) == std::vector<int>({0, 30, 60}) && editor::seqAudio(d, ref)->at(0).frame == 20);
    // An index past the end refuses the move.
    CHECK(!editor::moveSelection(d, ref, {{}, {3}, {}}, 1));

    // Delete keys from both lanes and both events: one step, all back with one undo.
    CHECK(!d.canUndo());  // back to the scene as loaded
    CHECK(editor::deleteSelection(d, ref, sel) == 5);
    CHECK(d.historySize() == 1 && frames(0) == std::vector<int>({0, 60}) && frames(1).empty());
    CHECK(editor::seqAudio(d, ref)->empty() && editor::seqSkin(d, ref)->empty());
    CHECK(editor::deleteSelection(d, ref, {}) == 0 && d.historySize() == 1);
    CHECK(d.undo() && !d.canUndo() && frames(1) == std::vector<int>({10, 40}) && editor::seqAudio(d, ref)->size() == 1);

    // Animations have no audio: a selection naming one is refused.
    splash::Animation an;
    an.name = "spin";
    an.tracks.push_back(trackOf(TrackType::ObjectRotation, {{0, 0}}));
    sc.animations.push_back(an);
    d.reset(sc);
    const editor::SeqRef aref{editor::SeqKind::Animation, 0};
    CHECK(!editor::seqAudio(d, aref) && editor::seqSkin(d, aref)->empty());
    CHECK(!editor::moveSelection(d, aref, {{{0, 0}}, {0}, {}}, 1));
    CHECK(editor::moveSelection(d, aref, {{{0, 0}}, {}, {}}, 1));
}

// Copy and paste at the scrub head: keys land in the track with their type
// and target, events come along, one step; limits refuse the whole paste.
void testTimelineClipboard() {
    using splash::TrackType;
    splash::Scene sc;
    splash::Cutscene cs;
    cs.name = "intro";
    cs.tracks.push_back(trackOf(TrackType::ObjectPosition, {{10, 1}, {20, 2}}));
    cs.tracks.push_back(trackOf(TrackType::ObjectRotation, {{15, 3}, {100, 4}}));
    cs.audioEvents.push_back({});
    cs.audioEvents[0].frame = 12;
    cs.audioEvents[0].clip = "bell";
    cs.audioEvents[0].volume = 80;
    sc.cutscenes.push_back(cs);
    splash::Cutscene other;
    other.name = "outro";
    other.tracks.push_back(trackOf(TrackType::ObjectRotation, {}));
    sc.cutscenes.push_back(other);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};

    editor::Selection sel{{{0, 0}, {0, 1}, {1, 0}}, {0}, {}};
    const editor::TimelineClipboard clip = editor::copySelection(d, ref, sel);
    CHECK(clip.keys.size() == 3 && clip.audio.size() == 1 && clip.skin.empty());
    CHECK(clip.keys[0].key.frame == 0 && clip.keys[1].key.frame == 10 && clip.keys[2].key.frame == 5);
    CHECK(clip.audio[0].frame == 2 && clip.audio[0].volume == 80);

    size_t h = d.historySize();
    int skipped = -1;
    auto pasted = editor::pasteClipboard(d, ref, clip, 50, &skipped);
    CHECK(pasted && skipped == 0 && d.historySize() == h + 1);
    CHECK(pasted && pasted->size() == 4);
    const auto& t0 = editor::seqTracks(d, ref)->at(0).keyframes;
    CHECK(t0.size() == 4 && t0[2].frame == 50 && t0[3].frame == 60 && near3(t0[3].value, 2, 4, 0));
    CHECK(editor::seqTracks(d, ref)->at(1).keyframes[1].frame == 55);
    CHECK(editor::seqAudio(d, ref)->size() == 2 && editor::seqAudio(d, ref)->at(1).frame == 52);
    // The pasted keys are the selection afterwards.
    for (const editor::KeyRef& k : pasted->keys) CHECK(editor::key(d, ref, k) && editor::key(d, ref, k)->frame >= 50);
    // On a frame that has a key, the pasted key replaces it.
    CHECK(d.undo());
    auto over = editor::pasteClipboard(d, ref, clip, 90);  // the rotation key lands on 95; position on 90, 100
    CHECK(over && editor::seqTracks(d, ref)->at(1).keyframes.size() == 3);
    CHECK(d.undo());
    auto onto = editor::pasteClipboard(d, ref, clip, 95);  // rotation key at 100 replaced
    CHECK(onto && editor::seqTracks(d, ref)->at(1).keyframes.size() == 2 &&
          near3(editor::seqTracks(d, ref)->at(1).keyframes[1].value, 3, 6, 0));
    CHECK(d.undo());
    // Past the last frame: refused, no step.
    h = d.historySize();
    CHECK(!editor::pasteClipboard(d, ref, clip, editor::kMaxTimelineFrame - 5) && d.historySize() == h);
    // Into another cutscene: the rotation key goes to its rotation track (the
    // position keys get a track of their own: testTimelinePasteAddsTrack).
    const editor::SeqRef out{editor::SeqKind::Cutscene, 1};
    auto there = editor::pasteClipboard(d, out, clip, 0, &skipped);
    CHECK(there && skipped == 0 && there->keys.size() == 3 && there->audio.size() == 1);
    CHECK(editor::seqTracks(d, out)->at(0).keyframes.size() == 1 && editor::seqTracks(d, out)->at(0).keyframes[0].frame == 5);
    CHECK(d.undo());
    // A track at 64 keys takes no more.
    for (int i = 0; i < editor::kMaxTimelineKeys - 2; ++i) CHECK(editor::addKey(d, ref, 0, 200 + i, editor::KeyValue{0, 0, 0}));
    h = d.historySize();
    CHECK(!editor::pasteClipboard(d, ref, clip, 1000) && d.historySize() == h);
}

// Pasting a key into a sequence without its track adds the track in the same
// undo step; a camera key has no track to go to in an animation; the tracks
// stay within the runtime's eight.
void testTimelinePasteAddsTrack() {
    using splash::TrackType;
    splash::Scene sc;
    splash::Cutscene cs;
    cs.name = "intro";
    cs.tracks.push_back(trackOf(TrackType::ObjectPosition, {{10, 1}, {20, 2}}));
    splash::CutsceneTrack cam = trackOf(TrackType::CameraPosition, {{30, 5}});
    cam.target = "";
    cs.tracks.push_back(cam);
    sc.cutscenes.push_back(cs);
    splash::Cutscene empty;
    empty.name = "outro";
    sc.cutscenes.push_back(empty);
    splash::Animation an;
    an.name = "wave";
    sc.animations.push_back(an);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    const editor::SeqRef out{editor::SeqKind::Cutscene, 1};
    const editor::SeqRef anim{editor::SeqKind::Animation, 0};
    const editor::TimelineClipboard clip = editor::copySelection(d, ref, {{{0, 0}, {0, 1}, {1, 0}}, {}, {}});
    CHECK(clip.keys.size() == 3);

    // An empty cutscene: both tracks are added, keys in place, one step.
    size_t h = d.historySize();
    int skipped = -1;
    auto pasted = editor::pasteClipboard(d, out, clip, 40, &skipped);
    CHECK(pasted && skipped == 0 && pasted->keys.size() == 3 && d.historySize() == h + 1);
    const auto* tracks = editor::seqTracks(d, out);
    CHECK(tracks->size() == 2);
    if (tracks->size() == 2) {
        CHECK(tracks->at(0).type == TrackType::ObjectPosition && tracks->at(0).target == "A");
        CHECK(tracks->at(0).keyframes.size() == 2 && tracks->at(0).keyframes[0].frame == 40 && tracks->at(0).keyframes[1].frame == 50);
        CHECK(near3(tracks->at(0).keyframes[1].value, 2, 4, 0));
        CHECK(tracks->at(1).type == TrackType::CameraPosition && tracks->at(1).target.empty());
        CHECK(tracks->at(1).keyframes.size() == 1 && tracks->at(1).keyframes[0].frame == 60);
    }
    for (const editor::KeyRef& k : pasted ? pasted->keys : std::vector<editor::KeyRef>())
        CHECK(editor::key(d, out, k) && editor::key(d, out, k)->frame >= 40);
    // One undo takes the keys and the added tracks back.
    CHECK(d.undo() && editor::seqTracks(d, out)->empty());
    // Pasting again finds the tracks the first paste added.
    CHECK(d.redo() && editor::pasteClipboard(d, out, clip, 100) && editor::seqTracks(d, out)->size() == 2 &&
          editor::seqTracks(d, out)->at(0).keyframes.size() == 4);

    // An animation has no camera tracks: the position track is added, the camera key skipped.
    auto inAnim = editor::pasteClipboard(d, anim, clip, 0, &skipped);
    CHECK(inAnim && skipped == 1 && inAnim->keys.size() == 2);
    CHECK(editor::seqTracks(d, anim)->size() == 1 && editor::seqTracks(d, anim)->at(0).type == TrackType::ObjectPosition);
    CHECK(d.undo());
    // Only a camera key: nothing can land, refused with no step.
    editor::TimelineClipboard camOnly;
    camOnly.keys = {clip.keys[2]};
    h = d.historySize();
    CHECK(!editor::pasteClipboard(d, anim, camOnly, 0, &skipped) && skipped == 1 && d.historySize() == h);

    // A sequence with the runtime's eight tracks takes no ninth: refused, no step.
    for (int i = 0; i < editor::kMaxTimelineTracks; ++i) CHECK(editor::addTrack(d, anim, TrackType::ObjectActive, "T" + std::to_string(i)) == i);
    h = d.historySize();
    CHECK(!editor::pasteClipboard(d, anim, clip, 0) && d.historySize() == h && editor::seqTracks(d, anim)->size() == 8);
}

// A skin event whose object or clip is gone is a problem the export refuses,
// and the timeline draws it red with that problem in its tooltip.
void testTimelineSkinEventProblem() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skin";
    splash::Scene sc = splash::loadScene(project / "skin.scene");
    editor::Document d;
    d.reset(sc, project);
    std::vector<editor::SkinTarget> targets = editor::skinTargets(d);
    CHECK(targets.size() == 1);
    splash::SkinAnimEvent e;
    e.object = "Arm";
    e.clip = "raise";
    CHECK(editor::skinEventProblem(targets, e).empty());
    e.clip = "wave";
    const std::string noClip = editor::skinEventProblem(targets, e);
    CHECK(noClip.find("'Arm' has no clip named 'wave'") != std::string::npos);
    e.clip = "raise";
    e.object = "Leg";
    CHECK(editor::skinEventProblem(targets, e).find("No active skinned object is named 'Leg'") != std::string::npos);
    // The object deleted (or made inactive): what was fine is now missing.
    e.object = "Arm";
    splash::Scene gone = sc;
    for (splash::Object& o : gone.objects)
        if (o.name == "Arm") o.active = false;
    d.reset(gone, project);
    CHECK(editor::skinEventProblem(editor::skinTargets(d), e).find("No active skinned object is named 'Arm'") != std::string::npos);
    // Two of that name: the export cannot tell them apart.
    targets.push_back(targets[0]);
    CHECK(editor::skinEventProblem(targets, e).find("Two skinned objects are named 'Arm'") != std::string::npos);
}

// Audio and skin clip events: added, edited and limited as the exporter
// limits them (64 audio events in a cutscene, none in an animation, 16 skin
// events, volume 0..128, pan 0..127), one undo step each.
void testTimelineEvents() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skin";
    splash::Scene sc = splash::loadScene(project / "skin.scene");
    splash::Object bell = named("Bell");
    bell.audio = splash::AudioComponent{};
    bell.audio->clipName = "bell";
    sc.objects.push_back(bell);
    splash::Object off = named("Off");
    off.active = false;
    off.audio = splash::AudioComponent{};
    off.audio->clipName = "muted";
    sc.objects.push_back(off);
    splash::Cutscene cs;
    cs.name = "intro";
    sc.cutscenes.push_back(cs);
    splash::Animation an;
    an.name = "wave";
    sc.animations.push_back(an);
    editor::Document d;
    d.reset(sc, project);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    const editor::SeqRef aref{editor::SeqKind::Animation, 0};

    // What the add menus list: the export's clip table and skinned objects' clip names.
    CHECK(editor::audioClips(d) == std::vector<std::string>({"bell"}));
    const std::vector<editor::SkinTarget> skins = editor::skinTargets(d);
    CHECK(skins.size() == 1 && skins[0].object == "Arm" && skins[0].clips == std::vector<std::string>({"bend", "raise"}));

    size_t h = d.historySize();
    CHECK(editor::addAudioEvent(d, ref, 30, "bell") == 0);
    CHECK(d.historySize() == h + 1);
    const splash::CutsceneAudioEvent& a = editor::seqAudio(d, ref)->at(0);
    CHECK(a.frame == 30 && a.clip == "bell" && a.volume == 100 && a.pan == 64);
    CHECK(editor::addAudioEvent(d, ref, 30, "") == -1);
    CHECK(editor::addAudioEvent(d, aref, 30, "bell") == -1);  // animations have no audio
    CHECK(editor::addAudioEvent(d, ref, 99999, "bell") == 1 && editor::seqAudio(d, ref)->at(1).frame == editor::kMaxTimelineFrame);
    CHECK(d.undo() && editor::seqAudio(d, ref)->size() == 1);
    // Volume and pan are clamped to what the runtime reads; no change, no step.
    splash::CutsceneAudioEvent ev = editor::seqAudio(d, ref)->at(0);
    ev.volume = 500;
    ev.pan = -3;
    CHECK(editor::setAudioEvent(d, ref, 0, ev));
    CHECK(editor::seqAudio(d, ref)->at(0).volume == editor::kMaxEventVolume && editor::seqAudio(d, ref)->at(0).pan == 0);
    h = d.historySize();
    CHECK(!editor::setAudioEvent(d, ref, 0, editor::seqAudio(d, ref)->at(0)) && d.historySize() == h);
    ev.clip = "";
    CHECK(!editor::setAudioEvent(d, ref, 0, ev) && !editor::setAudioEvent(d, ref, 4, editor::seqAudio(d, ref)->at(0)));
    CHECK(d.undo() && editor::seqAudio(d, ref)->at(0).volume == 100);
    // 64 at most.
    for (int i = 1; i < editor::kMaxAudioEvents; ++i) CHECK(editor::addAudioEvent(d, ref, i, "bell") == i);
    CHECK(editor::addAudioEvent(d, ref, 0, "bell") == -1);

    // Skin clip events, in cutscenes and animations; 16 at most.
    h = d.historySize();
    CHECK(editor::addSkinEvent(d, aref, 12, "Arm", "raise") == 0 && d.historySize() == h + 1);
    CHECK(editor::addSkinEvent(d, aref, 12, "", "raise") == -1 && editor::addSkinEvent(d, aref, 12, "Arm", "") == -1);
    splash::SkinAnimEvent se = editor::seqSkin(d, aref)->at(0);
    CHECK(se.frame == 12 && se.object == "Arm" && se.clip == "raise" && !se.loop);
    se.loop = true;
    se.clip = "bend";
    CHECK(editor::setSkinEvent(d, aref, 0, se) && editor::seqSkin(d, aref)->at(0).loop && editor::seqSkin(d, aref)->at(0).clip == "bend");
    CHECK(!editor::setSkinEvent(d, aref, 0, se));
    CHECK(d.undo() && !editor::seqSkin(d, aref)->at(0).loop);
    for (int i = 0; i < editor::kMaxSkinEvents; ++i) CHECK(editor::addSkinEvent(d, ref, i, "Arm", "bend") == i);
    CHECK(editor::addSkinEvent(d, ref, 0, "Arm", "bend") == -1);
    // Moved with a selection and deleted, one step each, then saved and loaded back.
    auto moved = editor::moveSelection(d, ref, {{}, {0}, {3}}, 7);
    CHECK(moved && editor::seqAudio(d, ref)->at(0).frame == 37 && editor::seqSkin(d, ref)->at(3).frame == 10);
    CHECK(editor::deleteSelection(d, ref, {{}, {1, 2}, {0}}) == 3);
    const fs::path dir = g_outDir / "timeline_events";
    fs::create_directories(dir);
    splash::saveScene(d.scene(), dir / "t.scene");
    splash::Scene back = splash::loadScene(dir / "t.scene");
    CHECK(back.cutscenes[0].audioEvents.size() == size_t(editor::kMaxAudioEvents - 2));
    CHECK(back.cutscenes[0].audioEvents[0].frame == 37 && back.cutscenes[0].audioEvents[0].clip == "bell");
    CHECK(back.cutscenes[0].skinEvents.size() == size_t(editor::kMaxSkinEvents - 1) && back.cutscenes[0].skinEvents[2].frame == 10);
    CHECK(back.animations[0].skinEvents.size() == 1 && back.animations[0].skinEvents[0].clip == "raise");
}

// Lua plays a sequence by name: renameSeqInLua rewrites the calls that name
// it with a plain string literal, for the right kind, and nothing else.
void testSeqRenameLua() {
    using editor::SeqKind;
    const std::string cs =
        "Cutscene.Play(\"intro\")\n"
        "Cutscene.Play('intro', {loop = true})\n"
        "Cutscene.Play \"intro\"\n"
        "Cutscene . Play (\r\n  \"intro\" )\n"
        "Cutscene.Play(\"intro2\") Cutscene.Play(\"Intro\") Cutscene.Stop() Cutscene.IsPlaying()\n"
        "-- Cutscene.Play(\"intro\")\n"
        "--[==[ Cutscene.Play(\"intro\") ]==]\n"
        "local s = \"Cutscene.Play(\\\"intro\\\")\" .. [[Cutscene.Play(\"intro\")]]\n"
        "Animation.Play(\"intro\") foo.Cutscene.Play(\"intro\") Cutscene.Play(name) Cutscene.Play(\"in\\116ro\")\n";
    int n = -1;
    const std::string out = editor::renameSeqInLua(cs, SeqKind::Cutscene, "intro", "opening", &n);
    CHECK(n == 4);
    CHECK(out ==
          "Cutscene.Play(\"opening\")\n"
          "Cutscene.Play('opening', {loop = true})\n"
          "Cutscene.Play \"opening\"\n"
          "Cutscene . Play (\r\n  \"opening\" )\n" +
              cs.substr(cs.find("Cutscene.Play(\"intro2\")")));
    // Animations: Play, Stop and IsPlaying take the name; a cutscene or a
    // skinned clip of the same name is someone else's.
    const std::string an =
        "Animation.Play('walk') Animation.Stop(\"walk\") Animation.IsPlaying ( 'walk' )\n"
        "Animation.Play(\"walk\", {loop = true, onComplete = function() Animation.Stop() end})\n"
        "Cutscene.Play(\"walk\") SkinnedAnim.Play(\"walk\", \"walk\") Animation.Find(\"walk\")\n";
    CHECK(editor::renameSeqInLua(an, SeqKind::Animation, "walk", "run", &n) ==
              "Animation.Play('run') Animation.Stop(\"run\") Animation.IsPlaying ( 'run' )\n"
              "Animation.Play(\"run\", {loop = true, onComplete = function() Animation.Stop() end})\n"
              "Cutscene.Play(\"walk\") SkinnedAnim.Play(\"walk\", \"walk\") Animation.Find(\"walk\")\n" &&
          n == 4);
    CHECK(editor::renameSeqInLua(an, SeqKind::Cutscene, "walk", "run", &n).find("Cutscene.Play(\"run\")") != std::string::npos &&
          n == 1);
    // The new name is quoted for the literal it lands in.
    CHECK(editor::renameSeqInLua("Animation.Play('walk')", SeqKind::Animation, "walk", "it's", &n) == "Animation.Play('it\\'s')");
    CHECK(editor::renameSeqInLua("x = 1 -- nothing\n", SeqKind::Animation, "walk", "run", &n) == "x = 1 -- nothing\n" && n == 0);
}

// Renaming a sequence rewrites the scene's scripts on disk in the same undo
// step; undo and redo follow, and a file edited since is left alone.
void testSeqRenameScripts() {
    namespace fs = std::filesystem;
    using editor::SeqKind;
    const fs::path dir = g_outDir / "seq_rename";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scripts", ec);
    auto put = [&](const char* p, const std::string& text) { std::ofstream(dir / p, std::ios::binary) << text; };
    auto get = [&](const char* p) {
        std::ifstream in(dir / p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string obj = "function onCreate(self)\r\n  Cutscene.Play(\"intro\")\r\nend\r\n";
    const std::string trig = "function onTriggerEnter(e) Cutscene.Play('intro') Animation.Play('intro') end\n";
    const std::string scn = "function onSceneCreationEnd() Animation.Play(\"spin\") end\n";
    const std::string other = "Cutscene.Play(\"intro\")\n";  // not the scene's: left alone
    put("scripts/obj.lua", obj);
    put("scripts/trig.lua", trig);
    put("scene.lua", scn);
    put("scripts/other.lua", other);
    splash::Scene sc;
    splash::Object parent, child, door;
    parent.name = "Parent";
    child.name = "Child";
    child.script = splash::ScriptComponent{"scripts/obj.lua", {}};
    parent.children.push_back(child);
    door.name = "Door";
    door.trigger = splash::TriggerComponent{};
    door.trigger->lua = "scripts/trig.lua";
    door.script = splash::ScriptComponent{"scripts/obj.lua", {}};  // the same file twice: rewritten once
    sc.objects = {parent, door};
    sc.settings.script = "scene.lua";
    splash::Cutscene c;
    c.name = "intro";
    sc.cutscenes.push_back(c);
    splash::Animation a;
    a.name = "spin";
    sc.animations.push_back(a);
    editor::Document d;
    d.reset(sc, dir, "s");
    CHECK((editor::sceneScripts(d) == std::vector<std::string>{"scripts/obj.lua", "scripts/trig.lua", "scene.lua"}));

    const size_t h = d.historySize();
    const unsigned assets = d.assetsId();
    editor::ScriptRenames sr;
    CHECK(editor::renameSequence(d, {SeqKind::Cutscene, 0}, "opening", &sr));
    CHECK(d.historySize() == h + 1 && *editor::seqName(d, {SeqKind::Cutscene, 0}) == "opening");
    CHECK(sr.refs == 2 && sr.failed.empty() &&
          (sr.files == std::vector<std::string>{"scripts/obj.lua", "scripts/trig.lua"}));
    CHECK(get("scripts/obj.lua") == "function onCreate(self)\r\n  Cutscene.Play(\"opening\")\r\nend\r\n");
    CHECK(get("scripts/trig.lua") == "function onTriggerEnter(e) Cutscene.Play('opening') Animation.Play('intro') end\n");
    CHECK(get("scene.lua") == scn && get("scripts/other.lua") == other);
    CHECK(d.assetsId() != assets);  // open script tabs reload from this
    // One step: undo puts the name and the files back, redo again.
    CHECK(d.undo() && *editor::seqName(d, {SeqKind::Cutscene, 0}) == "intro");
    CHECK(get("scripts/obj.lua") == obj && get("scripts/trig.lua") == trig);
    CHECK(d.redo() && get("scripts/obj.lua").find("\"opening\"") != std::string::npos &&
          get("scripts/trig.lua").find("'opening'") != std::string::npos);
    // A file changed since the step is not overwritten by its undo.
    put("scripts/trig.lua", "-- mine now\n");
    CHECK(d.undo() && *editor::seqName(d, {SeqKind::Cutscene, 0}) == "intro");
    CHECK(get("scripts/trig.lua") == "-- mine now\n" && get("scripts/obj.lua") == obj);
    // An animation rename touches Animation calls only.
    CHECK(editor::renameSequence(d, {SeqKind::Animation, 0}, "turn", &sr) && sr.refs == 1 &&
          (sr.files == std::vector<std::string>{"scene.lua"}));
    CHECK(get("scene.lua") == "function onSceneCreationEnd() Animation.Play(\"turn\") end\n");
    // No references: the rename is still one step and says so.
    put("scene.lua", scn);
    CHECK(editor::renameSequence(d, {SeqKind::Animation, 0}, "twirl", &sr) && sr.refs == 0 && sr.files.empty());
    fs::remove_all(dir, ec);
}

// A UI position track starts where psxsplash starts it: the element's
// resolved screen position (UISystem::getPosition: anchor byte * res >> 8
// plus the stored offset, clamped at 0), less the anchor correction
// writeSequences adds to each key. At 320x240 an anchor of 1.0 is byte 255,
// pixel 318 (x) and 239 (y), so the correction is 2 and 1.
void testTimelineUiPositionStart() {
    using editor::SeqKind;
    using splash::TrackType;
    splash::UICanvas cv;
    cv.name = "hud";
    splash::UIElement bar, mid, neg, line;
    bar.name = "bar";
    bar.anchorMin = bar.anchorMax = {1, 1};
    bar.rect = {-40, -20, 32, 8};
    mid.name = "mid";
    mid.anchorMin = mid.anchorMax = {0.5f, 0.5f};
    mid.rect = {-10, 4, 20, 8};
    neg.name = "neg";
    neg.rect = {-5, 3, 16, 16};
    line.name = "line";
    line.type = splash::UIElementType::Line;
    line.from = {7, 9};
    cv.elements = {bar, mid, neg, line};
    CHECK((splash::uiPositionTrackStart(bar) == std::array<int, 2>{278, 219}));  // on screen at 280, 220
    CHECK((splash::uiPositionTrackStart(mid) == std::array<int, 2>{150, 124}));
    CHECK((splash::uiPositionTrackStart(neg) == std::array<int, 2>{0, 3}));      // the runtime clamps at 0
    CHECK((splash::uiPositionTrackStart(line) == std::array<int, 2>{7, 9}));

    splash::Scene sc;
    sc.canvases.push_back(cv);
    splash::Cutscene c;
    c.name = "intro";
    c.durationFrames = 60;
    splash::CutsceneTrack t;
    t.type = TrackType::UIPosition;
    t.target = "hud/bar";
    c.tracks.push_back(t);
    t.target = "hud/mid";
    splash::Keyframe k;
    k.frame = 30;
    k.value = {0, 0, 0};
    t.keyframes.push_back(k);
    c.tracks.push_back(t);
    sc.cutscenes.push_back(c);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{SeqKind::Cutscene, 0};
    CHECK((editor::initialValue(d, d.scene().cutscenes[0].tracks[0]) == editor::KeyValue{278, 219, 0}));
    // The start key on an empty track is where the element sits: exported
    // as 278 + 2, 219 + 1, the position the runtime starts from, no jump.
    auto key = editor::addKey(d, ref, 0, 10);
    CHECK(key && (editor::key(d, ref, *key)->value == editor::KeyValue{278, 219, 0}));
    // The lane value (and preview) before the first key starts at the element.
    CHECK((editor::sampleAt(d, ref, 1, 0) == editor::KeyValue{150, 124, 0}));
    CHECK((editor::sampleAt(d, ref, 1, 15) == editor::KeyValue{75, 62, 0}));
    CHECK((editor::sampleAt(d, ref, 1, 30) == editor::KeyValue{0, 0, 0}));
}

// UI visible, progress and colour tracks start from the element as the scene
// has it (psxsplash's CutscenePlayer reads the UI system when a track starts),
// so the first key added to an empty track is the element's current value.
void testTimelineUiStartValues() {
    using splash::TrackType;
    splash::UICanvas cv;
    cv.name = "hud";
    splash::UIElement bar, label;
    bar.name = "bar";
    bar.type = splash::UIElementType::Progress;
    bar.value = 65;
    bar.visible = false;
    bar.color = {0.25f, 0.5f, 1.0f};
    label.name = "label";
    label.type = splash::UIElementType::Text;
    label.value = 40;  // not a progress bar: the runtime's getProgress gives 0
    cv.elements = {bar, label};
    splash::Scene sc;
    sc.canvases.push_back(cv);
    splash::Cutscene c;
    c.name = "intro";
    c.durationFrames = 60;
    sc.cutscenes.push_back(c);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    const int vis = editor::addTrack(d, ref, TrackType::UIElementVisible, "hud/bar");
    const int prog = editor::addTrack(d, ref, TrackType::UIProgress, "hud/bar");
    const int col = editor::addTrack(d, ref, TrackType::UIColor, "hud/bar");
    const int other = editor::addTrack(d, ref, TrackType::UIProgress, "hud/label");
    CHECK(vis == 0 && prog == 1 && col == 2 && other == 3);
    const auto& tr = *editor::seqTracks(d, ref);
    CHECK((editor::initialValue(d, tr[0]) == editor::KeyValue{0, 0, 0}));
    CHECK((editor::initialValue(d, tr[1]) == editor::KeyValue{65, 0, 0}));
    CHECK((editor::initialValue(d, tr[2]) == editor::KeyValue{0.25f, 0.5f, 1.0f}));
    CHECK((editor::initialValue(d, tr[3]) == editor::KeyValue{0, 0, 0}));
    // The lane shows the element's value before any key.
    CHECK(near3(editor::sampleAt(d, ref, prog, 0), 65, 0, 0));
    // The first key of each track is the element's value, not 0.
    auto kp = editor::addKey(d, ref, prog, 20);
    CHECK(kp && near3(editor::key(d, ref, *kp)->value, 65, 0, 0));
    auto kc = editor::addKey(d, ref, col, 20);
    CHECK(kc && near3(editor::key(d, ref, *kc)->value, 0.25f, 0.5f, 1.0f));
    // The bar starts hidden: its first visible key is off.
    auto kv = editor::addKey(d, ref, vis, 20);
    CHECK(kv && editor::key(d, ref, *kv)->value[0] < 0.5f);
    // A shown element starts on.
    splash::Scene shown = d.scene();
    shown.canvases[0].elements[0].visible = true;
    d.reset(shown);
    CHECK(editor::initialValue(d, d.scene().cutscenes[0].tracks[0])[0] == 1.0f);
}

void testTimelineRetarget() {
    using splash::TrackType;
    splash::Scene sc;
    for (const char* n : {"Crate", "Barrel", "Ghost"}) {
        splash::Object o = named(n);
        if (std::string(n) != "Ghost") o.mesh = splash::MeshComponent{};
        sc.objects.push_back(o);
    }
    splash::Object lamp = named("Lamp");
    lamp.light = splash::LightComponent{};
    lamp.light->runtime = true;
    sc.objects.push_back(lamp);
    splash::Cutscene cs;
    cs.name = "intro";
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    CHECK(editor::addTrack(d, ref, TrackType::ObjectPosition, "Crate") == 0);
    CHECK(editor::addTrack(d, ref, TrackType::ObjectPosition, "Barrel") == 1);
    CHECK(editor::addTrack(d, ref, TrackType::ObjectRotation, "Crate") == 2);
    CHECK(editor::addTrack(d, ref, TrackType::CameraPosition, "") == 3);
    CHECK(editor::addKey(d, ref, 2, 10, editor::KeyValue{0, 90, 0}));
    CHECK((editor::targetsFor(d, TrackType::ObjectPosition) == std::vector<std::string>{"Crate", "Barrel"}));
    CHECK((editor::targetsFor(d, TrackType::LightColor) == std::vector<std::string>{"Lamp"}));
    CHECK(editor::targetsFor(d, TrackType::CameraPosition).empty());

    size_t h = d.historySize();
    CHECK(editor::setTrackTarget(d, ref, 2, "Barrel"));
    CHECK(d.historySize() == h + 1 && d.scene().cutscenes[0].tracks[2].target == "Barrel");
    CHECK(d.scene().cutscenes[0].tracks[2].keyframes.size() == 1 && d.scene().cutscenes[0].tracks[2].keyframes[0].value[1] == 90);
    CHECK(d.undo() && d.scene().cutscenes[0].tracks[2].target == "Crate");
    h = d.historySize();
    CHECK(!editor::setTrackTarget(d, ref, 0, "Barrel"));  // Barrel already has a position track
    CHECK(!editor::setTrackTarget(d, ref, 0, "Crate"));   // unchanged
    CHECK(!editor::setTrackTarget(d, ref, 0, ""));
    CHECK(!editor::setTrackTarget(d, ref, 3, "Crate"));   // the camera has no target
    CHECK(!editor::setTrackTarget(d, ref, 9, "Crate"));
    CHECK(d.historySize() == h);
}

// Sampling as the console does it: keys packed to fixed point, then
// interpolation.cpp's integer blend. The float curve misses by a step.
void testTimelineFixedSample() {
    using splash::Interp;
    using splash::TrackType;
    // x 0 -> -1, y 0 -> -2 (packed with Y negated) over frames 0..3, GTE scaling 100.
    splash::CutsceneTrack t = trackOf(TrackType::ObjectPosition, {{0, 0.0f}, {3, -1.0f}});
    CHECK((editor::encodeValue(TrackType::ObjectPosition, {-1, -2, 0}, 100) == editor::FixedValue{-41, 82, 0}));
    // Frame 1: t = 4096 / 3 = 1365; x: -41 * 1365 >> 12 = -14 (floor), y: 82 * 1365 >> 12 = 27.
    CHECK((editor::sampleFixedRaw(t, 1, {0, 0, 0}, 100) == editor::FixedValue{-14, 27, 0}));
    const editor::KeyValue v = editor::sampleFixed(t, 1, {0, 0, 0}, 100);
    CHECK(near3(v, -14 * 100 / 4096.0f, -27 * 100 / 4096.0f, 0));
    // The float curve says -1/3: more than a tenth of a fixed-point step off.
    CHECK(std::fabs(editor::sample(t, 1)[0] - v[0]) > 0.1f * 100 / 4096);
    // Through the document: sampleAt is the console's value.
    splash::Scene sc;
    sc.objects.push_back(named("A"));
    splash::Cutscene cs;
    cs.name = "intro";
    cs.tracks.push_back(t);
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    CHECK(near3(editor::sampleAt(d, {editor::SeqKind::Cutscene, 0}, 0, 1), v[0], v[1], v[2]));
    // Sub-frames: frame 1.5 is 4096 + 2048 into a 3-frame span.
    CHECK(editor::sampleFixedRaw(t, 1.5f, {0, 0, 0}, 100)[0] == int16_t((-41 * int32_t((4096 + 2048) / 3)) >> 12));
    // The ease curves in 0.12: EaseInOut at t = 1024 is (3 * 1024^2 - 2 * (1024^3 >> 12)) >> 12 = 640.
    splash::CutsceneTrack e = trackOf(TrackType::ObjectPosition, {{0, 0.0f}, {4, 100.0f}}, Interp::EaseInOut);
    CHECK(editor::sampleFixedRaw(e, 1, {0, 0, 0}, 100)[0] == int16_t((4096 * 640) >> 12));
    // Before the first key: from the initial value, packed the same way.
    splash::CutsceneTrack late = trackOf(TrackType::ObjectPosition, {{10, 1.0f}});
    CHECK(editor::sampleFixedRaw(late, 5, {0, 0, 1}, 100)[2] == int16_t(41 + ((0 - 41) * 2048 >> 12)));
    // On the keys and past the end: the keys' packed values exactly.
    CHECK(editor::sampleFixedRaw(t, 3, {0, 0, 0}, 100)[0] == -41 && editor::sampleFixedRaw(t, 9, {0, 0, 0}, 100)[0] == -41);
    CHECK(editor::sampleFixedRaw(t, 0, {0, 0, 0}, 100)[0] == 0);
    // Angles: degrees * 1024 / 180, X negated; colours to 0..255.
    CHECK((editor::encodeValue(TrackType::ObjectRotation, {90, 45, 0}, 100) == editor::FixedValue{-512, 256, 0}));
    CHECK((editor::encodeValue(TrackType::LightColor, {1, 0.5f, 0}, 100) == editor::FixedValue{255, 128, 0}));
    CHECK(near3(editor::decodeValue(TrackType::ObjectRotation, {-512, 256, 0}, 100), 90, 45, 0));
}

// Light, UV offset and camera H tracks in the Scene view's preview.
void testTimelinePreviewMore() {
    using splash::TrackType;
    splash::Scene sc;
    splash::Object lamp = named("Lamp", 1);
    lamp.light = splash::LightComponent{};
    lamp.light->runtime = true;
    lamp.light->color = {1, 1, 1};
    lamp.light->intensity = 1;
    lamp.light->range = 10;
    sc.objects.push_back(lamp);
    splash::Object crate = named("Crate", 2);
    crate.mesh = splash::MeshComponent{};
    sc.objects.push_back(crate);
    splash::Cutscene cs;
    cs.name = "intro";
    auto track = [](TrackType ty, const char* target, int f, editor::KeyValue v) {
        splash::CutsceneTrack t;
        t.type = ty;
        t.target = target;
        splash::Keyframe k;
        k.frame = f;
        k.value = v;
        t.keyframes.push_back(k);
        return t;
    };
    cs.tracks.push_back(track(TrackType::LightColor, "Lamp", 10, {0, 0, 1}));
    cs.tracks.push_back(track(TrackType::LightIntensity, "Lamp", 0, {2, 0, 0}));
    cs.tracks.push_back(track(TrackType::LightEnabled, "Lamp", 4, {0, 0, 0}));
    cs.tracks.push_back(track(TrackType::LightPosition, "Lamp", 0, {0, 5, 0}));
    cs.tracks.push_back(track(TrackType::LightRadius, "Lamp", 0, {4, 0, 0}));
    cs.tracks.push_back(track(TrackType::ObjectUVOffset, "Crate", 0, {8, 16, 0}));
    cs.tracks.push_back(track(TrackType::CameraH, "", 10, {240, 0, 0}));
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};
    const editor::Preview p = editor::samplePreview(d, ref, 5);
    CHECK(p.lights.size() == 1 && p.lights.count("Lamp"));
    const editor::LightPose& lp = p.lights.at("Lamp");
    // Halfway into the first colour key, from the light's own white: 255 + (-255 * 2048 >> 12) = 127, not 127.5.
    CHECK(lp.color && std::fabs((*lp.color)[0] - 127 / 255.0f) < 1e-6f && (*lp.color)[2] == 1.0f);
    CHECK(lp.intensity && *lp.intensity == 2.0f && lp.enabled == false && lp.radius && std::fabs(*lp.radius - 4) < 0.025f);
    CHECK(lp.position && std::fabs(lp.position->y - 5) < 0.025f);
    CHECK(p.objects.at("Crate").uvOffset && (*p.objects.at("Crate").uvOffset == std::array<float, 2>{8, 16}));
    // Camera H blends from the runtime's 120.
    CHECK(p.cameraH && *p.cameraH == 180.0f);
    splash::Scene posed = d.scene();
    editor::applyPreview(posed, p);
    const splash::LightComponent& l = *posed.objects[0].light;
    CHECK(!l.enabled && l.intensity == 2.0f && std::fabs(l.range - 4) < 0.025f && std::fabs(posed.objects[0].transform.position.y - 5) < 0.025f);
    CHECK(d.scene().objects[0].light->enabled && d.scene().objects[0].light->intensity == 1.0f);
    CHECK(editor::samplePreview(d, ref, 3).lights.at("Lamp").enabled == true);
}

void testTimelinePreview() {
    using splash::TrackType;
    auto keyed = [](TrackType type, const char* target, std::initializer_list<std::pair<int, std::array<float, 3>>> keys) {
        splash::CutsceneTrack t;
        t.type = type;
        t.target = target;
        for (auto [f, v] : keys) {
            splash::Keyframe k;
            k.frame = f;
            k.value = v;
            t.keyframes.push_back(k);
        }
        return t;
    };
    auto nearV = [](splash::Vec3 a, splash::Vec3 b) {
        return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f && std::fabs(a.z - b.z) < 1e-3f;
    };
    // Sampled as the console plays it: within one fixed-point step (GTE
    // scaling 100: 100 / 4096 world units; angles 180 / 1024 degrees).
    auto nearQ = [](splash::Vec3 a, splash::Vec3 b) {
        return std::fabs(a.x - b.x) < 0.025f && std::fabs(a.y - b.y) < 0.025f && std::fabs(a.z - b.z) < 0.025f;
    };
    auto sameRot = [](splash::Quat a, splash::Quat b) {
        return std::fabs(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1) < 1e-4f;
    };
    // Euler round trip through Quaternion.eulerAngles.
    CHECK(nearV(splash::eulerAngles(editor::quatEuler({10, 20, 30})), {10, 20, 30}));

    splash::Scene sc = sample();  // A at x 1, B at x 2 with B1 at local x 3, C at x 4
    sc.objects[1].transform.rotation = editor::quatEuler({0, 30, 0});
    sc.objects[1].transform.scale = {2, 2, 2};
    sc.objects[2].mesh = splash::MeshComponent{};
    splash::Cutscene cs;
    cs.name = "intro";
    cs.tracks.push_back(keyed(TrackType::ObjectPosition, "A", {{0, {0, 0, 0}}, {10, {10, 0, 0}}}));
    cs.tracks.push_back(keyed(TrackType::ObjectPosition, "B1", {{0, {1, 2, 3}}}));
    cs.tracks.push_back(keyed(TrackType::ObjectRotation, "B1", {{0, {0, 0, 0}}, {10, {0, 90, 0}}}));
    cs.tracks.push_back(keyed(TrackType::ObjectActive, "C", {{0, {1, 0, 0}}, {5, {0, 0, 0}}}));
    cs.tracks.push_back(keyed(TrackType::CameraPosition, "", {{0, {0, 3, -10}}, {10, {10, 3, -10}}}));
    cs.tracks.push_back(keyed(TrackType::CameraRotation, "", {{0, {10, 0, 0}}}));
    cs.tracks.push_back(keyed(TrackType::ObjectPosition, "B", {}));  // keyless: B is left alone
    sc.cutscenes.push_back(cs);
    editor::Document d;
    d.reset(sc);
    const std::filesystem::path dir = g_outDir / "timeline_preview";
    std::filesystem::create_directories(dir);
    auto bytes = [](const std::filesystem::path& f) {
        std::ifstream in(f, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    splash::saveScene(d.scene(), dir / "before.scene");
    const unsigned rev = d.revision();
    const size_t hist = d.historySize();
    const editor::SeqRef ref{editor::SeqKind::Cutscene, 0};

    editor::Preview p = editor::samplePreview(d, ref, 5);
    CHECK(p.objects.size() == 3 && !p.objects.count("B"));
    CHECK(p.objects["A"].position && nearQ(*p.objects["A"].position, {5, 0, 0}) && !p.objects["A"].rotation);
    CHECK(p.objects["B1"].rotation && nearQ(*p.objects["B1"].rotation, {0, 45, 0}));
    CHECK(p.objects["C"].active == false && editor::samplePreview(d, ref, 4).objects["C"].active == true);
    CHECK(p.cameraPosition && nearQ(*p.cameraPosition, {5, 3, -10}) && p.cameraRotation && nearQ(*p.cameraRotation, {10, 0, 0}));
    CHECK(editor::samplePreview(d, ref, 5) == p && !(editor::samplePreview(d, ref, 6) == p));
    CHECK(editor::samplePreview(d, {editor::SeqKind::Cutscene, 3}, 5).empty());
    CHECK(editor::samplePreview(d, {editor::SeqKind::Animation, 0}, 5).empty());

    // Applied to a copy: world poses as sampled, a nested target included.
    splash::Scene posed = d.scene();
    editor::applyPreview(posed, p);
    std::vector<splash::FlatObject> f = splash::flatten(posed);  // A, B, B1, C
    CHECK(f.size() == 4 && nearQ(f[0].worldPosition, {5, 0, 0}));
    CHECK(nearV(f[1].worldPosition, {2, 0, 0}) && sameRot(f[1].worldRotation, editor::quatEuler({0, 30, 0})));
    CHECK(nearQ(f[2].worldPosition, {1, 2, 3}) && sameRot(f[2].worldRotation, editor::quatEuler({0, 45, 0})));
    CHECK(!posed.objects[2].mesh && d.scene().objects[2].mesh);

    // The document is untouched: values, revision, history, dirty flag and the saved bytes.
    CHECK(d.object({0})->transform.position.x == 1 && d.object({1, 0})->transform.position.x == 3);
    CHECK(d.revision() == rev && d.historySize() == hist && !d.dirty() && !d.canUndo());
    splash::saveScene(d.scene(), dir / "after.scene");
    CHECK(bytes(dir / "before.scene") == bytes(dir / "after.scene") && !bytes(dir / "after.scene").empty());
}

// Paste moves a copy off the object it would cover, one grid step along X per
// copy already there; a cut object pastes back where it was.
void testPasteOffset() {
    editor::Document d;
    d.reset(sample());
    const std::string text = d.copyObject({1});  // B at x 2, child B1 at x 3
    const float step = editor::Document::kPasteStep;
    CHECK(step == 0.25f);

    // Pasting three times: three different places, each one undo step.
    std::vector<float> xs;
    for (int i = 0; i < 3; ++i) {
        const size_t hist = d.historySize();
        auto p = d.pasteObjects(text, editor::ObjectPath{1});
        CHECK(p && d.historySize() == hist + 1);
        if (p && d.object(*p)) xs.push_back(d.object(*p)->transform.position.x);
    }
    CHECK(xs == std::vector<float>({2 + step, 2 + 2 * step, 2 + 3 * step}));
    CHECK(d.object({1})->transform.position.x == 2);  // the original stays
    // Children keep their place in the moved parent.
    for (int i = 2; i <= 4; ++i) CHECK(d.object({i, 0}) && d.object({i, 0})->transform.position.x == 3);
    // Undo takes the last paste back in one step; pasting again fills its place.
    CHECK(d.undo());
    CHECK(d.scene().objects.size() == 5);
    auto again = d.pasteObjects(text, editor::ObjectPath{1});
    CHECK(again && d.object(*again)->transform.position.x == 2 + 3 * step);
    CHECK(d.undo() && d.undo() && d.undo() && !d.canUndo());
    CHECK(d.scene().objects.size() == 3);

    // The step is the caller's to give.
    auto wide = d.pasteObjects(text, editor::ObjectPath{1}, 1.0f);
    CHECK(wide && d.object(*wide)->transform.position.x == 3);
    CHECK(d.undo());

    // Only a copy of the same object counts: another object where C sits
    // (x 4), one with a mesh or one turned another way, stays where it is.
    splash::Object m = named("M", 4);
    m.mesh = splash::MeshComponent{};
    m.mesh->mesh = "meshes/crate.mesh";
    auto pm = d.pasteObjects(splash::writeObjectsText({m}), std::nullopt);
    CHECK(pm && d.object(*pm)->transform.position.x == 4);
    splash::Object turned = named("T", 4);
    turned.transform.rotation = {0, 0.70710678f, 0, 0.70710678f};
    auto pt = d.pasteObjects(splash::writeObjectsText({turned}), std::nullopt);
    CHECK(pt && d.object(*pt)->transform.position.x == 4);
    auto other = d.pasteObjects(splash::writeObjectsText({named("Other", 4)}), std::nullopt);
    CHECK(other && d.object(*other)->transform.position.x == 4);
    CHECK(d.undo());
    // M again, on the first M: moved.
    auto pm2 = d.pasteObjects(splash::writeObjectsText({m}), std::nullopt);
    CHECK(pm2 && d.object(*pm2)->transform.position.x == 4 + step);
    CHECK(d.undo() && d.undo() && d.undo());

    // Several objects move together, by the same steps, keeping their layout.
    auto two = d.pasteObjects(splash::writeObjectsText({named("A", 1), named("Q", 9)}), std::nullopt);
    CHECK(two && d.scene().objects.size() == 5);
    CHECK(d.object({3})->transform.position.x == 1 + step && d.object({4})->transform.position.x == 9 + step);
    CHECK(d.undo());

    // Into a child level: checked against the new siblings there.
    auto child = d.pasteObjects(d.copyObject({1, 0}), editor::ObjectPath{1, 0});
    CHECK(child && *child == editor::ObjectPath({1, 1}) && d.object({1, 1})->transform.position.x == 3 + step);
    CHECK(d.undo());

    // Cut then paste moves the object: nothing is left to cover, no offset.
    // A second paste of the same text is a copy, and steps off it.
    CHECK(d.cutObject({1}) == text);
    auto back = d.pasteObjects(text, editor::ObjectPath{0});
    CHECK(back && *back == editor::ObjectPath({1}) && d.object({1})->name == "B" && d.object({1})->transform.position.x == 2);
    CHECK(d.historySize() == 2);
    auto copy = d.pasteObjects(text, editor::ObjectPath{1});
    CHECK(copy && d.object(*copy)->transform.position.x == 2 + step);
    CHECK(d.undo() && d.undo());
    CHECK(d.scene().objects.size() == 2 && !d.object({2}));
    CHECK(d.undo() && d.object({1})->name == "B" && d.object({1})->transform.position.x == 2 && !d.canUndo());
}

// Every warning of a model's last import is kept for the inspector, by the
// model's project path, and a reimport replaces them.
void testImportWarnings() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "importwarnings";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game";
    fs::create_directories(proj, ec);
    const fs::path rig = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skinimport" / "rig.gltf";
    editor::Document doc;
    doc.reset(splash::Scene{}, proj, "game");
    CHECK(doc.importWarnings("models/rig.gltf").empty());

    editor::PlacedModel p = editor::placeModel(doc, rig, std::nullopt, {0}, {0, 0, 0});
    CHECK(p.error.empty());
    // rig.gltf: blended weights and cubic-spline keys, at least.
    CHECK(p.model.warnings.size() >= 2);
    const std::vector<std::string>& kept = doc.importWarnings("models/rig.gltf");
    CHECK(kept == p.model.warnings);
    CHECK(doc.importWarnings(proj / "models" / "rig.gltf") == p.model.warnings);  // absolute works too
    CHECK(doc.importWarnings("models/other.gltf").empty());
    bool blended = false, cubic = false;
    for (const std::string& w : kept) {
        editor::ImportWarning e = editor::explainImportWarning(w);
        CHECK(!e.problem.empty());
        if (w.find("the heaviest joint wins") != std::string::npos) {
            blended = true;
            CHECK(e.fix == "Weight-paint to single bones for exact deformation");
            CHECK(e.problem.find("weight-paint") == std::string::npos);
        }
        if (w.find("cubic-spline") != std::string::npos) {
            cubic = true;
            CHECK(e.fix == "Bake the animation to linear keys for its exact curve");
        }
    }
    CHECK(blended && cubic);

    // The skin limits say what to change.
    editor::ImportWarning j = editor::explainImportWarning(
        "the skeleton has 70 joints, psxsplash draws at most 64: delete helper bones (IK targets, poles, twist bones) in the modelling tool");
    CHECK(j.problem == "the skeleton has 70 joints, psxsplash draws at most 64" && j.fix.find("at most 64 joints") != std::string::npos);
    editor::ImportWarning c = editor::explainImportWarning(
        "20 animations, a skin plays at most 16: the first 16 go on the skin component, swap in others by hand");
    CHECK(c.problem == "20 animations, a skin plays at most 16" && c.fix.find("at most 16 animations") != std::string::npos);
    editor::ImportWarning t = editor::explainImportWarning("texture models/x.png was not found, the material is left untextured");
    CHECK(t.problem == "texture models/x.png was not found, the material is left untextured" && !t.fix.empty());
    editor::ImportWarning u = editor::explainImportWarning("something new: and odd");
    CHECK(u.problem == "something new: and odd" && u.fix.empty());

    // Reimport: the same warnings again, not added to the old ones.
    editor::Reimported re = editor::reimportModel(doc, proj / "models" / "rig.gltf");
    CHECK(re.error.empty() && doc.importWarnings("models/rig.gltf") == re.model.warnings &&
          re.model.warnings.size() == p.model.warnings.size());
    // The model fixed (a plain one in its place, read from the copy): none left.
    splash::ImportSettings st = splash::loadImportSettings(proj / "models" / "rig.gltf");
    st.source.clear();
    splash::saveImportSettings(proj / "models" / "rig.gltf", st);
    std::ofstream(proj / "models" / "rig.gltf", std::ios::binary | std::ios::trunc) << robotGltf(1);
    re = editor::reimportModel(doc, proj / "models" / "rig.gltf");
    CHECK(re.error.empty() && re.model.warnings.empty() && doc.importWarnings("models/rig.gltf").empty());
    // Broken again: back.
    fs::copy_file(rig, proj / "models" / "rig.gltf", fs::copy_options::overwrite_existing, ec);
    re = editor::reimportModel(doc, proj / "models" / "rig.gltf");
    CHECK(re.error.empty() && doc.importWarnings("models/rig.gltf") == re.model.warnings && re.model.warnings.size() >= 2);

    // Kept through loading another scene of the project, dropped with the project.
    doc.reset(splash::Scene{}, proj, "other");
    CHECK(doc.importWarnings("models/rig.gltf").size() >= 2);
    doc.reset(splash::Scene{}, dir / "elsewhere", "game");
    CHECK(doc.importWarnings("models/rig.gltf").empty());
}

// The model import queue: models are read and baked on a worker thread into
// a temporary folder, one at a time in the order asked, and only applying the
// result changes the project and the scene. Cancel and a scene change drop an
// import with nothing changed.
// TMPDIR pointed at a folder of the test's own while it lives, so the
// temporary folders counted are this process's (std::filesystem::
// temp_directory_path honours TMPDIR on POSIX and TMP on Windows), not other
// processes' on a busy machine.
struct ScopedTmpdir {
#ifdef _WIN32
    static constexpr const char* kVar = "TMP";
#else
    static constexpr const char* kVar = "TMPDIR";
#endif
    std::optional<std::string> old;
    explicit ScopedTmpdir(const std::filesystem::path& dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (const char* v = std::getenv(kVar)) old = v;
        set(dir.string().c_str());
    }
    ~ScopedTmpdir() { set(old ? old->c_str() : nullptr); }
    static void set(const char* v) {
#ifdef _WIN32
        _putenv_s(kVar, v ? v : "");
#else
        if (v) setenv(kVar, v, 1);
        else unsetenv(kVar);
#endif
    }
};

static std::set<std::string> importTemps() {
    std::set<std::string> r;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::temp_directory_path(ec), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().rfind("splashedit-import-", 0) == 0) r.insert(it->path().filename().string());
    return r;
}

static std::string fileBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// update() until the queue is empty or 30 s pass; applies each Ready outcome
// with `apply` before the next update, as the editor does once a frame.
template <typename F>
static std::vector<editor::ModelImports::Outcome> drainImports(editor::ModelImports& q, editor::Document& doc, F apply) {
    std::vector<editor::ModelImports::Outcome> all;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (q.busy() && std::chrono::steady_clock::now() < until) {
        for (editor::ModelImports::Outcome& o : q.update(doc)) {
            if (o.kind == editor::ModelImports::Outcome::Ready) apply(o);
            all.push_back(std::move(o));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

void testModelImports() {
    namespace fs = std::filesystem;
    using Outcome = editor::ModelImports::Outcome;
    std::error_code ec;
    fs::path dir = g_outDir / "modelimports";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game", outside = dir / "downloads";
    fs::create_directories(proj, ec);
    fs::create_directories(outside, ec);
    const ScopedTmpdir scopedTmp(dir / "tmp");
    std::ofstream(outside / "robot.gltf") << robotGltf(0.5f);
    std::ofstream(outside / "crate.gltf") << robotGltf(0.75f);
    std::ofstream(outside / "tri.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream(outside / "junk.glb") << "not a model";
    const std::set<std::string> tempsBefore = importTemps();

    splash::Scene sc;
    sc.objects.push_back(named("A"));
    editor::Document doc;
    doc.reset(sc, proj, "game");
    auto request = [&](const fs::path& file) {
        editor::ModelImports::Request r;
        r.file = file;
        r.to.at = splash::Vec3{1, 2, 3};
        r.loadId = doc.loadId();
        return r;
    };
    auto place = [&](Outcome& o) {
        const editor::ObjectPath at{int(doc.scene().objects.size())};
        editor::PlacedModel p = editor::applyPlace(doc, o.staged, std::nullopt, at, *o.request.to.at);
        CHECK(p.error.empty());
    };

    // Two in a row: the first starts on the first update and runs on a worker
    // while the scene and the project are untouched; both land in order.
    editor::ModelImports q;
    CHECK(!q.busy() && q.stage() == editor::ModelImports::Stage::Idle && !q.current());
    q.add(request(outside / "robot.gltf"));
    q.add(request(outside / "tri.obj"));
    CHECK(q.busy() && q.size() == 2 && !q.current());
    std::vector<Outcome> first = q.update(doc);
    CHECK(first.empty() && q.stage() == editor::ModelImports::Stage::Import);
    CHECK(q.current() && q.current()->file == outside / "robot.gltf");
    CHECK(doc.historySize() == 0 && !fs::exists(proj / "models" / "robot.gltf") && !fs::exists(proj / "models" / "robot.mesh"));
    std::vector<std::string> order;
    bool untouchedWhenReady = true;
    std::vector<Outcome> got = drainImports(q, doc, [&](Outcome& o) {
        // Ready but not applied: nothing is in the project yet.
        untouchedWhenReady = untouchedWhenReady && !fs::exists(proj / "models" / o.request.file.filename());
        order.push_back(o.request.file.filename().string());
        place(o);
    });
    CHECK(got.size() == 2 && got[0].kind == Outcome::Ready && got[1].kind == Outcome::Ready);
    CHECK((order == std::vector<std::string>{"robot.gltf", "tri.obj"}));
    CHECK(untouchedWhenReady);
    CHECK(!q.busy());
    CHECK(doc.scene().objects.size() == 3 && doc.historySize() == 2);
    CHECK(doc.object({1}) && doc.object({1})->name == "robot" && doc.object({2}) && doc.object({2})->name == "tri");
    CHECK(fs::exists(proj / "models" / "robot.gltf") && fs::exists(proj / "models" / "robot" / "Body.mesh"));
    CHECK(fs::exists(proj / "models" / "tri.obj") && fs::exists(proj / "models" / "tri.mesh"));
    CHECK(fs::exists(proj / "models" / "robot.gltf.import.json"));
    // Applying removed the temporary folders.
    CHECK(importTemps() == tempsBefore);

    // Cancel while it runs: nothing is adopted, written or placed, and the
    // temporary folder goes once the worker is done with it.
    q.add(request(outside / "crate.gltf"));
    q.update(doc);
    CHECK(q.current() && q.stage() == editor::ModelImports::Stage::Import);
    q.cancel();
    bool appliedCancelled = false;
    got = drainImports(q, doc, [&](Outcome&) { appliedCancelled = true; });
    CHECK(got.size() == 1 && got[0].kind == Outcome::Cancelled && !appliedCancelled);
    CHECK(doc.scene().objects.size() == 3 && doc.historySize() == 2);
    CHECK(!fs::exists(proj / "models" / "crate.gltf") && !fs::exists(proj / "models" / "crate") &&
          !fs::exists(proj / "models" / "crate.mesh"));
    bool tempsGone = false;
    for (int i = 0; i < 1000 && !tempsGone; ++i) {
        tempsGone = importTemps() == tempsBefore;
        if (!tempsGone) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(tempsGone);

    // A file that is not a model fails, adopting nothing; the next one still runs.
    q.add(request(outside / "junk.glb"));
    q.add(request(outside / "crate.gltf"));
    got = drainImports(q, doc, place);
    CHECK(got.size() == 2 && got[0].kind == Outcome::Failed && !got[0].error.empty() && got[1].kind == Outcome::Ready);
    CHECK(!fs::exists(proj / "models" / "junk.glb"));
    CHECK(doc.scene().objects.size() == 4 && doc.object({3}) && doc.object({3})->name == "crate");

    // Asked for in a scene that is no longer open: dropped, never started.
    editor::ModelImports::Request stale = request(outside / "tri.obj");
    stale.loadId = doc.loadId() + 1;
    q.add(stale);
    got = q.update(doc);
    CHECK(got.size() == 1 && got[0].kind == Outcome::Dropped && !q.busy());

    // Reimport: asked twice before it starts, it runs once. Cancelled, the
    // meshes on disk and the scene stay as they were; applied, the objects
    // take the new materials as one undo step.
    std::ofstream(outside / "robot.gltf", std::ios::trunc) << robotGltf(0.25f, true);
    const fs::path bodyMesh = proj / "models" / "robot" / "Body.mesh";
    const std::string bodyBefore = fileBytes(bodyMesh);
    const unsigned historyBefore = unsigned(doc.historySize());
    const std::vector<splash::Material> matsBefore = doc.object({1, 0})->mesh->materials;
    editor::ModelImports::Request re;
    re.reimport = proj / "models" / "robot.gltf";
    re.loadId = doc.loadId();
    q.add(re);
    re.wasMerged = false;
    q.add(re);
    CHECK(q.size() == 1);
    q.update(doc);
    CHECK(q.current() && q.current()->reimport == re.reimport && q.current()->wasMerged == std::optional<bool>(false));
    q.cancel();
    got = drainImports(q, doc, [](Outcome&) {});
    CHECK(got.size() == 1 && got[0].kind == Outcome::Cancelled);
    CHECK(!bodyBefore.empty() && fileBytes(bodyMesh) == bodyBefore);
    CHECK(doc.historySize() == historyBefore && doc.object({1, 0})->mesh->materials[0].color == matsBefore[0].color);
    q.add(re);
    got = drainImports(q, doc, [&](Outcome& o) {
        editor::Reimported r = editor::applyReimport(doc, o.staged, o.request.wasMerged);
        CHECK(r.error.empty() && r.objects == 1);
    });
    CHECK(got.size() == 1 && got[0].kind == Outcome::Ready);
    CHECK(doc.historySize() == historyBefore + 1 && !(doc.object({1, 0})->mesh->materials[0].color == matsBefore[0].color));
    CHECK(fileBytes(bodyMesh) != bodyBefore);
    for (int i = 0; i < 1000 && importTemps() != tempsBefore; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(importTemps() == tempsBefore);
}

// A wavy n x n grid (2 n^2 triangles) with normals, as a GLB: large enough
// at a few hundred a side to take a while to import.
static void writeGridGlb(const std::filesystem::path& path, int n) {
    std::string bin;
    auto put = [&](const void* p, size_t size) { bin.append(static_cast<const char*>(p), size); };
    for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++) {
            const float x = float(i) / float(n) * 4 - 2, z = float(j) / float(n) * 4 - 2;
            const float v[3] = {x, 0.2f * std::sin(x * 5) * std::cos(z * 5), z};
            put(v, sizeof v);
        }
    const size_t posBytes = bin.size();
    for (int k = 0; k < (n + 1) * (n + 1); k++) {
        const float v[3] = {0, 1, 0};
        put(v, sizeof v);
    }
    const size_t norBytes = bin.size() - posBytes;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            const uint32_t a = uint32_t(j * (n + 1) + i), w = uint32_t(n + 1);
            const uint32_t t[6] = {a, a + w, a + 1, a + 1, a + w, a + w + 1};
            put(t, sizeof t);
        }
    const size_t idxBytes = bin.size() - posBytes - norBytes;
    const int nv = (n + 1) * (n + 1);
    std::string js = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"Grid\",\"mesh\":0}],"
                     "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
                     "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":["
                     "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(posBytes) + "},"
                     "{\"buffer\":0,\"byteOffset\":" + std::to_string(posBytes) + ",\"byteLength\":" + std::to_string(norBytes) + "},"
                     "{\"buffer\":0,\"byteOffset\":" + std::to_string(posBytes + norBytes) + ",\"byteLength\":" + std::to_string(idxBytes) + "}],"
                     "\"accessors\":["
                     "{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(nv) + ",\"type\":\"VEC3\",\"min\":[-2,-0.2,-2],\"max\":[2,0.2,2]},"
                     "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(nv) + ",\"type\":\"VEC3\"},"
                     "{\"bufferView\":2,\"componentType\":5125,\"count\":" + std::to_string(n * n * 6) + ",\"type\":\"SCALAR\"}]}";
    while (js.size() % 4) js += ' ';
    while (bin.size() % 4) bin += '\0';
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    u32(0x46546C67), u32(2), u32(uint32_t(28 + js.size() + bin.size()));
    u32(uint32_t(js.size())), u32(0x4E4F534A);
    out.write(js.data(), std::streamsize(js.size()));
    u32(uint32_t(bin.size())), u32(0x004E4942);
    out.write(bin.data(), std::streamsize(bin.size()));
}

// What PR #84's queue left open: a cancelled build stops within a moment
// (and the next import waits for it), a model whose name is taken in models/
// while it imports gets the next free name, folders a killed editor left in
// the temp folder are swept, and the build says how far it is.
void testModelImportsGaps() {
    namespace fs = std::filesystem;
    using Outcome = editor::ModelImports::Outcome;
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::milliseconds;
    std::error_code ec;
    const fs::path dir = g_outDir / "modelimportsgaps";
    fs::remove_all(dir, ec);
    const fs::path proj = dir / "game", outside = dir / "downloads";
    fs::create_directories(proj, ec);
    fs::create_directories(outside, ec);
    const ScopedTmpdir scopedTmp(dir / "tmp");
    writeGridGlb(outside / "terrain.glb", 300);  // 180000 triangles
    std::ofstream(outside / "tri.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream(outside / "rock.obj") << "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
    std::ofstream(outside / "robot.gltf") << robotGltf(0.5f);
    const std::set<std::string> tempsBefore = importTemps();
    splash::Scene sc;
    sc.objects.push_back(named("A"));
    editor::Document doc;
    doc.reset(sc, proj, "game");

    // Built to the end: the fraction climbs to 1. The time it takes is what
    // a cancel must cut short.
    float seenMid = -1;
    const auto t0 = clock::now();
    {
        editor::StagedBuild b(editor::stagePlace(doc, outside / "terrain.glb"));
        float last = 0;
        bool monotonic = true;
        while (!b.wait(ms(5))) {
            const float f = b.fraction();
            monotonic = monotonic && f >= last;
            last = f;
            if (f > 0 && f < 1) seenMid = f;
        }
        CHECK(monotonic);
        CHECK(b.fraction() == 1.0f);
        editor::StagedModel s = b.take();
        CHECK(s.error.empty() && s.model.triangles == 180000);
        editor::discardStaged(s);
    }
    const auto full = std::chrono::duration_cast<ms>(clock::now() - t0);
    std::printf("  terrain.glb builds in %lld ms\n", (long long)full.count());
    CHECK(seenMid > 0 && seenMid < 1);

    // Cancelled once it is under way: done within a bound well short of the
    // whole build, with nothing left in the temp folder.
    {
        editor::StagedBuild b(editor::stagePlace(doc, outside / "terrain.glb"));
        while (b.fraction() <= 0 && !b.done()) std::this_thread::sleep_for(ms(1));
        CHECK(!b.done());
        const auto c0 = clock::now();
        b.cancel();
        const bool stopped = b.wait(ms(10000));
        const auto took = std::chrono::duration_cast<ms>(clock::now() - c0);
        std::printf("  a cancelled build stops %lld ms after the cancel\n", (long long)took.count());
        CHECK(stopped);
        // At most a quarter of the whole build, and never above 2 s.
        CHECK(took.count() * 4 <= full.count() && took.count() < 2000);
    }
    for (int i = 0; i < 1000 && importTemps() != tempsBefore; ++i) std::this_thread::sleep_for(ms(10));
    CHECK(importTemps() == tempsBefore);

    // Through the queue: the next import does not start while the cancelled
    // build is still stopping, and the running import reports its fraction.
    {
        editor::ModelImports q;
        auto request = [&](const fs::path& file) {
            editor::ModelImports::Request r;
            r.file = file;
            r.to.at = splash::Vec3{0, 0, 0};
            r.loadId = doc.loadId();
            return r;
        };
        q.add(request(outside / "terrain.glb"));
        q.add(request(outside / "tri.obj"));
        CHECK(q.fraction() < 0);
        q.update(doc);
        while (q.fraction() <= 0 && !q.finishing()) std::this_thread::sleep_for(ms(1));
        CHECK(q.fraction() > 0 && q.fraction() < 1);
        q.cancel();
        bool overlapped = false;
        std::vector<Outcome::Kind> kinds;
        const auto until = clock::now() + std::chrono::seconds(30);
        while (q.busy() && clock::now() < until) {
            for (Outcome& o : q.update(doc)) {
                kinds.push_back(o.kind);
                if (o.kind == Outcome::Ready) editor::discardStaged(o.staged);
            }
            if (q.current() && q.current()->file == outside / "tri.obj" && q.stopping() > 0) overlapped = true;
        }
        CHECK((kinds == std::vector<Outcome::Kind>{Outcome::Cancelled, Outcome::Ready}));
        CHECK(!overlapped);
    }

    // The name taken while it imported: a different file of the same name
    // (or the baked mesh's name) appeared in models/. The copy takes the next
    // free name, the outputs follow it, and what appeared is left alone.
    auto placeWith = [&](const fs::path& file, auto meanwhile) {
        editor::StagedModel s = editor::stagePlace(doc, file);
        editor::buildStaged(s);
        meanwhile();
        const editor::ObjectPath at{int(doc.scene().objects.size())};
        return editor::applyPlace(doc, s, std::nullopt, at, splash::Vec3{});
    };
    fs::create_directories(proj / "models", ec);
    editor::PlacedModel p = placeWith(outside / "tri.obj", [&] { std::ofstream(proj / "models" / "tri.obj", std::ios::binary) << "# mine\n"; });
    CHECK(p.error.empty() && p.renamed == "models/tri_2.obj");
    CHECK(fileBytes(proj / "models" / "tri.obj") == "# mine\n" && !fs::exists(proj / "models" / "tri.obj.import.json"));
    CHECK(fs::exists(proj / "models" / "tri_2.obj") && fs::exists(proj / "models" / "tri_2.mesh"));
    CHECK(fs::exists(proj / "models" / "tri_2.obj.import.json") && !fs::exists(proj / "models" / "tri.mesh"));
    CHECK(p.model.mesh == "models/tri_2.mesh");
    CHECK(doc.object(p.path) && doc.object(p.path)->mesh && doc.object(p.path)->mesh->mesh == "models/tri_2.mesh");

    p = placeWith(outside / "rock.obj", [&] { std::ofstream(proj / "models" / "rock.mesh") << "mine"; });
    CHECK(p.error.empty() && p.renamed == "models/rock_2.obj");
    CHECK(fileBytes(proj / "models" / "rock.mesh") == "mine" && !fs::exists(proj / "models" / "rock.obj"));
    CHECK(fs::exists(proj / "models" / "rock_2.mesh") && fs::exists(proj / "models" / "rock_2.obj"));

    // A node tree: its folder follows the name too.
    p = placeWith(outside / "robot.gltf", [&] { std::ofstream(proj / "models" / "robot.gltf") << "{}"; });
    CHECK(p.error.empty() && p.renamed == "models/robot_2.gltf");
    CHECK(fs::exists(proj / "models" / "robot_2" / "Body.mesh") && !fs::exists(proj / "models" / "robot"));
    bool nodesFollow = !p.model.nodes.empty();
    for (const splash::ImportedNode& n : p.model.nodes)
        nodesFollow = nodesFollow && (n.mesh.empty() || n.mesh.rfind("models/robot_2/", 0) == 0);
    CHECK(nodesFollow);
    CHECK(fileBytes(proj / "models" / "robot.gltf") == "{}");

    // Nothing appeared: the name it was staged with, no note.
    p = placeWith(outside / "tri.obj", [] {});
    CHECK(p.error.empty() && p.renamed.empty());
    for (int i = 0; i < 1000 && importTemps() != tempsBefore; ++i) std::this_thread::sleep_for(ms(10));
    CHECK(importTemps() == tempsBefore);

    // Stale folders: only splashedit-import-* folders older than the age,
    // and not one a staged import in this process still holds.
    const fs::path tmp = dir / "sweep";
    fs::create_directories(tmp, ec);
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(2);
    auto folder = [&](const std::string& name, bool aged) {
        fs::create_directories(tmp / name / "models", ec);
        std::ofstream(tmp / name / "models" / "x.mesh") << "x";
        if (aged) fs::last_write_time(tmp / name, old, ec);
    };
    folder("splashedit-import-1-0", true);
    folder("splashedit-import-2-0", false);
    folder("other-import-3-0", true);
    folder("splashedit-import-held", true);
    std::ofstream(tmp / "splashedit-import-file") << "not a folder";
    fs::last_write_time(tmp / "splashedit-import-file", old, ec);
    editor::StagingFolder held;
    held.reset(tmp / "splashedit-import-held");
    const int removed = editor::sweepStaleImportFolders(std::chrono::hours(1), tmp);
    CHECK(removed == 1);
    CHECK(!fs::exists(tmp / "splashedit-import-1-0"));
    CHECK(fs::exists(tmp / "splashedit-import-2-0") && fs::exists(tmp / "other-import-3-0"));
    CHECK(fs::exists(tmp / "splashedit-import-held") && fs::is_regular_file(tmp / "splashedit-import-file"));
    held.reset();
    CHECK(editor::sweepStaleImportFolders(std::chrono::hours(1), tmp) == 0);  // the held one went with its owner
    CHECK(!fs::exists(tmp / "splashedit-import-held"));
}

// splash::BlendExport with a mock Blender (a shell script that starts a
// sleeping child and waits for it): the constructor returns at once, cancel()
// kills the script and its child, get() then throws BlendCancelled, nothing
// is written and the temporary folder is gone. Destroying a job that is still
// running cancels it the same way.
void testBlendCancel() {
#ifdef _WIN32
    std::printf("SKIP testBlendCancel: the mock Blender is a shell script\n");
#else
    namespace fs = std::filesystem;
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const fs::path dir = g_outDir / "blendcancel";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path mock = dir / "blender", blend = dir / "crate.blend", pids = dir / "pids";
    std::ofstream(blend) << "BLENDER-v300 not really";
    const ScopedTmpdir scopedTmp(dir / "tmp");
    // The child sleeps far longer than any bound below, so a job that is never
    // killed cannot pass by Blender ending on its own.
    std::ofstream(mock) << "#!/bin/sh\nsleep 120 &\necho $$ $! > '" << pids.string() << ".tmp'\nmv '" << pids.string()
                        << ".tmp' '" << pids.string() << "'\nwait\n";
    fs::permissions(mock, fs::perms::owner_all, fs::perm_options::add);
    setenv("SPLASHEDIT_BLENDER", mock.c_str(), 1);
    auto temps = [] {
        std::set<std::string> s;
        std::error_code e;
        for (fs::directory_iterator d(fs::temp_directory_path(e), e), end; !e && d != end; d.increment(e))
            if (d->path().filename().string().rfind("splashedit-blend-", 0) == 0) s.insert(d->path().filename().string());
        return s;
    };
    // Gone, or a zombie left for init to reap.
    auto alive = [](int pid) {
        if (pid <= 0 || kill(pid, 0) != 0) return false;
        std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        std::getline(st, line);
        return line.find(") Z ") == std::string::npos;
    };
    auto waitFor = [](auto cond, std::chrono::milliseconds limit) {
        const auto t = clock::now();
        while (!cond() && clock::now() - t < limit) std::this_thread::sleep_for(10ms);
        return cond();
    };
    const std::set<std::string> before = temps();
    for (int destroy = 0; destroy < 2; ++destroy) {
        fs::remove(pids, ec);
        int shell = 0, child = 0;
        const auto t0 = clock::now();
        {
            splash::BlendExport job(blend);
            CHECK(clock::now() - t0 < 30s);  // Blender runs on the worker thread (it would block 120 s)
            CHECK(waitFor([&] { return fs::exists(pids); }, 30000ms));
            std::ifstream(pids) >> shell >> child;
            CHECK(shell > 0 && child > 0);
            CHECK(!job.done());
            CHECK(temps().size() == before.size() + 1);
            if (!destroy) {
                const auto c0 = clock::now();
                job.cancel();
                CHECK(waitFor([&] { return job.done(); }, 30000ms));
                CHECK(clock::now() - c0 < 30s);
                bool cancelled = false;
                try {
                    job.get();
                } catch (const splash::BlendCancelled&) {
                    cancelled = true;
                } catch (const std::exception&) {
                }
                CHECK(cancelled);
            }
        }
        CHECK(clock::now() - t0 < 60s);  // never killed, the destructor would wait the 120 s out
        CHECK(waitFor([&] { return !alive(shell) && !alive(child); }, 30000ms));
        CHECK(waitFor([&] { return temps() == before; }, 10000ms));
    }
    unsetenv("SPLASHEDIT_BLENDER");
    fs::remove_all(dir, ec);
#endif
}

// ---- Quantizer speed and quality

// A 256x256 image with 63660 distinct colours (ramps plus noise), built from
// integers so it is the same on every platform: the 8 bpp quantizer's worst
// case, one k-means point per distinct colour.
static splash::Image busyImage() {
    splash::Image img;
    img.width = img.height = 256;
    img.pixels.resize(256 * 256);
    uint32_t s = 12345;
    auto noise = [&] {
        s = s * 1664525u + 1013904223u;
        return int(s >> 24) % 41 - 20;
    };
    auto c8 = [](int v) { return float(v < 0 ? 0 : v > 255 ? 255 : v) / 255.f; };
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int r = x + noise(), g = y * 3 / 4 + ((x * y) >> 9) + noise(), b = 255 - (x + y) / 2 + noise();
            img.pixels[size_t(y) * 256 + x] = {c8(r), c8(g), c8(b), 1.f};
        }
    return img;
}

void testQuantizer() {
    splash::Image img = busyImage();
    // Speed: the brute-force k-means took 5-8 s on this image in an optimised
    // build and over 40 s unoptimised; the bounded one 0.5 s and 1.6 s. Each
    // build type gets its own bound, loose for slow CI machines, so a CI
    // (Release) build still catches the old quantizer.
    auto t0 = std::chrono::steady_clock::now();
    splash::PsxTexture t8 = splash::convertTexture(img, splash::BitDepth::Bpp8);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "quantizer: 256x256 8 bpp in %.2f s\n", sec);
#ifdef NDEBUG
    const double limit = 3.0;
#else
    const double limit = 12.0;
#endif
    CHECK(sec < limit);
    // Quality: the same as the brute-force quantizer's (its output is
    // byte-identical on x86-64: deltaE 1.716 / 4.827, blurred 0.462 / 2.129).
    // The headroom only allows for other platforms' float maths (a perturbed
    // cbrt/pow or FMA moved none of them in the 4th decimal); a quantizer that
    // stops early or skips a centre it should not have lands 0.02+ away.
    splash::TextureError e8 = splash::measureTexture(img, t8);
    std::fprintf(stderr, "quantizer: 8 bpp deltaE %.4f blur %.4f, %d colours\n", e8.deltaE, e8.deltaEBlur, e8.colorsUsed);
    CHECK(e8.colorsUsed == 256);
    CHECK(e8.deltaE < 1.7163 + 0.005);
    CHECK(e8.deltaEBlur < 0.4621 + 0.005);
    splash::TextureError e4 = splash::measureTexture(img, splash::convertTexture(img, splash::BitDepth::Bpp4));
    std::fprintf(stderr, "quantizer: 4 bpp deltaE %.4f blur %.4f, %d colours\n", e4.deltaE, e4.deltaEBlur, e4.colorsUsed);
    CHECK(e4.colorsUsed == 16);
    CHECK(e4.deltaE < 4.8265 + 0.005);
    CHECK(e4.deltaEBlur < 2.1287 + 0.005);
    // Deterministic: the same image converts to the same words.
    splash::PsxTexture again = splash::convertTexture(img, splash::BitDepth::Bpp8);
    CHECK(again.palette == t8.palette && again.imageData == t8.imageData);
}

// ---- Texture preview (inspector side-by-side)

// The VRAM image an export's .vram file describes: atlases and CLUTs placed.
static std::vector<uint16_t> vramFromFile(const std::filesystem::path& file) {
    std::vector<uint16_t> vram(1024 * 512, 0);
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t p) { return p + 1 < d.size() ? uint16_t(d[p] | d[p + 1] << 8) : uint16_t(0); };
    if (d.size() < 8 || d[0] != 'V' || d[1] != 'R') return {};
    int na = u16(2), nc = u16(4);
    size_t p = 8;
    for (int i = 0; i < na; i++) {
        int x = u16(p), y = u16(p + 2), w = u16(p + 4), h = u16(p + 6);
        p += 8;
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++, p += 2)
                if (x + c < 1024 && y + r < 512) vram[size_t(y + r) * 1024 + x + c] = u16(p);
        p = (p + 3) & ~size_t(3);
    }
    for (int i = 0; i < nc; i++) {
        int x = u16(p) * 16, y = u16(p + 2), n = u16(p + 4);
        p += 8;
        for (int k = 0; k < n; k++, p += 2)
            if (x + k < 1024 && y < 512) vram[size_t(y) * 1024 + x + k] = u16(p);
        p = (p + 3) & ~size_t(3);
    }
    return vram;
}

// What the GPU samples for a placed texture, RGBA top-down, the 5-bit levels
// expanded as decodeTexture does.
static std::vector<uint8_t> sampleVram(const std::vector<uint16_t>& vram, const splash::ExportStats::TexturePlacement& t) {
    std::vector<uint8_t> out(size_t(t.width) * t.height * 4);
    auto at = [&](int x, int y) { return vram[size_t(y) * 1024 + x]; };
    for (int y = 0; y < t.height; y++)
        for (int x = 0; x < t.width; x++) {
            uint16_t w;
            if (t.bitDepth == 16) w = at(t.x + x, t.y + y);
            else if (t.bitDepth == 8) w = at(t.clutX + ((at(t.x + x / 2, t.y + y) >> ((x & 1) * 8)) & 0xff), t.clutY);
            else w = at(t.clutX + ((at(t.x + x / 4, t.y + y) >> ((x & 3) * 4)) & 0xf), t.clutY);
            auto e = [](unsigned v) { return uint8_t((v * 255 + 15) / 31); };
            uint8_t* o = &out[(size_t(y) * t.width + x) * 4];
            o[0] = e(w & 31), o[1] = e((w >> 5) & 31), o[2] = e((w >> 10) & 31), o[3] = w ? 255 : 0;
        }
    return out;
}

void testTexturePreview() {
    namespace fs = std::filesystem;
    const fs::path fixture = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "quantpreview";
    const fs::path image = fixture / "gradient.png";

    // The preview is what the export writes: one crate per bit depth,
    // exported, its texture read back out of the .vram file.
    for (splash::BitDepth depth : {splash::BitDepth::Bpp4, splash::BitDepth::Bpp8, splash::BitDepth::Bpp16}) {
        editor::TexturePreview p = editor::makeTexturePreview({image, depth, false});
        CHECK(p.error.empty());
        CHECK(p.width == 64 && p.height == 64);
        CHECK(p.source.size() == 64 * 64 * 4 && p.ps1.size() == 64 * 64 * 4);
        CHECK(p.paletteSize == (depth == splash::BitDepth::Bpp4 ? 16 : depth == splash::BitDepth::Bpp8 ? 256 : 0));
        CHECK(p.paletteUsed <= p.paletteSize);
        CHECK(depth == splash::BitDepth::Bpp16 || p.ps1Colors <= p.paletteUsed);
        CHECK(p.sourceColors > 256);  // so 4 and 8 bpp both go through k-means and dithering
        CHECK(p.deltaE > 0 && p.deltaE < 20);

        splash::Scene scene;
        splash::Object o = named("Crate");
        splash::MeshComponent m;
        m.mesh = "../../examples/courtyard/meshes/crate.mesh";
        m.materials.push_back({"gradient.png", {1, 1, 1, 1}, {}});
        m.bitDepth = depth;
        o.mesh = m;
        scene.objects.push_back(o);
        const fs::path dir = g_outDir / "texpreview";
        fs::create_directories(dir);
        splash::ExportResult r = splash::exportSplashpack(scene, fixture, dir / "scene.splashpack");
        CHECK(r.ok());
        CHECK(r.stats.textures.size() == 1);
        if (!r.ok() || r.stats.textures.size() != 1) continue;
        const auto& t = r.stats.textures[0];
        CHECK(t.bitDepth == int(depth) && t.width == 64 && t.height == 64 && !t.cutout);
        std::vector<uint16_t> vram = vramFromFile(dir / "scene.vram");
        CHECK(!vram.empty());
        if (vram.empty()) continue;
        std::vector<uint8_t> exported = sampleVram(vram, t);
        size_t differ = 0;
        for (size_t i = 0; i < exported.size(); i += 4)
            differ += std::memcmp(&exported[i], &p.ps1[i], 4) != 0;
        CHECK(differ == 0);
        if (differ) std::fprintf(stderr, "texture preview: %zu of %d texels differ from the export at %d bpp\n", differ, 64 * 64, int(depth));
    }

    // Few colours: exact palette, nothing dithered, every colour kept.
    {
        splash::Image img;
        img.width = 8;
        img.height = 4;
        const splash::Image::Px cols[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0.2f, 0.4f, 0.6f, 1}};
        for (int i = 0; i < 32; i++) img.pixels.push_back(cols[(i / 3) % 3]);
        editor::TexturePreview p = editor::makeTexturePreview(img, {"mem", splash::BitDepth::Bpp4, false});
        CHECK(p.error.empty());
        CHECK(p.sourceColors == 3 && p.ps1Colors == 3 && p.paletteUsed == 3 && p.paletteSize == 4);
        CHECK(p.deltaE < 0.5);
        bool same = true;
        for (size_t i = 0; i < p.ps1.size(); i++) same &= std::abs(int(p.ps1[i]) - int(p.source[i])) <= 4;
        CHECK(same);
        // Rows top-down: the bottom-up Image's last row is the first one out.
        CHECK(p.source[0] == uint8_t(std::lround(img.pixels[size_t(3 * 8)].r * 255)));

        // Cutout: transparent texels stay transparent in both.
        img.pixels[size_t(3 * 8)].a = 0;  // bottom-up row 3 = top row
        editor::TexturePreview c = editor::makeTexturePreview(img, {"mem", splash::BitDepth::Bpp4, true});
        CHECK(c.error.empty() && c.ps1[3] == 0 && c.source[3] == 0);
        editor::TexturePreview n = editor::makeTexturePreview(img, {"mem", splash::BitDepth::Bpp4, false});
        CHECK(n.error.empty() && n.ps1[3] == 255 && n.source[3] == 255);  // not cutout: the console draws it
    }

    // A width the bit depth cannot pack is an error, not a crash.
    {
        splash::Image img;
        img.width = 6;
        img.height = 2;
        img.pixels.assign(12, {0.5f, 0.5f, 0.5f, 1});
        CHECK(!editor::makeTexturePreview(img, {"mem", splash::BitDepth::Bpp4, false}).error.empty());
        CHECK(editor::makeTexturePreview(img, {"mem", splash::BitDepth::Bpp8, false}).error.empty());
        CHECK(!editor::makeTexturePreview({fixture / "missing.png", splash::BitDepth::Bpp8, false}).error.empty());
    }

    // Worst case for the UI: 256 x 256 noise at 8 bpp. Converting takes many
    // frames, so the previewer must hand it to its thread and return at once.
    {
        splash::Image img;
        img.width = img.height = 256;
        uint32_t s = 12345;
        auto rnd = [&] { return (s = s * 1664525u + 1013904223u) >> 8 & 0xffff; };
        for (int i = 0; i < 256 * 256; i++) {
            float x = float(i % 256) / 255.f, y = float(i / 256) / 255.f;
            img.pixels.push_back({x * 0.8f + float(rnd()) / 65535.f * 0.2f, y, (x + y) * 0.5f, 1});
        }
        const fs::path dir = g_outDir / "texpreview";
        fs::create_directories(dir);
        const fs::path big = dir / "noise256.png";
        splash::savePng(img, big);
        for (splash::BitDepth depth : {splash::BitDepth::Bpp4, splash::BitDepth::Bpp8, splash::BitDepth::Bpp16}) {
            editor::TexturePreview p = editor::makeTexturePreview({big, depth, false});
            CHECK(p.error.empty());
            std::printf("texture preview 256x256 %d bpp: %.1f ms\n", int(depth), p.convertMs);
        }

        editor::TexturePreviewer pv;
        CHECK(!pv.latest());
        auto t0 = std::chrono::steady_clock::now();
        pv.request({big, splash::BitDepth::Bpp8, false});
        pv.request({big, splash::BitDepth::Bpp4, false});  // newest wins
        double requestMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(requestMs < 5);
        pv.wait();
        CHECK(!pv.busy());
        auto last = pv.latest();
        CHECK(last && last->key.depth == splash::BitDepth::Bpp4 && last->error.empty());
        // Back to a depth already done: answered from the cache, no work.
        pv.request({big, splash::BitDepth::Bpp4, false});
        CHECK(!pv.busy());
        unsigned g = pv.generation();
        pv.request({image, splash::BitDepth::Bpp16, false});
        pv.wait();
        CHECK(pv.generation() > g && pv.latest()->key.file == image);
        pv.request({big, splash::BitDepth::Bpp4, false});
        CHECK(!pv.busy() && pv.latest()->key.file == big);  // cached
        // A changed file is converted again.
        pv.invalidate(big);
        pv.wait();
        CHECK(pv.latest()->key.file == big && pv.latest()->error.empty());
    }
}

// The Project panel's model: listing, classification, filters, references
// across scenes and the rename/delete rules.
void testAssetBrowser() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path proj = g_outDir / "assetbrowser" / "game";
    fs::remove_all(proj.parent_path(), ec);
    for (const char* d : {"textures", "scripts", "meshes", "models", ".git"}) fs::create_directories(proj / d, ec);
    auto write = [&](const char* rel, const std::string& text) { std::ofstream(proj / rel, std::ios::binary) << text; };
    write("textures/brick.png", "x");
    write("textures/Spare Brick.png", "xy");
    write("scripts/logic.lua", "-- logic\n");
    write("scripts/unused.lua", "-- unused\n");
    write("meshes/crate.mesh", "{}");
    write("music.wav", "RIFF");
    write("notes.txt", "hello");
    write(".hidden", "h");
    write(".git/HEAD", "ref");
    // An imported model: its sidecar (hidden) and what its import wrote.
    write("models/robot.gltf", "{}");
    write("models/robot.gltf.import.json", "{}");
    write("models/robot.mesh", "{}");
    write("models/robot_0.png", "p");
    fs::create_directories(proj / "models" / "robot", ec);
    write("models/robot/Arm.mesh", "{}");
    write("models/robot/walk.anim", "{}");
    write("models/loose.obj", "v 0 0 0\n");  // no sidecar: never imported

    // The open scene uses the brick texture, the crate mesh, the logic script
    // and the robot's arm; another scene on disk uses the wav.
    splash::Scene sc;
    splash::Object wall = named("Wall");
    wall.mesh.emplace();
    wall.mesh->mesh = "meshes/crate.mesh";
    wall.mesh->materials.push_back({"textures/brick.png", {1, 1, 1, 1}, {}});
    splash::Object logic = named("Logic");
    logic.script.emplace();
    logic.script->lua = "scripts/logic.lua";
    splash::Object robot = named("Robot");
    splash::Object arm = named("Arm");
    arm.mesh.emplace();
    arm.mesh->mesh = "models/robot/Arm.mesh";
    robot.children.push_back(arm);
    sc.objects = {wall, logic, robot};
    splash::saveScene(sc, proj / "game.scene");
    splash::Scene other;
    splash::Object amb = named("Ambience");
    amb.audio.emplace();
    amb.audio->clip = "music.wav";
    other.objects.push_back(amb);
    splash::saveScene(other, proj / "other.scene");

    editor::AssetIndex index;
    CHECK(index.scan(proj));
    const unsigned gen = index.generation();
    {
        // On a mismatch, name the entries that differ between two scans.
        const std::vector<editor::AssetEntry> before = index.entries();
        const bool again = index.scan(proj);
        CHECK(!again && index.generation() == gen);  // nothing changed
        if (again) {
            const auto& after = index.entries();
            std::fprintf(stderr, "rescan: %zu entries before, %zu after\n", before.size(), after.size());
            for (size_t i = 0; i < std::max(before.size(), after.size()); ++i) {
                const editor::AssetEntry* x = i < before.size() ? &before[i] : nullptr;
                const editor::AssetEntry* y = i < after.size() ? &after[i] : nullptr;
                if (x && y && x->path == y->path && x->size == y->size && x->mtime == y->mtime && x->type == y->type &&
                    x->madeBy == y->madeBy)
                    continue;
                auto show = [](const char* tag, const editor::AssetEntry* e) {
                    if (!e) return std::fprintf(stderr, "  %s (none)\n", tag), void();
                    std::fprintf(stderr, "  %s %s size %llu mtime %lld type %d madeBy %s\n", tag, e->path.c_str(),
                                 (unsigned long long)e->size, (long long)e->mtime.time_since_epoch().count(), int(e->type),
                                 e->madeBy.c_str());
                };
                show("before", x);
                show("after ", y);
            }
        }
    }
    CHECK(!index.find(".hidden") && !index.find(".git") && !index.find(".git/HEAD"));
    CHECK(!index.find("models/robot.gltf.import.json"));
    using T = editor::AssetType;
    auto type = [&](const char* p) { return index.find(p) ? index.find(p)->type : T::Other; };
    CHECK(index.find("textures") && type("textures") == T::Folder);
    CHECK(type("textures/brick.png") == T::Texture && type("scripts/logic.lua") == T::Script && type("meshes/crate.mesh") == T::Mesh);
    CHECK(type("music.wav") == T::Audio && type("game.scene") == T::Scene && type("models/robot.gltf") == T::Model);
    CHECK(type("models/robot/walk.anim") == T::Animation && type("notes.txt") == T::Other);
    CHECK(editor::classifyAsset("A.PNG") == T::Texture && editor::classifyAsset("x.GLB") == T::Model);
    // The model's outputs know it; a model without a sidecar has none.
    CHECK(index.find("models/robot.mesh") && index.find("models/robot.mesh")->madeBy == "models/robot.gltf");
    CHECK(index.find("models/robot_0.png")->madeBy == "models/robot.gltf");
    CHECK(index.find("models/robot/Arm.mesh")->madeBy == "models/robot.gltf");
    CHECK(index.find("models/robot")->madeBy == "models/robot.gltf");
    CHECK(index.find("models/loose.obj")->madeBy.empty() && index.find("textures/brick.png")->madeBy.empty());
    // Root: folders first, then files, by name ignoring case.
    {
        std::vector<std::string> names;
        for (const editor::AssetEntry* e : index.children("")) names.push_back(e->name);
        CHECK((names == std::vector<std::string>{"meshes", "models", "scripts", "textures", "game.scene", "music.wav", "notes.txt",
                                                 "other.scene"}));
        std::vector<std::string> folders = index.folders();
        CHECK(folders.size() == 6 && folders[0].empty() && folders[1] == "meshes" && folders[3] == "models/robot");
    }

    // Filters: the folder alone lists it; a search covers every folder below,
    // all words (any case); a type filter lists only that type.
    {
        editor::AssetFilter f;
        f.folder = "textures";
        CHECK(editor::filterAssets(index, f).size() == 2);
        f.folder = "";
        f.query = "BRICK";
        auto hits = editor::filterAssets(index, f);
        CHECK(hits.size() == 2 && hits[0]->path == "textures/brick.png" && hits[1]->path == "textures/Spare Brick.png");
        f.query = "spare brick";
        CHECK(editor::filterAssets(index, f).size() == 1);
        f.query = "brick";
        f.folder = "scripts";
        CHECK(editor::filterAssets(index, f).empty());
        f.folder = "";
        f.query.clear();
        f.types = 1u << unsigned(T::Script);
        hits = editor::filterAssets(index, f);
        CHECK(hits.size() == 2 && hits[0]->type == T::Script && hits[1]->type == T::Script);
        f.types |= 1u << unsigned(T::Mesh);
        CHECK(editor::filterAssets(index, f).size() == 5);  // + crate, robot, Arm
        f.query = "zzz";
        CHECK(editor::filterAssets(index, f).empty());
    }

    // References: the open document's scene from memory, other scenes from disk.
    editor::Document doc;
    doc.reset(sc, proj, "game");
    editor::AssetReferences refs;
    refs.update(index, doc.scene(), "game.scene", doc.revision());
    auto brickRefs = refs.to(index, "textures/brick.png");
    CHECK(brickRefs.size() == 1 && brickRefs[0].object == "Wall" && brickRefs[0].field == "Texture" &&
          brickRefs[0].scene == "game.scene" && brickRefs[0].path == editor::ObjectPath{0});
    CHECK(refs.to(index, "textures/Spare Brick.png").empty());
    auto wavRefs = refs.to(index, "music.wav");
    CHECK(wavRefs.size() == 1 && wavRefs[0].scene == "other.scene" && wavRefs[0].field == "Audio clip");
    auto armRefs = refs.to(index, "models/robot.gltf");  // through its outputs
    CHECK(armRefs.size() == 1 && armRefs[0].object == "Robot/Arm" && (armRefs[0].path == editor::ObjectPath{2, 0}));
    CHECK(refs.to(index, "textures").size() == 1 && refs.to(index, "scripts").size() == 1 && refs.to(index, "notes.txt").empty());
    // An unsaved edit counts at once.
    doc.edit({1}, [](splash::Object& o) { o.script->lua = "scripts/unused.lua"; });
    refs.update(index, doc.scene(), "game.scene", doc.revision());
    CHECK(refs.to(index, "scripts/logic.lua").empty() && refs.to(index, "scripts/unused.lua").size() == 1);
    doc.undo();
    refs.update(index, doc.scene(), "game.scene", doc.revision());
    CHECK(refs.to(index, "scripts/logic.lua").size() == 1);

    // Every rename, move and delete below goes through the document's history.
    auto rescan = [&] {
        index.scan(proj);
        refs.update(index, doc.scene(), "game.scene", doc.revision());
    };
    auto run = [&](const editor::AssetPlan& plan) {
        editor::AssetOp o = editor::applyAssetPlan(doc, index, "game.scene", plan);
        rescan();
        return o;
    };
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    auto wallTexture = [&] { return doc.object({0})->mesh->materials[0].texture; };

    // Rename a referenced file: the file moves and the open scene's reference
    // follows, as one undo step; undo puts both back, redo does it again.
    CHECK(!doc.dirty());
    editor::AssetOp op = run(editor::planRename(index, refs, "textures/brick.png", "wall", "game.scene"));
    CHECK(op.error.empty() && op.path == "textures/wall.png" && op.references == 1 && op.scenes.empty());
    CHECK(fs::exists(proj / "textures" / "wall.png") && !fs::exists(proj / "textures" / "brick.png"));
    // The open scene's reference changed in memory: it has to be saved.
    CHECK(wallTexture() == "textures/wall.png" && doc.canUndo() && !doc.canRedo() && doc.dirty());
    CHECK(refs.to(index, "textures/wall.png").size() == 1);
    CHECK(doc.undo() && wallTexture() == "textures/brick.png" && !doc.dirty());
    CHECK(fs::exists(proj / "textures" / "brick.png") && !fs::exists(proj / "textures" / "wall.png"));
    CHECK(doc.redo() && wallTexture() == "textures/wall.png" && fs::exists(proj / "textures" / "wall.png"));
    CHECK(editor::takeAssetHistoryError().empty());
    rescan();
    // An unused one is renamed, keeping its extension.
    op = run(editor::planRename(index, refs, "textures/Spare Brick.png", "spare", "game.scene"));
    CHECK(op.error.empty() && op.path == "textures/spare.png" && op.references == 0);
    CHECK(fs::exists(proj / "textures" / "spare.png") && !fs::exists(proj / "textures" / "Spare Brick.png"));
    CHECK(std::find(op.touched.begin(), op.touched.end(), "textures/spare.png") != op.touched.end());
    CHECK(index.find("textures/spare.png") && !index.find("textures/Spare Brick.png"));
    // Names: another extension, separators, a taken name, a hidden name.
    CHECK(editor::planRename(index, refs, "textures/spare.png", "spare.jpg", "game.scene").error.find("extension") != std::string::npos);
    CHECK(!editor::planRename(index, refs, "textures/spare.png", "../spare", "game.scene").error.empty());
    CHECK(editor::planRename(index, refs, "textures/spare.png", "wall.png", "game.scene").error.find("already exists") != std::string::npos);
    CHECK(!editor::planRename(index, refs, "textures/spare.png", ".spare", "game.scene").error.empty());
    CHECK(!editor::planRename(index, refs, "textures/spare.png", "  ", "game.scene").error.empty());
    {
        editor::AssetPlan same = editor::planRename(index, refs, "textures/spare.png", "spare.png", "game.scene");
        CHECK(same.error.empty() && same.moves.empty());  // unchanged: nothing to do
    }
    // Still refused, saying why: the open scene (the document keeps its name)
    // and a model's output (the next import writes it again).
    CHECK(editor::planRename(index, refs, "game.scene", "level", "game.scene").error.find("Save As") != std::string::npos);
    CHECK(editor::planRename(index, refs, "models/robot.mesh", "bot", "game.scene").error.find("rename the model") != std::string::npos);

    // A folder holding a used file: the references under it follow.
    op = run(editor::planRename(index, refs, "textures", "images", "game.scene"));
    CHECK(op.error.empty() && wallTexture() == "images/wall.png" && fs::exists(proj / "images" / "wall.png") && !fs::exists(proj / "textures"));
    CHECK(doc.undo() && wallTexture() == "textures/wall.png" && fs::exists(proj / "textures" / "wall.png") && !fs::exists(proj / "images"));
    rescan();

    // A file another scene uses on disk: that scene file is rewritten, and
    // undo puts its bytes back exactly.
    const std::string otherBefore = slurp(proj / "other.scene");
    op = run(editor::planRename(index, refs, "music.wav", "theme", "game.scene"));
    CHECK(op.error.empty() && op.references == 1 && op.scenes == std::vector<std::string>{"other.scene"});
    CHECK(splash::loadScene(proj / "other.scene").objects[0].audio->clip == "theme.wav" && fs::exists(proj / "theme.wav"));
    CHECK(doc.undo() && slurp(proj / "other.scene") == otherBefore && fs::exists(proj / "music.wav") && !fs::exists(proj / "theme.wav"));
    CHECK(doc.redo() && splash::loadScene(proj / "other.scene").objects[0].audio->clip == "theme.wav");
    rescan();
    CHECK(refs.to(index, "theme.wav").size() == 1 && refs.to(index, "music.wav").empty());
    // A scene that is not open may be renamed; its references move with it.
    op = run(editor::planRename(index, refs, "other.scene", "second", "game.scene"));
    CHECK(op.error.empty() && fs::exists(proj / "second.scene"));
    auto wavRefs2 = refs.to(index, "theme.wav");
    CHECK(wavRefs2.size() == 1 && wavRefs2[0].scene == "second.scene");

    // Lua scripts: an object's Script and Trigger, and a scene's own script,
    // in the open scene and in a saved one, are references like any other.
    {
        splash::Scene lua;
        lua.settings.script = "scripts/logic.lua";
        splash::Object door = named("Door");
        door.trigger.emplace();
        door.trigger->lua = "./scripts/logic.lua";  // spelled another way, still the same file
        lua.objects.push_back(door);
        lua.settings.memCard.icons = {"textures/spare.png"};
        splash::saveScene(lua, proj / "lua.scene");
    }
    rescan();
    {
        auto luaRefs = refs.to(index, "scripts/logic.lua");
        int script = 0, trigger = 0, sceneScript = 0;
        for (const editor::AssetRef& r : luaRefs) {
            script += r.field == "Script";
            trigger += r.field == "Trigger script";
            sceneScript += r.field == "Scene script";
        }
        CHECK(luaRefs.size() == 3 && script == 1 && trigger == 1 && sceneScript == 1);
        auto icon = refs.to(index, "textures/spare.png");
        CHECK(icon.size() == 1 && icon[0].field == "Memory card icon" && icon[0].scene == "lua.scene");
    }
    editor::takeAssetMoves();
    op = run(editor::planRename(index, refs, "scripts/logic.lua", "brain", "game.scene"));
    CHECK(op.error.empty() && op.references == 3 && op.scenes == std::vector<std::string>{"lua.scene"});
    CHECK(doc.object({1})->script->lua == "scripts/brain.lua");
    {
        splash::Scene lua = splash::loadScene(proj / "lua.scene");
        CHECK(lua.settings.script == "scripts/brain.lua" && lua.objects[0].trigger->lua == "scripts/brain.lua");
        // Open script tabs hear of the move (done, then undone).
        auto moved = editor::takeAssetMoves();
        CHECK(moved.size() == 1 && moved[0].first == proj / "scripts/logic.lua" && moved[0].second == proj / "scripts/brain.lua");
    }

    // Move: drag into another folder, the same reference update.
    op = run(editor::planMove(index, refs, "textures/wall.png", "meshes", "game.scene"));
    CHECK(op.error.empty() && op.path == "meshes/wall.png" && wallTexture() == "meshes/wall.png" && fs::exists(proj / "meshes" / "wall.png"));
    CHECK(doc.undo() && wallTexture() == "textures/wall.png" && fs::exists(proj / "textures" / "wall.png"));
    rescan();
    op = run(editor::planMove(index, refs, "textures/spare.png", "", "game.scene"));  // to the project root, the icon follows
    CHECK(op.error.empty() && op.path == "spare.png" && splash::loadScene(proj / "lua.scene").settings.memCard.icons[0] == "spare.png");
    CHECK(editor::planMove(index, refs, "textures", "textures", "game.scene").error.find("inside itself") != std::string::npos);
    CHECK(editor::planMove(index, refs, "notes.txt", "nowhere", "game.scene").error.find("not a folder") != std::string::npos);
    CHECK(editor::planMove(index, refs, "notes.txt", "", "game.scene").moves.empty());  // already there
    CHECK(editor::planMove(index, refs, "models/robot.gltf", "meshes", "game.scene").error.find("in place") != std::string::npos);
    CHECK(editor::planMove(index, refs, "game.scene", "meshes", "game.scene").error.find("open scene") != std::string::npos);

    // A scene that cannot be read: a file it names cannot be renamed (its
    // reference could not be updated); one it does not name can.
    write("broken.scene", "{\"objects\": [{\"name\": \"x\", \"mesh\": {\"mesh\": \"meshes/crate.mesh\"");
    rescan();
    CHECK(refs.unreadable().count("broken.scene") == 1);
    {
        const std::string why = editor::planRename(index, refs, "meshes/crate.mesh", "box", "game.scene").error;
        CHECK(why.find("broken.scene") != std::string::npos && why.find("could not be read") != std::string::npos);
        CHECK(editor::planRename(index, refs, "notes.txt", "readme", "game.scene").error.empty());
    }
    fs::remove(proj / "broken.scene", ec);
    rescan();

    // A referenced model takes its sidecar and outputs along, and the
    // references to its outputs follow.
    op = run(editor::planRename(index, refs, "models/robot.gltf", "bot", "game.scene"));
    CHECK(op.error.empty() && op.path == "models/bot.gltf" && doc.object({2, 0})->mesh->mesh == "models/bot/Arm.mesh");
    CHECK(fs::exists(proj / "models" / "bot.gltf") && fs::exists(proj / "models" / "bot.gltf.import.json"));
    CHECK(fs::exists(proj / "models" / "bot.mesh") && fs::exists(proj / "models" / "bot_0.png"));
    CHECK(fs::exists(proj / "models" / "bot" / "Arm.mesh") && fs::exists(proj / "models" / "bot" / "walk.anim"));
    CHECK(!fs::exists(proj / "models" / "robot.mesh") && !fs::exists(proj / "models" / "robot"));
    CHECK(index.find("models/bot/walk.anim") && index.find("models/bot/walk.anim")->madeBy == "models/bot.gltf");

    // Delete: refused while used, listing the users, and for the open scene.
    CHECK(editor::planDelete(index, refs, "scripts/brain.lua", "game.scene").error.find("Logic in game.scene") != std::string::npos);
    CHECK(!editor::planDelete(index, refs, "game.scene", "game.scene").error.empty());
    // Unused: into .trash/<n>/ with its paths, sidecar and outputs along;
    // undo brings it all back and leaves no empty trash folder.
    doc.edit({2, 0}, [](splash::Object& o) { o.mesh->mesh = "meshes/crate.mesh"; });
    rescan();
    std::vector<std::string> set = editor::deletionSet(index, "models/bot.gltf");
    CHECK(set.size() == 7);  // model, sidecar, bot.mesh, bot_0.png, bot/, bot/Arm.mesh, bot/walk.anim
    op = run(editor::planDelete(index, refs, "models/bot.gltf", "game.scene"));
    CHECK(op.error.empty() && op.path == ".trash/1/models/bot.gltf");
    CHECK(!fs::exists(proj / "models" / "bot.gltf") && !fs::exists(proj / "models" / "bot.gltf.import.json"));
    CHECK(!fs::exists(proj / "models" / "bot") && !fs::exists(proj / "models" / "bot.mesh") && fs::exists(proj / "models" / "loose.obj"));
    CHECK(fs::exists(proj / ".trash" / "1" / "models" / "bot" / "walk.anim") && fs::exists(proj / ".trash" / "1" / "models" / "bot.gltf.import.json"));
    CHECK(!index.find(".trash") && !index.find("models/bot.gltf"));  // the trash is hidden
    CHECK(doc.undo() && fs::exists(proj / "models" / "bot.gltf") && fs::exists(proj / "models" / "bot" / "walk.anim"));
    CHECK(fs::exists(proj / "models" / "bot.gltf.import.json") && !fs::exists(proj / ".trash" / "1"));
    CHECK(doc.redo() && !fs::exists(proj / "models" / "bot.gltf") && fs::exists(proj / ".trash" / "1" / "models" / "bot.gltf"));
    rescan();
    op = run(editor::planDelete(index, refs, "scripts/unused.lua", "game.scene"));
    CHECK(op.error.empty() && !fs::exists(proj / "scripts" / "unused.lua") && fs::exists(proj / ".trash" / "2" / "scripts" / "unused.lua"));
    CHECK((op.touched == std::vector<std::string>{"scripts/unused.lua", ".trash/2/scripts/unused.lua"}));
    CHECK(editor::takeAssetHistoryError().empty());
    // A project opened later empties what has lain in .trash/ past the
    // limit, and only that; a folder the user put there is left alone.
    {
        const auto now = fs::file_time_type::clock::now();
        fs::create_directories(proj / ".trash" / "keep");
        fs::last_write_time(proj / ".trash" / "1", now - editor::kTrashKeep - std::chrono::hours(1));
        fs::last_write_time(proj / ".trash" / "keep", now - editor::kTrashKeep * 2);
        CHECK(editor::emptyOldTrash(proj, now) == 1);
        CHECK(!fs::exists(proj / ".trash" / "1") && fs::exists(proj / ".trash" / "2" / "scripts" / "unused.lua"));
        CHECK(fs::exists(proj / ".trash" / "keep") && fs::exists(proj / "scripts" / "brain.lua"));
        CHECK(editor::emptyOldTrash(proj, now) == 0);  // the rest is recent
        fs::remove(proj / ".trash" / "keep");
        CHECK(editor::emptyOldTrash(proj, now + editor::kTrashKeep + std::chrono::hours(1)) == 1);
        CHECK(!fs::exists(proj / ".trash") && fs::exists(proj / "scripts"));  // the empty trash goes too
        CHECK(editor::emptyOldTrash(proj, now) == 0 && editor::emptyOldTrash({}, now) == 0);
    }
    // The scan notices a file written behind its back.
    write("textures/new.png", "n");
    CHECK(index.scan(proj) && index.find("textures/new.png"));

    // file:// URLs for the file manager.
    CHECK(editor::fileUrl("/tmp/a b/c#.png") == "file:///tmp/a%20b/c%23.png");
    CHECK(editor::fileUrl("C:/x") == "file:///C:/x");
    CHECK(editor::sizeLabel(10) == "10 B" && editor::sizeLabel(2048) == "2 KB" && editor::sizeLabel(3 * 1024 * 1024) == "3.0 MB");
}

// ---- UI canvas editor (ui_canvas.hh)

splash::UIElement uiBox(const char* name, int x, int y, int w, int h) {
    splash::UIElement e;
    e.type = splash::UIElementType::Box;
    e.name = name;
    e.rect = {x, y, w, h};
    return e;
}

// A canvas "hud": back (0..99 x 0..49), front (50..149 x 20..69), a hidden
// box over both and a diagonal line; and a second canvas "menu".
splash::Scene uiScene() {
    splash::Scene s;
    splash::UICanvas hud;
    hud.name = "hud";
    hud.elements.push_back(uiBox("back", 0, 0, 100, 50));
    hud.elements.push_back(uiBox("front", 50, 20, 100, 50));
    splash::UIElement hidden = uiBox("hidden", 0, 0, 200, 200);
    hidden.visible = false;
    hud.elements.push_back(hidden);
    splash::UIElement line;
    line.type = splash::UIElementType::Line;
    line.name = "line";
    line.from = {200, 100};
    line.to = {300, 200};
    hud.elements.push_back(line);
    s.canvases.push_back(hud);
    splash::UICanvas menu;
    menu.name = "menu";
    menu.elements.push_back(uiBox("panel", 10, 10, 20, 20));
    s.canvases.push_back(menu);
    return s;
}

void testUiLayout() {
    using namespace editor::uic;
    const Screen s;
    // tests/ui/ui.scene's anchored elements, where uicheck.py expects them.
    splash::UIElement bar = uiBox("bar", 0, -20, 0, 20);
    bar.anchorMin = {0, 1};
    bar.anchorMax = {1, 1};
    CHECK((layout(bar, s) == Rect{0, 220, 320, 20}));
    splash::UIElement corner = uiBox("corner", -8, 0, 8, 8);
    corner.anchorMin = corner.anchorMax = {1, 0};
    CHECK((layout(corner, s) == Rect{312, 0, 8, 8}));
    CHECK((layout(uiBox("hp", 8, 80, 120, 10), s) == Rect{8, 80, 120, 10}));
    // place() is layout()'s inverse, keeping the anchors.
    for (splash::UIElement e : {bar, corner}) {
        const auto amin = e.anchorMin, amax = e.anchorMax;
        place(e, Rect{10, 30, 50, 12}, s);
        CHECK((layout(e, s) == Rect{10, 30, 50, 12}));
        CHECK(e.anchorMin == amin && e.anchorMax == amax);
    }
    // Centre anchor at an odd resolution: the export rounds 160.5 and 120.5 to even.
    splash::UIElement mid = uiBox("mid", -10, -5, 20, 10);
    mid.anchorMin = mid.anchorMax = {0.5f, 0.5f};
    CHECK((layout(mid, Screen{321, 241}) == Rect{150, 115, 20, 10}));
    // What the runtime draws: a left overhang shrinks it, the right edge cuts it.
    CHECK((drawn(uiBox("l", -8, 4, 20, 10), s) == Rect{0, 4, 12, 10}));
    CHECK((drawn(uiBox("r", 310, 235, 20, 10), s) == Rect{310, 235, 10, 5}));
    CHECK((drawn(uiBox("gone", -30, 0, 20, 10), s) == Rect{0, 0, 1, 10}));
    // A line's layout is its endpoints' box; place() moves both endpoints.
    splash::UIElement line = uiScene().canvases[0].elements[3];
    CHECK((layout(line, s) == Rect{200, 100, 101, 101}));
    place(line, Rect{0, 0, 5, 5}, s);
    CHECK((line.from == std::array<int, 2>{0, 0} && line.to == std::array<int, 2>{100, 100}));
}

void testUiHitTest() {
    using namespace editor::uic;
    using H = Handle;
    const Screen s;
    const splash::UICanvas cv = uiScene().canvases[0];
    CHECK(hitTest(cv, s, 10, 10) == 0);    // only the back box
    CHECK(hitTest(cv, s, 60, 30) == 1);    // both: the front one wins
    CHECK(hitTest(cv, s, 99.5f, 49.5f) == 1);
    CHECK(hitTest(cv, s, 160, 30) == -1);  // only the hidden box: hidden ones do not count
    CHECK(hitTest(cv, s, 250.5f, 151.5f) == 3);  // on the line
    CHECK(hitTest(cv, s, 250, 160) == -1);       // 8 px off it
    CHECK(hitTest(cv, s, 250, 160, 10) == 3);    // within a wider slop
    CHECK(hitTest(cv, s, 150, 20) == -1);  // right edge is exclusive
    // Handles: corners first, then edges, inside moves.
    const Rect r{10, 10, 40, 20};
    CHECK(hitHandle(r, 10, 10, 2) == H::TopLeft);
    CHECK(hitHandle(r, 51, 31, 2) == H::BottomRight);
    CHECK(hitHandle(r, 49, 9, 2) == H::TopRight);
    CHECK(hitHandle(r, 11, 29, 2) == H::BottomLeft);
    CHECK(hitHandle(r, 30, 9, 2) == H::Top);
    CHECK(hitHandle(r, 30, 30, 2) == H::Bottom);
    CHECK(hitHandle(r, 9, 20, 2) == H::Left);
    CHECK(hitHandle(r, 50, 20, 2) == H::Right);
    CHECK(hitHandle(r, 30, 20, 2) == H::Move);
    CHECK(hitHandle(r, 70, 20, 2) == H::None);
    const splash::UIElement& line = cv.elements[3];
    CHECK(hitLineHandle(line, 200.5f, 100.5f, 3) == H::LineFrom);
    CHECK(hitLineHandle(line, 301, 201, 3) == H::LineTo);
    CHECK(hitLineHandle(line, 250.5f, 150.5f, 3) == H::Move);
    CHECK(hitLineHandle(line, 200, 150, 3) == H::None);
}

void testUiResize() {
    using namespace editor::uic;
    using H = Handle;
    const Rect r{10, 10, 40, 20};
    CHECK((resize(r, H::BottomRight, 5, 3) == Rect{10, 10, 45, 23}));
    CHECK((resize(r, H::TopLeft, 5, 3) == Rect{15, 13, 35, 17}));
    CHECK((resize(r, H::Right, 7, 99) == Rect{10, 10, 47, 20}));  // an edge moves on its own axis only
    CHECK((resize(r, H::Top, 99, -4) == Rect{10, 6, 40, 24}));
    // Dragged past the opposite edge: stops 1 px short, the opposite edge stays put.
    CHECK((resize(r, H::Left, 100, 0) == Rect{49, 10, 1, 20}));
    CHECK((resize(r, H::BottomRight, -100, -100) == Rect{10, 10, 1, 1}));
    CHECK((resize(r, H::TopRight, -100, 100) == Rect{10, 29, 1, 1}));
    // Within the format's 16-bit sizes.
    CHECK(resize(r, H::Right, 100000, 0).w == 32767);
    CHECK(resize(r, H::Left, -100000, 0).right() == r.right());
    CHECK(resize(r, H::Move, 5, 5) == r);
}

void testUiSnap() {
    using namespace editor::uic;
    const Screen s;
    const splash::UICanvas cv = uiScene().canvases[0];
    // Near the screen's left edge: snaps to it, with a full-height guide.
    Snapped a = snapMove(cv, 1, Rect{2, 120, 30, 10}, s, 3);
    CHECK((a.rect == Rect{0, 120, 30, 10}));
    CHECK(std::count(a.guides.begin(), a.guides.end(), Guide{true, 0, 0, 240}) == 1);
    // Centre on the screen's centre.
    Snapped b = snapMove(cv, 1, Rect{146, 300, 30, 10}, s, 3);
    CHECK(b.rect.x == 145 && b.rect.y == 300);
    CHECK(std::count(b.guides.begin(), b.guides.end(), Guide{true, 160, 0, 240}) == 1);
    // To another element's edge (the back box ends at x 100), the guide spanning both.
    Snapped c = snapMove(cv, 1, Rect{102, 60, 10, 10}, s, 3);
    CHECK(c.rect.x == 100);
    CHECK(std::count(c.guides.begin(), c.guides.end(), Guide{true, 100, 0, 70}) == 1);
    // Out of reach: unchanged, no guides.
    Snapped d = snapMove(cv, 1, Rect{110, 60, 10, 10}, s, 3);
    CHECK((d.rect == Rect{110, 60, 10, 10}) && d.guides.empty());
    // Hidden elements are no targets (the hidden box ends at 200), itself neither.
    CHECK(snapMove(cv, 1, Rect{203, 60, 10, 10}, s, 2).rect.x == 203);
    CHECK(snapMove(cv, 0, Rect{1, 60, 10, 10}, s, 0).rect.x == 1);
    // Both axes at once: bottom edge to the screen's bottom.
    Snapped e = snapMove(cv, 1, Rect{318, 228, 1, 10}, s, 3);
    CHECK(e.rect.x == 319 && e.rect.y == 230 && e.guides.size() == 2);
    // Resize: only the moving edge snaps, the opposite one stays.
    Snapped f = snapResize(cv, 1, Rect{50, 20, 52, 50}, Handle::Right, s, 3);
    CHECK((f.rect == Rect{50, 20, 50, 50}) && f.guides.size() == 1);
    Snapped g = snapResize(cv, 1, Rect{2, 20, 98, 50}, Handle::Left, s, 3);
    CHECK((g.rect == Rect{0, 20, 100, 50}));
    Snapped h = snapResize(cv, 1, Rect{2, 20, 98, 50}, Handle::Right, s, 3);  // the left edge does not
    CHECK((h.rect == Rect{2, 20, 98, 50}));
    // A line endpoint.
    int x = 158, y = 49;
    std::vector<Guide> pg = snapPoint(cv, 3, &x, &y, s, 3);
    CHECK(x == 160 && y == 50 && pg.size() == 2);
}

void testUiZOrder() {
    using namespace editor::uic;
    editor::Document d;
    d.reset(uiScene());
    auto names = [&](int c) {
        std::string out;
        for (const auto& e : d.scene().canvases[size_t(c)].elements) out += e.name + ",";
        return out;
    };
    d.selectUi({0, 0});
    CHECK(moveElement(d, 0, 0, 2));
    CHECK(names(0) == "front,hidden,back,line,");
    CHECK((d.uiSelection() == editor::Document::UiSelection{0, 2}));  // follows the element
    // Another element moving past the selected one shifts it.
    CHECK(moveElement(d, 0, 3, 0));
    CHECK(names(0) == "line,front,hidden,back,");
    CHECK(d.uiSelection().element == 3);
    // Clamped to the list; a move to where it is does nothing.
    CHECK(moveElement(d, 0, 1, 99) && names(0) == "line,hidden,back,front,");
    CHECK(!moveElement(d, 0, 3, 3) && !moveElement(d, 0, 9, 0) && !moveElement(d, 5, 0, 1));
    CHECK(d.historySize() == 3);
    // One undo per move, selection and order back.
    CHECK(d.undo() && names(0) == "line,front,hidden,back,");
    CHECK(d.undo() && d.undo() && names(0) == "back,front,hidden,line,");
    CHECK((d.uiSelection() == editor::Document::UiSelection{0, 0}));
    CHECK(names(1) == "panel,");
}

void testUiUndo() {
    using namespace editor::uic;
    const Screen s;
    editor::Document d;
    d.reset(uiScene());
    d.selectUi({0, 1});
    // A drag: many edits under one key, one undo step.
    for (int i = 1; i <= 10; i++) editElement(d, 0, 1, [&](splash::UIElement& e) { place(e, Rect{50 + i, 20 + i, 100, 50}, s); }, "drag");
    d.endMerge();
    CHECK((layout(d.scene().canvases[0].elements[1], s) == Rect{60, 30, 100, 50}));
    CHECK(d.historySize() == 1 && d.dirty());
    // A no-op edit pushes nothing.
    CHECK(!editElement(d, 0, 1, [](splash::UIElement&) {}));
    CHECK(d.historySize() == 1);
    // The next drag is its own step.
    editElement(d, 0, 1, [&](splash::UIElement& e) { e.rect[2] = 7; }, "drag");
    d.endMerge();
    CHECK(d.historySize() == 2);
    CHECK(d.undo() && d.scene().canvases[0].elements[1].rect[2] == 100);
    CHECK(d.undo() && (layout(d.scene().canvases[0].elements[1], s) == Rect{50, 20, 100, 50}));
    CHECK(!d.dirty());
    CHECK(d.redo() && d.scene().canvases[0].elements[1].rect[0] == 60);
    // Add, duplicate, delete: each one step, each selecting what it made.
    d.reset(uiScene());
    int added = addElement(d, 1, splash::UIElementType::Text, s);
    CHECK(added == 1 && d.scene().canvases[1].elements[1].name == "Text");
    CHECK((d.uiSelection() == editor::Document::UiSelection{1, 1}));
    CHECK((layout(d.scene().canvases[1].elements[1], s) == Rect{100, 112, 120, 16}));
    CHECK(addElement(d, 1, splash::UIElementType::Text, s) == 2 && d.scene().canvases[1].elements[2].name == "Text (2)");
    int dup = duplicateElement(d, 0, 0);
    CHECK(dup == 1 && d.scene().canvases[0].elements[1].name == "back (2)");
    CHECK((layout(d.scene().canvases[0].elements[1], s) == Rect{8, 8, 100, 50}));
    CHECK(duplicateElement(d, 0, 1) == 2 && d.scene().canvases[0].elements[2].name == "back (3)");
    CHECK(deleteElement(d, 0, 0) && d.scene().canvases[0].elements[0].name == "back (2)");
    CHECK((d.uiSelection() == editor::Document::UiSelection{0, -1}));
    CHECK(d.historySize() == 5);
    CHECK(d.undo() && d.scene().canvases[0].elements[0].name == "back");
    CHECK((d.uiSelection() == editor::Document::UiSelection{0, 2}));  // the selection from before the delete
    CHECK(d.undo() && d.undo() && d.scene().canvases[0].elements.size() == 4);
    CHECK(d.undo() && d.undo() && d.scene().canvases[1].elements.size() == 1);
    // Canvases.
    int c = addCanvas(d);
    CHECK(c == 2 && d.scene().canvases[2].name == "Canvas" && (d.uiSelection() == editor::Document::UiSelection{2, -1}));
    CHECK(addCanvas(d) == 3 && d.scene().canvases[3].name == "Canvas (2)");
    CHECK(editCanvas(d, 0, [](splash::UICanvas& cv) { cv.visible = false; }) && !d.scene().canvases[0].visible);
    CHECK(deleteCanvas(d, 3) && d.scene().canvases.size() == 3 && d.uiSelection().canvas == 2);
    CHECK(d.undo() && d.undo() && d.scene().canvases[0].visible && d.scene().canvases.size() == 4);
    // Undo and save: the file holds what the document shows.
    d.reset(uiScene(), g_outDir, "uicanvas");
    std::filesystem::create_directories(g_outDir);
    CHECK(d.saveAs("uicanvas.scene") == std::nullopt);
    editElement(d, 0, 0, [&](splash::UIElement& e) { place(e, Rect{5, 6, 7, 8}, s); });
    CHECK(d.save() == std::nullopt);
    splash::Scene back = splash::loadScene(g_outDir / "uicanvas.scene");
    CHECK((layout(back.canvases[0].elements[0], s) == Rect{5, 6, 7, 8}));
}

// Copy, cut and paste of UI elements: the clipboard text, the offset, the
// names, one undo step each, and text of the other kind refused both ways.
void testUiClipboard() {
    using namespace editor::uic;
    const Screen s;
    editor::Document d;
    d.reset(uiScene());
    const std::string text = copyElementText(d, 0, 1);
    CHECK(!text.empty() && text.find("splashedit-ui-elements") != std::string::npos);
    CHECK(copyElementText(d, 0, 9).empty() && copyElementText(d, 5, 0).empty());
    CHECK(d.historySize() == 0);
    // Pasted in front of the selection, 8 px down and right, renamed.
    int p = pasteElements(d, 0, 1, text, s);
    CHECK(p == 2 && d.scene().canvases[0].elements[2].name == "front (2)");
    CHECK((layout(d.scene().canvases[0].elements[2], s) == Rect{58, 28, 100, 50}));
    CHECK((d.uiSelection() == editor::Document::UiSelection{0, 2}));
    CHECK(d.historySize() == 1);
    // Again: past the first copy, not on it.
    CHECK(pasteElements(d, 0, 2, text, s) == 3 && d.scene().canvases[0].elements[3].name == "front (3)");
    CHECK((layout(d.scene().canvases[0].elements[3], s) == Rect{66, 36, 100, 50}));
    // One undo takes back one paste.
    CHECK(d.undo() && d.scene().canvases[0].elements.size() == 5 && d.scene().canvases[0].elements[2].name == "front (2)");
    CHECK(d.undo() && d.scene().canvases[0].elements.size() == 4);
    // Cut (copy, then delete) and paste: back where it was, under its own name.
    const std::string cut = copyElementText(d, 0, 0);
    CHECK(deleteElement(d, 0, 0));
    CHECK(pasteElements(d, 0, -1, cut, s) == 3);
    const splash::UIElement& back = d.scene().canvases[0].elements[3];
    CHECK(back.name == "back" && (layout(back, s) == Rect{0, 0, 100, 50}));
    // Into another canvas, on top; a line moves by its endpoints.
    CHECK(pasteElements(d, 1, -1, copyElementText(d, 0, 2), s) == 1);
    const splash::UIElement& line = d.scene().canvases[1].elements[1];
    CHECK(line.type == splash::UIElementType::Line && line.name == "line" && line.from[0] == 200 && line.to[1] == 200);
    CHECK(pasteElements(d, 1, -1, copyElementText(d, 0, 2), s) == 2);
    CHECK(d.scene().canvases[1].elements[2].from[0] == 208 && d.scene().canvases[1].elements[2].to[1] == 208);
    // Anchored elements keep their anchors and move on screen.
    d.reset(uiScene());
    editElement(d, 0, 0, [](splash::UIElement& e) {
        e.anchorMin = e.anchorMax = {1, 1};
        e.rect = {-110, -60, 100, 50};
    });
    CHECK(pasteElements(d, 0, 0, copyElementText(d, 0, 0), s) == 1);
    const splash::UIElement& anch = d.scene().canvases[0].elements[1];
    CHECK(anch.anchorMin[0] == 1 && (layout(anch, s) == Rect{s.w - 102, s.h - 52, 100, 50}));
    // Not ours: no change, no history.
    const size_t steps = d.historySize();
    CHECK(pasteElements(d, 0, -1, "hello", s) == -1);
    CHECK(pasteElements(d, 0, -1, "{\"splashedit-ui-elements\": 2, \"elements\": [{\"type\": \"box\"}]}", s) == -1);
    CHECK(pasteElements(d, 0, -1, "{\"splashedit-ui-elements\": 1, \"elements\": []}", s) == -1);
    CHECK(pasteElements(d, 7, -1, text, s) == -1);
    // Object clipboard text is not UI elements, and UI element text is not objects.
    editor::Document objs;
    splash::Scene withObj;
    withObj.objects.emplace_back();
    withObj.objects.back().name = "Crate";
    objs.reset(withObj);
    const std::string objText = objs.copyObject({0});
    CHECK(!objText.empty() && pasteElements(d, 0, -1, objText, s) == -1);
    CHECK(!splash::readObjectsText(text) && !objs.pasteObjects(text, std::nullopt));
    CHECK(d.historySize() == steps && d.scene().canvases[0].elements.size() == 5);
}

// Multi-selection: the selection helpers, the rubber-band query and every
// group edit as one undo step.
void testUiMultiSelect() {
    using namespace editor::uic;
    const Screen s;
    editor::Document d;
    d.reset(uiScene());
    auto names = [&](int c) {
        std::vector<std::string> out;
        for (const auto& e : d.scene().canvases[size_t(c)].elements) out.push_back(e.name);
        return out;
    };
    using Names = std::vector<std::string>;

    // The selection: sorted, primary kept apart, stale indices dropped.
    const editor::Document::UiSelection sel = selectionOf(0, {3, 0, 1, 1}, 1);
    CHECK(sel.canvas == 0 && sel.element == 1 && sel.more == std::vector<int>({0, 3}));
    CHECK(selectionOf(0, {2, 0}).element == 2 && selectionOf(0, {}).element == -1 && selectionOf(0, {4}, 9).element == 4);
    d.selectUi(selectionOf(0, {0, 1, 3, 9, -2}));
    CHECK(selectedElements(d) == std::vector<int>({0, 1, 3}));
    d.selectUi(selectionOf(5, {0}));
    CHECK(selectedElements(d).empty());

    // The rubber band: what the box touches, hidden elements left out, a line by where it runs.
    const splash::UICanvas& hud = d.scene().canvases[0];
    CHECK((elementsInBox(hud, s, 90, 40, 110, 45) == std::vector<int>{0, 1}));
    CHECK((elementsInBox(hud, s, 110, 45, 90, 40) == std::vector<int>{0, 1}));  // either corner first
    CHECK((elementsInBox(hud, s, 0, 0, 1, 1) == std::vector<int>{0}));
    CHECK((elementsInBox(hud, s, 240, 140, 250, 150) == std::vector<int>{3}));
    CHECK(elementsInBox(hud, s, 200, 180, 210, 190).empty());  // beside the line's box corner, not on the line
    CHECK(elementsInBox(hud, s, 300, 0, 310, 10).empty());
    CHECK((elementsInBox(hud, s, -50, -50, 400, 400) == std::vector<int>{0, 1, 3}));
    CHECK(d.historySize() == 0);

    // One edit on several elements is one step.
    d.selectUi(selectionOf(0, {0, 1}));
    CHECK(editElements(d, 0, {0, 1}, [&](splash::UIElement& e, size_t k) { place(e, Rect{layout(e, s).x + 10, layout(e, s).y + int(k), 100, 50}, s); }));
    CHECK((layout(d.scene().canvases[0].elements[0], s) == Rect{10, 0, 100, 50}));
    CHECK((layout(d.scene().canvases[0].elements[1], s) == Rect{60, 21, 100, 50}));
    CHECK(d.historySize() == 1 && selectedElements(d) == std::vector<int>({0, 1}));
    CHECK(!editElements(d, 0, {0, 1}, [](splash::UIElement&, size_t) {}) && d.historySize() == 1);
    CHECK(!editElements(d, 0, {0, 9}, [](splash::UIElement& e, size_t) { e.name = "x"; }) && d.historySize() == 1);
    CHECK(d.undo() && (layout(d.scene().canvases[0].elements[0], s) == Rect{0, 0, 100, 50}) &&
          (layout(d.scene().canvases[0].elements[1], s) == Rect{50, 20, 100, 50}) && !d.canUndo());

    // Drag merging: many frames under one key, one step.
    for (int i = 1; i <= 5; i++)
        editElements(d, 0, {0, 1}, [&](splash::UIElement& e, size_t k) { place(e, Rect{i * 2 + int(k) * 50, i, 100, 50}, s); }, "drag");
    d.endMerge();
    CHECK(d.historySize() == 1 && d.scene().canvases[0].elements[0].rect[0] == 10);
    CHECK(d.undo() && d.scene().canvases[0].elements[0].rect[0] == 0);

    // Delete: one step, undo brings them all back with the selection.
    d.selectUi(selectionOf(0, {0, 2}));
    CHECK(deleteElements(d, 0, {0, 2}));
    CHECK(names(0) == Names({"front", "line"}) && d.historySize() == 1 && d.uiSelection().element == -1 && d.uiSelection().more.empty());
    CHECK(d.undo() && names(0) == Names({"back", "front", "hidden", "line"}) && selectedElements(d) == std::vector<int>({0, 2}));
    CHECK(!deleteElements(d, 0, {}) && !deleteElements(d, 0, {1, 9}) && !deleteElements(d, 7, {0}) && !d.canUndo() && d.canRedo());

    // Duplicate: each copy right in front of its original, 8 px down and right, the copies selected.
    CHECK(duplicateElements(d, 0, {0, 1}) == 2);
    CHECK(names(0) == Names({"back", "back (2)", "front", "front (2)", "hidden", "line"}) && d.historySize() == 1);
    CHECK((layout(d.scene().canvases[0].elements[1], s) == Rect{8, 8, 100, 50}));
    CHECK((layout(d.scene().canvases[0].elements[3], s) == Rect{58, 28, 100, 50}));
    CHECK(selectedElements(d) == std::vector<int>({1, 3}));
    CHECK(d.undo() && names(0).size() == 4);
    CHECK(duplicateElements(d, 0, {3}) == 1 && d.scene().canvases[0].elements[4].from == (std::array<int, 2>{208, 108}));
    CHECK(d.undo());

    // Copy and paste of a group: both in order, the pasted ones selected, one step.
    const std::string two = copyElementsText(d, 0, {1, 0});
    CHECK(!two.empty() && copyElementsText(d, 0, {0, 9}).empty() && copyElementsText(d, 0, {}).empty());
    CHECK(pasteElements(d, 1, -1, two, s) == 2);
    CHECK(names(1) == Names({"panel", "back", "front"}) && selectedElements(d) == std::vector<int>({1, 2}) && d.historySize() == 1);
    CHECK(d.undo() && names(1) == Names({"panel"}));
    // In front of an element, the group stays together.
    CHECK(pasteElements(d, 0, 0, two, s) == 2 && names(0) == Names({"back", "back (2)", "front (2)", "front", "hidden", "line"}));
    CHECK(selectedElements(d) == std::vector<int>({1, 2}));
    CHECK(d.undo());

    // Pasted at a spot: the top-left of the group's box lands there, keeping how they sit to each other.
    CHECK((clipboardBounds(two, s) == Rect{0, 0, 150, 70}));
    const std::array<int, 2> at{100, 90};
    CHECK(pasteElements(d, 1, -1, two, s, &at) == 2);
    CHECK((layout(d.scene().canvases[1].elements[1], s) == Rect{100, 90, 100, 50}) &&
          (layout(d.scene().canvases[1].elements[2], s) == Rect{150, 110, 100, 50}));
    CHECK(d.undo());
    // A line counts by its endpoints: front (50..149 x 20..69) and the line (200..300 x 100..200).
    const std::string mixed = copyElementsText(d, 0, {1, 3});
    CHECK((clipboardBounds(mixed, s) == Rect{50, 20, 250, 180}));
    const std::array<int, 2> at0{0, 0};
    CHECK(pasteElements(d, 1, -1, mixed, s, &at0) == 2);
    CHECK((layout(d.scene().canvases[1].elements[1], s) == Rect{0, 0, 100, 50}) &&
          d.scene().canvases[1].elements[2].from == (std::array<int, 2>{150, 80}) &&
          d.scene().canvases[1].elements[2].to == (std::array<int, 2>{250, 180}));
    CHECK(d.undo());
    CHECK(!clipboardBounds("hello", s));

    // z-order keeps a multi-selection on the same elements.
    d.selectUi(selectionOf(0, {0, 2}, 2));
    CHECK(moveElement(d, 0, 2, 0));
    CHECK(names(0) == Names({"hidden", "back", "front", "line"}) && selectedElements(d) == std::vector<int>({0, 1}) &&
          d.uiSelection().element == 0);
    CHECK(d.undo());

    // The per-canvas limit: a paste or duplicate past it is refused with nothing changed.
    editor::Document full;
    splash::Scene sc = uiScene();
    while (int(sc.canvases[1].elements.size()) < kMaxCanvasElements - 1)
        sc.canvases[1].elements.push_back(uiBox("n", 0, 0, 4, 4));
    full.reset(sc);
    const std::string one = copyElementText(d, 0, 0);
    const size_t before = full.scene().canvases[1].elements.size();
    CHECK(before == size_t(kMaxCanvasElements - 1));
    CHECK(pasteElements(full, 1, -1, two, s) == -2);  // 254 + 2
    CHECK(duplicateElements(full, 1, {0, 1}) == -2);
    CHECK(full.historySize() == 0 && !full.dirty() && full.scene().canvases[1].elements.size() == before);
    CHECK(pasteElements(full, 1, -1, one, s) >= 0 && int(full.scene().canvases[1].elements.size()) == kMaxCanvasElements);
    CHECK(pasteElements(full, 1, -1, one, s) == -2 && pasteElements(full, 1, -1, one, s, &at) == -2);
    CHECK(duplicateElements(full, 1, {0}) == -2 && full.historySize() == 1);
    CHECK(pasteElements(full, 0, -1, one, s) >= 0);  // another canvas still takes it
}

void testUiProblems() {
    using namespace editor::uic;
    const Screen s;
    auto titles = [](const std::vector<Problem>& ps) {
        std::string t;
        for (const Problem& p : ps) t += p.title + ";";
        return t;
    };
    editor::Document d;
    splash::Scene sc = uiScene();
    auto& els = sc.canvases[0].elements;
    els.push_back(uiBox("off", 400, 10, 20, 20));    // 4
    els.push_back(uiBox("right", 310, 10, 20, 20));  // 5
    els.push_back(uiBox("left", -4, 10, 20, 20));    // 6
    splash::UIElement t = uiBox("t", 0, 0, 100, 16);
    t.type = splash::UIElementType::Text;
    t.text = std::string(70, 'a');
    t.font = "nope";
    els.push_back(t);  // 7
    d.reset(sc);
    CHECK(elementProblems(d, 0, 0, s).empty());
    const std::vector<Problem> off = elementProblems(d, 0, 4, s);
    CHECK(titles(off) == "Off screen;");
    CHECK(off[0].body.find("300, 10") != std::string::npos);  // says where to put it
    const std::vector<Problem> right = elementProblems(d, 0, 5, s);
    CHECK(titles(right) == "Past the right edge;" && right[0].body.find("10 px left") != std::string::npos);
    CHECK(titles(elementProblems(d, 0, 6, s)) == "Cut by the left edge;");
    CHECK(titles(elementProblems(d, 0, 7, s)) == "Text too long;No such font;");
    CHECK(elementProblems(d, 0, 7, s)[0].body.find("by 7") != std::string::npos);
    // The one-click fixes, one undo step each.
    CHECK(off[0].fix == Fix::MoveOnScreen && applyFix(d, 0, 4, off[0].fix, s));
    CHECK((layout(d.scene().canvases[0].elements[4], s) == Rect{300, 10, 20, 20}));
    CHECK(elementProblems(d, 0, 4, s).empty());
    CHECK(applyFix(d, 0, 6, Fix::MoveOnScreen, s) && layout(d.scene().canvases[0].elements[6], s).x == 0);
    CHECK(applyFix(d, 0, 7, Fix::TrimText, s) && d.scene().canvases[0].elements[7].text.size() == 63);
    CHECK(applyFix(d, 0, 7, Fix::SystemFont, s) && elementProblems(d, 0, 7, s).empty());
    CHECK(!applyFix(d, 0, 7, Fix::SystemFont, s));  // nothing left to fix: no step
    CHECK(d.historySize() == 4 && d.undo() && d.scene().canvases[0].elements[7].font == "nope");
    editElement(d, 0, 7, [](splash::UIElement& e) { e.text = "caf\xc3\xa9!"; });
    CHECK(applyFix(d, 0, 7, Fix::AsciiText, s) && d.scene().canvases[0].elements[7].text == "caf?!");
    // Over the 256-element budget: the elements past it say so, and the canvas.
    sc = uiScene();
    for (int i = 0; i < 260; i++) sc.canvases[1].elements.push_back(uiBox("n", 0, 0, 4, 4));
    d.reset(sc);
    CHECK(titles(elementProblems(d, 1, 0, s)).empty());
    CHECK(titles(elementProblems(d, 1, 260, s)) == "Over the element budget;Over the canvas budget;");
    CHECK(elementProblems(d, 1, 260, s)[0].body.find("Delete 9") != std::string::npos);
    CHECK(titles(canvasProblems(d, 1)) == "Too many elements;Over the element budget;");
    CHECK(canvasProblems(d, 0).size() == 1);
}

void testUiRender() {
    using namespace editor::uic;
    const Screen s;
    splash::Scene sc = uiScene();
    sc.canvases[0].elements[0].color = {1, 0, 0};
    sc.canvases[0].elements[1].color = {0, 1, 0};
    splash::UIElement bar = uiBox("bar", 0, 100, 100, 4);
    bar.type = splash::UIElementType::Progress;
    bar.value = 30;
    bar.color = {0, 0, 1};
    bar.background = {0.5f, 0.5f, 0.5f};
    sc.canvases[0].elements.push_back(bar);
    splash::UIElement text = uiBox("text", 0, 150, 100, 16);
    text.type = splash::UIElementType::Text;
    text.text = "A";
    sc.canvases[0].elements.push_back(text);
    Assets assets;
    assets.refresh(g_outDir, 0);
    const uint32_t bg = 0xff102030;
    std::vector<uint8_t> cov;
    std::vector<uint32_t> px = render(sc, {true, false}, s, assets, bg, &cov);
    auto at = [&](int x, int y) { return px[size_t(y) * 320 + size_t(x)]; };
    CHECK(px.size() == 320u * 240u && cov.size() == px.size());
    CHECK(at(10, 10) == 0xff0000ffu);   // red back box (0xAABBGGRR)
    CHECK(at(60, 30) == 0xff00ff00u);   // green front box over it
    CHECK(at(160, 30) == bg && !cov[30 * 320 + 160]);  // hidden box not drawn
    CHECK(at(200, 100) == 0xffffffffu && at(300, 200) == 0xffffffffu && at(250, 150) == 0xffffffffu);  // line, both ends
    CHECK(at(29, 101) == 0xffff0000u && at(30, 101) == 0xff848484u);  // 30% fill, 0.5 grey as 5-bit
    CHECK(at(15, 175) == bg);  // the menu canvas is not shown
    // System font 'A': the left stroke at column 1 from row 2, the hole at 3..4.
    CHECK(at(1, 152) == 0xffffffffu && at(3, 154) == bg);
    std::vector<uint32_t> both = render(sc, {true, true}, s, assets, bg);
    CHECK(both[15 * 320 + 15] == 0xffffffffu);
    // Canvases by sortOrder: menu first in the list but drawn behind once hud sorts higher.
    sc.canvases[1].elements[0].rect = {5, 5, 20, 20};
    sc.canvases[1].elements[0].color = {0, 0, 1};
    std::vector<uint32_t> over = render(sc, {true, true}, s, assets, bg);
    CHECK(over[10 * 320 + 10] == 0xffff0000u);  // menu (later) in front at equal sortOrder
    sc.canvases[0].sortOrder = 5;
    std::vector<uint32_t> under = render(sc, {true, true}, s, assets, bg);
    CHECK(under[10 * 320 + 10] == 0xff0000ffu);
    // An image: a red, green, blue strip stretched over 6x2, tinted by colour / 128.
    splash::Image img;
    img.width = 3;
    img.height = 1;
    img.pixels = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    std::filesystem::create_directories(g_outDir);
    splash::savePng(img, g_outDir / "uitex.png");
    splash::Scene isc;
    isc.canvases.resize(1);
    splash::UIElement im = uiBox("im", 10, 10, 6, 2);
    im.type = splash::UIElementType::Image;
    im.texture = "uitex.png";
    im.bitDepth = splash::BitDepth::Bpp16;
    im.color = {0.5f, 0.5f, 0.5f};
    isc.canvases[0].elements.push_back(im);
    std::vector<uint32_t> ip = render(isc, {true}, s, assets, bg);
    auto iat = [&](int x, int y) { return ip[size_t(y) * 320 + size_t(x)]; };
    // u = x * (3 - 1) / 6: red, red, red, green, green, green; blue never shows.
    CHECK(iat(10, 10) == 0xff0000ffu && iat(12, 11) == 0xff0000ffu);
    CHECK(iat(13, 10) == 0xff00ff00u && iat(15, 11) == 0xff00ff00u && iat(16, 10) == bg);
    isc.canvases[0].elements[0].color = {0.25f, 0.25f, 0.25f};  // tint 64: half as bright
    std::vector<uint32_t> dim = render(isc, {true}, s, assets, bg);
    CHECK(dim[10 * 320 + 10] == 0xff00007bu);
    CHECK(textWidth(sc, assets, "", "Hello") == 40);
}

// tests/ui/ui.scene as psxsplash draws it: tests/ui/ui-runtime.png is frame
// 300 of tests/boot/boot.sh (pcsx-redux, psxsplash 669fb6b). The preview must
// match it pixel for pixel in 15-bit colour: both Inter fonts (the body text
// lighter than its colour, the GPU's tint doubling), the system font, the
// progress bar, the image, the line and the anchored boxes.
void testUiMatchesRuntime() {
    using namespace editor::uic;
    const std::filesystem::path dir = std::filesystem::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "ui";
    splash::Scene sc = splash::loadScene(dir / "ui.scene");
    Assets assets;
    assets.refresh(dir, 0);
    std::vector<uint32_t> px = render(sc, {true}, Screen{}, assets, 0xff000000u);
    splash::Image ref = splash::loadImage(dir / "ui-runtime.png");
    CHECK(ref.width == 320 && ref.height == 240);
    if (ref.width != 320 || ref.height != 240) return;
    int differ = 0, lit = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            const splash::Image::Px& p = ref.pixels[size_t(239 - y) * 320 + size_t(x)];  // rows bottom-up
            const uint32_t e = px[size_t(y) * 320 + size_t(x)];
            const int want[3] = {int(std::lround(p.r * 31)), int(std::lround(p.g * 31)), int(std::lround(p.b * 31))};
            const int got[3] = {int(e & 0xff) >> 3, int((e >> 8) & 0xff) >> 3, int((e >> 16) & 0xff) >> 3};
            if (want[0] != got[0] || want[1] != got[1] || want[2] != got[2]) differ++;
            if (got[0] || got[1] || got[2]) lit++;
        }
    CHECK(differ == 0);
    CHECK(lit > 10000);
    if (differ) std::fprintf(stderr, "UI preview: %d pixels differ from the runtime frame\n", differ);
}

int main(int argc, char** argv) {
    std::error_code ec;
    std::filesystem::path exe = argc > 0 ? std::filesystem::absolute(argv[0], ec) : std::filesystem::path();
    g_outDir = (exe.has_parent_path() ? exe.parent_path() : std::filesystem::current_path()) / "editor_tests_out";
    testApplyUndoRedo();
    testMerge();
    testSettings();
    testRenderPeak();
    testMemCard();
    testDirtyAndSave();
    testStructure();
    testMove();
    testPick();
    testGizmoMath();
    testScaleClamp();
    testGizmoUndo();
    testCatalog();
    testExportStats();
    testObjectRamBudget();
    testScriptWithoutMesh();
    testWorldStreamPlan();
    testWorldStreamExport();
    testNavBigWorld();
    testEditMany();
    testTimelineSample();
    testTimelineEdit();
    testTimelinePreview();
    testTimelineRenameDelete();
    testTimelineRetarget();
    testTimelineFixedSample();
    testTimelinePreviewMore();
    testTimelineSelection();
    testTimelineClipboard();
    testTimelineEvents();
    testSeqRenameLua();
    testSeqRenameScripts();
    testTimelineUiPositionStart();
    testTimelineUiStartValues();
    testTimelinePasteAddsTrack();
    testTimelineSkinEventProblem();
    testLuaTokenize();
    testLuaBuffer();
    testLuaComplete();
    testLuaDefinitions();
    testLuaCheck();
    testLuaScriptFile();
    testLuaReplace();
    testLuaFindOptions();
    testLuaAutoClose();
    testLuaReload();
    testLuaDeleted();
    testLiveExport();
    testBlendCancel();
    testPlay();
    testProject();
    testAdoptAsset();
    testNewScript();
    testPlaceModel();
    testPlaceSkinnedModel();
    testImportSettings();
    testModelImports();
    testModelImportsGaps();
    testUnirom();
    testPsxmon();
    testHardwareRun();
    runHardwarePsxmon(0x0007);
    runHardwarePsxmon(0x0001);
    testClipboard();
    testPasteOffset();
    testImportWarnings();
    testQuantizer();
    testTexturePreview();
    testAssetBrowser();
    testUiLayout();
    testUiHitTest();
    testUiResize();
    testUiSnap();
    testUiZOrder();
    testUiUndo();
    testUiClipboard();
    testUiMultiSelect();
    testUiProblems();
    testUiRender();
    testUiMatchesRuntime();
    if (g_failures) {
        std::fprintf(stderr, "editor_tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("editor_tests: all %d checks passed\n", g_checks);
    return 0;
}
