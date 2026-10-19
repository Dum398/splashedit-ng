// Headless tests for the editor's document model: the undo history and the
// viewport's ray picker and gizmo maths. No SDL, no GL. Run through CTest (`ctest`).
#include <algorithm>
#include <atomic>
#include <bit>
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
#include <functional>
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
#include "editor/blocks_edit.hh"
#include "editor/catalog.hh"
#include "editor/document.hh"
#include "editor/hardware.hh"
#include "editor/gizmo.hh"
#include "editor/live_export.hh"
#include "editor/log_view.hh"
#include "editor/lua_editor.hh"
#include "editor/memcard.hh"
#include "editor/sprites.hh"
#include "editor/tilemap.hh"
#include "editor/model_import.hh"
#include "editor/pick.hh"
#include "editor/play.hh"
#include "editor/project.hh"
#include "editor/psxmon.hh"
#include "editor/serial.hh"
#include "editor/texture_preview.hh"
#include "editor/timeline.hh"
#include "editor/unirom.hh"
#include "audio.hh"
#include "blend.hh"
#include "blocks.hh"
#include "luacompile.hh"
#include "editor/ui_canvas.hh"
#include "ui/dialog_gate.hh"
#include "ui/window_chrome.hh"
#include "ui/ui_scale.hh"
#include "ui/scrub.hh"
#include "ui/colour_hex.hh"
#include "budget.hh"
#include "disc.hh"
#include "font.hh"
#include "navregion.hh"
#include "rooms.hh"
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

// The scene inspector's Rendering section: fog, GTE scaling and scene type
// edit as one step each (a drag merges), survive save and load, and undo.
void testSceneRendering() {
    std::error_code ec;
    std::filesystem::path dir = g_outDir;
    std::filesystem::create_directories(dir, ec);
    editor::Document d;
    d.reset(sample(), dir, "rendering");
    d.editSettings([](splash::SceneSettings& s) { s.fog.enabled = true; });
    for (int v : {3, 6, 7}) d.editSettings([&](splash::SceneSettings& s) { s.fog.density = v; }, "fogdensity");
    d.endMerge();
    d.editSettings([](splash::SceneSettings& s) { s.fog.color = {0.25f, 0.5f, 1.f}; }, "fogcol");
    d.endMerge();
    d.editSettings([](splash::SceneSettings& s) { s.gteScaling = 50.f; }, "gte");
    d.endMerge();
    d.editSettings([](splash::SceneSettings& s) { s.sceneType = splash::SceneType::Interior; });
    CHECK(d.historySize() == 5);
    CHECK(!d.save());

    editor::Document r;
    CHECK(!r.load(dir, "rendering.scene"));
    const splash::SceneSettings& rs = r.scene().settings;
    CHECK(rs.fog.enabled && rs.fog.density == 7);
    CHECK(rs.fog.color[0] == 0.25f && rs.fog.color[1] == 0.5f && rs.fog.color[2] == 1.f);
    CHECK(rs.gteScaling == 50.f);
    CHECK(rs.sceneType == splash::SceneType::Interior);

    const splash::SceneSettings& ds = d.scene().settings;
    CHECK(d.undo() && ds.sceneType == splash::SceneType::Exterior && ds.gteScaling == 50.f);
    CHECK(d.undo() && ds.gteScaling == 100.f);
    CHECK(d.undo() && ds.fog.color[0] == 0.5f && ds.fog.density == 7);
    CHECK(d.undo() && ds.fog.density == 5 && ds.fog.enabled);  // the whole drag is one step
    CHECK(d.undo() && !ds.fog.enabled);
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

// The component fields the object inspector edits: each set through
// doc.edit survives a save and reload of the scene JSON, and one undo puts
// the old value back.
void testComponentFieldsSaveUndo() {
    std::error_code ec;
    std::filesystem::path dir = g_outDir;
    std::filesystem::create_directories(dir, ec);
    splash::Scene sc;
    splash::Object lever = named("Lever");
    lever.interactable = splash::InteractableComponent{};
    sc.objects.push_back(lever);
    splash::Object hero = named("Hero");
    hero.player = splash::PlayerComponent{};
    sc.objects.push_back(hero);
    splash::Object bell = named("Bell");
    bell.audio = splash::AudioComponent{};
    bell.audio->clipName = "bell";
    sc.objects.push_back(bell);
    splash::Object box = named("Box");
    box.mesh = splash::MeshComponent{};
    sc.objects.push_back(box);
    splash::Object lamp = named("Lamp");
    lamp.light = splash::LightComponent{};
    lamp.light->kind = splash::LightKind::Spot;
    sc.objects.push_back(lamp);

    editor::Document d;
    d.reset(sc, dir, "compfields");
    // Each edit is its own undo step; undo restores the value before it.
    struct Step {
        std::function<void(splash::Object&)> set;
        std::function<bool(const splash::Object&)> isNew, isOld;
        int at;
    };
    const std::vector<Step> steps = {
        {[](splash::Object& o) { o.interactable->button = 12; }, [](const splash::Object& o) { return o.interactable->button == 12; },
         [](const splash::Object& o) { return o.interactable->button == 14; }, 0},
        {[](splash::Object& o) { o.interactable->repeatable = false; }, [](const splash::Object& o) { return !o.interactable->repeatable; },
         [](const splash::Object& o) { return o.interactable->repeatable; }, 0},
        {[](splash::Object& o) { o.interactable->cooldownFrames = 65535; },
         [](const splash::Object& o) { return o.interactable->cooldownFrames == 65535; },
         [](const splash::Object& o) { return o.interactable->cooldownFrames == 30; }, 0},
        {[](splash::Object& o) { o.interactable->showPrompt = true; }, [](const splash::Object& o) { return o.interactable->showPrompt; },
         [](const splash::Object& o) { return !o.interactable->showPrompt; }, 0},
        {[](splash::Object& o) { o.interactable->promptCanvas = "Hint"; },
         [](const splash::Object& o) { return o.interactable->promptCanvas == "Hint"; },
         [](const splash::Object& o) { return o.interactable->promptCanvas.empty(); }, 0},
        {[](splash::Object& o) { o.player->sprintSpeed = 6.5f; }, [](const splash::Object& o) { return o.player->sprintSpeed == 6.5f; },
         [](const splash::Object& o) { return o.player->sprintSpeed == 8.0f; }, 1},
        {[](splash::Object& o) { o.player->gravity = 12.0f; }, [](const splash::Object& o) { return o.player->gravity == 12.0f; },
         [](const splash::Object& o) { return o.player->gravity == 20.0f; }, 1},
        {[](splash::Object& o) { o.player->nav.maxStepHeight = 0.5f; },
         [](const splash::Object& o) { return o.player->nav.maxStepHeight == 0.5f; },
         [](const splash::Object& o) { return o.player->nav.maxStepHeight == 0.35f; }, 1},
        {[](splash::Object& o) { o.player->nav.walkableSlopeAngle = 30.0f; },
         [](const splash::Object& o) { return o.player->nav.walkableSlopeAngle == 30.0f; },
         [](const splash::Object& o) { return o.player->nav.walkableSlopeAngle == 46.0f; }, 1},
        {[](splash::Object& o) { o.audio->clipName = "chime"; }, [](const splash::Object& o) { return o.audio->clipName == "chime"; },
         [](const splash::Object& o) { return o.audio->clipName == "bell"; }, 2},
        {[](splash::Object& o) { o.audio->sampleRate = 11025; }, [](const splash::Object& o) { return o.audio->sampleRate == 11025; },
         [](const splash::Object& o) { return o.audio->sampleRate == 22050; }, 2},
        {[](splash::Object& o) { o.audio->loop = true; }, [](const splash::Object& o) { return o.audio->loop; },
         [](const splash::Object& o) { return !o.audio->loop; }, 2},
        {[](splash::Object& o) { o.audio->trimLeadingSilence = true; }, [](const splash::Object& o) { return o.audio->trimLeadingSilence; },
         [](const splash::Object& o) { return !o.audio->trimLeadingSilence; }, 2},
        {[](splash::Object& o) { o.mesh->smoothNormals = false; }, [](const splash::Object& o) { return !o.mesh->smoothNormals; },
         [](const splash::Object& o) { return o.mesh->smoothNormals; }, 3},
        {[](splash::Object& o) { o.light->innerSpotAngle = 20.0f; }, [](const splash::Object& o) { return o.light->innerSpotAngle == 20.0f; },
         [](const splash::Object& o) { return o.light->innerSpotAngle == 0.0f; }, 4},
    };
    for (size_t i = 0; i < steps.size(); i++) {
        const Step& st = steps[i];
        const editor::ObjectPath p{st.at};
        CHECK(st.isOld(*d.object(p)));
        d.edit(p, st.set);
        CHECK(st.isNew(*d.object(p)));
        CHECK(d.undo());
        if (!st.isOld(*d.object(p))) std::fprintf(stderr, "compfields: undo of step %zu did not restore\n", i);
        CHECK(st.isOld(*d.object(p)));
        CHECK(d.redo());
        CHECK(st.isNew(*d.object(p)));
    }
    CHECK(d.historySize() == steps.size());
    CHECK(!d.save());

    editor::Document r;
    CHECK(!r.load(dir, "compfields.scene"));
    for (size_t i = 0; i < steps.size(); i++) {
        const splash::Object* o = r.object(editor::ObjectPath{steps[i].at});
        const bool ok = o && steps[i].isNew(*o);
        if (!ok) std::fprintf(stderr, "compfields: step %zu did not survive save and reload\n", i);
        CHECK(ok);
    }
    // The reloaded scene keeps undoing nothing; the edited one undoes all the way back.
    for (size_t i = 0; i < steps.size(); i++) CHECK(d.undo());
    for (const Step& st : steps) CHECK(st.isOld(*d.object(editor::ObjectPath{st.at})));
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

// A Trigger box in world space is the axis-aligned bounds of the turned,
// scaled box: the splashpack stores it and the viewport draws it.
void testTriggerWorldBox() {
    splash::Object o;
    o.trigger = splash::TriggerComponent{};
    o.trigger->size = {2, 4, 6};
    const float s45 = std::sqrt(0.5f);
    splash::FlatObject fo{&o, {1, 2, 3}, splash::Quat{0, s45, 0, s45}, {1, 1, 1},
                          splash::Mat34::trs({1, 2, 3}, splash::Quat{0, s45, 0, s45}, {1, 1, 1}), true};
    splash::Vec3 mn, mx;
    splash::triggerWorldBox(fo, &mn, &mx);
    auto near = [](splash::Vec3 a, splash::Vec3 b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
    };
    CHECK(near(mn, {-2, 0, 2}));  // a quarter turn about Y swaps the X and Z extents
    CHECK(near(mx, {4, 4, 4}));
    fo.localToWorld = splash::Mat34::trs({0, 0, 0}, splash::Quat{0, 0, 0, 1}, {2, 1, 0.5f});
    splash::triggerWorldBox(fo, &mn, &mx);
    CHECK(near(mn, {-2, -2, -1.5f}) && near(mx, {2, 2, 1.5f}));  // scale stretches the box
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

// What the Problems popup keeps of a build over the build list: every scene's
// tagged message except those the open scene's live export already lists,
// and the status line of a failed build.
void testBuildReport() {
    CHECK(splash::sceneTag(0, 1, "courtyard").empty());
    CHECK(splash::sceneTag(1, 3, "cave") == "scene 1 (cave): ");
    CHECK(splash::sceneTag(2, 3, "") == "scene 2: ");

    splash::ExportResult live;
    live.errors = {"bad mesh"};
    live.warnings = {"no icon"};
    splash::ExportResult build;
    build.errors = {"scene 0 (a): bad mesh", "scene 1 (cave): bad mesh", "Cannot load c.scene: oops"};
    build.warnings = {"scene 0 (a): no icon", "scene 1 (cave): no icon", "scene 0 (a): only in the build"};

    // The open scene is scene 0: its copies of the live messages are left out, nothing else.
    editor::BuildReport r = editor::buildReport("Play", 3, build, std::string("scene 0 (a): "), &live);
    CHECK(r.what == "Play" && r.scenes == 3);
    CHECK(r.errors == std::vector<std::string>({"scene 1 (cave): bad mesh", "Cannot load c.scene: oops"}));
    CHECK(r.warnings == std::vector<std::string>({"scene 1 (cave): no icon", "scene 0 (a): only in the build"}));
    // Open at scene 1 instead: scene 0's copies stay, scene 1's go.
    r = editor::buildReport("Play", 3, build, std::string("scene 1 (cave): "), &live);
    CHECK(r.errors == std::vector<std::string>({"scene 0 (a): bad mesh", "Cannot load c.scene: oops"}));
    CHECK(r.warnings == std::vector<std::string>({"scene 0 (a): no icon", "scene 0 (a): only in the build"}));
    // The open scene is not in the build list, or there is no live export yet: everything stays.
    r = editor::buildReport("Export", 3, build, std::nullopt, &live);
    CHECK(r.errors == build.errors && r.warnings == build.warnings);
    r = editor::buildReport("Export", 3, build, std::string("scene 0 (a): "), nullptr);
    CHECK(r.errors == build.errors && r.warnings == build.warnings);
    // One scene, the open one: untagged, so an exact match is left out.
    splash::ExportResult one;
    one.errors = {"bad mesh", "Cannot write reload.flag"};
    one.warnings = {"no icon"};
    r = editor::buildReport("Run on hardware", 1, one, std::string(), &live);
    CHECK(r.errors == std::vector<std::string>({"Cannot write reload.flag"}) && r.warnings.empty() && !r.empty());
    CHECK(editor::buildReport("Play", 1, splash::ExportResult(), std::string(), &live).empty());

    // A failed build's status line puts the count first.
    CHECK(editor::buildFailure("Play", splash::ExportResult()).empty());
    CHECK(editor::buildFailure("Play", one) == "Play, 2 errors: bad mesh");
    one.errors.pop_back();
    CHECK(editor::buildFailure("Export failed", one) == "Export failed: bad mesh");
}

// The build list and a Play of several scenes: scenes.list round trip, what
// buildScenes makes of missing files and an empty list, exportForPlay writing
// scene_<i> per scene and dropping stale ones, and a live reload of one index.
void testBuildScenes() {
    namespace fs = std::filesystem;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "multiscene";
    fs::remove_all(project);
    fs::copy(src / "examples" / "courtyard", project, fs::copy_options::recursive);
    fs::create_directories(project / "levels");
    fs::copy_file(project / "courtyard.scene", project / "levels" / fs::path(u8"höhle.scene"));

    // Round trip, '/'-separated and UTF-8; a missing file is an empty list.
    CHECK(editor::loadBuildList(project).empty());
    const std::vector<fs::path> list = {"courtyard.scene", fs::path(u8"levels/höhle.scene"), "gone.scene"};
    CHECK(editor::saveBuildList(project, list));
    CHECK(editor::loadBuildList(project) == list);
    {
        std::ifstream in(project / "scenes.list", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text == "courtyard.scene\nlevels/h\xc3\xb6hle.scene\ngone.scene\n");
    }

    // A listed file that is gone is left out; an empty list is the open scene.
    const std::vector<fs::path> built = editor::buildScenes(project, "courtyard.scene");
    CHECK(built.size() == 2 && built[0] == "courtyard.scene" && built[1] == fs::path(u8"levels/höhle.scene"));
    CHECK(editor::saveBuildList(project, {}));
    CHECK(editor::loadBuildList(project).empty());
    CHECK(editor::buildScenes(project, "courtyard.scene") == std::vector<fs::path>{"courtyard.scene"});
    CHECK(editor::saveBuildList(project, {"gone.scene"}));
    CHECK(editor::buildScenes(project, "courtyard.scene") == std::vector<fs::path>{"courtyard.scene"});

    // The open scene comes from memory, the rest from disk; a file that does
    // not load is an error naming it.
    splash::Scene open = splash::loadScene(project / "courtyard.scene");
    open.objects.push_back(named("unsaved"));
    open.settings.fog.enabled = !open.settings.fog.enabled;  // an edit the pack shows
    editor::BuildScenes b = editor::loadBuildScenes(project, built, "courtyard.scene", open);
    CHECK(b.errors.empty() && b.scenes.size() == 2 && b.names.size() == 2 && b.names[1] == "h\xc3\xb6hle");
    CHECK(b.scenes.size() == 2 && b.scenes[0].objects.size() == open.objects.size() &&
          b.scenes[1].objects.size() + 1 == open.objects.size());
    { std::ofstream(project / "broken.scene", std::ios::binary) << "not a scene"; }
    editor::BuildScenes bad = editor::loadBuildScenes(project, {"courtyard.scene", "broken.scene"}, "courtyard.scene", open);
    CHECK(bad.errors.size() == 1 && bad.errors[0].find("broken.scene") != std::string::npos);

    // Two scenes: scene_0.* and scene_1.*, and an earlier scene_2 is gone.
    const fs::path dir = g_outDir / "multiplay";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const char* ext : {".splashpack", ".vram", ".spu"}) std::ofstream(dir / (std::string("scene_2") + ext), std::ios::binary) << "old";
    splash::ExportResult r = editor::exportForPlay(b.pointers(), project, dir, b.names);
    CHECK(r.ok());
    for (const char* ext : {".splashpack", ".vram", ".spu"}) {
        CHECK(fs::exists(dir / (std::string("scene_0") + ext)));
        CHECK(fs::exists(dir / (std::string("scene_1") + ext)));
        CHECK(!fs::exists(dir / (std::string("scene_2") + ext)));
    }
    auto bytes = [&](const char* name) {
        std::ifstream in(dir / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    std::error_code ec;
    CHECK(fs::file_size(dir / "scene_0.splashpack", ec) == r.stats.splashpackBytes);
    CHECK(!bytes("scene_1.splashpack").empty() && bytes("scene_1.splashpack") != bytes("scene_0.splashpack"));

    // An error names the scene it is in.
    splash::Scene broken = b.scenes[1];
    splash::Object o = named("broken");
    o.mesh = splash::MeshComponent{};
    o.mesh->mesh = "no/such.mesh";
    broken.objects.push_back(o);
    splash::ExportResult e = editor::exportForPlay({&b.scenes[0], &broken}, project, g_outDir / "multiplay-bad", b.names);
    CHECK(!e.ok() && e.errors.front().rfind(std::string("scene 1 (") + b.names[1] + "): ", 0) == 0);

    // A live reload of index 1 rewrites scene_1 and leaves scene_0 alone.
    const std::string pack0 = bytes("scene_0.splashpack"), vram0 = bytes("scene_0.vram"), pack1 = bytes("scene_1.splashpack");
    splash::Scene edited = b.scenes[1];
    edited.settings.fog.color = {0.25f, 0.5f, 0.75f};
    splash::ExportResult lr = editor::reloadForPlay(edited, project, dir, 7, 1);
    CHECK(lr.ok());
    CHECK(bytes("scene_1.splashpack").size() == lr.stats.splashpackBytes && bytes("scene_1.splashpack") != pack1);
    CHECK(bytes("scene_0.splashpack") == pack0 && bytes("scene_0.vram") == vram0);
    CHECK(!fs::exists(dir / "scene_0.splashpack.tmp") && !fs::exists(dir / "next" / "scene_1.splashpack"));
    CHECK(bytes("reload.flag") == "7");
}

// The project's VRAM layout: vram.json round trip, a missing file and
// malformed entries; the export's vramMap (framebuffers, the font column,
// atlases kept out of a prohibited area) on a dry run, and the live export
// and Play packing with the project's areas.
void testVramLayout() {
    namespace fs = std::filesystem;
    using K = splash::ExportStats::VramKind;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "vramlayout";
    fs::remove_all(project);
    fs::copy(src / "examples" / "courtyard", project, fs::copy_options::recursive);

    // Missing file (or no project) = the defaults.
    CHECK(editor::loadVramSettings(project).prohibited.empty());
    CHECK(editor::loadVramSettings({}).prohibited.empty());
    const splash::VramSettings def = editor::loadVramSettings(project);
    CHECK(def.resolutionX == 320 && def.resolutionY == 240 && def.dualBuffering && def.verticalBuffering);
    CHECK(!editor::saveVramSettings({}, def));

    // Round trip.
    splash::VramSettings vs;
    vs.prohibited = {{512, 0, 64, 64}, {8, 300, 16, 24}};
    CHECK(editor::saveVramSettings(project, vs));
    const splash::VramSettings back = editor::loadVramSettings(project);
    CHECK(back.prohibited.size() == 2);
    if (back.prohibited.size() == 2) {
        CHECK(back.prohibited[0].x == 512 && back.prohibited[0].y == 0 && back.prohibited[0].w == 64 && back.prohibited[0].h == 64);
        CHECK(back.prohibited[1].x == 8 && back.prohibited[1].y == 300 && back.prohibited[1].w == 16 && back.prohibited[1].h == 24);
    }

    // Malformed entries are skipped, the rest clamped into VRAM; a file that
    // is no JSON is the defaults.
    {
        std::ofstream(project / "vram.json", std::ios::binary)
            << R"({"prohibited":[{"x":64,"y":8,"w":32,"h":16},{"x":"a","y":0,"w":1,"h":1},5,{"x":1,"y":2},)"
            << R"({"x":1000,"y":500,"w":100,"h":0},{"x":-8,"y":0,"w":16,"h":16}]})";
    }
    const splash::VramSettings odd = editor::loadVramSettings(project);
    CHECK(odd.prohibited.size() == 3);
    if (odd.prohibited.size() == 3) {
        CHECK(odd.prohibited[0].x == 64 && odd.prohibited[0].y == 8 && odd.prohibited[0].w == 32 && odd.prohibited[0].h == 16);
        CHECK(odd.prohibited[1].x == 1000 && odd.prohibited[1].y == 500 && odd.prohibited[1].w == 24 && odd.prohibited[1].h == 1);
        CHECK(odd.prohibited[2].x == 0 && odd.prohibited[2].w == 16);
    }
    { std::ofstream(project / "vram.json", std::ios::binary) << "not json"; }
    CHECK(editor::loadVramSettings(project).prohibited.empty());
    fs::remove(project / "vram.json");

    // The map of a dry run: both framebuffers, the font column, the atlases.
    const splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    splash::ExportOptions opt;
    opt.dryRun = true;
    const splash::ExportResult plain = splash::exportSplashpack(scene, project, project / "x.splashpack", opt);
    CHECK(plain.ok());
    CHECK(!fs::exists(project / "x.splashpack"));
    auto has = [](const splash::ExportResult& r, K k, int x, int y, int w, int h) {
        for (const auto& m : r.stats.vramMap)
            if (m.kind == k && m.x == x && m.y == y && m.w == w && m.h == h) return true;
        return false;
    };
    CHECK(has(plain, K::Framebuffer, 0, 0, 320, 240));
    CHECK(has(plain, K::Framebuffer, 0, 256, 320, 240));
    CHECK(has(plain, K::Reserved, 960, 0, 64, 512));
    auto firstAtlas = [](const splash::ExportResult& r) -> const splash::ExportStats::VramRect* {
        for (const auto& m : r.stats.vramMap)
            if (m.kind == K::Atlas4 || m.kind == K::Atlas8 || m.kind == K::Atlas16) return &m;
        return nullptr;
    };
    const auto* atlas0 = firstAtlas(plain);
    CHECK(atlas0 != nullptr);
    int cluts = 0;
    for (const auto& m : plain.stats.vramMap)
        if (m.kind == K::Clut) {
            cluts++;
            CHECK(m.h == 1 && m.w >= 1 && m.w <= 256);  // the palette's length, as packed
        }
    CHECK(cluts > 0);
    if (!atlas0) return;

    // A prohibited area over the spot the first atlas took moves it, and no
    // atlas or CLUT overlaps the area.
    const splash::VramSettings::Area area{atlas0->x, atlas0->y, 64, 64};
    auto clean = [&](const splash::ExportResult& r) {
        bool ok = true;
        for (const auto& m : r.stats.vramMap) {
            if (m.kind == K::Prohibited || m.kind == K::Framebuffer || m.kind == K::Reserved || m.kind == K::Font) continue;
            if (m.x < area.x + area.w && area.x < m.x + m.w && m.y < area.y + area.h && area.y < m.y + m.h) ok = false;
        }
        return ok;
    };
    opt.vram.prohibited = {area};
    const splash::ExportResult moved = splash::exportSplashpack(scene, project, project / "x.splashpack", opt);
    CHECK(moved.ok());
    const auto* atlas1 = firstAtlas(moved);
    CHECK(atlas1 && (atlas1->x != area.x || atlas1->y != area.y));
    CHECK(has(moved, K::Prohibited, area.x, area.y, area.w, area.h));
    CHECK(clean(moved));
    CHECK(!clean(plain));

    // The live export packs with the project's vram.json, and exports again
    // when it changes though the scene did not.
    editor::Document d;
    CHECK(!d.load(project, "courtyard.scene"));
    editor::LiveExport live(0.3);
    live.update(d, 10.0);
    live.update(d, 10.4);
    live.wait();
    CHECK(live.result() && live.result()->ok() && !has(*live.result(), K::Prohibited, area.x, area.y, area.w, area.h));
    live.update(d, 11.0);
    CHECK(!live.busy());
    splash::VramSettings saved;
    saved.prohibited = {area};
    CHECK(editor::saveVramSettings(project, saved));
    live.settingsChanged();
    live.update(d, 11.1);
    CHECK(!live.busy());  // settles first
    live.update(d, 11.5);
    CHECK(live.busy());
    live.wait();
    CHECK(live.result() && has(*live.result(), K::Prohibited, area.x, area.y, area.w, area.h) && clean(*live.result()));

    // Play packs with it too.
    const fs::path dir = g_outDir / "vramplay";
    fs::remove_all(dir);
    const splash::ExportResult played = editor::exportForPlay(scene, project, dir);
    CHECK(played.ok() && has(played, K::Prohibited, area.x, area.y, area.w, area.h) && clean(played));
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

// Build > Performance / Room Debug Overlay: Play and Run on hardware boot the
// psxsplash build beside the chosen one that has them; the disc never does.
void testPlayOverlays() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = g_outDir / "play-overlays";
    fs::remove_all(dir, ec);
    const fs::path e = "/x/engine/psxsplash.ps-exe";
    CHECK(editor::overlayEngine(e, false, false) == e);
    CHECK(editor::overlayEngine(e, true, false) == fs::path("/x/engine/psxsplash-perf.ps-exe"));
    CHECK(editor::overlayEngine(e, false, true) == fs::path("/x/engine/psxsplash-rooms.ps-exe"));
    // A variant located as the psxsplash build maps to its siblings, never "-rooms-rooms".
    CHECK(editor::overlayEngine(fs::path("/x/engine/psxsplash-rooms.ps-exe"), false, true) == fs::path("/x/engine/psxsplash-rooms.ps-exe"));
    CHECK(editor::overlayEngine(fs::path("/x/engine/psxsplash-perf-rooms.ps-exe"), true, false) == fs::path("/x/engine/psxsplash-perf.ps-exe"));
    CHECK(editor::overlayEngine(e, true, true) == fs::path("/x/engine/psxsplash-perf-rooms.ps-exe"));
    CHECK(editor::overlayEngine("/y/my build.exe", true, false) == fs::path("/y/my build-perf.exe"));
    CHECK(editor::overlayEngine({}, true, true).empty());

    // The two keys round-trip; a file written before them reads both off.
    const fs::path cfg = dir / "play.cfg";
    for (int k = 0; k < 4; ++k) {
        editor::PlayTools t;
        t.psxsplash = e;
        t.overlayPerf = k & 1;
        t.overlayRooms = k & 2;
        CHECK(editor::savePlayTools(cfg, t));
        const editor::PlayTools l = editor::loadPlayTools(cfg);
        CHECK(l.overlayPerf == t.overlayPerf && l.overlayRooms == t.overlayRooms && l.psxsplash == e);
    }
    { std::ofstream(cfg, std::ios::trunc) << "redux=/r\npsxsplash=/p.ps-exe\nport=COM3\n"; }
    const editor::PlayTools old = editor::loadPlayTools(cfg);
    CHECK(!old.overlayPerf && !old.overlayRooms && old.port == "COM3");

    // A bundle carrying the variants: Play's command boots them, Export keeps the disc build.
    setEnv("SPLASHEDIT_PSXSPLASH", "");
    setEnv("SPLASHEDIT_DISC_ENGINE", "");
    const fs::path bundle = dir / "bundle";
    fs::create_directories(bundle / "engine");
    for (const char* n : {"psxsplash.ps-exe", "psxsplash-cdrom.ps-exe", "psxsplash-perf.ps-exe", "psxsplash-rooms.ps-exe",
                          "psxsplash-perf-rooms.ps-exe"})
        std::ofstream(bundle / "engine" / n) << "x";
    editor::PlayTools want;
    want.redux = cfg;  // any existing file stands in for pcsx-redux
    want.port = "COM3";
    const char* names[4] = {"psxsplash.ps-exe", "psxsplash-perf.ps-exe", "psxsplash-rooms.ps-exe", "psxsplash-perf-rooms.ps-exe"};
    for (int k = 0; k < 4; ++k) {
        want.overlayPerf = k & 1;
        want.overlayRooms = k & 2;
        const editor::PlayTools eff = editor::withDefaults(want, bundle);
        CHECK(editor::engineFor(eff) == bundle / "engine" / names[k]);
        CHECK(eff.disc == bundle / "engine" / "psxsplash-cdrom.ps-exe");
        CHECK(editor::missingTools(eff).empty() && editor::missingHardwareTools(eff).empty());
        const std::vector<std::string> cmd = editor::reduxCommand(eff, "/b");
        // Compared as paths: the bundle default keeps "engine/..." with a forward slash on Windows.
        CHECK(std::find_if(cmd.begin(), cmd.end(), [&](const std::string& a) {
                  return fs::path(a) == bundle / "engine" / names[k];
              }) != cmd.end());
    }
    // A missing variant is reported by name, not swapped for the plain build.
    fs::remove(bundle / "engine" / "psxsplash-rooms.ps-exe", ec);
    want.overlayPerf = false;
    want.overlayRooms = true;
    const editor::PlayTools eff = editor::withDefaults(want, bundle);
    CHECK(editor::engineFor(eff) == bundle / "engine" / "psxsplash-rooms.ps-exe");
    const std::vector<std::string> miss = editor::missingTools(eff), hwMiss = editor::missingHardwareTools(eff);
    CHECK(miss.size() == 1 && miss[0].find("room debug overlay") != std::string::npos &&
          miss[0].find("psxsplash-rooms.ps-exe") != std::string::npos);
    CHECK(hwMiss.size() == 1 && hwMiss[0].find("psxsplash-rooms.ps-exe") != std::string::npos);
    CHECK(editor::missingDiscTools(eff).empty());
}

// Build > Boot Logo in Test Runs and the Disc section's Boot logo: with the
// logo off, Play, Run on hardware and Export boot the -nologo build beside
// the chosen one, or the build with the logo when there is none (saying so).
void testPlayBootLogo() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = g_outDir / "play-bootlogo";
    fs::remove_all(dir, ec);

    // Names: overlays first, then -nologo; a -nologo or overlay name maps to its family.
    const fs::path e = "/x/engine/psxsplash.ps-exe";
    CHECK(editor::overlayEngine(e, false, false, true) == fs::path("/x/engine/psxsplash-nologo.ps-exe"));
    CHECK(editor::overlayEngine(e, true, false, true) == fs::path("/x/engine/psxsplash-perf-nologo.ps-exe"));
    CHECK(editor::overlayEngine(e, false, true, true) == fs::path("/x/engine/psxsplash-rooms-nologo.ps-exe"));
    CHECK(editor::overlayEngine(e, true, true, true) == fs::path("/x/engine/psxsplash-perf-rooms-nologo.ps-exe"));
    CHECK(editor::overlayEngine(fs::path("/x/engine/psxsplash-nologo.ps-exe"), false, false, true) ==
          fs::path("/x/engine/psxsplash-nologo.ps-exe"));
    CHECK(editor::overlayEngine(fs::path("/x/engine/psxsplash-perf-nologo.ps-exe"), false, true, false) ==
          fs::path("/x/engine/psxsplash-rooms.ps-exe"));
    CHECK(editor::overlayEngine(fs::path("/x/engine/psxsplash-rooms-nologo.ps-exe"), true, true, true) ==
          fs::path("/x/engine/psxsplash-perf-rooms-nologo.ps-exe"));
    CHECK(editor::overlayEngine("/y/my build.exe", false, false, true) == fs::path("/y/my build-nologo.exe"));
    CHECK(editor::overlayEngine({}, false, false, true).empty());
    const fs::path d = "/x/engine/psxsplash-cdrom.ps-exe";
    CHECK(editor::discEngine(d, false) == d);
    CHECK(editor::discEngine(d, true) == fs::path("/x/engine/psxsplash-cdrom-nologo.ps-exe"));
    CHECK(editor::discEngine(fs::path("/x/engine/psxsplash-cdrom-nologo.ps-exe"), true) ==
          fs::path("/x/engine/psxsplash-cdrom-nologo.ps-exe"));
    CHECK(editor::discEngine({}, true).empty());

    // boot_logo round-trips; a file written before it reads on.
    const fs::path cfg = dir / "play.cfg";
    for (bool logo : {false, true}) {
        editor::PlayTools t;
        t.psxsplash = e;
        t.overlayRooms = true;
        t.bootLogo = logo;
        CHECK(editor::savePlayTools(cfg, t));
        const editor::PlayTools l = editor::loadPlayTools(cfg);
        CHECK(l.bootLogo == logo && l.overlayRooms && !l.overlayPerf);
    }
    { std::ofstream(cfg, std::ios::trunc) << "redux=/r\npsxsplash=/p.ps-exe\noverlay_perf=1\n"; }
    CHECK(editor::loadPlayTools(cfg).bootLogo);
    CHECK(editor::PlayTools().bootLogo);

    // A bundle carrying all ten builds: each combination boots its own.
    setEnv("SPLASHEDIT_PSXSPLASH", "");
    setEnv("SPLASHEDIT_DISC_ENGINE", "");
    const fs::path bundle = dir / "bundle", eng = bundle / "engine";
    fs::create_directories(eng);
    for (const char* v : {"", "-cdrom", "-perf", "-rooms", "-perf-rooms"}) {
        std::ofstream(eng / (std::string("psxsplash") + v + ".ps-exe")) << "x";
        std::ofstream(eng / (std::string("psxsplash") + v + "-nologo.ps-exe")) << "x";
    }
    editor::PlayTools want;
    want.redux = cfg;  // any existing file stands in for pcsx-redux
    want.port = "COM3";
    const char* stems[4] = {"psxsplash", "psxsplash-perf", "psxsplash-rooms", "psxsplash-perf-rooms"};
    for (int k = 0; k < 8; ++k) {
        want.overlayPerf = k & 1;
        want.overlayRooms = k & 2;
        want.bootLogo = !(k & 4);
        const editor::PlayTools eff = editor::withDefaults(want, bundle);
        const fs::path boots = eng / (std::string(stems[k & 3]) + (want.bootLogo ? "" : "-nologo") + ".ps-exe");
        std::string note = "stale";
        CHECK(editor::engineFor(eff, &note) == boots);
        CHECK(note.empty());
        CHECK(editor::missingTools(eff).empty() && editor::missingHardwareTools(eff).empty());
        const std::vector<std::string> cmd = editor::reduxCommand(eff, "/b");
        CHECK(std::find_if(cmd.begin(), cmd.end(), [&](const std::string& a) { return fs::path(a) == boots; }) != cmd.end());
        // The toggle is Play's: the disc build stays the one with the logo.
        CHECK(eff.disc == eng / "psxsplash-cdrom.ps-exe");
    }
    std::string note = "stale";
    CHECK(editor::discEngineFor(eng / "psxsplash-cdrom.ps-exe", true, &note) == eng / "psxsplash-cdrom.ps-exe" && note.empty());
    CHECK(editor::discEngineFor(eng / "psxsplash-cdrom.ps-exe", false, &note) == eng / "psxsplash-cdrom-nologo.ps-exe" && note.empty());

    // A psxsplash picked by hand with no -nologo beside it: the logo build boots, and the note names the missing file.
    fs::remove(eng / "psxsplash-rooms-nologo.ps-exe", ec);
    fs::remove(eng / "psxsplash-cdrom-nologo.ps-exe", ec);
    want.overlayPerf = false;
    want.overlayRooms = true;
    want.bootLogo = false;
    const editor::PlayTools eff = editor::withDefaults(want, bundle);
    CHECK(editor::engineFor(eff, &note) == eng / "psxsplash-rooms.ps-exe");
    CHECK(note.find("without the boot logo not found") != std::string::npos &&
          note.find("psxsplash-rooms-nologo.ps-exe") != std::string::npos && note.find("psxsplash-rooms.ps-exe instead") != std::string::npos);
    CHECK(editor::engineFor(eff) == eng / "psxsplash-rooms.ps-exe");  // no note asked for
    CHECK(editor::missingTools(eff).empty() && editor::missingHardwareTools(eff).empty());
    CHECK(editor::discEngineFor(eng / "psxsplash-cdrom.ps-exe", false, &note) == eng / "psxsplash-cdrom.ps-exe");
    CHECK(note.find("disc build without the boot logo not found") != std::string::npos &&
          note.find("psxsplash-cdrom-nologo.ps-exe") != std::string::npos);
    // With the logo build missing too, that one is what is reported missing.
    fs::remove(eng / "psxsplash-rooms.ps-exe", ec);
    const std::vector<std::string> miss = editor::missingTools(editor::withDefaults(want, bundle));
    CHECK(miss.size() == 1 && miss[0].find("psxsplash-rooms.ps-exe") != std::string::npos);
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

// New Project's templates: the listing (built-in empty first, then by order
// and name, broken folders skipped), a project copied from one, and the
// shipped templates themselves.
void testProjectTemplates() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = g_outDir / "templates";
    fs::remove_all(root, ec);
    const fs::path lib = root / "lib";
    auto write = [](const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    };
    // An empty or missing templates folder still offers Empty.
    std::vector<editor::TemplateInfo> none = editor::listTemplates(root / "missing");
    CHECK(none.size() == 1 && none[0].id == "empty" && none[0].dir.empty() && !none[0].name.empty());

    write(lib / "zeta" / "template.json", R"({"name": "Zeta", "description": "last by order", "order": 30})");
    write(lib / "zeta" / "zeta.scene", "{}");
    write(lib / "beta" / "template.json", R"({"name": "Beta", "description": "b", "order": 5})");
    write(lib / "alpha" / "template.json", R"({"name": "Alpha", "order": 5})");
    write(lib / "early" / "template.json", R"({"name": "Early", "order": -10})");
    write(lib / "nojson" / "readme.txt", "no template.json");
    write(lib / "broken" / "template.json", "{not json");
    write(lib / "noname" / "template.json", R"({"description": "no name", "order": 1})");
    write(lib / "badorder" / "template.json", R"({"name": "Bad", "order": "first"})");
    write(lib / "empty" / "template.json", R"({"name": "Shadow of empty", "order": -100})");
    write(lib / "loose.json", R"({"name": "Not a folder"})");
    std::vector<editor::TemplateInfo> list = editor::listTemplates(lib);
    std::vector<std::string> ids;
    for (const editor::TemplateInfo& t : list) ids.push_back(t.id);
    // Empty first even though "early" orders below 0; equal orders by name.
    CHECK((ids == std::vector<std::string>{"empty", "early", "alpha", "beta", "zeta"}));
    CHECK(list.size() == 5 && list[0].dir.empty() && list[4].name == "Zeta" && list[4].description == "last by order" &&
          list[4].order == 30 && list[4].dir == lib / "zeta" && list[2].description.empty());

    // The shipped templates: empty, first person, third person.
    const fs::path shipped = fs::path(SPLASHEDIT_SOURCE_DIR) / "templates";
    std::vector<editor::TemplateInfo> real = editor::listTemplates(shipped);
    // Samples fetched into the tree (templates/samples.lock) are not part of it.
    {
        std::vector<std::string> samples;
        std::ifstream lock(shipped / "samples.lock", std::ios::binary);
        for (std::string line; std::getline(lock, line);)
            if (!line.empty() && line[0] != '#') samples.push_back(line.substr(0, line.find(' ')));
        std::erase_if(real, [&](const editor::TemplateInfo& t) {
            return std::find(samples.begin(), samples.end(), t.id) != samples.end();
        });
    }
    ids.clear();
    for (const editor::TemplateInfo& t : real) ids.push_back(t.id);
    CHECK((ids == std::vector<std::string>{"empty", "first-person", "third-person"}));

    // Copying one: the scene (named after the folder), its meshes, textures
    // and both scripts, and no template.json.
    for (size_t i = 1; i < real.size(); ++i) {
        const fs::path proj = root / ("Game " + real[i].id);
        editor::NewProject made = editor::createProject(proj, &real[i]);
        CHECK(made.error.empty());
        CHECK(made.scene == fs::path("Game " + real[i].id + ".scene"));
        CHECK(fs::is_regular_file(proj / made.scene) && editor::firstScene(proj) == made.scene);
        CHECK(fs::is_regular_file(proj / "scripts" / "lever_lua.lua"));
        CHECK(fs::is_regular_file(proj / "scripts" / "lever_blocks.blocks"));
        CHECK(fs::is_regular_file(proj / "meshes" / "floor.mesh") && fs::is_regular_file(proj / "textures" / "stone.png"));
        CHECK(!fs::exists(proj / "template.json"));
        CHECK(fs::is_regular_file(proj / "scripts" / "third_person_camera.lua") == (real[i].id == "third-person"));
        editor::Document doc;
        CHECK(!doc.load(proj, made.scene) && !doc.dirty());
        CHECK(doc.objectCount() > 5 && doc.selectByName("Player") && doc.selectByName("Lever (Lua)") &&
              doc.selectByName("Lever (Blocks)"));
        // Again into the same folder: refused, it already holds a scene.
        editor::NewProject again = editor::createProject(proj, &real[i]);
        CHECK(!again.error.empty() && again.scene.empty() && again.error.find("already holds") != std::string::npos);
    }

    // Empty through the template path behaves as createProject(dir).
    {
        const fs::path proj = root / "Plain";
        editor::NewProject made = editor::createProject(proj, &real[0]);
        CHECK(made.error.empty() && made.scene == fs::path("Plain.scene") && fs::is_regular_file(proj / made.scene));
        CHECK(fs::is_directory(proj / "scripts") && fs::is_empty(proj / "scripts"));
    }

    // Several top scenes: the template's scenes.list names the one that
    // opens; without it the alphabetically first does.
    {
        write(lib / "multi" / "template.json", R"({"name": "Multi"})");
        write(lib / "multi" / "b.scene", "{}");
        write(lib / "multi" / "a.scene", "{}");
        write(lib / "multi" / "scenes.list", "gone.scene\nb.scene\na.scene\n");
        write(lib / "plain" / "template.json", R"({"name": "Plain"})");
        write(lib / "plain" / "b.scene", "{}");
        write(lib / "plain" / "a.scene", "{}");
        std::vector<editor::TemplateInfo> got = editor::listTemplates(lib);
        auto find = [&](const char* id) {
            for (const editor::TemplateInfo& t : got)
                if (t.id == id) return t;
            return editor::TemplateInfo{};
        };
        const editor::TemplateInfo multi = find("multi"), plain = find("plain");
        CHECK(!multi.dir.empty() && !plain.dir.empty());
        editor::NewProject listed = editor::createProject(root / "Listed", &multi);
        CHECK(listed.error.empty() && listed.scene == fs::path("b.scene"));
        CHECK(fs::is_regular_file(root / "Listed" / "a.scene") && fs::is_regular_file(root / "Listed" / "scenes.list"));
        editor::NewProject unlisted = editor::createProject(root / "Unlisted", &plain);
        CHECK(unlisted.error.empty() && unlisted.scene == fs::path("a.scene"));
        fs::remove_all(lib / "multi", ec);
        fs::remove_all(lib / "plain", ec);
    }

    // A file that would land on an existing one: nothing copied, the error names it.
    {
        const fs::path proj = root / "Clash";
        write(proj / "scripts" / "lever_lua.lua", "-- mine\n");
        editor::NewProject made = editor::createProject(proj, &real[1]);
        CHECK(!made.error.empty() && made.scene.empty() && made.error.find("scripts/lever_lua.lua") != std::string::npos);
        CHECK(editor::firstScene(proj).empty() && !fs::exists(proj / "meshes" / "floor.mesh"));
        std::ifstream in(proj / "scripts" / "lever_lua.lua", std::ios::binary);
        std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(kept == "-- mine\n");
    }
    // A template whose files cannot be read fails naming the file.
    {
        editor::TemplateInfo ghost = list[4];  // zeta: its scene is there
        write(lib / "zeta" / "meshes" / "m.mesh", "x");
        fs::permissions(lib / "zeta" / "meshes" / "m.mesh", fs::perms::none, ec);
        std::ifstream probe(lib / "zeta" / "meshes" / "m.mesh", std::ios::binary);
        if (!probe) {  // not when running as root, or on Windows
            editor::NewProject made = editor::createProject(root / "Ghost", &ghost);
            CHECK(!made.error.empty() && made.error.find("meshes/m.mesh") != std::string::npos);
            CHECK(editor::firstScene(root / "Ghost").empty());  // what was copied is taken back
        }
        probe.close();
        fs::permissions(lib / "zeta" / "meshes" / "m.mesh", fs::perms::owner_all, ec);
    }
    fs::remove_all(root, ec);
}

// Block scripts: the Lua each block makes, parenthesised only where Lua's
// precedence needs it, decimals as fixed-point fractions, hats merged per
// event, every error located, and the file format byte-stable.
splash::Block blk(const char* op, const char* field = "", std::vector<splash::Block> args = {},
                  std::vector<std::vector<splash::Block>> bodies = {}) {
    return {op, field, std::move(args), std::move(bodies)};
}
splash::Block bnum(const char* v) { return blk("number", v); }
splash::Block bvar(const char* v) { return blk("var", v); }
splash::BlockScript blockScript(std::vector<splash::BlockVariable> vars, std::vector<std::vector<splash::Block>> stacks) {
    splash::BlockScript s;
    s.variables = std::move(vars);
    for (auto& st : stacks) s.stacks.push_back({0, 0, std::move(st)});
    return s;
}

void testBlocks() {
    using splash::Block;
    std::vector<std::string> generated;  // every output must compile
    auto gen = [&](const splash::BlockScript& s) {
        splash::LuaGen g = splash::blocksToLua(s);
        if (g.errors.empty()) generated.push_back(g.lua);
        return g;
    };
    auto lua = [&](const splash::BlockScript& s) {
        splash::LuaGen g = gen(s);
        for (const std::string& e : g.errors) std::fprintf(stderr, "  blocks: %s\n", e.c_str());
        return g.errors.empty() ? g.lua : std::string("<errors>");
    };
    // One error, exactly.
    auto error = [&](const splash::BlockScript& s) {
        splash::LuaGen g = gen(s);
        return g.errors.size() == 1 ? g.errors[0] : "<" + std::to_string(g.errors.size()) + " errors>";
    };
    // `x = <e>` in onCreate, with a, b, c, x declared.
    auto expr = [&](Block e) {
        std::string out = lua(blockScript({{"a", "1"}, {"b", "2"}, {"c", "3"}, {"x", "0"}},
                                          {{blk("on_create"), blk("set_var", "x", {std::move(e)})}}));
        const std::string head = "local a = 1\nlocal b = 2\nlocal c = 3\nlocal x = 0\n\nfunction onCreate(self)\n    x = ";
        if (out.rfind(head, 0) != 0 || out.size() < head.size() + 5) return out;
        return out.substr(head.size(), out.size() - head.size() - 5);  // "\nend\n"
    };
    Block a = bvar("a"), b = bvar("b"), c = bvar("c");

    // The door example.
    CHECK(lua(blockScript({{"opened", "false"}},
                          {{blk("on_interact"),
                            blk("if", "", {blk("not", "", {bvar("opened")})},
                                {{blk("set_var", "opened", {blk("bool", "true")}), blk("hide", "Door"),
                                  blk("play_sound", "creak")}})}})) ==
          "local opened = false\n\nfunction onInteract(self)\n    if not opened then\n        opened = true\n"
          "        Entity.SetActive(Entity.Find(\"Door\"), false)\n        Audio.Play(\"creak\")\n    end\nend\n");

    // Statements.
    CHECK(lua(blockScript({{"n", "0"}, {"t", "hi"}},
                          {{blk("on_create"), blk("change_var", "n", {bnum("2")}), blk("change_var", "n", {bnum("-1")}),
                            blk("set_var", "t", {blk("text", "a\"b\\c\nd\te")}), blk("show", "self"), blk("hide", "Lamp"),
                            blk("move_by", "self", {bnum("1"), bnum("0"), bnum("-2")}),
                            blk("go_to", "Crate", {bnum("0"), bnum("0.5"), bnum("3")}), blk("play_cutscene", "intro"),
                            blk("load_scene", "", {bnum("2")}), blk("log", "", {bvar("t")}), blk("stop")}})) ==
          "local n = 0\nlocal t = \"hi\"\n\nfunction onCreate(self)\n"
          "    n = n + 2\n    n = n - 1\n    t = \"a\\\"b\\\\c\\nd\\te\"\n    Entity.SetActive(self, true)\n"
          "    Entity.SetActive(Entity.Find(\"Lamp\"), false)\n"
          "    Entity.SetPosition(self, Vec3.add(Entity.GetPosition(self), Vec3.new(1, 0, -2)))\n"
          "    Entity.SetPosition(Entity.Find(\"Crate\"), Vec3.new(0, FixedPoint.new(1) / 2, 3))\n"
          "    Cutscene.Play(\"intro\")\n    Scene.Load(2)\n    Debug.Log(t)\n    return\nend\n");
    CHECK(lua(blockScript({{"n", "0"}, {"t", "hi"}}, {{blk("on_create"), blk("change_var", "n", {blk("sub", "", {bvar("n"), bnum("1")})})}})) ==
          "local n = 0\nlocal t = \"hi\"\n\nfunction onCreate(self)\n    n = n + (n - 1)\nend\n");
    // C blocks and loop variables by depth.
    CHECK(lua(blockScript({{"n", "0"}},
                          {{blk("on_update"),
                            blk("repeat", "", {bnum("3")},
                                {{blk("repeat", "", {bvar("n")}, {{blk("change_var", "n", {bnum("1")})}})}}),
                            blk("repeat", "", {bnum("2")}, {{}}),
                            blk("if_else", "", {blk("gt", "", {bvar("n"), bnum("5")})},
                                {{blk("set_var", "n", {bnum("0")})}, {blk("log", "", {blk("text", "small")})}}),
                            blk("while", "", {blk("lt", "", {bvar("n"), bnum("3")})}, {{blk("change_var", "n", {bnum("1")})}}),
                            blk("repeat_until", "", {blk("eq", "", {bvar("n"), bnum("9")})}, {{}}),
                            blk("repeat_until", "", {blk("button_held", "CROSS")}, {{}}),
                            blk("if", "", {blk("bool", "true")})}})) ==
          "local n = 0\n\nfunction onUpdate(self, dt)\n    for i = 1, 3 do\n        for j = 1, n do\n"
          "            n = n + 1\n        end\n    end\n    for i = 1, 2 do\n    end\n    if n > 5 then\n        n = 0\n"
          "    else\n        Debug.Log(\"small\")\n    end\n    while n < 3 do\n        n = n + 1\n    end\n"
          "    while not (n == 9) do\n    end\n    while not Input.IsHeldPlayer1(Input.CROSS) do\n    end\n"
          "    if true then\n    end\nend\n");
    // A variable named like a loop variable is skipped.
    CHECK(lua(blockScript({{"i", "0"}}, {{blk("on_create"), blk("repeat", "", {bnum("2")}, {{blk("change_var", "i", {bnum("1")})}})}})) ==
          "local i = 0\n\nfunction onCreate(self)\n    for j = 1, 2 do\n        i = i + 1\n    end\nend\n");

    // Reporters.
    CHECK(expr(blk("random", "", {bnum("1"), bnum("6")})) == "Random.Range(1, 6)");
    CHECK(expr(blk("frame_count")) == "Timer.GetFrameCount()");
    CHECK(expr(blk("position_x", "self")) == "Entity.GetPosition(self).x");
    CHECK(expr(blk("position_z", "Crate")) == "Entity.GetPosition(Entity.Find(\"Crate\")).z");
    CHECK(expr(blk("button_held", "R3")) == "Input.IsHeldPlayer1(Input.R3)");
    CHECK(expr(blk("text", "")) == "\"\"");
    CHECK(expr(blk("bool", "false")) == "false");
    CHECK(expr(blk("mod", "", {a, b})) == "a % b");
    CHECK(expr(blk("div", "", {a, b})) == "a / b");

    // Precedence: parentheses only where Lua needs them.
    CHECK(expr(blk("sub", "", {a, blk("sub", "", {b, c})})) == "a - (b - c)");
    CHECK(expr(blk("sub", "", {blk("sub", "", {a, b}), c})) == "a - b - c");
    CHECK(expr(blk("mul", "", {blk("add", "", {a, b}), c})) == "(a + b) * c");
    CHECK(expr(blk("add", "", {a, blk("mul", "", {b, c})})) == "a + b * c");
    CHECK(expr(blk("div", "", {a, blk("mul", "", {b, c})})) == "a / (b * c)");
    CHECK(expr(blk("not", "", {blk("eq", "", {a, b})})) == "not (a == b)");
    CHECK(expr(blk("not", "", {blk("not", "", {a})})) == "not not a");
    CHECK(expr(blk("join", "", {a, blk("join", "", {b, c})})) == "a .. b .. c");
    CHECK(expr(blk("join", "", {blk("join", "", {a, b}), c})) == "(a .. b) .. c");
    CHECK(expr(blk("join", "", {blk("add", "", {a, b}), c})) == "a + b .. c");
    CHECK(expr(blk("or", "", {blk("and", "", {a, b}), c})) == "a and b or c");
    CHECK(expr(blk("and", "", {blk("or", "", {a, b}), c})) == "(a or b) and c");
    CHECK(expr(blk("lt", "", {blk("add", "", {a, b}), blk("join", "", {b, c})})) == "a + b < b .. c");
    CHECK(expr(blk("eq", "", {blk("eq", "", {a, b}), c})) == "a == b == c");
    CHECK(expr(blk("mul", "", {bnum("0.5"), a})) == "FixedPoint.new(1) / 2 * a");
    CHECK(expr(blk("div", "", {a, bnum("0.5")})) == "a / (FixedPoint.new(1) / 2)");
    CHECK(expr(blk("add", "", {a, bnum("-0.5")})) == "a + FixedPoint.new(-1) / 2");
    CHECK(expr(blk("sub", "", {a, bnum("-3")})) == "a - -3");
    CHECK(expr(blk("mul", "", {bnum("-3"), a})) == "-3 * a");

    // Decimals: integer-only psxlua takes them as fixed-point fractions.
    CHECK(expr(bnum("0.5")) == "FixedPoint.new(1) / 2");
    CHECK(expr(bnum("1.25")) == "FixedPoint.new(5) / 4");
    CHECK(expr(bnum("0.1")) == "FixedPoint.new(1) / 10");
    CHECK(expr(bnum("-0.5")) == "FixedPoint.new(-1) / 2");
    CHECK(expr(bnum("2.50")) == "FixedPoint.new(5) / 2");
    CHECK(expr(bnum("3")) == "3");
    CHECK(expr(bnum("-3")) == "-3");
    CHECK(expr(bnum("3.0")) == "3");
    CHECK(expr(bnum("007")) == "7");
    CHECK(expr(bnum("52428.7")) == "FixedPoint.new(524287) / 10");
    CHECK(expr(bnum("-104857.6")) == "FixedPoint.new(-524288) / 5");
    CHECK(expr(bnum("104857.4")) == "FixedPoint.new(524287) / 5");
    auto oneExpr = [&](Block e) {
        return error(blockScript({{"x", "0"}}, {{blk("on_create"), blk("set_var", "x", {std::move(e)})}}));
    };
    CHECK(oneExpr(bnum("52428.9")) ==
          "stack 1, block 2 (set_var), input 1 (number): number 52428.9 is out of range (decimals go from -524288 to 524287)");
    CHECK(oneExpr(bnum("104857.8")) ==
          "stack 1, block 2 (set_var), input 1 (number): number 104857.8 is out of range (decimals go from -524288 to 524287)");
    CHECK(oneExpr(bnum("524287.5")) ==
          "stack 1, block 2 (set_var), input 1 (number): number 524287.5 is out of range (decimals go from -524288 to 524287)");
    CHECK(oneExpr(bnum("2147483648")) == "stack 1, block 2 (set_var), input 1 (number): number 2147483648 is out of range");
    CHECK(oneExpr(bnum("0.1234567891")) ==
          "stack 1, block 2 (set_var), input 1 (number): number 0.1234567891 has more than 9 decimal places");
    CHECK(oneExpr(bnum("1e3")) == "stack 1, block 2 (set_var), input 1 (number): \"1e3\" is not a number");
    CHECK(oneExpr(bnum(".5")) == "stack 1, block 2 (set_var), input 1 (number): \".5\" is not a number");
    CHECK(oneExpr(bnum("")) == "stack 1, block 2 (set_var), input 1 (number): no number");
    CHECK(oneExpr(blk("bool", "yes")) == "stack 1, block 2 (set_var), input 1 (bool): \"yes\" is not true or false");
    // Variable values: integer, decimal, bool, else text.
    CHECK(lua(blockScript({{"p", "-4"}, {"q", "0.75"}, {"r", "true"}, {"s", "1.2.3"}, {"u", ""}}, {})) ==
          "local p = -4\nlocal q = FixedPoint.new(3) / 4\nlocal r = true\nlocal s = \"1.2.3\"\nlocal u = \"\"\n");
    CHECK(error(blockScript({{"p", "999999.5"}}, {})) ==
          "variable 1 (p): number 999999.5 is out of range (decimals go from -524288 to 524287)");

    // Hats: one function per event in catalogue order, stacks merged in
    // stack order; button hats become one if/elseif chain.
    CHECK(lua(blockScript({}, {{blk("on_interact"), blk("log", "", {bnum("1")})},
                               {blk("on_create")},
                               {blk("on_interact"), blk("log", "", {bnum("2")})},
                               {blk("on_interact")},
                               {blk("on_interact"), blk("log", "", {bnum("3")})}})) ==
          "function onCreate(self)\nend\n\nfunction onInteract(self)\n    Debug.Log(1)\n\n    Debug.Log(2)\n\n"
          "    Debug.Log(3)\nend\n");
    CHECK(lua(blockScript({}, {{blk("on_button_press", "START"), blk("log", "", {bnum("1")})},
                               {blk("on_button_release", "CROSS")},
                               {blk("on_button_press", "CROSS"), blk("log", "", {bnum("2")})},
                               {blk("on_button_press", "START"), blk("log", "", {bnum("3")})}})) ==
          "function onButtonPress(self, button)\n    if button == Input.CROSS then\n        Debug.Log(2)\n"
          "    elseif button == Input.START then\n        Debug.Log(1)\n\n        Debug.Log(3)\n    end\nend\n\n"
          "function onButtonRelease(self, button)\n    if button == Input.CROSS then\n    end\nend\n");
    std::string allHats;
    std::vector<std::vector<Block>> hatStacks;
    for (auto it = splash::blockCatalogue().rbegin(); it != splash::blockCatalogue().rend(); ++it)
        if (it->kind == splash::BlockKind::Hat) hatStacks.push_back({blk(std::string(it->op).c_str(), "L1")});
    CHECK(lua(blockScript({}, hatStacks)) ==
          "function onCreate(self)\nend\n\nfunction onUpdate(self, dt)\nend\n\nfunction onCollideWithPlayer(self)\nend\n\n"
          "function onInteract(self)\nend\n\nfunction onButtonPress(self, button)\n    if button == Input.L1 then\n    end\nend\n\n"
          "function onButtonRelease(self, button)\n    if button == Input.L1 then\n    end\nend\n\n"
          "function onEnable(self)\nend\n\nfunction onDisable(self)\nend\n\n"
          "function onSceneCreationEnd()\nend\n\nfunction onTriggerEnter(index)\nend\n\nfunction onTriggerExit(index)\nend\n");
    // psxsplash never calls onDestroy: no hat offers it, and a saved stack
    // under the old hat is an error rather than a silently loose stack.
    CHECK(!splash::findBlockSpec("on_destroy"));
    CHECK(error(blockScript({}, {{blk("on_create")}, {blk("on_destroy"), blk("log", "", {bnum("1")})}})) ==
          "stack 2, block 1 (on_destroy): \"when destroyed\" never runs, psxsplash does not destroy objects; move "
          "these blocks under another event");
    CHECK(splash::blockNameError("onDestroy").empty());
    // Loose stacks make nothing and are not checked.
    CHECK(lua(blockScript({}, {{blk("log", "", {bnum("1")})}, {}, {blk("nonsense")}, {blk("on_create")}})) ==
          "function onCreate(self)\nend\n");
    CHECK(lua(blockScript({}, {})).empty());

    // Errors, each located.
    auto stmt = [&](Block s) { return error(blockScript({{"n", "0"}}, {{blk("on_create"), std::move(s)}})); };
    CHECK(stmt(blk("move_by", "self", {bnum("1"), bnum("2")})) == "stack 1, block 2 (move_by): missing input 3");
    CHECK(stmt(blk("move_by", "self", {bnum("1"), blk(""), bnum("1")})) == "stack 1, block 2 (move_by): missing input 2");
    CHECK(stmt(blk("log", "", {bnum("1"), bnum("2")})) == "stack 1, block 2 (log): too many inputs");
    CHECK(stmt(blk("log", "", {bnum("1")}, {{}})) == "stack 1, block 2 (log): too many bodies");
    CHECK(stmt(blk("teleport")) == "stack 1, block 2 (teleport): unknown block");
    CHECK(stmt(blk("")) == "stack 1, block 2 (empty): empty block");
    CHECK(stmt(blk("add", "", {bnum("1"), bnum("2")})) == "stack 1, block 2 (add): a reporter block cannot be a statement");
    CHECK(stmt(blk("on_create")) == "stack 1, block 2 (on_create): a hat block can only start a stack");
    CHECK(stmt(blk("log", "", {blk("stop")})) == "stack 1, block 2 (log), input 1 (stop): a statement block cannot be an input");
    CHECK(stmt(blk("set_var", "q", {bnum("1")})) == "stack 1, block 2 (set_var): undeclared variable \"q\"");
    CHECK(stmt(blk("change_var", "", {bnum("1")})) == "stack 1, block 2 (change_var): no variable chosen");
    CHECK(stmt(blk("log", "", {bvar("q")})) == "stack 1, block 2 (log), input 1 (var): undeclared variable \"q\"");
    CHECK(stmt(blk("if", "", {bnum("1")}, {{blk("set_var", "q", {bnum("1")})}})) ==
          "stack 1, block 2 (if), body 1, block 1 (set_var): undeclared variable \"q\"");
    CHECK(stmt(blk("if_else", "", {bnum("1")}, {{}, {blk("log"), }})) ==
          "stack 1, block 2 (if_else), body 2, block 1 (log): missing input 1");
    CHECK(stmt(blk("log", "", {blk("add", "", {bnum("1")})})) == "stack 1, block 2 (log), input 1 (add): missing input 2");
    CHECK(stmt(blk("hide")) == "stack 1, block 2 (hide): no object chosen");
    CHECK(stmt(blk("play_sound")) == "stack 1, block 2 (play_sound): no sound chosen");
    CHECK(stmt(blk("play_cutscene")) == "stack 1, block 2 (play_cutscene): no cutscene chosen");
    CHECK(stmt(blk("log", "", {blk("button_held", "X")})) == "stack 1, block 2 (log), input 1 (button_held): unknown button \"X\"");
    CHECK(error(blockScript({}, {{blk("on_button_press", "")}})) == "stack 1, block 1 (on_button_press): no button chosen");
    CHECK(error(blockScript({}, {{blk("on_button_release", "cross")}})) ==
          "stack 1, block 1 (on_button_release): unknown button \"cross\"");
    CHECK(error(blockScript({}, {{blk("on_scene_start"), blk("show", "self")}})) ==
          "stack 1, block 2 (show): self is not available in on_scene_start");
    CHECK(error(blockScript({}, {{blk("on_trigger_enter"), blk("log", "", {blk("position_y", "self")})}})) ==
          "stack 1, block 2 (log), input 1 (position_y): self is not available in on_trigger_enter");
    CHECK(error(blockScript({}, {{blk("on_create"), blk("stop"), blk("log", "", {bnum("1")})}})) ==
          "stack 1, block 2 (stop): nothing can follow stop");
    // A stop returns from the merged function, so only the last stack may stop.
    CHECK(error(blockScript({}, {{blk("on_create"), blk("if", "", {bnum("1")}, {{blk("stop")}})}, {blk("on_create")}})) ==
          "stack 1, block 2 (if), body 1, block 1 (stop): stop would also skip stack 2, which has the same hat");
    CHECK(lua(blockScript({}, {{blk("on_create")}, {blk("on_create"), blk("stop")}})) == "function onCreate(self)\n    return\nend\n");
    CHECK(error(blockScript({{"end", "1"}}, {})) == "variable 1 (end): \"end\" is reserved");
    CHECK(error(blockScript({{"self", "1"}}, {})) == "variable 1 (self): \"self\" is reserved");
    CHECK(error(blockScript({{"dt", "1"}}, {})) == "variable 1 (dt): \"dt\" is reserved");
    CHECK(error(blockScript({{"Entity", "1"}}, {})) == "variable 1 (Entity): \"Entity\" is reserved");
    CHECK(error(blockScript({{"onCreate", "1"}}, {})) == "variable 1 (onCreate): \"onCreate\" is reserved");
    CHECK(error(blockScript({{"2x", "1"}}, {})) ==
          "variable 1 (2x): not a name (letters, digits and _, not starting with a digit)");
    CHECK(error(blockScript({{"my var", "1"}}, {})) ==
          "variable 1 (my var): not a name (letters, digits and _, not starting with a digit)");
    CHECK(error(blockScript({{"", "1"}}, {})) == "variable 1 (): no name");
    CHECK(error(blockScript({{"n", "1"}, {"n", "2"}}, {})) == "variable 2 (n): declared twice");
    CHECK(lua(blockScript({{"_n2", "1"}}, {})) == "local _n2 = 1\n");
    // wait: a stack that waits runs as a task resumed from onUpdate; others stay plain.
    {
        const std::string noWait = lua(blockScript({}, {{blk("on_create"), blk("log", "", {blk("text", "hi")})}}));
        CHECK(noWait == "function onCreate(self)\n    Debug.Log(\"hi\")\nend\n");
        const std::string w = lua(blockScript({}, {{blk("on_create"), blk("wait", "", {bnum("2")}), blk("stop")},
                                                   {blk("on_create"), blk("wait_until", "", {blk("bool", "true")})},
                                                   {blk("on_create"), blk("log", "", {blk("text", "now")})}}));
        CHECK(w.find("local function startTask(name, fn)") != std::string::npos);
        CHECK(w.find("local function wait(seconds)") != std::string::npos);
        CHECK(w.find("function onCreate(self)\n    startTask(\"onCreate_1\", function()\n        wait(2)\n        return\n    end)\n\n"
                     "    startTask(\"onCreate_2\", function()\n        while not true do\n            coroutine.yield()\n        end\n"
                     "    end)\n\n    Debug.Log(\"now\")\nend\n") != std::string::npos);
        CHECK(w.find("function onUpdate(self, dt)\n    if tasks.onCreate_1 then\n        resumeTask(\"onCreate_1\", dt)\n    end\n"
                     "    if tasks.onCreate_2 then") != std::string::npos);
        // wait until alone needs no wait helper.
        const std::string u = lua(blockScript({}, {{blk("on_interact"), blk("wait_until", "", {blk("bool", "true")})}}));
        CHECK(u.find("startTask(\"onInteract\"") != std::string::npos && u.find("local function wait(") == std::string::npos);
        // A waiting every-frame stack is itself a task, started after the resumes.
        const std::string f = lua(blockScript({}, {{blk("on_update"), blk("wait", "", {bnum("1")})}}));
        CHECK(f.find("function onUpdate(self, dt)\n    if tasks.onUpdate then\n        resumeTask(\"onUpdate\", dt)\n    end\n\n"
                     "    startTask(\"onUpdate\", function()\n        wait(1)\n    end)\nend\n") != std::string::npos);
        CHECK(error(blockScript({}, {{blk("on_scene_start"), blk("wait", "", {bnum("1")})}})) ==
              "stack 1, block 2 (wait): wait only works in object scripts: the scene and trigger events get no frames");
        CHECK(error(blockScript({}, {{blk("on_trigger_enter"), blk("wait_until", "", {blk("bool", "true")})}})) ==
              "stack 1, block 2 (wait_until): wait only works in object scripts: the scene and trigger events get no frames");
        CHECK(error(blockScript({}, {{blk("on_create"), blk("wait")}})) == "stack 1, block 2 (wait): missing input 1");
        CHECK(error(blockScript({{"wait", "1"}}, {})) == "variable 1 (wait): \"wait\" is reserved");
        CHECK(error(blockScript({{"tasks", "1"}}, {})) == "variable 1 (tasks): \"tasks\" is reserved");
    }
    // Lights, Persist and my name: the engine calls they make, and what they refuse.
    {
        CHECK(lua(blockScript({}, {{blk("on_interact"), blk("light_on", "Beacon \"1\""), blk("light_off", "Lamp")}})) ==
              "function onInteract(self)\n    Light.SetEnabled(Light.Find(\"Beacon \\\"1\\\"\"), true)\n"
              "    Light.SetEnabled(Light.Find(\"Lamp\"), false)\nend\n");
        CHECK(error(blockScript({}, {{blk("on_create"), blk("light_on")}})) == "stack 1, block 2 (light_on): no light chosen");
        // Scene scripts can switch lights too: Light.Find needs no self.
        CHECK(lua(blockScript({}, {{blk("on_scene_start"), blk("light_off", "Lamp")}})) ==
              "function onSceneCreationEnd()\n    Light.SetEnabled(Light.Find(\"Lamp\"), false)\nend\n");
        CHECK(lua(blockScript({{"n", "2"}}, {{blk("on_scene_start"), blk("remember", "score", {blk("add", "", {bvar("n"), bnum("1")})}),
                                              blk("set_var", "n", {blk("remembered", "score")})}})) ==
              "local n = 2\n\nfunction onSceneCreationEnd()\n    Persist.Set(\"score\", n + 1)\n    n = (Persist.Get(\"score\") or 0)\nend\n");
        CHECK(expr(blk("lt", "", {blk("remembered", "best"), bnum("3")})) == "(Persist.Get(\"best\") or 0) < 3");
        CHECK(expr(blk("add", "", {blk("remembered", "best"), bnum("1")})) == "(Persist.Get(\"best\") or 0) + 1");
        CHECK(error(blockScript({}, {{blk("on_create"), blk("remember", "", {bnum("1")})}})) ==
              "stack 1, block 2 (remember): no name to remember it by");
        CHECK(error(blockScript({}, {{blk("on_create"), blk("remember", "k")}})) == "stack 1, block 2 (remember): missing input 1");
        const std::string k31(31, 'k');
        CHECK(lua(blockScript({}, {{blk("on_create"), blk("remember", k31.c_str(), {bnum("1")})}})).find("Persist.Set(\"" + k31 + "\", 1)") !=
              std::string::npos);
        CHECK(error(blockScript({}, {{blk("on_create"), blk("remember", (k31 + "k").c_str(), {bnum("1")})}})) ==
              "stack 1, block 2 (remember): the name is longer than 31 bytes, more than the game keeps");
        for (splash::Block v : {blk("bool", "true"), blk("text", "3"), bnum("0.5"), blk("position_x", "self"), blk("my_name"),
                                blk("gt", "", {bnum("1"), bnum("2")})})
            CHECK(error(blockScript({}, {{blk("on_create"), blk("remember", "k", {v})}})) ==
                  "stack 1, block 2 (remember): only whole numbers can be remembered");
        CHECK(lua(blockScript({}, {{blk("on_create"), blk("remember", "k", {bnum("-7")})}})).find("Persist.Set(\"k\", -7)") !=
              std::string::npos);
        CHECK(error(blockScript({}, {{blk("on_create"), blk("log", "", {blk("remembered")})}})) ==
              "stack 1, block 2 (log), input 1 (remembered): no name to remember it by");
        CHECK(lua(blockScript({}, {{blk("on_create"), blk("log", "", {blk("my_name")})}})) ==
              "function onCreate(self)\n    Debug.Log(Actor.GetName(self))\nend\n");
        CHECK(error(blockScript({}, {{blk("on_trigger_enter"), blk("log", "", {blk("my_name")})}})) ==
              "stack 1, block 2 (log), input 1 (my_name): my name is not available in on_trigger_enter");
        CHECK(splash::findBlockSpec("light_on")->field == splash::BlockField::Light &&
              splash::findBlockSpec("remember")->field == splash::BlockField::Key &&
              splash::findBlockSpec("remembered")->kind == splash::BlockKind::Reporter &&
              splash::findBlockSpec("my_name")->field == splash::BlockField::None);
        CHECK(!splash::blockNameError("Light").empty() && !splash::blockNameError("Persist").empty() &&
              !splash::blockNameError("Actor").empty());
    }
    // Several errors are all reported; the file form throws the first.
    CHECK(splash::blocksToLua(blockScript({}, {{blk("on_create"), blk("hide"), blk("teleport")}})).errors.size() == 2);
    std::string thrown;
    try {
        splash::blocksFileToLua(splash::saveBlocks(blockScript({}, {{blk("on_create"), blk("hide"), blk("teleport")}})),
                                "scripts/x.blocks");
    } catch (const std::runtime_error& e) {
        thrown = e.what();
    }
    CHECK(thrown == "scripts/x.blocks: stack 1, block 2 (hide): no object chosen");

    // File format: stable key order, fields left out when empty.
    splash::BlockScript round = blockScript({{"opened", "false"}, {"name", "Door \"A\""}},
                                            {{blk("on_interact"),
                                              blk("if_else", "", {blk("not", "", {bvar("opened")})},
                                                  {{blk("move_by", "self", {bnum("0.5"), blk(""), bnum("-1")})}, {}})},
                                             {blk("log", "", {blk("text", "loose \xc3\xa9")})}});
    round.stacks[0].x = 12.5f;
    round.stacks[0].y = -3;
    round.stacks[1].x = 300;
    const std::string saved = splash::saveBlocks(round);
    CHECK(splash::parseBlocks(saved, "r.blocks") == round);
    CHECK(splash::saveBlocks(splash::parseBlocks(saved, "r.blocks")) == saved);
    CHECK(saved.rfind("{\n  \"format\": \"splashedit-blocks\",\n  \"version\": 1,\n  \"variables\": [\n    {\n"
                      "      \"name\": \"opened\",\n      \"value\": \"false\"\n    },", 0) == 0);
    CHECK(saved.find("\"x\": 12.5,\n      \"y\": -3,\n      \"blocks\"") != std::string::npos);
    CHECK(saved.find("{\n          \"op\": \"on_interact\"\n        }") != std::string::npos);
    CHECK(saved.find("\"op\": \"var\",\n                  \"field\": \"opened\"\n") != std::string::npos);
    CHECK(saved.find("\"op\": \"\"\n") != std::string::npos);
    CHECK(saved.size() > 2 && saved.substr(saved.size() - 2) == "}\n");
    CHECK(splash::saveBlocks({}) == "{\n  \"format\": \"splashedit-blocks\",\n  \"version\": 1,\n  \"variables\": [],\n"
                                    "  \"stacks\": []\n}\n");
    CHECK(splash::parseBlocks("{\"format\":\"splashedit-blocks\",\"version\":1}", "e.blocks") == splash::BlockScript{});
    // An unknown op loads (a newer editor's block) and fails at codegen.
    CHECK(splash::parseBlocks("{\"format\":\"splashedit-blocks\",\"version\":1,\"stacks\":[{\"blocks\":[{\"op\":\"on_create\"},"
                              "{\"op\":\"fly\"}]}]}", "f.blocks").stacks[0].blocks[1].op == "fly");
    auto parseError = [](const std::string& text) {
        try {
            splash::parseBlocks(text, "p.blocks");
        } catch (const std::runtime_error& e) {
            return std::string(e.what());
        }
        return std::string("<no error>");
    };
    CHECK(parseError("{").rfind("p.blocks: ", 0) == 0);
    CHECK(parseError("[]") == "p.blocks: not a block script");
    CHECK(parseError("{\"format\":\"splashedit-ng/scene\",\"version\":1}") == "p.blocks: not a splashedit block script");
    CHECK(parseError("{\"format\":\"splashedit-blocks\"}") == "p.blocks: missing or invalid \"version\"");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":2}") == "p.blocks: format version 2, this build reads up to 1");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":1,\"stack\":[]}") == "p.blocks: file: unknown key \"stack\"");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":1,\"variables\":[{\"name\":\"a\",\"value\":1}]}") ==
          "p.blocks: variables[0].value: not a string");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":1,\"stacks\":[{\"blocks\":[{\"op\":\"if\",\"bodies\":[[{\"op\":3}]]}]}]}") ==
          "p.blocks: stacks[0].blocks[0].bodies[0][0].op: not a string");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":1,\"stacks\":[{\"x\":\"1\"}]}") ==
          "p.blocks: stacks[0].x: not a number");
    CHECK(parseError("{\"format\":\"splashedit-blocks\",\"version\":1,\"stacks\":[{\"blocks\":[{\"op\":\"log\",\"arg\":[]}]}]}") ==
          "p.blocks: stacks[0].blocks[0]: unknown key \"arg\"");

    // The catalogue: unique ops, labels naming each input, hats first.
    std::set<std::string> ops;
    int hats = 0;
    bool labelsOk = true, hatsFirst = true;
    for (const splash::BlockSpec& s : splash::blockCatalogue()) {
        ops.insert(std::string(s.op));
        if (s.kind == splash::BlockKind::Hat) hatsFirst = hatsFirst && hats == int(ops.size()) - 1, hats++;
        for (int i = 1; i <= 9; i++)
            labelsOk = labelsOk && (std::string(s.label).find("%" + std::to_string(i)) != std::string::npos) == (i <= s.args);
        labelsOk = labelsOk && (std::string(s.label).find("%f") != std::string::npos) == (s.field != splash::BlockField::None);
        labelsOk = labelsOk && (s.kind == splash::BlockKind::CBlock) == (s.bodies > 0);
    }
    CHECK(ops.size() == splash::blockCatalogue().size() && hats == 11 && hatsFirst && labelsOk);
    CHECK(splash::findBlockSpec("move_by") && splash::findBlockSpec("move_by")->args == 3 && !splash::findBlockSpec("fly"));
    CHECK(splash::blockButtons().size() == 16 && splash::blockButtons().front() == "CROSS");

    // Every script generated above compiles for the PS1.
    int compiled = 0;
    for (const std::string& src : generated) {
        try {
            splash::compileLua(src, "blocks.lua");
            compiled++;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  blocks: does not compile: %s\n%s", e.what(), src.c_str());
        }
    }
    CHECK(generated.size() >= 50 && compiled == int(generated.size()));
}

// The block editor's model: where blocks sit, what a point hits, the edits
// and the kind rules they keep, undo, new blocks' defaults. Text is 7 px a
// character here.
float blkWidth(std::string_view s) { return 7.0f * float(s.size()); }
bool blkInside(const editor::blk::Rect& a, const editor::blk::Rect& b) {
    return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1;
}
// on interact: hide Door; if not opened then show self, stop; move self by 1 2 3. Then a loose log "hi".
splash::BlockScript blkDoor() {
    splash::BlockScript s = blockScript({{"opened", "false"}},
                                        {{blk("on_interact"), blk("hide", "Door"),
                                          blk("if", "", {blk("not", "", {bvar("opened")})}, {{blk("show", "self"), blk("stop")}}),
                                          blk("move_by", "self", {bnum("1"), bnum("2"), bnum("3")})}});
    s.stacks[0].x = 40;
    s.stacks[0].y = 30;
    s.stacks.push_back({300, 50, {blk("log", "", {blk("text", "hi")})}});
    return s;
}

void testBlocksLayout() {
    namespace eb = editor::blk;
    const eb::Metrics m;
    const splash::BlockScript s = blkDoor();
    const eb::Layout l = eb::layout(s, blkWidth, m);
    auto box = [&](const char* p) { return l.find(eb::parsePath(p)); };
    const eb::BlockBox *hat = box("s0.b0"), *hide = box("s0.b1"), *iff = box("s0.b2"), *move = box("s0.b3");
    CHECK(hat && hide && iff && move);
    if (!hat || !hide || !iff || !move) return;
    // A stack's blocks one under another from the stack's corner.
    CHECK(hat->rect.x0 == 40 && hat->rect.y0 == 30 && hat->rect.h() == m.row + m.hatTop && hide->rect.h() == m.row);
    CHECK(hide->rect.x0 == 40 && hide->rect.y0 == hat->rect.y1 && iff->rect.y0 == hide->rect.y1 && move->rect.y0 == iff->rect.y1);
    CHECK(l.stacks.size() == 2 && l.stacks[0].hat && !l.stacks[1].hat && l.stacks[0].bottomX == 40 &&
          l.stacks[0].bottomY == move->rect.y1 && l.stacks[1].rect.x0 == 300 && l.stacks[1].rect.y0 == 50);
    CHECK(eb::stackSize(s.stacks[0], blkWidth, m).second == move->rect.y1 - 30);
    // A C block's mouth under its header row, indented; its blocks fill it; the arm under it.
    CHECK(iff->mouths.size() == 1);
    const eb::Rect mouth = iff->mouths.empty() ? eb::Rect{} : iff->mouths[0];
    const eb::BlockBox *show = box("s0.b2.d0.b0"), *stop = box("s0.b2.d0.b1");
    CHECK(show && stop && !iff->parts.empty());
    if (!show || !stop || iff->parts.empty()) return;
    CHECK(mouth.x0 == 40 + m.mouthIndent && mouth.y0 == iff->parts[0].y1 && mouth.y0 > iff->rect.y0);
    CHECK(show->rect.x0 == mouth.x0 && show->rect.y0 == mouth.y0 && stop->rect.y0 == show->rect.y1 && mouth.y1 == stop->rect.y1);
    CHECK(iff->rect.y1 == mouth.y1 + m.arm && iff->parts.back().y0 == mouth.y1);
    // An empty mouth still has room; if_else has two, the second under the else row.
    splash::BlockScript e;
    e.stacks.push_back({0, 0, {eb::newBlock("if_else")}});
    const eb::Layout el = eb::layout(e, blkWidth, m);
    const eb::BlockBox* ie = el.find(eb::parsePath("s0.b0"));
    CHECK(ie && ie->mouths.size() == 2 && ie->mouths[0].h() == m.mouthMin && ie->mouths[1].y0 == ie->mouths[0].y1 + m.row &&
          ie->pieces.back().text == "else" && ie->pieces.back().rect.y0 >= ie->mouths[0].y1);
    // Inputs sit inside their block, left to right; a reporter in one is laid out in its rect.
    CHECK(move->slots.size() == 3);
    bool slotsOk = move->slots.size() == 3;
    for (size_t i = 0; slotsOk && i < 3; i++) {
        const eb::BlockBox* lit = l.find(eb::parsePath("s0.b3").then('a', int(i)));
        slotsOk = blkInside(move->slots[i], move->rect) && lit && lit->rect == move->slots[i] && (i == 0 || move->slots[i - 1].x1 < move->slots[i].x0);
    }
    CHECK(slotsOk);
    const eb::BlockBox *no = box("s0.b2.a0"), *opened = box("s0.b2.a0.a0");
    CHECK(no && opened && !iff->slots.empty() && no->rect == iff->slots[0] && blkInside(no->rect, iff->parts[0]) &&
          blkInside(opened->rect, no->rect) && no->depth == 1 && opened->depth == 2);
    // A field's well fits its text and sits in its block.
    CHECK(hide->hasField && hide->field.w() == 4 * 7 + 2 * m.wellPad && hide->field.h() == m.slotH && blkInside(hide->field, hide->rect));
    // Parents come before what they hold.
    auto order = [&](const eb::BlockBox* b) { return b - l.blocks.data(); };
    CHECK(order(iff) < order(no) && order(no) < order(opened) && order(iff) < order(show));
    // An empty field shows what goes there.
    splash::BlockScript ph;
    ph.stacks.push_back({0, 0, {blk("play_sound")}});
    const eb::Layout pl = eb::layout(ph, blkWidth, m);
    CHECK(pl.blocks.size() == 1 && pl.blocks[0].pieces.size() == 2 && pl.blocks[0].pieces[1].placeholder &&
          pl.blocks[0].pieces[1].text == "sound");
    // turn light [light] on; remember [0] as [name].
    splash::BlockScript lp;
    lp.stacks.push_back({0, 0, {blk("light_on"), eb::newBlock("remember")}});
    const eb::Layout ll = eb::layout(lp, blkWidth, m);
    auto fieldText = [&](const char* path) {
        if (const eb::BlockBox* bb = ll.find(eb::parsePath(path)))
            for (const eb::Piece& p : bb->pieces)
                if (p.kind == eb::Piece::Field) return (p.placeholder ? "?" : "") + p.text;
        return std::string("<none>");
    };
    CHECK(fieldText("s0.b0") == "?light" && fieldText("s0.b1") == "?name");
    // Paths, as control names spell them.
    CHECK(eb::parsePath("s0.b2.d0.b1").str() == "s0.b2.d0.b1" && eb::parsePath("s12.b3.a1").stack == 12);
    CHECK(eb::parsePath("s0.x1").empty() && eb::parsePath("b1").empty() && eb::parsePath("s0.b").empty());
    CHECK(eb::parsePath("s0.b2.d0.b1").parent().str() == "s0.b2" && eb::parsePath("s0.b1.a0").parent().str() == "s0.b1" &&
          eb::parsePath("s0.b1").parent().empty());
}

void testBlocksHitTest() {
    namespace eb = editor::blk;
    splash::BlockScript s = blkDoor();
    s.stacks.push_back({600, 40, {eb::newBlock("if")}});
    const eb::Layout l = eb::layout(s, blkWidth);
    auto box = [&](const char* p) { return l.find(eb::parsePath(p)); };
    auto at = [&](float x, float y) {
        const eb::Hit h = eb::hitTest(l, x, y);
        return std::to_string(int(h.kind)) + ":" + (h.path.empty() ? "-" : h.path.str()) + ":" + std::to_string(h.slot);
    };
    const eb::BlockBox *hide = box("s0.b1"), *iff = box("s0.b2"), *move = box("s0.b3"), *show = box("s0.b2.d0.b0"),
                        *opened = box("s0.b2.a0.a0"), *no = box("s0.b2.a0"), *empty = box("s2.b0");
    CHECK(hide && iff && move && show && opened && no && empty);
    if (!hide || !iff || !move || !show || !opened || !no || !empty) return;
    const float hy = (hide->rect.y0 + hide->rect.y1) * 0.5f;
    CHECK(at(hide->rect.x0 + 3, hy) == "1:s0.b1:-1");                            // the block
    CHECK(at(hide->field.x0 + 2, hide->field.y0 + 2) == "3:s0.b1:-1");          // its field
    CHECK(at(move->slots[1].x0 + 2, move->slots[1].y0 + 2) == "3:s0.b3.a1:-1"); // a literal in an input: its field
    CHECK(at(opened->field.x0 + 1, opened->field.y0 + 1) == "3:s0.b2.a0.a0:-1");  // innermost: the variable's field
    CHECK(at(no->rect.x0 + 3, (no->rect.y0 + no->rect.y1) * 0.5f) == "1:s0.b2.a0:-1");
    CHECK(at(show->rect.x0 + 2, show->rect.y0 + 2) == "1:s0.b2.d0.b0:-1");      // a body block, not its C block
    CHECK(at(iff->rect.x0 + 2, show->rect.y0 + 2) == "1:s0.b2:-1");             // the C block's arm
    CHECK(show->rect.x1 + 2 < iff->mouths[0].x1);
    CHECK(at(show->rect.x1 + 1, show->rect.y0 + 2) == "0:-:-1");                // the mouth past the body: nothing
    CHECK(at(empty->slots[0].x0 + 2, empty->slots[0].y0 + 2) == "2:s2.b0:0");   // an empty input
    CHECK(at(5, 5) == "0:-:-1");
}

void testBlocksEdits() {
    namespace eb = editor::blk;
    using T = eb::Target;
    const splash::BlockScript s = blkDoor();
    auto P = [](const char* p) { return eb::parsePath(p); };
    auto frag = [](std::vector<splash::Block> bs) { return splash::BlockStack{0, 0, std::move(bs)}; };
    // Take: the block and every block under it.
    std::optional<eb::Taken> t = eb::takeStack(s, P("s0.b2"));
    CHECK(t && t->fragment.blocks.size() == 2 && t->fragment.blocks[0].op == "if" && t->fragment.blocks[1].op == "move_by" &&
          t->rest.stacks.size() == 2 && t->rest.stacks[0].blocks.size() == 2 && t->rest.stacks[0].blocks[1].op == "hide");
    t = eb::takeStack(s, P("s0.b2.d0.b1"));
    CHECK(t && t->fragment.blocks.size() == 1 && t->fragment.blocks[0].op == "stop" && t->rest.stacks[0].blocks[2].bodies[0].size() == 1);
    // The first block takes its stack; the stacks after move up.
    t = eb::takeStack(s, P("s0.b0"));
    CHECK(t && t->fragment.blocks.size() == 4 && t->rest.stacks.size() == 1 && t->rest.stacks[0].blocks[0].op == "log");
    // A reporter out of an input leaves the input empty.
    t = eb::takeStack(s, P("s0.b2.a0"));
    CHECK(t && t->fragment.blocks.size() == 1 && t->fragment.blocks[0].op == "not" && t->rest.stacks[0].blocks[2].args.size() == 1 &&
          t->rest.stacks[0].blocks[2].args[0] == splash::Block{});
    CHECK(!eb::takeStack(s, P("s0.b9")) && !eb::takeStack(s, P("s5.b0")) && !eb::takeStack(s, P("s0.b2.d0")));
    std::optional<splash::BlockScript> d = eb::deleteFragment(s, P("s0.b1"));
    CHECK(d && d->stacks[0].blocks.size() == 1 && d->stacks[0].blocks[0].op == "on_interact");

    // Drops.
    const splash::BlockStack hide = frag({blk("hide", "Crate")});
    eb::Path placed;
    d = eb::dropStack(s, hide, {T::After, P("s0.b0")}, &placed);
    CHECK(d && d->stacks[0].blocks.size() == 5 && d->stacks[0].blocks[1].field == "Crate" && d->stacks[0].blocks[2].field == "Door" &&
          placed.str() == "s0.b1");
    d = eb::dropStack(s, hide, {T::BodyTop, P("s0.b2"), 0}, &placed);
    CHECK(d && d->stacks[0].blocks[2].bodies[0].size() == 3 && d->stacks[0].blocks[2].bodies[0][0].field == "Crate" &&
          placed.str() == "s0.b2.d0.b0");
    d = eb::dropStack(s, hide, {T::Canvas, {}, 0, 500, 420}, &placed);
    CHECK(d && d->stacks.size() == 3 && d->stacks[2].x == 500 && d->stacks[2].y == 420 && placed.str() == "s2.b0");
    d = eb::dropStack(s, hide, {T::StackTop, P("s1"), 0, 300, 18}, &placed);
    CHECK(d && d->stacks[1].blocks.size() == 2 && d->stacks[1].blocks[0].op == "hide" && d->stacks[1].y == 18 && placed.str() == "s1.b0");
    // A hat over a loose stack makes it run.
    d = eb::dropStack(s, frag({blk("on_create")}), {T::StackTop, P("s1"), 0, 300, 6});
    CHECK(d && d->stacks[1].blocks[0].op == "on_create" && splash::blocksToLua(*d).lua.find("Debug.Log(\"hi\")") != std::string::npos);
    // A reporter into an input: a literal there gives way, any other reporter pops out.
    d = eb::dropStack(s, frag({blk("add", "", {bnum("1"), bnum("2")})}), {T::Slot, P("s0.b3"), 0, 10, 10}, &placed);
    CHECK(d && d->stacks.size() == 2 && d->stacks[0].blocks[3].args[0].op == "add" && placed.str() == "s0.b3.a0");
    d = eb::dropReporter(s, blk("eq", "", {bnum("1"), bnum("1")}), P("s0.b2"), 0, 77, 88);
    CHECK(d && d->stacks.size() == 3 && d->stacks[0].blocks[2].args[0].op == "eq" && d->stacks[2].blocks.size() == 1 &&
          d->stacks[2].blocks[0].op == "not" && d->stacks[2].blocks[0].args[0].field == "opened" && d->stacks[2].x == 77 &&
          d->stacks[2].y == 88);
    // An empty input is filled.
    t = eb::takeStack(s, P("s0.b2.a0"));
    d = t ? eb::dropStack(t->rest, t->fragment, {T::Slot, P("s0.b2"), 0, 0, 0}) : std::nullopt;
    CHECK(d && *d == s);

    // The kind rules.
    const splash::BlockStack hat = frag({blk("on_create")}), rep = frag({bvar("opened")}), boolean = frag({blk("bool", "true")});
    const splash::BlockStack stop = frag({blk("stop")});
    CHECK(eb::canDrop(s, hat, {T::Canvas, {}, 0, 0, 0}));
    CHECK(!eb::canDrop(s, hat, {T::After, P("s0.b1")}) && !eb::canDrop(s, hat, {T::BodyTop, P("s0.b2"), 0}));
    CHECK(!eb::canDrop(s, hat, {T::StackTop, P("s0")}));  // that stack has a hat
    CHECK(eb::canDrop(s, hat, {T::StackTop, P("s1")}));
    CHECK(!eb::dropStack(s, hat, {T::After, P("s0.b0")}));
    CHECK(!eb::canDrop(s, hide, {T::Slot, P("s0.b3"), 0}));                                  // a statement is no input
    CHECK(!eb::canDrop(s, rep, {T::After, P("s0.b1")}) && !eb::canDrop(s, rep, {T::BodyTop, P("s0.b2"), 0}));
    CHECK(!eb::canDrop(s, rep, {T::StackTop, P("s1")}) && eb::canDrop(s, rep, {T::Canvas, {}, 0, 0, 0}));
    // A condition: a Boolean, or a variable (it may hold true or false), not any other reporter.
    CHECK(!eb::canDrop(s, frag({blk("add", "", {bnum("1"), bnum("2")})}), {T::Slot, P("s0.b2"), 0}) &&
          eb::canDrop(s, boolean, {T::Slot, P("s0.b2"), 0}) && eb::canDrop(s, rep, {T::Slot, P("s0.b2"), 0}));
    CHECK(!eb::canDrop(s, frag({blk("number", "1")}), {T::Slot, P("s0.b2"), 0}));
    CHECK(eb::canDrop(s, rep, {T::Slot, P("s0.b3"), 2}) && !eb::canDrop(s, rep, {T::Slot, P("s0.b3"), 3}));
    CHECK(!eb::canDrop(s, hide, {T::After, P("s0.b2.d0.b1")}));                              // nothing under a stop
    CHECK(!eb::canDrop(s, stop, {T::After, P("s0.b1")}) && eb::canDrop(s, stop, {T::After, P("s0.b3")}));
    CHECK(!eb::canDrop(s, stop, {T::BodyTop, P("s0.b2"), 0}) && !eb::canDrop(s, stop, {T::StackTop, P("s1")}));
    CHECK(!eb::canDrop(s, hide, {T::After, P("s0.b3.a0")}));                                 // in an input, not a list
    CHECK(!eb::canDrop(s, hide, {T::BodyTop, P("s0.b1"), 0}) && !eb::canDrop(s, hide, {T::BodyTop, P("s0.b2"), 1}));
    splash::BlockScript loose = s;
    loose.stacks.push_back({0, 400, {bnum("5")}});
    CHECK(!eb::canDrop(loose, hide, {T::After, P("s2.b0")}) && !eb::canDrop(loose, hide, {T::StackTop, P("s2")}));
    CHECK(!eb::canDrop(s, frag({}), {T::Canvas, {}, 0, 0, 0}) && !eb::canDrop(s, hide, T{}));

    // Fields.
    d = eb::setField(s, P("s0.b1"), "Crate");
    CHECK(d && d->stacks[0].blocks[1].field == "Crate");
    d = eb::setField(s, P("s0.b3.a2"), "9");
    CHECK(d && d->stacks[0].blocks[3].args[2].field == "9");
    CHECK(!eb::setField(s, P("s0.b2"), "x") && !eb::setField(s, P("s0.b7"), "x"));
}

void testBlocksSnap() {
    namespace eb = editor::blk;
    using T = eb::Target;
    const eb::Metrics m;
    const splash::BlockScript s = blkDoor();
    const eb::Layout l = eb::layout(s, blkWidth, m);
    const eb::BlockBox *hat = l.find(eb::parsePath("s0.b0")), *iff = l.find(eb::parsePath("s0.b2")),
                        *move = l.find(eb::parsePath("s0.b3"));
    CHECK(hat && iff && move);
    if (!hat || !iff || !move) return;
    const splash::BlockStack hide{0, 0, {blk("hide", "Crate")}};
    auto near = [&](const splash::BlockStack& f, float x, float y) { return eb::findTarget(s, l, f, x, y, blkWidth, m); };
    // Under the hat: near its bottom edge.
    T t = near(hide, hat->rect.x0 + 6, hat->rect.y1 + 5);
    CHECK(t.kind == T::After && t.path.str() == "s0.b0");
    // Into the mouth, past the end of the stack, and too far from anything.
    t = near(hide, iff->mouths[0].x0 + 3, iff->mouths[0].y0 + 2);
    CHECK(t.kind == T::BodyTop && t.path.str() == "s0.b2" && t.index == 0);
    t = near(hide, move->rect.x0 + 2, move->rect.y1 + 3);
    CHECK(t.kind == T::After && t.path.str() == "s0.b3");
    CHECK(near(hide, 900, 900).kind == T::None);
    // Over the loose stack: the fragment's bottom at its top; the stack moves up to make room.
    const float fh = eb::stackSize(hide, blkWidth, m).second;
    t = near(hide, 302, 50 - fh - 4);
    CHECK(t.kind == T::StackTop && t.path.str() == "s1" && t.x == 300 && t.y == 50 - fh);
    // A hat snaps only over a loose stack.
    const splash::BlockStack hat2{0, 0, {blk("on_create")}};
    CHECK(near(hat2, hat->rect.x0, hat->rect.y1).kind == T::None);
    const float hh = eb::stackSize(hat2, blkWidth, m).second;
    t = near(hat2, 300, 50 - hh);
    CHECK(t.kind == T::StackTop && t.path.str() == "s1");
    // A reporter snaps to the input its left edge is at; a condition only takes a Boolean or a variable.
    const splash::BlockStack var{0, 0, {bvar("opened")}}, cond{0, 0, {blk("bool", "true")}};
    const eb::Rect& y = move->slots[1];
    const float vh = eb::stackSize(var, blkWidth, m).second;
    t = near(var, y.x0 + 2, (y.y0 + y.y1) * 0.5f - vh * 0.5f);
    CHECK(t.kind == T::Slot && t.path.str() == "s0.b3" && t.index == 1);
    const eb::Rect& c = iff->slots[0];
    t = near(cond, c.x0, c.y0);
    CHECK(t.kind == T::Slot && t.path.str() == "s0.b2" && t.index == 0);
    // A variable may hold true or false: it snaps there too; a sum does not.
    t = near(var, c.x0, c.y0);
    CHECK(t.kind == T::Slot && t.path.str() == "s0.b2" && t.index == 0);
    const splash::BlockStack sum{0, 0, {blk("add", "", {bnum("1"), bnum("2")})}};
    CHECK(near(sum, c.x0, c.y0).kind != T::Slot || near(sum, c.x0, c.y0).path.str() != "s0.b2");
    // The snap is what dropStack takes.
    std::optional<splash::BlockScript> d = eb::dropStack(s, hide, near(hide, hat->rect.x0 + 6, hat->rect.y1 + 5));
    CHECK(d && d->stacks[0].blocks[1].field == "Crate");
}

void testBlocksHistory() {
    namespace eb = editor::blk;
    const splash::BlockScript a = blkDoor();
    const splash::BlockScript b = *eb::setField(a, eb::parsePath("s0.b1"), "Crate");
    const splash::BlockScript c = *eb::deleteFragment(b, eb::parsePath("s1.b0"));
    eb::History h(a);
    CHECK(!h.canUndo() && !h.canRedo() && h.current() == a);
    CHECK(!h.commit(a));  // nothing changed: no step
    CHECK(h.commit(b) && h.commit(c) && h.current() == c && h.canUndo());
    CHECK(h.undo() && h.current() == b && h.canRedo());
    CHECK(h.undo() && h.current() == a && !h.undo() && h.current() == a);
    CHECK(h.redo() && h.current() == b && h.redo() && h.current() == c && !h.redo());
    CHECK(h.undo() && h.commit(a) && !h.canRedo() && h.current() == a);  // a new edit drops the redo
    CHECK(h.undo() && h.current() == b);
    h.reset(c);
    CHECK(h.current() == c && !h.canUndo() && !h.canRedo());
}

void testBlocksVariables() {
    namespace eb = editor::blk;
    // opened in a field of the if's not (an input), of a set_var in a body,
    // of a change_var in a loose stack; "count" in nothing; a text "opened" is no use.
    splash::BlockScript s = blockScript(
        {{"opened", "false"}, {"count", "0"}},
        {{blk("on_interact"), blk("if", "", {blk("not", "", {bvar("opened")})}, {{blk("set_var", "opened", {blk("bool", "true")})}}),
          blk("log", "", {blk("text", "opened")})}});
    s.stacks.push_back({200, 10, {blk("change_var", "opened", {bnum("1")})}});
    CHECK(eb::variableUses(s, "opened") == 3 && eb::variableUses(s, "count") == 0 && eb::variableUses(s, "nope") == 0);
    CHECK(eb::usedBy(1) == "used by 1 block" && eb::usedBy(3) == "used by 3 blocks");

    // Add: the first free my_variable name, value 0.
    std::string name;
    splash::BlockScript a = eb::addVariable(s, &name);
    CHECK(name == "my_variable" && a.variables.size() == 3 && a.variables[2] == (splash::BlockVariable{"my_variable", "0"}) &&
          a.stacks == s.stacks);
    a = eb::addVariable(a, &name);
    CHECK(name == "my_variable2" && a.variables.size() == 4 && splash::blockNameError(name).empty());
    a = eb::addVariable(*eb::renameVariable(a, "my_variable", "x"), &name);
    CHECK(name == "my_variable" && a.variables.size() == 5);

    // Rename: every field naming it follows; the text literal does not.
    std::string why;
    std::optional<splash::BlockScript> r = eb::renameVariable(s, "opened", "is_open", &why);
    CHECK(r && r->variables[0].name == "is_open" && eb::variableUses(*r, "is_open") == 3 && eb::variableUses(*r, "opened") == 0);
    CHECK(r && r->stacks[0].blocks[1].args[0].args[0].field == "is_open" && r->stacks[0].blocks[1].bodies[0][0].field == "is_open" &&
          r->stacks[1].blocks[0].field == "is_open" && r->stacks[0].blocks[2].args[0].field == "opened");
    CHECK(r && splash::blocksToLua(*r).errors.empty());
    // Refused: not a name, reserved, an event function, taken, no such variable.
    CHECK(!eb::renameVariable(s, "opened", "1bad", &why) && why.find("not a name") != std::string::npos);
    CHECK(!eb::renameVariable(s, "opened", "end", &why) && why == "\"end\" is reserved");
    CHECK(!eb::renameVariable(s, "opened", "onInteract", &why) && why == "\"onInteract\" is reserved");
    CHECK(!eb::renameVariable(s, "opened", "", &why) && why == "no name");
    CHECK(!eb::renameVariable(s, "opened", "count", &why) && why.find("already") != std::string::npos);
    CHECK(!eb::renameVariable(s, "nope", "fine", &why) && why.find("no variable") != std::string::npos);
    CHECK(eb::renameVariable(s, "opened", "opened") == s);

    // Value.
    r = eb::setVariableValue(s, 1, "5");
    CHECK(r && r->variables[1].value == "5" && r->variables[0] == s.variables[0] && r->stacks == s.stacks);
    CHECK(!eb::setVariableValue(s, 2, "5") && !eb::setVariableValue(s, -1, "5"));

    // Delete: refused while a block names it.
    CHECK(!eb::deleteVariable(s, 0, &why) && why == "used by 3 blocks");
    r = eb::deleteVariable(s, 1, &why);
    CHECK(r && r->variables.size() == 1 && r->variables[0].name == "opened" && r->stacks == s.stacks);
    CHECK(!eb::deleteVariable(s, 2, &why) && !eb::deleteVariable(s, -1, &why));
    // Once its blocks are gone, it goes.
    splash::BlockScript bare = s;
    bare.stacks.clear();
    r = eb::deleteVariable(bare, 0, &why);
    CHECK(r && r->variables.size() == 1 && r->variables[0].name == "count");

    // The codegen's own check is the one exposed.
    CHECK(splash::blockNameError("ok_name2").empty() && !splash::blockNameError("self").empty() &&
          !splash::blockNameError("a-b").empty());
}

void testBlocksNewBlock() {
    namespace eb = editor::blk;
    auto args = [](const splash::Block& b) {
        std::string s;
        for (const splash::Block& a : b.args) s += (s.empty() ? "" : ",") + (a.op.empty() ? std::string("_") : a.op + "=" + a.field);
        return s;
    };
    CHECK(args(eb::newBlock("move_by")) == "number=0,number=0,number=0" && eb::newBlock("move_by").field == "self");
    CHECK(args(eb::newBlock("go_to")) == "number=0,number=0,number=0");
    CHECK(args(eb::newBlock("set_var")) == "number=0" && eb::newBlock("set_var").field.empty());
    CHECK(args(eb::newBlock("change_var")) == "number=1");
    CHECK(args(eb::newBlock("repeat")) == "number=10" && eb::newBlock("repeat").bodies.size() == 1);
    CHECK(args(eb::newBlock("wait")) == "number=1");
    CHECK(eb::newBlock("wait_until").args.size() == 1 && eb::newBlock("wait_until").args[0].op.empty());
    CHECK(args(eb::newBlock("random")) == "number=1,number=10" && args(eb::newBlock("add")) == "number=0,number=0");
    CHECK(args(eb::newBlock("log")) == "text=" && args(eb::newBlock("join")) == "text=,text=" && args(eb::newBlock("load_scene")) == "text=");
    CHECK(args(eb::newBlock("if")) == "_" && eb::newBlock("if").bodies.size() == 1 && eb::newBlock("if_else").bodies.size() == 2);
    CHECK(args(eb::newBlock("and")) == "_,_" && args(eb::newBlock("not")) == "_" && args(eb::newBlock("while")) == "_");
    CHECK(args(eb::newBlock("eq")) == "number=0,number=0");
    CHECK(eb::newBlock("on_button_press").field == "CROSS" && eb::newBlock("bool").field == "true" && eb::newBlock("number").field == "0");
    CHECK(eb::newBlock("hide").args.empty() && eb::newBlock("hide").bodies.empty() && eb::newBlock("fly").op == "fly");
    CHECK(eb::newBlock("light_on").field.empty() && eb::newBlock("light_off").args.empty());
    CHECK(args(eb::newBlock("remember")) == "number=0" && eb::newBlock("remember").field.empty() && eb::newBlock("remembered").args.empty());
    // Every block fits the catalogue, and the statements that need no choice make Lua as they come.
    bool shapes = true;
    for (const splash::BlockSpec& sp : splash::blockCatalogue()) {
        const splash::Block b = eb::newBlock(sp.op);
        shapes = shapes && b.args.size() == size_t(sp.args) && b.bodies.size() == size_t(sp.bodies);
    }
    CHECK(shapes);
    const splash::LuaGen g = splash::blocksToLua(blockScript({}, {{eb::newBlock("on_create"), eb::newBlock("move_by"),
                                                                   eb::newBlock("repeat"), eb::newBlock("log")}}));
    CHECK(g.errors.empty() && g.lua.find("Debug.Log(\"\")") != std::string::npos);
    // The palette's categories, every block in one of them.
    CHECK(splash::blockCategories().size() == 8 && splash::blockCategories().front() == "Events" &&
          splash::blockCategories()[3] == "Sound and scenes" && splash::blockCategories().back() == "Debug");
    bool cats = true;
    for (const splash::BlockSpec& sp : splash::blockCatalogue())
        cats = cats && std::find(splash::blockCategories().begin(), splash::blockCategories().end(), sp.category) != splash::blockCategories().end() &&
               ((sp.kind == splash::BlockKind::Hat) == (sp.category == "Events"));
    CHECK(cats && splash::findBlockSpec("hide")->category == "Objects" && splash::findBlockSpec("join")->category == "Operators");
}

void testSeqRenameBlocks() {
    using editor::SeqKind;
    const splash::BlockScript s = blockScript(
        {}, {{blk("on_interact"), blk("play_cutscene", "intro"), blk("if", "", {blk("bool", "true")}, {{blk("play_cutscene", "intro")}}),
              blk("play_cutscene", "outro"), blk("log", "", {blk("text", "intro")})}});
    const std::string text = splash::saveBlocks(s);
    int n = -1;
    const std::string out = editor::renameSeqInBlocks(text, SeqKind::Cutscene, "intro", "opening", &n);
    splash::BlockScript want = s;
    want.stacks[0].blocks[1].field = "opening";
    want.stacks[0].blocks[2].bodies[0][0].field = "opening";
    CHECK(n == 2 && out == splash::saveBlocks(want));  // the text block that says "intro" is no cutscene
    // Animations have no block; nothing named: the text as it was, byte for byte.
    CHECK(editor::renameSeqInBlocks(text, SeqKind::Animation, "intro", "opening", &n) == text && n == 0);
    const std::string compact = "{\"format\":\"splashedit-blocks\",\"version\":1,\"stacks\":[{\"blocks\":[{\"op\":\"on_create\"}]}]}";
    CHECK(editor::renameSeqInBlocks(compact, SeqKind::Cutscene, "intro", "opening", &n) == compact && n == 0);
    // Not a block script: left alone.
    CHECK(editor::renameSeqInBlocks("{", SeqKind::Cutscene, "intro", "opening", &n) == "{" && n == 0);
    CHECK(editor::renameSeqInBlocks("Cutscene.Play(\"intro\")", SeqKind::Cutscene, "intro", "opening", &n) == "Cutscene.Play(\"intro\")" && n == 0);

    // renameSequence sends a .blocks scene script through it, on disk and in its undo.
    namespace fs = std::filesystem;
    const fs::path dir = g_outDir / "seq_rename_blocks";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scripts", ec);
    auto get = [&](const char* p) {
        std::ifstream in(dir / p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    std::ofstream(dir / "scripts/door.blocks", std::ios::binary) << text;
    std::ofstream(dir / "scripts/bad.blocks", std::ios::binary) << "{ \"intro\"";
    splash::Scene sc;
    splash::Object door, bad;
    door.name = "Door";
    door.script = splash::ScriptComponent{"scripts/door.blocks", {}};
    bad.name = "Bad";
    bad.script = splash::ScriptComponent{"scripts/bad.blocks", {}};
    sc.objects = {door, bad};
    splash::Cutscene c;
    c.name = "intro";
    sc.cutscenes.push_back(c);
    editor::Document d;
    d.reset(sc, dir, "s");
    editor::ScriptRenames sr;
    CHECK(editor::renameSequence(d, {SeqKind::Cutscene, 0}, "opening", &sr));
    CHECK(sr.refs == 2 && (sr.files == std::vector<std::string>{"scripts/door.blocks"}) && sr.leftovers.empty() && sr.failed.empty());
    CHECK(get("scripts/door.blocks") == splash::saveBlocks(want) && get("scripts/bad.blocks") == "{ \"intro\"");
    CHECK(d.undo() && get("scripts/door.blocks") == text);
    fs::remove_all(dir, ec);
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
    // psxsplash never calls onDestroy, so autocomplete does not offer it.
    CHECK(!editor::lua::findApi("", "onDestroy") && editor::lua::findApi("", "onDisable"));
    editor::NewScript t =editor::createScript(proj, "Game Logic", editor::ScriptKind::Trigger);
    CHECK(t.error.empty() && t.path == "scripts/game_logic_trigger.lua");
    std::ifstream tin(proj / "scripts" / "game_logic_trigger.lua", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(tin)), {}) == editor::scriptTemplate("Game Logic", editor::ScriptKind::Trigger));
    // An Interactable object's script: the object starter plus onInteract,
    // which psxsplash calls (L.OnInteract(go)) when the player uses it.
    const std::string inter = editor::scriptTemplate("Lever", editor::ScriptKind::Interactable);
    CHECK(inter.find("function onCreate(self)") != std::string::npos);
    CHECK(inter.find("function onInteract(self)") != std::string::npos);
    CHECK(inter.find("-- The player pressed the interact button near this object.") != std::string::npos);
    CHECK(editor::lua::check(inter).ok());
    CHECK(editor::scriptDefinesCallback(inter, false, "onInteract") && editor::scriptDefinesCallback(inter, false, "onCreate"));
    {
        const editor::lua::ApiSymbol* api = editor::lua::findApi("", "onInteract");
        CHECK(api && inter.find(api->signature) != std::string::npos);
    }
    editor::NewScript ia = editor::createScript(proj, "Lever", editor::ScriptKind::Interactable);
    CHECK(ia.error.empty() && ia.path == "scripts/lever.lua");
    std::ifstream iin(proj / "scripts" / "lever.lua", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(iin)), {}) == inter);
}

// View > Set up external editor (VS Code): the embedded psxsplash.d.lua is
// the committed one; .luarc.json is written once and then left alone; a
// stale psxsplash.d.lua is rewritten when the project opens.
void testExternalEditorSetup() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    const std::string committed = slurp(fs::path(SPLASHEDIT_SOURCE_DIR) / "src" / "editor" / "psxsplash.d.lua");
    CHECK(!committed.empty() && editor::lua::luaDefinitions() == committed);
    CHECK(editor::lua::luarcJson().find("\"runtime.version\": \"Lua 5.2\"") != std::string_view::npos);

    fs::path proj = g_outDir / "luadefs";
    fs::remove_all(proj, ec);
    fs::create_directories(proj, ec);
    CHECK(!editor::lua::refreshLuaDefinitions(proj));  // none there: nothing written
    CHECK(!fs::exists(proj / "psxsplash.d.lua"));
    editor::lua::ExternalEditorSetup r = editor::lua::setUpExternalEditor(proj);
    CHECK(r.error.empty() && r.wroteLuarc);
    CHECK(slurp(proj / "psxsplash.d.lua") == committed && slurp(proj / ".luarc.json") == editor::lua::luarcJson());
    { std::ofstream(proj / ".luarc.json", std::ios::binary) << "{\"mine\": true}"; }
    { std::ofstream(proj / "psxsplash.d.lua", std::ios::binary) << "---@meta old"; }
    r = editor::lua::setUpExternalEditor(proj);
    CHECK(r.error.empty() && !r.wroteLuarc && slurp(proj / ".luarc.json") == "{\"mine\": true}");
    CHECK(slurp(proj / "psxsplash.d.lua") == committed);
    { std::ofstream(proj / "psxsplash.d.lua", std::ios::binary) << "---@meta old"; }
    CHECK(editor::lua::refreshLuaDefinitions(proj) && slurp(proj / "psxsplash.d.lua") == committed);
    CHECK(!editor::lua::refreshLuaDefinitions(proj));  // current: left alone
}

// The Interactable section's Event row: does the script define the callback,
// and Add onInteract's edit of a Lua or block script.
void testScriptDefinesCallback() {
    using editor::scriptDefinesCallback;
    // Lua.
    CHECK(scriptDefinesCallback("function onInteract(self)\nend\n", false, "onInteract"));
    CHECK(scriptDefinesCallback("-- x\n  \tfunction onInteract(self)\nend", false, "onInteract"));
    CHECK(scriptDefinesCallback("function  onInteract (self) end", false, "onInteract"));
    CHECK(scriptDefinesCallback("function onCreate(self)\nend\r\nfunction onInteract(self)\r\nend\r\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("", false, "onInteract"));
    CHECK(!scriptDefinesCallback("function onCreate(self)\nend\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("-- function onInteract(self)\n-- end\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("  --function onInteract(self)\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("--[[\nfunction onInteract(self)\nend\n]]\n", false, "onInteract"));
    CHECK(scriptDefinesCallback("--[[ off ]]\nfunction onInteract(self)\nend\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("function onInteractX(self)\nend\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("function onInteract_2(self)\nend\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("functiononInteract(self)\n", false, "onInteract"));
    CHECK(!scriptDefinesCallback("local function_onInteract = 1\n", false, "onInteract"));
    // Blocks: a stack whose hat makes the function.
    const std::string withHat = editor::blocksTemplate(editor::ScriptKind::Interactable);
    const std::string without = editor::blocksTemplate(editor::ScriptKind::Object);
    {
        const splash::BlockScript hats = splash::parseBlocks(withHat, "lever.blocks");
        CHECK(hats.stacks.size() == 2 && hats.stacks[0].blocks[0].op == "on_create" && hats.stacks[1].blocks[0].op == "on_interact");
    }
    CHECK(scriptDefinesCallback(withHat, true, "onInteract"));
    CHECK(scriptDefinesCallback(withHat, true, "onCreate"));
    CHECK(!scriptDefinesCallback(without, true, "onInteract"));
    CHECK(!scriptDefinesCallback("{ not json", true, "onInteract"));
    CHECK(!scriptDefinesCallback("", true, "onInteract"));
    // Lua text read as blocks, and blocks JSON read as Lua: neither counts.
    CHECK(!scriptDefinesCallback("function onInteract(self)\nend\n", true, "onInteract"));
    CHECK(!scriptDefinesCallback(withHat, false, "onInteract"));
    {
        // A hat that is not the first block of its stack does not count.
        splash::BlockScript bs;
        bs.stacks.push_back({40, 40, {splash::Block{"on_create", {}, {}, {}}, splash::Block{"on_interact", {}, {}, {}}}});
        CHECK(!scriptDefinesCallback(splash::saveBlocks(bs), true, "onInteract"));
    }
    CHECK(splash::hatCallback("on_interact") == "onInteract" && splash::hatCallback("say").empty());

    // Add onInteract: Lua gets the stub once, at the end.
    const std::string obj = editor::scriptTemplate("Lever", editor::ScriptKind::Object);
    const std::string added = editor::addInteractCallback(obj, false);
    CHECK(added.starts_with(obj) && scriptDefinesCallback(added, false, "onInteract"));
    CHECK(added.find("function onInteract(self)\nend\n") == added.size() - std::string("function onInteract(self)\nend\n").size());
    CHECK(editor::lua::check(added).ok());
    CHECK(editor::addInteractCallback("x = 1", false).starts_with("x = 1\n"));
    CHECK(scriptDefinesCallback(editor::addInteractCallback("", false), false, "onInteract"));
    // Blocks: an on_interact stack below the others; the rest untouched.
    const std::string addedB = editor::addInteractCallback(without, true);
    CHECK(scriptDefinesCallback(addedB, true, "onInteract"));
    const splash::BlockScript before = splash::parseBlocks(without, "a"), after = splash::parseBlocks(addedB, "b");
    CHECK(after.stacks.size() == before.stacks.size() + 1);
    CHECK(std::equal(before.stacks.begin(), before.stacks.end(), after.stacks.begin()));
    for (const splash::BlockStack& st : before.stacks) CHECK(after.stacks.back().y >= st.y + 80);
    CHECK(splash::blocksFileToLua(addedB, "b").find("function onInteract(self)") != std::string::npos);
    CHECK(editor::addInteractCallback("{ not json", true).empty());
}

// New blocks: a .blocks starter per kind that makes Lua with the same
// callbacks as the Lua starter, and a stem taken by either extension.
void testNewBlocksScript() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    fs::path proj = g_outDir / "newblocks";
    fs::remove_all(proj, ec);
    fs::create_directories(proj, ec);
    struct Want {
        editor::ScriptKind kind;
        const char* path;
        std::vector<std::string> functions;
    };
    for (const Want& w : {Want{editor::ScriptKind::Object, "scripts/game_logic.blocks", {"function onCreate(self)"}},
                          Want{editor::ScriptKind::Scene, "scripts/game_logic.blocks", {"function onSceneCreationEnd()"}},
                          Want{editor::ScriptKind::Trigger, "scripts/game_logic_trigger.blocks",
                               {"function onTriggerEnter(index)", "function onTriggerExit(index)"}},
                          Want{editor::ScriptKind::Interactable, "scripts/game_logic.blocks",
                               {"function onCreate(self)", "function onInteract(self)"}}}) {
        fs::remove_all(proj / "scripts", ec);
        editor::NewScript n = editor::createScript(proj, "Game Logic", w.kind, editor::ScriptLanguage::Blocks);
        CHECK(n.error.empty() && n.path == w.path);
        const std::string text = read(proj / fs::path(n.path));
        CHECK(text == editor::blocksTemplate(w.kind));
        std::string lua, err;
        try {
            lua = splash::blocksFileToLua(text, n.path);
        } catch (const std::exception& e) {
            err = e.what();
        }
        CHECK(err.empty());
        for (const std::string& f : w.functions) CHECK(lua.find(f) != std::string::npos);
        const splash::BlockScript bs = splash::parseBlocks(text, n.path);
        CHECK(bs.stacks.size() == w.functions.size() && bs.variables.empty());
        // One hat per stack, and the stacks apart.
        for (size_t i = 0; i < bs.stacks.size(); i++) {
            CHECK(bs.stacks[i].blocks.size() == 1);
            for (size_t j = 0; j < i; j++) CHECK(std::abs(bs.stacks[i].y - bs.stacks[j].y) >= 80);
        }
    }
    // A stem is taken by a file of either language.
    fs::remove_all(proj / "scripts", ec);
    fs::create_directories(proj / "scripts", ec);
    std::ofstream(proj / "scripts" / "door.lua", std::ios::binary) << "mine";
    CHECK(editor::createScript(proj, "Door", editor::ScriptKind::Object, editor::ScriptLanguage::Blocks).path == "scripts/door_2.blocks");
    CHECK(!fs::exists(proj / "scripts" / "door.blocks"));
    CHECK(editor::createScript(proj, "Door", editor::ScriptKind::Object, editor::ScriptLanguage::Lua).path == "scripts/door_3.lua");
    std::ofstream(proj / "scripts" / "gate.blocks", std::ios::binary) << "mine";
    CHECK(editor::createScript(proj, "Gate", editor::ScriptKind::Object).path == "scripts/gate_2.lua");
    CHECK(read(proj / "scripts" / "door.lua") == "mine" && read(proj / "scripts" / "gate.blocks") == "mine");
    // The asset browser's: any folder, the project's top included.
    editor::NewScript top = editor::writeNewScript(proj, "", "new_script", editor::ScriptLanguage::Blocks, "x");
    CHECK(top.error.empty() && top.path == "new_script.blocks" && read(proj / "new_script.blocks") == "x");
    CHECK(editor::writeNewScript(proj, "", "new_script", editor::ScriptLanguage::Lua, "y").path == "new_script_2.lua");
    editor::NewScript deep = editor::writeNewScript(proj, "scripts/ai", "new_script", editor::ScriptLanguage::Lua, "z");
    CHECK(deep.error.empty() && deep.path == "scripts/ai/new_script.lua" && read(proj / "scripts" / "ai" / "new_script.lua") == "z");
    CHECK(!editor::writeNewScript({}, "", "x", editor::ScriptLanguage::Blocks, "").error.empty());
    fs::remove_all(proj, ec);
}

// Every block says what it does: one plain sentence.
void testBlockHelp() {
    for (const splash::BlockSpec& s : splash::blockCatalogue()) {
        const std::string h(s.help);
        CHECK(!h.empty() && h.back() == '.' && h.find("%f") == std::string::npos);
        if (h.empty() || h.back() != '.') std::printf("  block %s: help \"%s\"\n", std::string(s.op).c_str(), h.c_str());
    }
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

// The loop an imported clip starts with comes from its name: a word starting
// with idle, walk, run, ... loops; anything else plays once.
void testDefaultClipLoop() {
    for (const char* n : {"Walk", "idle", "IDLE_2", "Run", "Running", "jog", "Loop", "walk_cycle", "WalkCycle", "Patrol", "Swim",
                          "Flying", "Breathe", "Hover", "Armature|Idle", "mixamo.com|Walk", "attackIdle", "Rune"})
        if (!splash::defaultClipLoop(n)) {
            std::fprintf(stderr, "  defaultClipLoop(%s) should loop\n", n);
            CHECK(false);
        }
    for (const char* n : {"Attack", "Death", "Jump", "Wave", "Nod_Big", "Prune", "Overrun", "Open", "", "Armature|mixamo.com|Layer0",
                          "Hit_Reaction", "Throw"})
        if (splash::defaultClipLoop(n)) {
            std::fprintf(stderr, "  defaultClipLoop(%s) should play once\n", n);
            CHECK(false);
        }
}

// tests/skinimport/rig.gltf with its clips renamed Walk and Attack: the
// import writes Walk.anim looping and Attack.anim playing once. Flipping
// Attack's loop in the editor rewrites the file as one undo step, keeping the
// rest of the clip; undo brings the file back byte for byte.
void testClipLoopImportAndEdit() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = g_outDir / "cliploop";
    fs::remove_all(dir, ec);
    fs::path proj = dir / "game";
    fs::create_directories(proj, ec);
    std::string gltf;
    {
        std::ifstream in(fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skinimport" / "rig.gltf", std::ios::binary);
        gltf.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto rename = [&](const std::string& from, const std::string& to) {
        const size_t at = gltf.find("\"name\": \"" + from + "\"");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) gltf.replace(at, from.size() + 10, "\"name\": \"" + to + "\"");
    };
    rename("Wave", "Walk");
    rename("Nod/Big", "Attack");
    std::ofstream(dir / "rig.gltf", std::ios::binary) << gltf;
    editor::Document doc;
    doc.reset(splash::Scene{}, proj, "game");
    editor::PlacedModel p = editor::placeModel(doc, dir / "rig.gltf", std::nullopt, {0}, {0, 0, 0});
    CHECK(p.error.empty() && p.model.clips.size() == 2);
    const fs::path walk = proj / "models" / "rig" / "Walk.anim", attack = proj / "models" / "rig" / "Attack.anim";
    CHECK(fs::exists(walk) && fs::exists(attack));
    if (!fs::exists(walk) || !fs::exists(attack)) return;
    CHECK(splash::loadAnim(walk).loop && !splash::loadAnim(attack).loop);

    auto bytes = [](const fs::path& f) {
        std::ifstream in(f, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string before = bytes(attack);
    const splash::AnimClip was = splash::loadAnim(attack);
    const size_t steps = doc.historySize();
    const unsigned rev = doc.revision();
    CHECK(editor::setAnimLoop(doc, "models/rig/Attack.anim", true).empty());
    const splash::AnimClip now = splash::loadAnim(attack);
    CHECK(now.loop && now.name == was.name && now.length == was.length && now.channels.size() == was.channels.size());
    bool same = now.channels.size() == was.channels.size();
    for (size_t i = 0; same && i < now.channels.size(); i++)
        same = now.channels[i].joint == was.channels[i].joint && now.channels[i].times == was.channels[i].times &&
               now.channels[i].values == was.channels[i].values;
    CHECK(same);
    CHECK(doc.historySize() == steps + 1 && doc.revision() != rev);
    // Already looping: nothing to do, no step.
    CHECK(editor::setAnimLoop(doc, "models/rig/Attack.anim", true).empty() && doc.historySize() == steps + 1);
    CHECK(doc.undo() && !splash::loadAnim(attack).loop && bytes(attack) == before);
    CHECK(doc.redo() && splash::loadAnim(attack).loop);
    CHECK(doc.undo() && bytes(attack) == before);
    CHECK(editor::takeAssetHistoryError().empty());
    // A file that is not a clip is refused, untouched, with no step.
    std::ofstream(proj / "bad.anim") << "{";
    CHECK(!editor::setAnimLoop(doc, "bad.anim", true).empty() && bytes(proj / "bad.anim") == "{" && doc.historySize() == steps + 1);
    CHECK(!editor::setAnimLoop(doc, "missing.anim", true).empty());
    // No temporary left beside the clip.
    int files = 0;
    for (const auto& e : fs::directory_iterator(proj / "models" / "rig")) files += e.path().extension() == ".anim" ? 0 : e.path().filename().string()[0] == '.';
    CHECK(files == 0);
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
    // The viewport outlines every baked region, on the courtyard's ground.
    CHECK(ok.stats.navOutlines.size() == size_t(ok.stats.navRegions));
    for (const std::vector<splash::Vec3>& poly : ok.stats.navOutlines) {
        CHECK(poly.size() >= 3);
        for (const splash::Vec3& v : poly) CHECK(v.y > -1 && v.y < 3);
    }
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
    CHECK(none.stats.navOutlines.empty());
    CHECK(std::any_of(none.warnings.begin(), none.warnings.end(),
                      [](const std::string& w) { return w == splash::navEmptyWarning(); }));
}

// ---- Rooms and portals
namespace roomtest {
namespace fs = std::filesystem;

uint16_t rd16(const std::string& b, size_t at) { return uint16_t(uint8_t(b[at]) | (uint8_t(b[at + 1]) << 8)); }
int16_t rdi16(const std::string& b, size_t at) { return int16_t(rd16(b, at)); }
int32_t rd32(const std::string& b, size_t at) {
    return int32_t(uint32_t(rd16(b, at)) | (uint32_t(rd16(b, at + 2)) << 16));
}

splash::Object meshObject(const char* name, const char* mesh, splash::Vec3 pos, bool walkable) {
    splash::Object o;
    o.name = name;
    o.transform.position = pos;
    splash::MeshComponent mc;
    mc.mesh = mesh;
    mc.materials.push_back({"", {1, 1, 1, 1}, {}});
    o.mesh = mc;
    if (walkable) o.collider = splash::ColliderComponent{splash::ColliderKind::Static, false, {}};
    return o;
}

splash::Object roomObject(const std::string& name, splash::Vec3 pos, splash::Vec3 size) {
    splash::Object o;
    o.name = name;
    o.transform.position = pos;
    o.room = splash::RoomComponent{size, {}, {}};
    return o;
}

// Two 6 x 2 x 6 rooms side by side along X (A at x -3, B at x +3) with a
// portal between them facing -X (so the exporter has to flip it), a floor
// in each room, one triangle straddling the boundary and a quad far away
// from both. A Navigation object bakes nav on the floors.
splash::Scene twoRooms(const fs::path& project) {
    fs::create_directories(project / "meshes");
    splash::Mesh quad;
    const float h = 2.25f;
    quad.positions = {{-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h}};
    quad.normals.assign(4, {0, 1, 0});
    quad.submeshes = {{0, 1, 2, 0, 2, 3}};
    splash::saveMesh(quad, project / "meshes/quad.mesh");
    splash::Mesh tri;
    tri.positions = {{-1.5f, 0.5f, 1}, {1.5f, 0.5f, 1}, {0, 0.5f, 2}};
    tri.normals.assign(3, {0, 1, 0});
    tri.submeshes = {{0, 1, 2}};
    splash::saveMesh(tri, project / "meshes/tri.mesh");

    splash::Scene s;
    s.objects.push_back(roomObject("Room A", {-3, 1, 0}, {6, 2, 6}));
    s.objects.push_back(roomObject("Room B", {3, 1, 0}, {6, 2, 6}));
    splash::Object door;
    door.name = "Door";
    door.transform.position = {0, 1, 0};
    door.transform.rotation = {0, -0.70710678f, 0, 0.70710678f};  // forward = -X
    door.portal = splash::PortalComponent{"Room A", "Room B", 2, 2, {}};
    s.objects.push_back(door);
    s.objects.push_back(meshObject("Floor A", "meshes/quad.mesh", {-3, 0, 0}, true));
    s.objects.push_back(meshObject("Floor B", "meshes/quad.mesh", {3, 0, 0}, true));
    s.objects.push_back(meshObject("Straddle", "meshes/tri.mesh", {0, 0, 0}, false));
    s.objects.push_back(meshObject("Outside", "meshes/quad.mesh", {50, 0, 50}, false));
    splash::Object nav;
    nav.name = "Nav";
    nav.transform.position = {-3, 0, 0};
    nav.navigation = splash::NavigationComponent{};
    nav.navigation->agentRadius = 0.3f;
    s.objects.push_back(nav);
    return s;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool hasError(const splash::ExportResult& r, const std::string& part) {
    return std::any_of(r.errors.begin(), r.errors.end(),
                       [&](const std::string& e) { return e.find(part) != std::string::npos; });
}

}  // namespace roomtest

// The Triangles meter: the most triangles that can be in view in one frame,
// against the psxsplash docs' 1000-2000 visible triangles at 30 fps. Without
// streaming every triangle can be in view; with it, only the non-streamed
// ones plus the streamed ones within the draw distance (streamDrawTris).
void testTriangleBudget() {
    using namespace roomtest;
    const fs::path project = g_outDir / "tris-in-view";
    fs::remove_all(project);
    splash::Scene rooms = twoRooms(project);
    splash::ExportResult r = splash::exportSplashpack(rooms, project, project / "scene.splashpack");
    CHECK(r.ok() && r.stats.triangles > 0 && r.stats.streamRegions == 0);
    CHECK(r.stats.trianglesInView == uint64_t(r.stats.triangles));
    CHECK(splash::triangleBudget(r.stats).used == size_t(r.stats.triangles));
    CHECK(splash::triangleBudget(r.stats).capacity == splash::kVisibleTriangleGuide);

    // 40 floor quads 50 m apart along X, far past the draw distance, and one
    // that cannot stream (a dynamic collider keeps it loaded).
    splash::Scene world;
    for (int i = 0; i < 40; i++)
        world.objects.push_back(meshObject(("Tile " + std::to_string(i)).c_str(), "meshes/quad.mesh",
                                           {float(50 * i), 0, 0}, false));
    splash::Object kept = meshObject("Kept", "meshes/quad.mesh", {0, 0, 10}, false);
    kept.collider = splash::ColliderComponent{splash::ColliderKind::Dynamic, false, {}};
    world.objects.push_back(kept);
    world.settings.streamWorld = true;
    splash::ExportResult w = splash::exportSplashpack(world, project, project / "world.splashpack");
    CHECK(w.ok());
    const splash::ExportStats& s = w.stats;
    CHECK(s.triangles == 41 * 2 && s.streamRegions > 1 && s.streamedObjects == 40);
    CHECK(s.streamDrawTris > 0 && s.streamDrawTris < 80);
    CHECK(s.trianglesInView == 2 + s.streamDrawTris);
    CHECK(s.trianglesInView < uint64_t(s.triangles));

    // The bands the meter colours by: under 75% good, under 90% warn.
    splash::ExportStats b;
    b.trianglesInView = 1499;
    CHECK(std::string(splash::budgetBand(splash::triangleBudget(b).fraction())) == "good");
    b.trianglesInView = 1500;
    CHECK(std::string(splash::budgetBand(splash::triangleBudget(b).fraction())) == "warn");
    b.trianglesInView = 1799;
    CHECK(std::string(splash::budgetBand(splash::triangleBudget(b).fraction())) == "warn");
    b.trianglesInView = 1800;
    CHECK(std::string(splash::budgetBand(splash::triangleBudget(b).fraction())) == "bad");
}

// Exports the two-room scene and reads the room block back the way psxsplash
// does. The block is found through ExportStats::roomBlockOffset; the nav
// block right before it is found by its size from the header counts, which
// also cross-checks that offset.
void testRoomsExport() {
    using namespace roomtest;
    const fs::path project = g_outDir / "rooms";
    fs::remove_all(project);
    splash::Scene scene = twoRooms(project);
    splash::ExportResult r = splash::exportSplashpack(scene, project, project / "scene.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "rooms export: %s\n", e.c_str());
    CHECK(r.ok());
    if (!r.ok()) return;
    const std::string b = readAll(project / "scene.splashpack");
    const float gte = scene.settings.gteScaling;

    const size_t roomCount = rd16(b, 78), portalCount = rd16(b, 80), triRefCount = rd16(b, 82),
                 cellCount = rd16(b, 86), portalRefCount = rd16(b, 106);
    CHECK(roomCount == 3 && portalCount == 1);
    CHECK(r.stats.rooms.size() == 2 && r.stats.roomPortals == 1);
    const size_t off = r.stats.roomBlockOffset;
    CHECK(off > 0 && off % 4 == 0);
    const size_t portalsAt = off + 36 * roomCount, refsAt = portalsAt + 40 * portalCount,
                 cellsAt = refsAt + 4 * triRefCount, prefsAt = cellsAt + 28 * cellCount,
                 end = prefsAt + 4 * portalRefCount;
    CHECK(end <= b.size());
    if (end > b.size()) return;

    struct Room {
        int32_t mn[3], mx[3];
        size_t firstRef, refCount, firstCell, cellCount, portalRefCount, firstPortalRef;
    };
    std::vector<Room> rooms(roomCount);
    size_t sumRefs = 0, sumCells = 0, sumPrefs = 0;
    for (size_t i = 0; i < roomCount; i++) {
        const size_t at = off + 36 * i;
        Room& rm = rooms[i];
        for (int k = 0; k < 3; k++) {
            rm.mn[k] = rd32(b, at + 4 * size_t(k));
            rm.mx[k] = rd32(b, at + 12 + 4 * size_t(k));
        }
        rm.firstRef = rd16(b, at + 24);
        rm.refCount = rd16(b, at + 26);
        rm.firstCell = rd16(b, at + 28);
        rm.cellCount = uint8_t(b[at + 30]);
        rm.portalRefCount = uint8_t(b[at + 31]);
        rm.firstPortalRef = rd16(b, at + 32);
        CHECK(rd16(b, at + 34) == 0);
        CHECK(rm.firstRef == sumRefs && rm.firstCell == sumCells && rm.firstPortalRef == sumPrefs);
        sumRefs += rm.refCount;
        sumCells += rm.cellCount;
        sumPrefs += rm.portalRefCount;
        CHECK(rm.mn[0] <= rm.mx[0] && rm.mn[1] <= rm.mx[1] && rm.mn[2] <= rm.mx[2]);
    }
    // Header counts are the record counts.
    CHECK(sumRefs == triRefCount && sumCells == cellCount && sumPrefs == portalRefCount);

    // Pack objects (92 bytes: offset, position, 3x3 i32 rotation, polyCount):
    // identify them by their stored position.
    const size_t objCount = rd16(b, 6);
    const size_t luaCount = rd16(b, 4);
    auto objX = [&](size_t i) { return rd32(b, 156 + 8 * luaCount + 92 * i + 4); };
    auto objZ = [&](size_t i) { return rd32(b, 156 + 8 * luaCount + 92 * i + 12); };
    auto polyCount = [&](size_t i) { return size_t(rd16(b, 156 + 8 * luaCount + 92 * i + 52)); };
    int straddle = -1, outside = -1;
    for (size_t i = 0; i < objCount; i++) {
        if (objX(i) == 0 && objZ(i) == 0) straddle = int(i);
        if (objX(i) == splash::toWorldFixed12(50 / gte) && objZ(i) == splash::toWorldFixed12(50 / gte)) outside = int(i);
    }
    CHECK(objCount == 4 && straddle >= 0 && outside >= 0);

    std::vector<std::set<std::pair<int, int>>> inRoom(roomCount);
    for (size_t i = 0; i < roomCount; i++)
        for (size_t k = 0; k < rooms[i].refCount; k++) {
            const size_t at = refsAt + 4 * (rooms[i].firstRef + k);
            inRoom[i].insert({rd16(b, at), rd16(b, at + 2)});
            CHECK(rd16(b, at) < objCount && rd16(b, at + 2) < polyCount(rd16(b, at)));
        }

    // The catch-all is last: huge box, no portals, exactly the outside quad.
    const Room& ca = rooms.back();
    CHECK(ca.mn[0] == splash::toWorldFixed12(-1000 / gte) && ca.mx[2] == splash::toWorldFixed12(1000 / gte));
    CHECK(ca.portalRefCount == 0);
    CHECK(inRoom.back().size() == polyCount(size_t(outside)) && ca.refCount == polyCount(size_t(outside)));
    for (const auto& ref : inRoom.back()) CHECK(ref.first == outside);
    CHECK(r.stats.roomCatchAllTris == int(polyCount(size_t(outside))));

    // Every exported triangle is drawn by some room (the BVH is not used).
    size_t missing = 0;
    for (size_t o = 0; o < objCount; o++)
        for (size_t t = 0; t < polyCount(o); t++) {
            bool found = false;
            for (const auto& set : inRoom) found = found || set.count({int(o), int(t)});
            if (!found) missing++;
        }
    CHECK(missing == 0);

    // The straddling triangle is in both rooms (portal boundary duplication).
    CHECK(inRoom[0].count({straddle, 0}) && inRoom[1].count({straddle, 0}));

    // Each room's cells partition exactly its tri-ref range, in order.
    for (size_t i = 0; i < roomCount; i++) {
        const Room& rm = rooms[i];
        CHECK(rm.cellCount >= 1 && rm.cellCount <= 8);
        size_t next = rm.firstRef;
        for (size_t c = 0; c < rm.cellCount; c++) {
            const size_t at = cellsAt + 28 * (rm.firstCell + c);
            CHECK(rd16(b, at + 24) == next);
            CHECK(rd16(b, at + 26) > 0);
            for (int k = 0; k < 3; k++) CHECK(rd32(b, at + 4 * size_t(k)) <= rd32(b, at + 12 + 4 * size_t(k)));
            next += rd16(b, at + 26);
        }
        CHECK(next == rm.firstRef + rm.refCount);
    }

    // The portal joins A and B and its normal points from A to B. The room
    // boxes and the normal are both stored with Y negated, so the dot product
    // in pack space equals the world one.
    CHECK(rd16(b, portalsAt) == 0 && rd16(b, portalsAt + 2) == 1);
    const int16_t nx = rdi16(b, portalsAt + 20), ny = rdi16(b, portalsAt + 22), nz = rdi16(b, portalsAt + 24);
    double ab[3];
    for (int k = 0; k < 3; k++)
        ab[k] = (double(rooms[1].mn[k]) + rooms[1].mx[k]) / 2 - (double(rooms[0].mn[k]) + rooms[0].mx[k]) / 2;
    CHECK(nx * ab[0] + ny * ab[1] + nz * ab[2] > 0);
    CHECK(nx == 4096);
    const int16_t rx = rdi16(b, portalsAt + 28), rz = rdi16(b, portalsAt + 32);
    CHECK(rx == 0 && rz == -4096);  // right was flipped along with the normal (from +Z)
    CHECK(rdi16(b, portalsAt + 16) == splash::roundToInt(1 / gte * 4096) &&
          rdi16(b, portalsAt + 18) == splash::roundToInt(1 / gte * 4096));

    // Portal refs are symmetric: room r lists (p, other) iff other lists (p, r).
    std::set<std::tuple<int, int, int>> prefs;  // room, portal, other
    for (size_t i = 0; i < roomCount; i++)
        for (size_t k = 0; k < rooms[i].portalRefCount; k++) {
            const size_t at = prefsAt + 4 * (rooms[i].firstPortalRef + k);
            prefs.insert({int(i), rd16(b, at), rd16(b, at + 2)});
        }
    CHECK(prefs.size() == 2);
    for (const auto& [room, p, other] : prefs) {
        CHECK(prefs.count({other, p, room}));
        const int a = rd16(b, portalsAt + 40 * size_t(p)), bb = rd16(b, portalsAt + 40 * size_t(p) + 2);
        CHECK((a == room && bb == other) || (a == other && bb == room));
    }

    // Nav regions (just before the room block) carry the room their centroid
    // is in, from the room boxes 0.5 m larger per side.
    const size_t navRegions = rd16(b, 44), navPortals = rd16(b, 46);
    CHECK(navRegions > 0);
    const size_t navAt = off - (8 + 84 * navRegions + 20 * navPortals);
    CHECK(rd16(b, navAt) == navRegions && rd16(b, navAt + 2) == navPortals);
    const int32_t margin = splash::toWorldFixed12(0.5f / gte);
    std::set<int> seenRooms;
    for (size_t i = 0; i < navRegions; i++) {
        const size_t at = navAt + 8 + 84 * i;
        const size_t nv = uint8_t(b[at + 79]);
        CHECK(nv >= 3 && nv <= 8);
        double cx = 0, cz = 0;
        for (size_t v = 0; v < nv; v++) {
            cx += rd32(b, at + 4 * v);
            cz += rd32(b, at + 32 + 4 * v);
        }
        cx /= double(nv);
        cz /= double(nv);
        int want = 0xFF;
        for (int rm = 0; rm < 2 && want == 0xFF; rm++)
            if (cx >= rooms[size_t(rm)].mn[0] - margin && cx <= rooms[size_t(rm)].mx[0] + margin &&
                cz >= rooms[size_t(rm)].mn[2] - margin && cz <= rooms[size_t(rm)].mx[2] + margin)
                want = rm;
        CHECK(uint8_t(b[at + 81]) == want);
        seenRooms.insert(uint8_t(b[at + 81]));
    }
    CHECK(seenRooms.count(0) && seenRooms.count(1));

    // Without Room objects there is no room block and nav falls back to islands.
    std::erase_if(scene.objects, [](const splash::Object& o) { return o.room || o.portal; });
    splash::ExportResult plain = splash::exportSplashpack(scene, project, project / "plain.splashpack");
    CHECK(plain.ok() && plain.stats.rooms.empty() && plain.stats.roomBlockOffset == 0);
    const std::string pb = readAll(project / "plain.splashpack");
    CHECK(rd16(pb, 78) == 0 && rd16(pb, 80) == 0 && rd16(pb, 82) == 0 && rd16(pb, 86) == 0 && rd16(pb, 106) == 0);
}

// An object that is off, or under an off parent, is off in the hierarchy
// (Unity's activeInHierarchy): flatten says so, the export leaves it out
// (its broken components unchecked), its lights neither bake nor ship, and a
// cutscene track naming it says it is switched off. Start active off still
// exports the object.
void testActiveInHierarchyExport() {
    using namespace roomtest;
    const fs::path project = g_outDir / "hieractive";
    fs::remove_all(project);
    fs::create_directories(project / "meshes");
    splash::Mesh quad;
    quad.positions = {{-2, 0, -2}, {-2, 0, 2}, {2, 0, 2}, {2, 0, -2}};
    quad.normals.assign(4, {0, 1, 0});
    quad.submeshes = {{0, 1, 2, 0, 2, 3}};
    splash::saveMesh(quad, project / "meshes/quad.mesh");

    splash::Scene base;
    base.objects.push_back(meshObject("Ground", "meshes/quad.mesh", {0, 0, 0}, false));
    base.objects.push_back(meshObject("OnSibling", "meshes/quad.mesh", {6, 0, 0}, false));
    splash::Object offParent = meshObject("OffParent", "meshes/quad.mesh", {-6, 0, 0}, false);
    offParent.active = false;
    offParent.children.push_back(meshObject("HiddenKid", "meshes/quad.mesh", {0, 1, 0}, false));
    base.objects.push_back(offParent);
    splash::Object selfOff = meshObject("SelfOff", "meshes/quad.mesh", {0, 0, 6}, false);
    selfOff.active = false;
    base.objects.push_back(selfOff);
    splash::Object sleeper = meshObject("Sleeper", "meshes/quad.mesh", {0, 0, -6}, false);
    sleeper.startActive = false;
    base.objects.push_back(sleeper);

    // (vi) flatten: off itself, under an off parent, and an on sibling.
    {
        const std::vector<splash::FlatObject> flat = splash::flatten(base);
        auto inHierarchy = [&](const std::string& n) {
            for (const splash::FlatObject& fo : flat)
                if (fo.object->name == n) return int(fo.activeInHierarchy);
            return -1;
        };
        CHECK(inHierarchy("OffParent") == 0);
        CHECK(inHierarchy("HiddenKid") == 0);  // its own flag is on
        CHECK(inHierarchy("SelfOff") == 0);
        CHECK(inHierarchy("OnSibling") == 1);
        CHECK(inHierarchy("Ground") == 1 && inHierarchy("Sleeper") == 1);
    }

    // (i)-(iii) What the pack holds.
    splash::ExportResult r = splash::exportSplashpack(base, project, project / "base.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "hieractive export: %s\n", e.c_str());
    CHECK(r.ok());
    CHECK(r.stats.gameObjects == 3);  // Ground, OnSibling, Sleeper
    const std::string b = readAll(project / "base.splashpack");
    CHECK(b.find("OnSibling") != std::string::npos && b.find("Sleeper") != std::string::npos);
    CHECK(b.find("HiddenKid") == std::string::npos);  // child of an off parent
    CHECK(b.find("OffParent") == std::string::npos);
    CHECK(b.find("SelfOff") == std::string::npos);
    // An off object's components are not checked: a mesh file that does not
    // exist is never loaded, so it is no error.
    {
        splash::Scene broken = base;
        broken.objects[3].mesh->mesh = "meshes/missing.mesh";  // SelfOff
        broken.objects[2].children[0].mesh->mesh = "meshes/missing.mesh";  // HiddenKid
        CHECK(splash::exportSplashpack(broken, project, project / "broken.splashpack").ok());
    }

    // (iv) Lights on an off object or under an off parent change nothing.
    auto lamp = [](const char* name, splash::LightKind kind, bool runtime) {
        splash::Object o = named(name);
        o.transform.position = {0, 1, 0};
        splash::LightComponent l;
        l.kind = kind;
        l.color = {1, 0.2f, 0.2f};
        l.intensity = 2;
        l.runtime = runtime;
        o.light = l;
        return o;
    };
    splash::Scene lit = base;
    splash::Object offLamp = lamp("OffLamp", splash::LightKind::Point, false);
    offLamp.active = false;
    lit.objects.push_back(offLamp);
    splash::Object offSun = lamp("OffSun", splash::LightKind::Directional, false);
    offSun.active = false;
    lit.objects.push_back(offSun);
    splash::Object offRuntime = lamp("OffRuntime", splash::LightKind::Point, true);
    offRuntime.active = false;
    lit.objects.push_back(offRuntime);
    lit.objects[2].children.push_back(lamp("KidLamp", splash::LightKind::Point, false));       // under OffParent
    lit.objects[2].children.push_back(lamp("KidSun", splash::LightKind::Directional, false));  // under OffParent
    splash::ExportResult rl = splash::exportSplashpack(lit, project, project / "lit.splashpack");
    CHECK(rl.ok());
    CHECK(readAll(project / "lit.splashpack") == b);
    {
        const std::vector<splash::FlatObject> fb = splash::flatten(base), fl = splash::flatten(lit);
        CHECK(splash::exportVertexColors(fl, fl[0], quad) == splash::exportVertexColors(fb, fb[0], quad));
    }
    // The same lamp switched on does change the pack: the comparison can fail.
    splash::Scene onLit = base;
    onLit.objects.push_back(lamp("OnLamp", splash::LightKind::Point, false));
    CHECK(splash::exportSplashpack(onLit, project, project / "onlit.splashpack").ok());
    CHECK(readAll(project / "onlit.splashpack") != b);

    // (v) A cutscene track naming an off object says why it is missing.
    auto withTrack = [&](const char* target) {
        splash::Scene s = base;
        splash::Cutscene cs;
        cs.name = "intro";
        splash::CutsceneTrack t;
        t.type = splash::TrackType::ObjectPosition;
        t.target = target;
        t.keyframes.push_back(splash::Keyframe{});
        cs.tracks.push_back(t);
        s.cutscenes.push_back(cs);
        return splash::exportSplashpack(s, project, project / "cs.splashpack");
    };
    auto hasError = [](const splash::ExportResult& res, const std::string& text) {
        return std::any_of(res.errors.begin(), res.errors.end(),
                           [&](const std::string& e) { return e.find(text) != std::string::npos; });
    };
    const splash::ExportResult cOff = withTrack("SelfOff");
    CHECK(!cOff.ok() && hasError(cOff, "'SelfOff' is switched off (or under an off parent), so it is not exported"));
    const splash::ExportResult cKid = withTrack("HiddenKid");
    CHECK(!cKid.ok() && hasError(cKid, "'HiddenKid' is switched off (or under an off parent), so it is not exported"));
    const splash::ExportResult cNone = withTrack("Nobody");
    CHECK(!cNone.ok() && hasError(cNone, "no exported object named 'Nobody'"));
    CHECK(withTrack("Sleeper").ok());
}

// What the room block cannot carry is an export error, never a clamp.
// The BVH boxes the viewport draws (ExportStats::bvhBoxes): one per node,
// breadth first. buildBvh gives an inner node exactly two children, so in
// breadth-first order the k-th inner node's children are nodes 2k+1 and 2k+2.
void testBvhBoxes() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = src / "examples" / "courtyard";
    const splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    const fs::path dir = g_outDir / "bvh";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::ExportOptions dry;
    dry.dryRun = true;
    const splash::ExportResult d = splash::exportSplashpack(scene, project, dir / "scene.splashpack", dry);
    const splash::ExportResult r = splash::exportSplashpack(scene, project, dir / "scene.splashpack");
    CHECK(d.ok() && r.ok());
    if (!d.ok() || !r.ok()) return;
    const std::string b = readAll(dir / "scene.splashpack");
    const auto& boxes = r.stats.bvhBoxes;
    // The header's bvhNodeCount, and the dry run (the editor's live export) agrees.
    CHECK(boxes.size() == rd16(b, 32));
    CHECK(boxes.size() > 3);
    CHECK(d.stats.bvhBoxes.size() == boxes.size() && d.stats.bvhDepth == r.stats.bvhDepth);
    if (boxes.empty()) return;
    CHECK(boxes[0].depth == 0);
    int deepest = 0, leaves = 0, inner = 0;
    long leafTris = 0;
    const float eps = 1e-3f;
    auto inside = [eps](const splash::ExportStats::BvhBox& in, const splash::ExportStats::BvhBox& out) {
        return in.lo.x >= out.lo.x - eps && in.lo.y >= out.lo.y - eps && in.lo.z >= out.lo.z - eps &&
               in.hi.x <= out.hi.x + eps && in.hi.y <= out.hi.y + eps && in.hi.z <= out.hi.z + eps;
    };
    for (size_t i = 0; i < boxes.size(); i++) {
        const auto& n = boxes[i];
        deepest = std::max(deepest, n.depth);
        CHECK(n.lo.x <= n.hi.x && n.lo.y <= n.hi.y && n.lo.z <= n.hi.z);
        if (n.leaf) {
            leaves++;
            leafTris += n.tris;
            CHECK(n.tris > 0);
            continue;
        }
        CHECK(n.tris == 0);
        const size_t c0 = 2 * size_t(inner) + 1, c1 = c0 + 1;
        inner++;
        CHECK(c1 < boxes.size());
        if (c1 >= boxes.size()) continue;
        for (size_t c : {c0, c1}) {
            CHECK(boxes[c].depth == n.depth + 1);
            CHECK(inside(boxes[c], n));
        }
    }
    CHECK(r.stats.bvhDepth == deepest);
    CHECK(deepest > 0);
    CHECK(leaves == inner + 1);
    CHECK(leafTris == long(r.stats.bvhTriangleRefs));
    CHECK(rd16(b, 34) == r.stats.bvhTriangleRefs);
    // The root holds every triangle of the scene.
    CHECK(r.stats.bvhTriangleRefs == uint32_t(r.stats.triangles));

    // No geometry: no boxes, depth 0.
    splash::Scene empty;
    empty.settings = scene.settings;
    const splash::ExportResult e = splash::exportSplashpack(empty, project, dir / "empty.splashpack", dry);
    CHECK(e.stats.bvhBoxes.empty() && e.stats.bvhDepth == 0);
}

// The output popups' filter, clear point and Copy text (editor::LogView).
void testLogViewFilter() {
    const std::deque<std::string> lines = {"tick 1", "Lua ERROR: boom", "tick 2", "lua error: again", "Errorless? no: erro"};
    editor::LogView v;
    CHECK(editor::visibleLines(lines, 5, v) == (std::vector<size_t>{0, 1, 2, 3, 4}));
    v.filter = "error";
    CHECK(editor::visibleLines(lines, 5, v) == (std::vector<size_t>{1, 3, 4}));
    v.filter = "ErRoR:";
    CHECK(editor::visibleLines(lines, 5, v) == (std::vector<size_t>{1, 3}));
    v.filter = "nothing";
    CHECK(editor::visibleLines(lines, 5, v).empty());
    CHECK(editor::containsNoCase("ABC", "") && editor::containsNoCase("xAbC", "bc") && !editor::containsNoCase("ab", "abc"));
}

void testLogViewClear() {
    // 10 lines printed, the newest 4 kept (lines 6..9): a clear at 8 leaves 8 and 9.
    const std::deque<std::string> kept = {"line 6", "line 7", "line 8", "line 9"};
    editor::LogView v;
    v.clearedAt = 8;
    CHECK(editor::visibleLines(kept, 10, v) == (std::vector<size_t>{2, 3}));
    CHECK(editor::linesSinceClear(10, v) == 2);
    // A clear before what is kept hides nothing that is left.
    v.clearedAt = 3;
    CHECK(editor::visibleLines(kept, 10, v) == (std::vector<size_t>{0, 1, 2, 3}));
    CHECK(editor::linesSinceClear(10, v) == 7);
    // Cleared at the end: nothing until more lines arrive.
    v.clearedAt = 10;
    CHECK(editor::visibleLines(kept, 10, v).empty());
    CHECK(editor::linesSinceClear(10, v) == 0);
    const std::deque<std::string> more = {"line 7", "line 8", "line 9", "line 10"};
    CHECK(editor::visibleLines(more, 11, v) == (std::vector<size_t>{3}));
    // With a filter, both apply.
    v.clearedAt = 8;
    v.filter = "9";
    CHECK(editor::visibleLines(kept, 10, v) == (std::vector<size_t>{3}));
    // A total below the clear point is a new run: nothing is cleared.
    const std::deque<std::string> fresh = {"a", "b", "c"};
    editor::LogView r;
    r.clearedAt = 50;
    CHECK(editor::visibleLines(fresh, 3, r) == (std::vector<size_t>{0, 1, 2}));
    CHECK(editor::linesSinceClear(3, r) == 3);
}

// A new run drops the clear point but keeps the filter and wrapping, so a
// run that soon prints past the old clear point still shows from its start.
void testLogViewRestart() {
    editor::LogView v;
    v.filter = "tick";
    v.wrap = true;
    v.clearedAt = 3;
    editor::restartLog(v);
    CHECK(v.clearedAt == 0);
    CHECK(v.filter == "tick" && v.wrap);
    const std::deque<std::string> run = {"tick 0", "tick 1", "tick 2", "tick 3", "tick 4"};
    CHECK(editor::visibleLines(run, 5, v).size() == 5);
    CHECK(editor::linesSinceClear(5, v) == 5);
}

void testLogViewJoin() {
    const std::deque<std::string> lines = {"one", "two", "three"};
    CHECK(editor::joinLines(lines, {}).empty());
    CHECK(editor::joinLines(lines, {1}) == "two\n");
    CHECK(editor::joinLines(lines, {0, 2}) == "one\nthree\n");
    CHECK(editor::joinLines(lines, {0, 7}) == "one\n");  // an index past the end is skipped
}

// What the Navigation inspector shows per nav region (ExportStats::navInfo):
// one entry per outline, the centroid on the region's floor plane (the plane
// is linear, so its height at the vertex average is the average of the
// outline's heights), portals listed from both sides, and the room either a
// Room index or, with no Room objects, a connectivity island.
void testNavInfo() {
    using namespace roomtest;
    const fs::path dir = g_outDir / "navinfo";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::ExportOptions dry;
    dry.dryRun = true;
    auto checkShape = [](const splash::ExportStats& s) {
        CHECK(s.navInfo.size() == size_t(s.navRegions));
        CHECK(s.navOutlines.size() == s.navInfo.size());
        size_t listed = 0;
        for (size_t i = 0; i < s.navInfo.size() && i < s.navOutlines.size(); i++) {
            const auto& info = s.navInfo[i];
            const auto& poly = s.navOutlines[i];
            CHECK(!poly.empty());
            if (poly.empty()) continue;
            double x = 0, y = 0, z = 0;
            for (const splash::Vec3& v : poly) x += v.x, y += v.y, z += v.z;
            x /= double(poly.size()), y /= double(poly.size()), z /= double(poly.size());
            CHECK(std::fabs(info.centroid.x - x) < 1e-4 && std::fabs(info.centroid.z - z) < 1e-4);
            CHECK(std::fabs(info.centroid.y - y) < 1e-4);
            CHECK(info.floorY == info.centroid.y);
            CHECK(info.surface >= 0 && info.surface <= 2);
            listed += info.portals.size();
            for (const auto& p : info.portals) {
                CHECK(p.neighbor >= 0 && size_t(p.neighbor) < s.navInfo.size() && size_t(p.neighbor) != i);
                if (p.neighbor < 0 || size_t(p.neighbor) >= s.navInfo.size()) continue;
                bool back = false;
                for (const auto& q : s.navInfo[size_t(p.neighbor)].portals) back = back || size_t(q.neighbor) == i;
                CHECK(back);
            }
        }
        CHECK(listed == size_t(s.navPortals));
    };

    // The courtyard has no Room objects: islands, one connected walkable area.
    const fs::path court = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    const splash::Scene cs = splash::loadScene(court / "courtyard.scene");
    const splash::ExportResult c = splash::exportSplashpack(cs, court, dir / "court.splashpack", dry);
    CHECK(c.ok());
    if (!c.ok()) return;
    CHECK(c.stats.navRegions > 1 && c.stats.navPortals > 0);
    CHECK(c.stats.navIslands);
    checkShape(c.stats);
    for (const auto& info : c.stats.navInfo) CHECK(info.room >= 0);

    // Two Rooms: every region's room is one of them, or none.
    const fs::path project = dir / "rooms";
    const splash::Scene rs = twoRooms(project);
    const splash::ExportResult r = splash::exportSplashpack(rs, project, project / "scene.splashpack", dry);
    CHECK(r.ok());
    if (!r.ok()) return;
    CHECK(r.stats.navRegions > 0);
    CHECK(!r.stats.navIslands);
    checkShape(r.stats);
    bool inRoom = false;
    for (const auto& info : r.stats.navInfo) {
        CHECK(info.room >= -1 && info.room < int(r.stats.rooms.size()));
        inRoom = inRoom || info.room >= 0;
    }
    CHECK(inRoom);

    // A ramp rising 0.3 per unit of X, its middle at x = 10: both scenes above
    // have flat floors, where the plane's slope terms are zero and a centroid
    // read off planeD alone would pass. The heights are checked against the
    // mesh, not the writer's plane. Recast puts the walkable surface on voxel
    // tops, a little above the triangles (cellHeight 0.025), so the region
    // sits a constant height over the ramp rather than on it.
    const fs::path rampDir = dir / "ramp";
    fs::create_directories(rampDir / "meshes");
    splash::Mesh ramp;
    const float k = 0.3f, ox = 10;
    ramp.positions = {{-3, -3 * k, -3}, {-3, -3 * k, 3}, {3, 3 * k, 3}, {3, 3 * k, -3}};
    ramp.normals.assign(4, {-k, 1, 0});
    ramp.submeshes = {{0, 1, 2, 0, 2, 3}};
    splash::saveMesh(ramp, rampDir / "meshes/ramp.mesh");
    splash::Scene ps;
    ps.objects.push_back(meshObject("Ramp", "meshes/ramp.mesh", {ox, 0, 0}, true));
    splash::Object rampNav;
    rampNav.name = "Nav";
    rampNav.transform.position = {ox, 0, 0};
    rampNav.navigation = splash::NavigationComponent{};
    rampNav.navigation->agentRadius = 0.3f;
    ps.objects.push_back(rampNav);
    const splash::ExportResult p = splash::exportSplashpack(ps, rampDir, rampDir / "scene.splashpack", dry);
    CHECK(p.ok());
    if (!p.ok()) return;
    CHECK(p.stats.navRegions > 0);
    checkShape(p.stats);
    // Height above the ramp at a point: the same small lift everywhere.
    auto lift = [&](const splash::Vec3& v) { return v.y - k * (v.x - ox); };
    bool sloped = false;
    for (size_t i = 0; i < p.stats.navInfo.size() && i < p.stats.navOutlines.size(); i++) {
        const auto& info = p.stats.navInfo[i];
        sloped = sloped || info.surface == 1;
        CHECK(info.centroid.x > ox - 3 && info.centroid.x < ox + 3);
        const float c = lift(info.centroid);
        CHECK(c > -1e-3f && c < 0.1f);
        for (const splash::Vec3& v : p.stats.navOutlines[i]) CHECK(std::fabs(lift(v) - c) < 1e-3f);
    }
    CHECK(sloped);
}

// The nav bake presets (SplashEdit's Indoor / Terrain / Multi-Level): the ten
// region fields each sets, which preset a settings block matches, and the
// per-region floor plane error the inspector warns about.
void testNavPresets() {
    struct Want {
        float cs, ch;
        int minA, mergeA;
        float simp, edge;
        splash::NavPartition part;
        float dist, err, plane;
    };
    const Want want[3] = {{0.05f, 0.025f, 8, 20, 1.3f, 12, splash::NavPartition::Watershed, 6, 0.025f, 0.15f},
                          {0.08f, 0.04f, 4, 6, 0.4f, 5, splash::NavPartition::Monotone, 4, 0.02f, 0.1f},
                          {0.05f, 0.02f, 6, 12, 0.8f, 8, splash::NavPartition::Layer, 5, 0.02f, 0.12f}};
    // Indoor is the struct's defaults.
    CHECK(splash::matchingNavPreset(splash::NavBakeSettings{}) == 0);
    for (int i = 0; i < 3; i++) {
        splash::NavBakeSettings n;
        n.maxStepHeight = 0.7f;
        n.walkableSlopeAngle = 30;
        // Start from another preset so every field has to be written.
        splash::applyNavPreset(n, splash::NavPreset((i + 1) % 3));
        splash::applyNavPreset(n, splash::NavPreset(i));
        const Want& w = want[i];
        CHECK(n.cellSize == w.cs && n.cellHeight == w.ch);
        CHECK(n.minRegionArea == w.minA && n.mergeRegionArea == w.mergeA);
        CHECK(n.maxSimplifyError == w.simp && n.maxEdgeLength == w.edge);
        CHECK(n.partition == w.part);
        CHECK(n.detailSampleDist == w.dist && n.detailMaxError == w.err && n.maxPlaneError == w.plane);
        CHECK(n.maxStepHeight == 0.7f && n.walkableSlopeAngle == 30);  // never touched
        CHECK(splash::matchingNavPreset(n) == i);
        // Any one field off makes it custom.
        for (int f = 0; f < 10; f++) {
            splash::NavBakeSettings m = n;
            switch (f) {
                case 0: m.cellSize += 0.01f; break;
                case 1: m.cellHeight += 0.01f; break;
                case 2: m.minRegionArea += 1; break;
                case 3: m.mergeRegionArea += 1; break;
                case 4: m.maxSimplifyError += 0.1f; break;
                case 5: m.maxEdgeLength += 1; break;
                case 6: m.partition = splash::NavPartition((int(m.partition) + 1) % 3); break;
                case 7: m.detailSampleDist += 1; break;
                case 8: m.detailMaxError += 0.001f; break;
                default: m.maxPlaneError += 0.01f; break;
            }
            CHECK(splash::matchingNavPreset(m) == -1);
        }
    }

    // Floor plane error: a flat floor fits its plane; a bumpy one, baked into
    // big regions, does not.
    using namespace roomtest;
    const fs::path dir = g_outDir / "navpresets";
    fs::remove_all(dir);
    fs::create_directories(dir / "meshes");
    splash::ExportOptions dry;
    dry.dryRun = true;
    auto grid = [](float amp) {
        splash::Mesh m;
        const int n = 24;
        const float step = 0.5f, half = n * step / 2;
        for (int z = 0; z <= n; z++)
            for (int x = 0; x <= n; x++) {
                const float px = x * step - half, pz = z * step - half;
                m.positions.push_back({px, amp * std::sin(px * 0.8f) * std::cos(pz * 0.8f), pz});
                m.normals.push_back({0, 1, 0});
            }
        std::vector<int> idx;
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) {
                const int a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
                idx.insert(idx.end(), {a, c, d, a, d, b});
            }
        m.submeshes = {idx};
        return m;
    };
    auto bake = [&](const char* name, float amp, float planeErr) {
        splash::saveMesh(grid(amp), dir / "meshes" / (std::string(name) + ".mesh"));
        splash::Scene s;
        s.objects.push_back(meshObject("Ground", ("meshes/" + std::string(name) + ".mesh").c_str(), {0, 0, 0}, true));
        splash::Object nav;
        nav.name = "Nav";
        nav.transform.position = {0, 1, 0};
        nav.navigation = splash::NavigationComponent{};
        nav.navigation->agentRadius = 0.3f;
        nav.navigation->nav.mergeRegionArea = 400;  // big regions over the bumps
        nav.navigation->nav.maxPlaneError = planeErr;
        s.objects.push_back(nav);
        return splash::exportSplashpack(s, dir, dir / (std::string(name) + ".splashpack"), dry);
    };
    const splash::ExportResult flat = bake("flat", 0, 0.07f);
    CHECK(flat.ok());
    CHECK(flat.stats.navRegions > 0);
    CHECK(flat.stats.navMaxPlaneError == 0.07f);
    for (const auto& info : flat.stats.navInfo) CHECK(info.planeDeviation < 1e-3f);
    const splash::ExportResult bumpy = bake("bumpy", 0.4f, 0.15f);
    CHECK(bumpy.ok());
    CHECK(bumpy.stats.navRegions > 0);
    CHECK(bumpy.stats.navMaxPlaneError == 0.15f);
    float worst = 0;
    for (const auto& info : bumpy.stats.navInfo) worst = std::max(worst, info.planeDeviation);
    CHECK(worst > 0.05f);
}

// Nav walk-off zones (PSXNavWalkoffZone): a boundary edge whose midpoint, on
// its region's floor plane, lies inside a zone's world box gets its
// walkoffEdgeMask bit; the export counts the edges per zone, skips zones on
// inactive objects, and the scene file keeps the size.
void testNavWalkoff() {
    using namespace roomtest;
    // Bake level: a 6 m plane, its walkable edge at x ~ +2.7 (agent radius 0.3).
    {
        std::vector<std::string> errors;
        splash::NavMesh base = bakePlane(6, errors);
        CHECK(errors.empty() && !base.regions.empty());
        auto midOf = [](const splash::NavRegion& r, size_t j) {
            const splash::Vec2 a = r.verts[j], b = r.verts[(j + 1) % r.verts.size()];
            return splash::Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        };
        // A slab over x in [2, 4], the whole depth: exactly the boundary edges
        // whose midpoint is past x = 2 (the +X side) open.
        splash::NavMesh nav = base;
        const splash::Bounds slab({3, 0, 0}, {2, 2, 20});
        splash::applyWalkoffZones(nav, {slab});
        int opened = 0, expected = 0;
        for (const splash::NavRegion& r : nav.regions) {
            uint8_t want = 0;
            for (size_t j = 0; j < r.verts.size(); j++)
                if ((r.boundaryEdgeMask >> j & 1) && midOf(r, j).x > 2) want |= uint8_t(1 << j);
            CHECK(r.walkoffEdgeMask == want);
            CHECK((r.walkoffEdgeMask & ~r.boundaryEdgeMask) == 0);
            expected += std::popcount(want);
            opened += std::popcount(r.walkoffEdgeMask);
        }
        CHECK(expected > 0 && opened == expected);
        CHECK(nav.walkoffEdgesPerZone.size() == 1 && nav.walkoffEdgesPerZone[0] == expected);
        // The same box twice: every edge counts for the first only.
        splash::NavMesh twice = base;
        splash::applyWalkoffZones(twice, {slab, slab});
        CHECK(twice.walkoffEdgesPerZone.size() == 2 && twice.walkoffEdgesPerZone[0] == expected &&
              twice.walkoffEdgesPerZone[1] == 0);
        // Above the floor (y 1..3): the midpoints are on the plane, nothing opens.
        splash::NavMesh high = base;
        splash::applyWalkoffZones(high, {splash::Bounds({3, 2, 0}, {2, 2, 20})});
        for (const splash::NavRegion& r : high.regions) CHECK(r.walkoffEdgeMask == 0);
        CHECK(high.walkoffEdgesPerZone.size() == 1 && high.walkoffEdgesPerZone[0] == 0);
        // Through buildNavRegions' trailing parameter.
        static std::vector<splash::Vec3> pos;
        pos = {{-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3}};
        splash::NavInputObject in{true, false, splash::Mat34::trs({0, 0, 0}, splash::Quat{0, 0, 0, 1}, {1, 1, 1}), &pos,
                                  {0, 1, 2, 0, 2, 3}, splash::Bounds({0, 0, 0}, {6, 0, 6})};
        splash::NavMesh built = splash::buildNavRegions({in}, splash::NavBuildParams{}, {0, 0, 0}, errors, {}, {slab});
        CHECK(built.walkoffEdgesPerZone.size() == 1 && built.walkoffEdgesPerZone[0] == expected);

        // The written nav table carries the mask: header 8 bytes, then
        // 84-byte regions whose last byte is walkoffEdgeMask.
        splash::BinWriter w;
        splash::writeNavRegions(w, nav, 100.f);
        const std::vector<uint8_t>& b = w.data();
        CHECK(b.size() >= 8 + 84 * nav.regions.size());
        bool anyWritten = false;
        for (size_t i = 0; i < nav.regions.size() && 8 + 84 * (i + 1) <= b.size(); i++) {
            CHECK(b[8 + 84 * i + 83] == nav.regions[i].walkoffEdgeMask);
            CHECK(b[8 + 84 * i + 82] == nav.regions[i].flags);
            anyWritten = anyWritten || b[8 + 84 * i + 83] != 0;
        }
        CHECK(anyWritten);
    }

    // Export level.
    const fs::path project = g_outDir / "walkoff";
    fs::remove_all(project);
    fs::create_directories(project / "meshes");
    splash::Mesh quad;
    quad.positions = {{-3, 0, -3}, {-3, 0, 3}, {3, 0, 3}, {3, 0, -3}};
    quad.normals.assign(4, {0, 1, 0});
    quad.submeshes = {{0, 1, 2, 0, 2, 3}};
    splash::saveMesh(quad, project / "meshes/quad.mesh");
    auto sceneWith = [&](splash::Vec3 zonePos, splash::Vec3 zoneSize, bool active, bool parentOff) {
        splash::Scene s;
        s.objects.push_back(meshObject("Floor", "meshes/quad.mesh", {0, 0, 0}, true));
        splash::Object nav;
        nav.name = "Nav";
        nav.navigation = splash::NavigationComponent{};
        nav.navigation->agentRadius = 0.3f;
        s.objects.push_back(nav);
        splash::Object zone;
        zone.name = "Ledge";
        zone.active = active;
        zone.transform.position = zonePos;
        zone.walkoff = splash::WalkoffComponent{};
        zone.walkoff->size = zoneSize;
        if (parentOff) {
            splash::Object parent;
            parent.name = "Group";
            parent.active = false;
            parent.children.push_back(zone);
            s.objects.push_back(parent);
        } else {
            s.objects.push_back(zone);
        }
        return s;
    };
    splash::ExportOptions dry;
    dry.dryRun = true;
    auto masks = [](const splash::ExportStats& s) {
        int n = 0;
        for (const auto& info : s.navInfo) n += std::popcount(unsigned(info.walkoffEdges));
        return n;
    };
    // (a) Straddling the +X edge.
    const splash::ExportResult on = splash::exportSplashpack(sceneWith({3, 0, 0}, {2, 2, 20}, true, false), project,
                                                              project / "on.splashpack", dry);
    CHECK(on.ok());
    CHECK(on.stats.walkoffZones.size() == 1);
    if (on.stats.walkoffZones.size() == 1) {
        CHECK(on.stats.walkoffZones[0].object == "Ledge" && on.stats.walkoffZones[0].flatIndex == 2);
        CHECK(on.stats.walkoffZones[0].edges > 0 && on.stats.walkoffZones[0].edges == masks(on.stats));
    }
    // (b) Away from the floor.
    const splash::ExportResult off = splash::exportSplashpack(sceneWith({20, 0, 0}, {2, 2, 20}, true, false), project,
                                                               project / "off.splashpack", dry);
    CHECK(off.ok() && off.stats.walkoffZones.size() == 1 && off.stats.walkoffZones[0].edges == 0 && masks(off.stats) == 0);
    // (c) On an inactive object, or under an inactive parent: ignored.
    const splash::ExportResult inactive = splash::exportSplashpack(sceneWith({3, 0, 0}, {2, 2, 20}, false, false), project,
                                                                    project / "inactive.splashpack", dry);
    CHECK(inactive.ok() && inactive.stats.walkoffZones.empty() && masks(inactive.stats) == 0);
    const splash::ExportResult hidden = splash::exportSplashpack(sceneWith({3, 0, 0}, {2, 2, 20}, true, true), project,
                                                                  project / "hidden.splashpack", dry);
    CHECK(hidden.ok() && hidden.stats.walkoffZones.empty() && masks(hidden.stats) == 0);
    CHECK(on.stats.navRegions == inactive.stats.navRegions);

    // (d) The scene file keeps the size and unknown keys.
    splash::Scene saved = sceneWith({3, 0, 0}, {1.5f, 2.5f, 3.5f}, true, false);
    saved.objects[2].walkoff->extra.push_back({"note", "\"cliff\""});
    splash::saveScene(saved, project / "w.scene");
    const splash::Scene loaded = splash::loadScene(project / "w.scene");
    CHECK(loaded.objects.size() == 3 && loaded.objects[2].walkoff);
    if (loaded.objects.size() == 3 && loaded.objects[2].walkoff) {
        CHECK(loaded.objects[2].walkoff->size == (splash::Vec3{1.5f, 2.5f, 3.5f}));
        CHECK(loaded.objects[2].walkoff->extra.size() == 1 && loaded.objects[2].walkoff->extra[0].first == "note");
        CHECK(!loaded.objects[0].walkoff && !loaded.objects[1].walkoff);
    }
    // Defaults: Unity's (1, 2, 0.5).
    splash::Object fresh;
    CHECK(editor::addComponent(fresh, editor::ComponentKind::Walkoff) && fresh.walkoff &&
          fresh.walkoff->size == (splash::Vec3{1, 2, 0.5f}));
    CHECK(std::string(editor::info(editor::ComponentKind::Walkoff).label) == "Nav walk-off zone");
    CHECK(editor::removeComponent(fresh, editor::ComponentKind::Walkoff) && !fresh.walkoff);
}

static std::string fileBytes(const std::filesystem::path& p);

// The Agent component (PSXAgent): the 28-byte SPLASHPACKAgentV2 record and
// waypoints for a known agent, its warnings, the scene file round trip, the
// catalog, undo, and which objects the export makes agents of.
void testAgent() {
    namespace fs = std::filesystem;
    // (a) Record bytes, worked out by hand from docs/splashpack-format.md 2.7 at gte 100:
    // speed 3 m/s -> 3/30/100*4096 = 4.096 -> 4; stop 0.75 -> 30.72 -> 31; vision 10 -> 409.6 -> 410;
    // fov 120 -> cos 60 deg * 4096 = 2048; hearing 2.5 -> 102.4 -> 102; waypoint (1, 2, -3) ->
    // (40.96, -81.92, -122.88) -> (41, -82, -123), Y negated.
    splash::AgentComponent a;
    a.moveSpeed = 3;
    a.stopDistance = 0.75f;
    a.visionRange = 10;
    a.fov = 120;
    a.hearingRange = 2.5f;
    a.alertTimeout = 45;
    a.visionRegionDepth = 4;
    a.waypoints = {{1, 2, -3}};
    a.stateClips[1] = 0;  // Patrol
    a.stateClips[3] = 2;  // Flee
    a.stateClips[5] = 7;  // Wander: past a 3-clip skin, so none
    const splash::AgentRecord r = splash::agentRecord(a, 0x1234, 3, 100.f);
    const std::array<uint8_t, 28> want = {0x34, 0x12, 0x0F, 0x01, 4, 0, 31, 0, 0x9A, 0x01, 0x00, 0x08, 102, 0,
                                          45,   0,    0xFF, 0x00, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 4, 0, 0, 0};
    CHECK(r.bytes == want);
    CHECK(r.waypoints.size() == 1 && r.waypoints[0] == (std::array<int32_t, 3>{41, -82, -123}));
    // No skin: every clip none. Vision off, no hearing, no waypoints: only the enabled bit.
    a.vision = false;
    a.hearingRange = 0;
    a.waypoints.clear();
    const splash::AgentRecord bare = splash::agentRecord(a, 0, 0, 100.f);
    CHECK(bare.bytes[2] == 0x01 && bare.bytes[3] == 0 && bare.waypoints.empty());
    CHECK(std::all_of(bare.bytes.begin() + 16, bare.bytes.begin() + 24, [](uint8_t b) { return b == 0xFF; }));
    // Vision on but range 0 is no vision; disabled at load clears bit 0; fov 360 sees all around.
    a.vision = true;
    a.visionRange = 0;
    a.startEnabled = false;
    a.fov = 360;
    const splash::AgentRecord off = splash::agentRecord(a, 0, 0, 100.f);
    CHECK(off.bytes[2] == 0x00 && off.bytes[10] == 0x00 && off.bytes[11] == 0xF0);  // -4096
    // Nine waypoints: eight written.
    a.waypoints.assign(9, {0, 0, 0});
    const splash::AgentRecord nine = splash::agentRecord(a, 0, 0, 100.f);
    CHECK(nine.bytes[3] == 8 && nine.waypoints.size() == 8 && (nine.bytes[2] & 0x08));
    CHECK(splash::agentBytes(a) == 28 + 12 * 8);

    // (b) Warnings.
    splash::Object o;
    o.name = "Guard";
    CHECK(splash::agentWarnings(o).empty());
    a.stateClips.fill(-1);
    o.agent = a;  // 9 waypoints, fov 360
    o.agent->stateClips[0] = 0;
    std::vector<std::string> w = splash::agentWarnings(o);
    CHECK(w.size() == 2 && w[0].find("Guard: agent has 9 waypoints") == 0 && w[1].find("state Idle") != std::string::npos &&
          w[1].find("no skin") != std::string::npos);
    o.agent->fov = 0;
    o.skin = splash::SkinComponent{};
    o.skin->clips = {"a.anim"};
    o.agent->waypoints.clear();
    w = splash::agentWarnings(o);
    CHECK(w.size() == 1 && w[0].find("field of view") != std::string::npos);  // clip 0 of 1 is fine
    o.agent->stateClips[0] = 1;
    o.agent->fov = 361;
    w = splash::agentWarnings(o);
    CHECK(w.size() == 2 && w[1].find("plays clip 2 of a skin with 1") != std::string::npos);
    // A held state (Patrol) on a clip that plays once is warned about, given the
    // loaded clips; a looping clip, or a state that ends (Attack), is not.
    o.agent->fov = 90;
    o.agent->stateClips.fill(-1);
    o.agent->stateClips[1] = 0;  // Patrol
    o.skin->clips = {"swing.anim", "walk.anim"};
    std::vector<splash::AgentClipLoop> clipsIn{{"Swing", false}, {"Walk", true}};
    CHECK(splash::agentWarnings(o).empty());  // without the loaded clips the loop flag is unknown
    w = splash::agentWarnings(o, clipsIn);
    CHECK(w.size() == 1 &&
          w[0] == "Guard: agent state Patrol plays clip Swing, which plays once; it stops on its last frame");
    clipsIn[0].loop = true;
    CHECK(splash::agentWarnings(o, clipsIn).empty());
    clipsIn[0].loop = false;
    o.agent->stateClips[1] = 1;  // Patrol on Walk, which loops
    CHECK(splash::agentWarnings(o, clipsIn).empty());
    o.agent->stateClips.fill(-1);
    o.agent->stateClips[4] = 0;  // Attack on Swing, which plays once
    CHECK(splash::agentWarnings(o, clipsIn).empty());
    for (int s = 0; s < splash::kAgentStates; s++) {  // the held set is the inspector card's
        o.agent->stateClips.fill(-1);
        o.agent->stateClips[size_t(s)] = 0;
        CHECK(splash::agentWarnings(o, clipsIn).size() == ((s <= 3 || s == 5) ? 1u : 0u));
    }

    // (c) The scene file keeps every field and unknown keys.
    const fs::path project = g_outDir / "agent";
    fs::remove_all(project);
    fs::create_directories(project);
    splash::Scene s;
    splash::Object guard;
    guard.name = "Guard";
    guard.agent = splash::AgentComponent{};
    splash::AgentComponent& g = *guard.agent;
    g.startEnabled = false;
    g.moveSpeed = 1.25f;
    g.stopDistance = 0.3f;
    g.vision = false;
    g.visionRange = 12.5f;
    g.fov = 75;
    g.visionRegionDepth = 5;
    g.hearingRange = 4;
    g.alertTimeout = 300;
    g.waypoints = {{1, 0.5f, -2}, {-4, 0, 3.25f}};
    g.stateClips[2] = 3;
    g.stateClips[7] = 0;
    g.extra.push_back({"note", "\"hi\""});
    s.objects.push_back(guard);
    splash::Object plain;
    plain.name = "Plain";
    s.objects.push_back(plain);
    splash::saveScene(s, project / "a.scene");
    const splash::Scene loaded = splash::loadScene(project / "a.scene");
    CHECK(loaded.objects.size() == 2 && loaded.objects[0].agent && !loaded.objects[1].agent);
    if (loaded.objects.size() == 2 && loaded.objects[0].agent) {
        const splash::AgentComponent& l = *loaded.objects[0].agent;
        CHECK(!l.startEnabled && l.moveSpeed == 1.25f && l.stopDistance == 0.3f && !l.vision && l.visionRange == 12.5f &&
              l.fov == 75 && l.visionRegionDepth == 5 && l.hearingRange == 4 && l.alertTimeout == 300);
        CHECK(l.waypoints == g.waypoints && l.stateClips == g.stateClips);
        CHECK(l.extra.size() == 1 && l.extra[0].first == "note");
    }
    // Saving again writes the same bytes.
    splash::saveScene(loaded, project / "b.scene");
    CHECK(fileBytes(project / "a.scene") == fileBytes(project / "b.scene"));

    // (d) Catalog defaults and undo.
    splash::Object fresh;
    CHECK(editor::addComponent(fresh, editor::ComponentKind::Agent) && fresh.agent && fresh.agent->startEnabled &&
          fresh.agent->moveSpeed == 2 && fresh.agent->stopDistance == 0.5f && fresh.agent->vision && fresh.agent->visionRange == 8 &&
          fresh.agent->fov == 90 && fresh.agent->visionRegionDepth == 2 && fresh.agent->hearingRange == 0 &&
          fresh.agent->alertTimeout == 90 && fresh.agent->waypoints.empty() && fresh.agent->stateClips[0] == -1);
    CHECK(editor::hasComponent(fresh, editor::ComponentKind::Agent));
    CHECK(std::string(editor::info(editor::ComponentKind::Agent).label) == "Agent (NPC)");
    CHECK(editor::removeComponent(fresh, editor::ComponentKind::Agent) && !fresh.agent);
    {
        editor::Document doc;
        doc.reset(s, project, "a");
        doc.edit({0}, [](splash::Object& ob) { ob.agent->waypoints.push_back({9, 9, 9}); });
        CHECK(doc.object({0})->agent->waypoints.size() == 3);
        CHECK(doc.undo() && doc.object({0})->agent->waypoints.size() == 2);
        CHECK(doc.redo() && doc.object({0})->agent->waypoints.size() == 3);
    }

    // (e) Export: active objects only, in scene order, mesh-less agents become game objects.
    splash::Scene ex;
    splash::Object ghost = guard, offAgent = guard, keeper;
    ghost.name = "Ghost";
    offAgent.name = "Off";
    offAgent.active = false;
    keeper.name = "Keeper";
    keeper.agent = splash::AgentComponent{};
    ex.objects = {plain, ghost, offAgent, keeper};
    splash::ExportOptions dry;
    dry.dryRun = true;
    const splash::ExportResult res = splash::exportSplashpack(ex, project, project / "e.splashpack", dry);
    CHECK(res.ok() && res.stats.gameObjects == 2 && res.stats.agents.size() == 2);
    if (res.stats.agents.size() == 2) {
        CHECK(res.stats.agents[0].object == "Ghost" && res.stats.agents[0].gameObject == 0 && res.stats.agents[0].flatIndex == 1 &&
              res.stats.agents[0].waypoints == 2 && res.stats.agents[0].bytes == 52);
        CHECK(res.stats.agents[1].object == "Keeper" && res.stats.agents[1].gameObject == 1 && res.stats.agents[1].bytes == 28);
    }
    // No nav regions: warned.
    CHECK(std::any_of(res.warnings.begin(), res.warnings.end(),
                      [](const std::string& m) { return m.find("can't move: the scene has no nav regions") != std::string::npos; }));
}

// What the skin section of the inspector shows (ExportStats::skins): one
// entry per skinned mesh with its bones, triangles and clips, and byte counts
// that match the skin table layout psxsplash reads (recomputed here from the
// format, not from the writer).
void testSkinInfo() {
    namespace fs = std::filesystem;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "skin";
    const fs::path dir = g_outDir / "skininfo";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::ExportOptions dry;
    dry.dryRun = true;
    const splash::Mesh mesh = splash::loadMesh(project / "arm.mesh");
    CHECK(mesh.skin.has_value());
    if (!mesh.skin) return;
    const size_t bones = mesh.skin->joints.size();
    auto pad = [](size_t n, size_t a) { return (n + a - 1) / a * a; };

    auto exportAt = [&](int fps) {
        splash::Scene sc = splash::loadScene(project / "skin.scene");
        for (splash::Object& o : sc.objects)
            if (o.skin) o.skin->fps = fps;
        return splash::exportSplashpack(sc, project, dir / ("skin" + std::to_string(fps) + ".splashpack"), dry);
    };
    auto checkLayout = [&](const splash::ExportResult& r, int fps) {
        CHECK(r.ok());
        CHECK(r.stats.skins.size() == 1);
        if (!r.ok() || r.stats.skins.size() != 1) return;
        const splash::ExportStats::SkinInfo& s = r.stats.skins[0];
        CHECK(s.object == "Arm");
        CHECK(size_t(s.bones) == bones);
        size_t meshTris = 0;
        for (const auto& sm : mesh.submeshes) meshTris += sm.size() / 3;
        CHECK(s.triangles > 0 && s.triangles == meshTris);
        CHECK(s.clips.size() == 2);
        if (s.clips.size() != 2) return;
        CHECK(s.clips[0].name == "bend" && s.clips[1].name == "raise");
        // The skin table starts 4-aligned and each entry is 12 bytes, so the
        // data block starts 4-aligned too: no padding before it.
        size_t expect = 12 + 4 + pad(s.triangles * 3, 4);
        size_t clipSum = 0;
        for (const auto& c : s.clips) {
            CHECK(c.fps == fps);
            CHECK(c.frames > 0);
            // name length, name, terminator, loop, fps; pad to 2; frame count; matrices
            const size_t head = pad(1 + c.name.size() + 1 + 1 + 1, 2) + 2;
            const size_t want = head + size_t(c.frames) * bones * 24;
            CHECK(c.bytes == want);
            if (c.bytes != want) std::printf("  clip %s bytes %zu want %zu\n", c.name.c_str(), c.bytes, want);
            clipSum += c.bytes;
            expect += want;  // every clip ends 2-aligned, so the next starts unpadded
        }
        CHECK(clipSum < s.bytes);
        expect = pad(expect, 2) + bones * 6;  // bind joint positions
        for (const splash::Joint& j : mesh.skin->joints) expect += 1 + j.name.size() + 1;
        expect += s.object.size() + 1;
        CHECK(s.bytes == expect);
        if (s.bytes != expect) std::printf("  skin bytes %zu want %zu\n", s.bytes, expect);
    };

    const splash::ExportResult r15 = exportAt(15);
    checkLayout(r15, 15);
    const splash::ExportResult r30 = exportAt(30);
    checkLayout(r30, 30);
    if (r15.stats.skins.size() != 1 || r30.stats.skins.size() != 1) return;
    const auto& a = r15.stats.skins[0];
    const auto& b = r30.stats.skins[0];
    CHECK(b.bytes > a.bytes);
    CHECK(a.clips.size() == b.clips.size());
    for (size_t i = 0; i < a.clips.size() && i < b.clips.size(); i++) {
        CHECK(b.clips[i].frames > a.clips[i].frames);
        CHECK(b.clips[i].bytes > a.clips[i].bytes);
    }
}

void testRoomsErrors() {
    using namespace roomtest;
    const fs::path project = g_outDir / "rooms-errors";
    fs::remove_all(project);
    const splash::Scene base = twoRooms(project);
    const fs::path out = project / "scene.splashpack";

    // 31 rooms export; 32 do not (psxsplash's visited mask covers rooms < 32).
    splash::Scene many = base;
    for (int i = 0; i < 29; i++)
        many.objects.push_back(roomObject("Extra " + std::to_string(i), {float(10 + 2 * i), 1, -20}, {1, 1, 1}));
    splash::ExportResult ok31 = splash::exportSplashpack(many, project, out);
    CHECK(ok31.ok() && ok31.stats.rooms.size() == 31);
    many.objects.push_back(roomObject("Extra 29", {80, 1, -20}, {1, 1, 1}));
    splash::ExportResult bad32 = splash::exportSplashpack(many, project, out);
    CHECK(!bad32.ok() && hasError(bad32, "32 Rooms, at most 31"));

    // A portal naming a room that does not exist, or the same room twice.
    splash::Scene missing = base;
    for (splash::Object& o : missing.objects)
        if (o.portal) o.portal->roomB = "Room C";
    splash::ExportResult noRoom = splash::exportSplashpack(missing, project, out);
    CHECK(!noRoom.ok() && hasError(noRoom, "portal Door") && hasError(noRoom, "'Room C'"));
    for (splash::Object& o : missing.objects)
        if (o.portal) o.portal->roomB = "Room A";
    splash::ExportResult same = splash::exportSplashpack(missing, project, out);
    CHECK(!same.ok() && hasError(same, "portal Door: room A and room B are the same"));

    // Rooms and world streaming together.
    splash::Scene streaming = base;
    streaming.settings.streamWorld = true;
    splash::ExportResult st = splash::exportSplashpack(streaming, project, out);
    CHECK(!st.ok() && hasError(st, "Rooms and world streaming"));
}

// Room and Portal components survive save and load, unknown keys included.
// The editor's side of rooms: the pickers add and remove both kinds, a
// portal's room edit is one undo step, the inspector's warnings are the
// exporter's, and the viewport arrow's frame points from room A into room B.
void testRoomsEditor() {
    using namespace roomtest;
    using editor::ComponentKind;
    splash::Object o;
    o.name = "Hall";
    CHECK(editor::cannotAdd(o, ComponentKind::Room).empty() && editor::cannotAdd(o, ComponentKind::Portal).empty());
    CHECK(editor::addComponent(o, ComponentKind::Room) && o.room && o.room->size.x == 1 && o.room->offset.y == 0);
    CHECK(editor::addComponent(o, ComponentKind::Portal) && o.portal && o.portal->width == 2 && o.portal->height == 3 &&
          o.portal->roomA.empty());
    CHECK(editor::cannotAdd(o, ComponentKind::Room) == "Already added" && !editor::addComponent(o, ComponentKind::Portal));
    CHECK(editor::removeComponent(o, ComponentKind::Room) && !o.room && o.portal);
    CHECK(editor::removeComponent(o, ComponentKind::Portal) && !o.portal && !editor::removeComponent(o, ComponentKind::Portal));
    std::vector<std::pair<const char*, const char*>> items;
    for (const editor::ObjectPreset& p : editor::presets()) items.push_back({p.label, p.keywords});
    CHECK(std::string(editor::presets()[size_t(editor::rank("portal", items)[0])].label) == "Portal");
    CHECK(std::string(editor::presets()[size_t(editor::rank("room", items)[0])].label) == "Room");
    CHECK(std::string(editor::info(ComponentKind::Portal).label) == "Portal");

    const splash::Scene scene = twoRooms(g_outDir / "rooms-editor");
    editor::Document d;
    d.reset(scene);
    CHECK(d.edit({2}, [](splash::Object& ob) { ob.portal->roomB = ""; }));
    CHECK(d.edit({2}, [](splash::Object& ob) { ob.portal->roomA = "Room B"; }));
    CHECK(d.object({2})->portal->roomA == "Room B" && d.object({2})->portal->roomB.empty());
    CHECK(d.undo() && d.object({2})->portal->roomA == "Room A" && d.object({2})->portal->roomB.empty());
    CHECK(d.undo() && d.object({2})->portal->roomB == "Room B");

    auto problems = [](const splash::Scene& sc) {
        const std::vector<splash::FlatObject> flat = splash::flatten(sc);
        std::vector<const splash::FlatObject*> rooms;
        for (const splash::FlatObject& fo : flat)
            if (fo.object->room) rooms.push_back(&fo);
        return splash::portalProblems(rooms, flat[2]);
    };
    CHECK(problems(scene).empty());
    splash::Scene unset = scene;
    unset.objects[2].portal->roomB.clear();
    CHECK(problems(unset) == std::vector<std::string>{"portal Door room B: not set, pick a Room object"});
    unset.objects[2].portal->roomB = "Room A";
    CHECK(problems(unset) == std::vector<std::string>{"portal Door: room A and room B are the same Room 'Room A'"});

    // The door faces -X; A is at -3 and B at +3, so the frame turns to +X.
    const std::vector<splash::FlatObject> flat = splash::flatten(scene);
    splash::PortalFrame ab = splash::portalFrameAtoB(flat[2], flat[0], flat[1]);
    splash::PortalFrame ba = splash::portalFrameAtoB(flat[2], flat[1], flat[0]);
    CHECK(splash::portalWorldFrame(flat[2]).normal.x < -0.99f);
    CHECK(ab.normal.x > 0.99f && ba.normal.x < -0.99f && ab.right.z == -ba.right.z);
    CHECK(std::fabs(ab.halfW - 1) < 1e-5f && std::fabs(ab.halfH - 1) < 1e-5f);
}

// Renaming a Room carries the portals naming it, on either side and at any
// depth, as one undo step; a second Room still holding the old name keeps
// them where they are, and renaming a non-room touches none.
void testRoomRename() {
    using namespace roomtest;
    splash::Scene scene = twoRooms(g_outDir / "rooms-rename");
    splash::Object back;
    back.name = "Back door";
    back.portal = splash::PortalComponent{"Room B", "Room A", 2, 2, {}};
    scene.objects[1].children.push_back(back);  // {1, 0}: Room A on its B side
    auto a = [](const editor::Document& d, const editor::ObjectPath& p) { return d.object(p)->portal->roomA; };
    auto b = [](const editor::Document& d, const editor::ObjectPath& p) { return d.object(p)->portal->roomB; };

    editor::Document d;
    d.reset(scene);
    CHECK(d.renameObject({0}, "Hall") == 2);
    CHECK(d.object({0})->name == "Hall" && a(d, {2}) == "Hall" && b(d, {2}) == "Room B");
    CHECK(a(d, {1, 0}) == "Room B" && b(d, {1, 0}) == "Hall");
    CHECK(d.undo() && d.object({0})->name == "Room A" && a(d, {2}) == "Room A" && b(d, {1, 0}) == "Room A");
    CHECK(d.redo() && d.object({0})->name == "Hall" && a(d, {2}) == "Hall" && b(d, {1, 0}) == "Hall");
    CHECK(d.undo());
    CHECK(d.renameObject({1}, "Kitchen") == 2 && b(d, {2}) == "Kitchen" && a(d, {1, 0}) == "Kitchen");
    CHECK(d.undo());

    CHECK(d.renameObject({0}, "Room A") == -1 && d.renameObject({0}, "") == -1 && d.renameObject({99}, "X") == -1);
    CHECK(!d.canUndo());

    // A second Room named "Room A": the portals still resolve to it.
    splash::Scene twin = scene;
    twin.objects.push_back(roomObject("Room A", {0, 1, 9}, {2, 2, 2}));
    editor::Document t;
    t.reset(twin);
    CHECK(t.renameObject({0}, "Hall") == 0);
    CHECK(t.object({0})->name == "Hall" && a(t, {2}) == "Room A" && b(t, {1, 0}) == "Room A");

    // A non-room named like a room: only the name changes.
    splash::Scene plain = scene;
    plain.objects[3].name = "Room B";
    plain.objects[1].name = "Lobby";
    plain.objects[2].portal->roomB = "Room B";
    editor::Document n;
    n.reset(plain);
    CHECK(!n.object({3})->room && n.renameObject({3}, "Floor") == 0);
    CHECK(n.object({3})->name == "Floor" && b(n, {2}) == "Room B" && a(n, {1, 0}) == "Room B");
}

void testRoomsSceneRoundTrip() {
    using namespace roomtest;
    const fs::path dir = g_outDir / "rooms-roundtrip";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::Scene s;
    splash::Object room = roomObject("Hall", {1, 2, 3}, {4, 5, 6});
    room.room->offset = {0.5f, -1, 2};
    room.room->extra.push_back({"label", "\"Great hall\""});
    s.objects.push_back(room);
    splash::Object door;
    door.name = "Door";
    door.portal = splash::PortalComponent{"Hall", "Kitchen", 1.5f, 2.5f, {}};
    door.portal->extra.push_back({"future", "{\"a\":1}"});
    s.objects.push_back(door);
    splash::saveScene(s, dir / "a.scene");
    const splash::Scene l = splash::loadScene(dir / "a.scene");
    CHECK(l.objects.size() == 2);
    if (l.objects.size() != 2) return;
    const splash::Object& lr = l.objects[0];
    CHECK(lr.room && !lr.portal);
    CHECK(lr.room->size == (splash::Vec3{4, 5, 6}) && lr.room->offset == (splash::Vec3{0.5f, -1, 2}));
    CHECK(lr.room->extra.size() == 1 && lr.room->extra[0].first == "label" && lr.room->extra[0].second == "\"Great hall\"");
    const splash::Object& lp = l.objects[1];
    CHECK(lp.portal && !lp.room);
    CHECK(lp.portal->roomA == "Hall" && lp.portal->roomB == "Kitchen");
    CHECK(lp.portal->width == 1.5f && lp.portal->height == 2.5f);
    CHECK(lp.portal->extra.size() == 1 && lp.portal->extra[0].first == "future");
    CHECK(lp.unknownComponents.empty() && lr.unknownComponents.empty());
    splash::saveScene(l, dir / "b.scene");
    CHECK(readAll(dir / "a.scene") == readAll(dir / "b.scene"));

    // The world box honours offset and rotation; the portal frame honours scale.
    splash::Scene t;
    splash::Object rot = roomObject("Rot", {10, 0, 0}, {2, 2, 4});
    rot.room->offset = {0, 0, 1};
    rot.transform.rotation = {0, 0.70710678f, 0, 0.70710678f};  // +90 about Y: local +Z -> world +X
    t.objects.push_back(rot);
    splash::Object sp;
    sp.name = "Scaled";
    sp.transform.scale = {2, 3, 1};
    sp.portal = splash::PortalComponent{"Rot", "Rot", 2, 2, {}};
    t.objects.push_back(sp);
    const std::vector<splash::FlatObject> flat = splash::flatten(t);
    splash::Vec3 mn, mx;
    splash::roomWorldBox(flat[0], &mn, &mx);
    CHECK(std::fabs(mn.x - 9) < 1e-4f && std::fabs(mx.x - 13) < 1e-4f);
    CHECK(std::fabs(mn.z + 1) < 1e-4f && std::fabs(mx.z - 1) < 1e-4f);
    const splash::PortalFrame f = splash::portalWorldFrame(flat[1]);
    CHECK(std::fabs(f.halfW - 2) < 1e-5f && std::fabs(f.halfH - 3) < 1e-5f);
    CHECK(f.normal == (splash::Vec3{0, 0, 1}) && f.right == (splash::Vec3{1, 0, 0}) && f.up == (splash::Vec3{0, 1, 0}));
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

void testLuaScopes() {
    std::vector<std::string> l = luaLines(
        "function a() local x = 1 end\n"                                                   // 0
        "function b() return x end\n"                                                      // 1
        "function c(p) return p end\n"                                                     // 2
        "function d() return p end\n"                                                      // 3
        "for i = 1, 3 do print(i) end\n"                                                   // 4
        "print(i)\n"                                                                       // 5
        "local v = 1\n"                                                                    // 6
        "do\n"                                                                             // 7
        "  local v = v + 1\n"                                                              // 8
        "  print(v)\n"                                                                     // 9
        "end\n"                                                                            // 10
        "print(v)\n"                                                                       // 11
        "local function fact(n) if n < 2 then return 1 end return n * fact(n - 1) end\n"  // 12
        "repeat local done = true until done\n"                                            // 13
        "if v then local w = 1 else print(w) end\n");                                     // 14
    // The nth whole-word `w` on line `line`.
    auto at = [&](int line, const std::string& w, int nth = 0) {
        const std::string& s = l[line];
        auto ident = [](char ch) { return std::isalnum((unsigned char)ch) || ch == '_'; };
        for (size_t c = s.find(w); c != std::string::npos; c = s.find(w, c + 1))
            if ((c == 0 || !ident(s[c - 1])) && (c + w.size() >= s.size() || !ident(s[c + w.size()])) && nth-- == 0)
                return lua::Pos{line, (int)c};
        return lua::Pos{line, -1};
    };
    lua::Lookup r = lua::lookup(l, at(1, "x"));  // b's x: not a()'s local
    CHECK(!r.def);
    r = lua::lookup(l, at(2, "p", 1));
    CHECK(r.def && r.def->pos == (lua::Pos{2, 11}) && r.def->kind == lua::DefKind::Parameter);
    r = lua::lookup(l, at(3, "p"));  // c's parameter, not d's
    CHECK(!r.def);
    r = lua::lookup(l, at(4, "i", 1));
    CHECK(r.def && r.def->pos == (lua::Pos{4, 4}));
    r = lua::lookup(l, at(5, "i"));  // past the loop
    CHECK(!r.def);
    r = lua::lookup(l, at(8, "v", 1));  // local v = v + 1: the outer v
    CHECK(r.def && r.def->pos.line == 6);
    r = lua::lookup(l, at(8, "v"));  // the declaration itself
    CHECK(r.def && r.def->pos == at(8, "v"));
    r = lua::lookup(l, at(9, "v"));  // shadowed inside the do
    CHECK(r.def && r.def->pos.line == 8);
    r = lua::lookup(l, at(11, "v"));  // back to the outer one after `end`
    CHECK(r.def && r.def->pos.line == 6);
    r = lua::lookup(l, at(12, "fact", 1));  // recursion
    CHECK(r.def && r.def->pos == at(12, "fact") && r.def->kind == lua::DefKind::LocalFunction);
    r = lua::lookup(l, at(13, "done", 1));  // until sees the body's locals
    CHECK(r.def && r.def->pos == at(13, "done"));
    r = lua::lookup(l, at(14, "w", 1));  // the else branch does not see the then branch's local
    CHECK(!r.def);
    // A global assigned at file level, with the same name as another function's local.
    std::vector<std::string> g = luaLines("function a() local x = 1 end\nfunction b() return x end\nx = 5\n");
    r = lua::lookup(g, {1, 20});
    CHECK(r.def && r.def->pos == (lua::Pos{2, 0}) && r.def->kind == lua::DefKind::Global);
    // A statement after a local's: the local is in scope there.
    g = luaLines("local t = {}\nfunction t.go() return t end\nlocal f = function() return 1 end\nprint(f)\n");
    r = lua::lookup(g, {1, 23});
    CHECK(r.def && r.def->pos == (lua::Pos{0, 6}));
    r = lua::lookup(g, {3, 6});
    CHECK(r.def && r.def->pos == (lua::Pos{2, 6}));
    // Completion offers only the locals in scope.
    using K = lua::ItemKind;
    std::vector<std::string> cl = luaLines("function a(param) local secret = 1 end\nfunction b() return sec par end\n");
    lua::Completions c = lua::complete(cl, {1, 23});
    CHECK(!hasItem(c, "secret", K::Local) && !hasItem(c, "secret", K::Function));
    c = lua::complete(cl, {1, 27});
    CHECK(!hasItem(c, "param", K::Local) && !hasItem(c, "param", K::Function));
    cl = luaLines("function a(param) local secret = 1 return sec par end\n");
    CHECK(hasItem(lua::complete(cl, {0, 45}), "secret", K::Local));
    CHECK(hasItem(lua::complete(cl, {0, 49}), "param", K::Local));
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

// A name held in a variable: the literal assigned to it is rewritten when the
// same variable (Lua scope) is passed to a call of the right kind; any other
// literal spelling the old name is left and listed, path:line.
void testSeqRenameScriptVars() {
    namespace fs = std::filesystem;
    using editor::SeqKind;
    const fs::path dir = g_outDir / "seq_rename_vars";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scripts", ec);
    auto put = [&](const char* p, const std::string& text) { std::ofstream(dir / p, std::ios::binary) << text; };
    auto get = [&](const char* p) {
        std::ifstream in(dir / p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string scn =
        "local seq = \"intro\"\n"                            // 1 local, played: rewritten
        "Cutscene.Play(seq)\n"                               // 2
        "current = 'intro'\n"                                // 3 global, played: rewritten
        "function onSceneCreationEnd() Cutscene.Play(current) end\n"
        "local said = \"intro\" -- dialogue\n"               // 5 never played: left
        "print(said)\n"
        "local spin = \"intro\"\n"                           // 7 an animation's: left
        "Animation.Play(spin)\n"
        "local a, b = 1, \"intro\"\n"                        // 9 second of two: rewritten
        "Cutscene.Play(b, {loop = true})\n"
        "local t = {name = \"intro\"}\n"                     // 11 a table field: left
        "Cutscene.Play(t.name)\n"
        "do local cue = \"intro\" print(cue) end\n"          // 13 another variable of that name: left
        "cue = \"intro\"\n"                                  // 14 the global played: rewritten
        "Cutscene.Play(cue)\n"
        "Cutscene.Play(\"intro\")\n"                         // 16 a literal call: rewritten
        "local joined = \"intro\" .. \"!\"\n"                // 17 not the whole value: left
        "Cutscene.Play(joined)\n"
        "print(\"caf\xc3\xa9\") Animation.Play(\"intro\")\n"   // 19 an animation's: left, not listed
        "if Animation.IsPlaying(\"intro\") then end\n";      // 20 an animation's: left, not listed
    std::string want = scn;
    for (const char* line : {"local seq = \"", "current = '", "local a, b = 1, \"", "\ncue = \"", "Cutscene.Play(\""}) {
        const size_t at = want.find(line) + std::strlen(line);
        CHECK(want.compare(at, 5, "intro") == 0);
        want.replace(at, 5, "opening");
    }
    const std::string obj = "local s = \"intro\"\r\nCutscene.Play(s)\r\nprint(\"intro\")\r\n";
    put("scene.lua", scn);
    put("scripts/obj.lua", obj);
    // The string rewrite alone: the variables count with the calls.
    int n = -1;
    CHECK(editor::renameSeqInLua(scn, SeqKind::Cutscene, "intro", "opening", &n) == want && n == 5);
    // An animation rename takes the Animation variable, and none of the cutscene's.
    const std::string anim = editor::renameSeqInLua(scn, SeqKind::Animation, "intro", "turn", &n);
    CHECK(n == 3 && anim.find("local spin = \"turn\"\n") != std::string::npos && anim.find("local seq = \"intro\"") != std::string::npos);

    splash::Scene sc;
    splash::Object o;
    o.name = "Thing";
    o.script = splash::ScriptComponent{"scripts/obj.lua", {}};
    sc.objects = {o};
    sc.settings.script = "scene.lua";
    splash::Cutscene c;
    c.name = "intro";
    sc.cutscenes.push_back(c);
    splash::Animation an;
    an.name = "intro";
    sc.animations.push_back(an);
    editor::Document d;
    d.reset(sc, dir, "s");
    editor::ScriptRenames sr;
    CHECK(editor::renameSequence(d, {SeqKind::Cutscene, 0}, "opening", &sr));
    CHECK(sr.refs == 6 && sr.failed.empty() && (sr.files == std::vector<std::string>{"scripts/obj.lua", "scene.lua"}));
    CHECK(get("scene.lua") == want);
    CHECK(get("scripts/obj.lua") == "local s = \"opening\"\r\nCutscene.Play(s)\r\nprint(\"intro\")\r\n");
    // Lines 7, 19 and 20 name the animation "intro": not listed.
    CHECK((sr.leftovers == std::vector<std::string>{"scripts/obj.lua:3", "scene.lua:5", "scene.lua:11", "scene.lua:13",
                                                    "scene.lua:17"}));
    // Undo puts both files back, redo again.
    CHECK(d.undo() && get("scene.lua") == scn && get("scripts/obj.lua") == obj);
    CHECK(d.redo() && get("scene.lua") == want);
    CHECK(d.undo() && get("scene.lua") == scn);
    // Renaming the animation lists none of the cutscene's literals (lines 1, 3,
    // 9, 14, 16 and obj.lua:1), only the unattributed ones.
    CHECK(editor::renameSequence(d, {SeqKind::Animation, 0}, "turn", &sr));
    CHECK(sr.refs == 3 && (sr.files == std::vector<std::string>{"scene.lua"}) && get("scene.lua") == anim);
    CHECK((sr.leftovers == std::vector<std::string>{"scripts/obj.lua:3", "scene.lua:5", "scene.lua:11", "scene.lua:13",
                                                    "scene.lua:17"}));
    CHECK(d.undo() && get("scene.lua") == scn);
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

    // Built to the end: the fraction climbs to 1, past many points where it
    // looks for a cancel, which is what a cancel cuts short.
    float seenMid = -1;
    unsigned fullChecks = 0;
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
        fullChecks = b.checks();
        editor::StagedModel s = b.take();
        CHECK(s.error.empty() && s.model.triangles == 180000);
        editor::discardStaged(s);
    }
    const auto full = std::chrono::duration_cast<ms>(clock::now() - t0);
    std::printf("  terrain.glb builds in %lld ms, looking for a cancel %u times\n", (long long)full.count(), fullChecks);
    CHECK(seenMid > 0 && seenMid < 1);
    // Often enough that stopping at the next one is stopping soon.
    CHECK(fullChecks >= 16);

    // Cancelled once it is under way: it stops at the next point it looks
    // (one already past the look may still count itself), short of the end,
    // with nothing left in the temp folder. Measured in those points, not in
    // milliseconds a loaded machine stretches.
    {
        editor::StagedBuild b(editor::stagePlace(doc, outside / "terrain.glb"));
        while (b.fraction() <= 0 && !b.done()) std::this_thread::sleep_for(ms(1));
        CHECK(!b.done());
        const auto c0 = clock::now();
        b.cancel();
        const unsigned checksAtCancel = b.checks();
        const bool stopped = b.wait(ms(60000));
        const auto took = std::chrono::duration_cast<ms>(clock::now() - c0);
        const unsigned past = b.checks() - checksAtCancel;
        std::printf("  a cancelled build stops %u checks (%lld ms) after the cancel, at %.2f of the build\n", past,
                    (long long)took.count(), double(b.fraction()));
        CHECK(stopped);
        CHECK(past <= 1);
        CHECK(b.fraction() < 1.0f);
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
        // Never killed, the job runs into Blender's 120 s timeout (or the child's
        // 120 s sleep); 90 s leaves room for the two 30 s waits above on a loaded machine.
        CHECK(clock::now() - t0 < 90s);
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
// Convert to Lua: a blocks script becomes <stem>.lua (numbered when taken)
// holding the exporter's Lua, every reference following, as one undo step
// that brings the blocks back byte for byte. An errored script is refused.
void testBlocksConvert() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path proj = g_outDir / "blocksconvert" / "game";
    fs::remove_all(proj.parent_path(), ec);
    fs::create_directories(proj / "scripts", ec);
    auto write = [&](const std::string& rel, const std::string& text) { std::ofstream(proj / rel, std::ios::binary) << text; };
    auto slurp = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string door = splash::saveBlocks(blockScript({{"opened", "0"}}, {{blk("on_create"), blk("log", "", {bnum("1")})}}));
    const std::string broken = splash::saveBlocks(blockScript({}, {{blk("on_create"), blk("hide")}}));
    const splash::LuaGen gen = splash::blocksToLua(splash::parseBlocks(door, "scripts/door.blocks"));
    CHECK(gen.errors.empty() && !gen.lua.empty());
    write("scripts/door.blocks", door);
    write("scripts/gate.blocks", door);
    write("scripts/gate.lua", "-- the gate's own Lua\n");
    write("scripts/bad.blocks", broken);

    // The open scene uses door.blocks as a Script; another scene file as a
    // trigger and as its scene script.
    splash::Scene sc;
    splash::Object a = named("Door");
    a.script.emplace();
    a.script->lua = "scripts/door.blocks";
    splash::Object g = named("Gate");
    g.script.emplace();
    g.script->lua = "scripts/gate.blocks";
    splash::Object b = named("Bad");
    b.script.emplace();
    b.script->lua = "scripts/bad.blocks";
    sc.objects = {a, g, b};
    splash::saveScene(sc, proj / "game.scene");
    {
        splash::Scene other;
        other.settings.script = "scripts/door.blocks";
        splash::Object t = named("Zone");
        t.trigger.emplace();
        t.trigger->lua = "scripts/door.blocks";
        other.objects.push_back(t);
        splash::saveScene(other, proj / "other.scene");
    }
    const std::string otherBefore = slurp(proj / "other.scene");

    editor::AssetIndex index;
    index.scan(proj);
    editor::Document doc;
    doc.reset(sc, proj, "game");
    editor::takeAssetMoves();

    // An errored script is refused, nothing touched.
    {
        editor::AssetOp op = editor::convertBlocksToLua(doc, index, "game.scene", "scripts/bad.blocks");
        CHECK(op.error.rfind("Can't convert: ", 0) == 0 && op.error.find("no object chosen") != std::string::npos);
        CHECK(slurp(proj / "scripts/bad.blocks") == broken && !fs::exists(proj / "scripts/bad.lua"));
        CHECK(doc.object({2})->script->lua == "scripts/bad.blocks" && !doc.canUndo() && editor::takeAssetMoves().empty());
    }

    // Convert: the Lua the exporter makes, every reference follows.
    editor::AssetOp op = editor::convertBlocksToLua(doc, index, "game.scene", "scripts/door.blocks");
    CHECK(op.error.empty() && op.path == "scripts/door.lua");
    CHECK(!fs::exists(proj / "scripts/door.blocks") && slurp(proj / "scripts/door.lua") == gen.lua);
    CHECK(doc.object({0})->script->lua == "scripts/door.lua");
    {
        splash::Scene other = splash::loadScene(proj / "other.scene");
        CHECK(other.settings.script == "scripts/door.lua" && other.objects[0].trigger->lua == "scripts/door.lua");
        CHECK(op.scenes == std::vector<std::string>{"other.scene"} && op.references == 3);
        auto moved = editor::takeAssetMoves();
        CHECK(moved.size() == 1 && moved[0].first == proj / "scripts/door.blocks" && moved[0].second == proj / "scripts/door.lua");
    }

    // Undo: the blocks back byte for byte, every reference with them.
    CHECK(doc.undo());
    CHECK(slurp(proj / "scripts/door.blocks") == door && !fs::exists(proj / "scripts/door.lua"));
    CHECK(doc.object({0})->script->lua == "scripts/door.blocks" && slurp(proj / "other.scene") == otherBefore);
    {
        auto moved = editor::takeAssetMoves();
        CHECK(moved.size() == 1 && moved[0].first == proj / "scripts/door.lua" && moved[0].second == proj / "scripts/door.blocks");
    }
    CHECK(editor::takeAssetHistoryError().empty());
    // Redo converts again.
    CHECK(doc.redo());
    CHECK(!fs::exists(proj / "scripts/door.blocks") && slurp(proj / "scripts/door.lua") == gen.lua);
    CHECK(doc.object({0})->script->lua == "scripts/door.lua" &&
          splash::loadScene(proj / "other.scene").objects[0].trigger->lua == "scripts/door.lua");
    CHECK(editor::takeAssetHistoryError().empty());
    editor::takeAssetMoves();

    // A taken name: gate.lua is there, so gate.blocks becomes gate_2.lua.
    index.scan(proj);
    op = editor::convertBlocksToLua(doc, index, "game.scene", "scripts/gate.blocks");
    CHECK(op.error.empty() && op.path == "scripts/gate_2.lua");
    CHECK(slurp(proj / "scripts/gate.lua") == "-- the gate's own Lua\n" && slurp(proj / "scripts/gate_2.lua") == gen.lua);
    CHECK(!fs::exists(proj / "scripts/gate.blocks") && doc.object({1})->script->lua == "scripts/gate_2.lua");
    CHECK(doc.undo() && slurp(proj / "scripts/gate.blocks") == door && doc.object({1})->script->lua == "scripts/gate.blocks");
    CHECK(slurp(proj / "scripts/gate.lua") == "-- the gate's own Lua\n" && !fs::exists(proj / "scripts/gate_2.lua"));
    // Not a blocks script: refused.
    index.scan(proj);
    CHECK(editor::convertBlocksToLua(doc, index, "game.scene", "scripts/gate.lua").error.rfind("Can't convert: ", 0) == 0);
    editor::takeAssetMoves();
}

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
    // Set up external editor's lua-language-server definitions are no script.
    CHECK(editor::classifyAsset("psxsplash.d.lua") == T::Other && editor::classifyAsset("d.lua") == T::Script);
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

// The scene's loading canvas follows a rename of that canvas in the same undo
// step; renaming another canvas or deleting it leaves the name alone.
void testUiLoadingRename() {
    using namespace editor::uic;
    editor::Document d;
    splash::Scene sc = uiScene();
    sc.settings.loadingCanvas = "menu";
    d.reset(sc);
    CHECK(editCanvas(d, 1, [](splash::UICanvas& cv) { cv.name = "Loading"; }));
    CHECK(d.scene().settings.loadingCanvas == "Loading" && d.historySize() == 1);
    CHECK(d.undo() && d.scene().canvases[1].name == "menu" && d.scene().settings.loadingCanvas == "menu");
    CHECK(d.redo() && d.scene().settings.loadingCanvas == "Loading");
    CHECK(editCanvas(d, 0, [](splash::UICanvas& cv) { cv.name = "hud2"; }) && d.scene().settings.loadingCanvas == "Loading");
    CHECK(editCanvas(d, 1, [](splash::UICanvas& cv) { cv.visible = false; }) && d.scene().settings.loadingCanvas == "Loading");
    CHECK(deleteCanvas(d, 1) && d.scene().canvases.size() == 1 && d.scene().settings.loadingCanvas == "Loading");
    CHECK(d.undo() && d.scene().canvases.size() == 2 && d.scene().settings.loadingCanvas == "Loading");
    // A setting changed after a UI edit comes back on undo, the UI edit's own name with it.
    d.editSettings([](splash::SceneSettings& st) { st.loadingCanvas = "hud2"; });
    CHECK(editCanvas(d, 0, [](splash::UICanvas& cv) { cv.name = "HUD"; }) && d.scene().settings.loadingCanvas == "HUD");
    CHECK(d.undo() && d.scene().settings.loadingCanvas == "hud2");
    CHECK(d.undo() && d.scene().settings.loadingCanvas == "Loading");
    // No loading canvas: a rename sets none.
    d.reset(uiScene());
    CHECK(editCanvas(d, 0, [](splash::UICanvas& cv) { cv.name = "x"; }) && d.scene().settings.loadingCanvas.empty());
}

// The Scene fonts: add (numbered names, at most three), edit, rename (every
// text element that uses the font follows, refusals), delete (its text
// elements go back to the system font), each one undo step that carries the
// fonts and the canvases together; and the rendered sheets follow the edits.
void testUiFonts() {
    using namespace editor::uic;
    editor::Document d;
    splash::Scene sc = uiScene();
    splash::UIElement t0 = uiBox("t0", 0, 0, 50, 16), t1 = uiBox("t1", 0, 20, 50, 16);
    t0.type = t1.type = splash::UIElementType::Text;
    t0.text = t1.text = "Hi";
    t0.font = t1.font = "font";
    sc.canvases[0].elements.push_back(t0);
    sc.canvases[1].elements.push_back(t1);
    d.reset(sc);
    CHECK(addFont(d, "fonts/a.ttf") == 0 && d.scene().fonts[0].name == "font" && d.scene().fonts[0].source == "fonts/a.ttf");
    CHECK(addFont(d, "fonts/b.png") == 1 && d.scene().fonts[1].name == "font (2)" && d.scene().fonts[1].bitmap == "fonts/b.png" &&
          d.scene().fonts[1].source.empty());
    CHECK(addFont(d, "") == 2 && d.scene().fonts[2].name == "font (3)" && d.scene().fonts[2].source.empty());
    CHECK(d.historySize() == 3);
    CHECK(addFont(d, "fonts/a.ttf") == -1 && d.scene().fonts.size() == 3 && d.historySize() == 3);
    CHECK(d.undo() && d.scene().fonts.size() == 2 && d.redo() && d.scene().fonts.size() == 3);
    // Edits: the name stays; one merge key is one step.
    CHECK(editFont(d, 0, [](splash::UIFont& f) { f.size = 20; f.name = "sneaky"; }, "k") && d.scene().fonts[0].name == "font");
    CHECK(editFont(d, 0, [](splash::UIFont& f) { f.size = 24; }, "k") && d.scene().fonts[0].size == 24 && d.historySize() == 4);
    d.endMerge();
    CHECK(!editFont(d, 0, [](splash::UIFont& f) { f.size = 24; }) && !editFont(d, 7, [](splash::UIFont&) {}));
    setFontFile(d.sceneMut().fonts[2], "x.ttf");
    CHECK(d.scene().fonts[2].source == "x.ttf" && d.scene().fonts[2].bitmap.empty());
    // Rename: both canvases' text elements follow, in the same step.
    CHECK(renameFont(d, 0, "title") && d.scene().fonts[0].name == "title");
    CHECK(d.scene().canvases[0].elements.back().font == "title" && d.scene().canvases[1].elements.back().font == "title");
    CHECK(d.historySize() == 5);
    CHECK(d.undo() && d.scene().fonts[0].name == "font" && d.scene().canvases[0].elements.back().font == "font" &&
          d.scene().canvases[1].elements.back().font == "font");
    CHECK(d.redo() && d.scene().canvases[1].elements.back().font == "title");
    // Refused: a name another font has, an empty one; nothing changes.
    std::string why;
    CHECK(!renameFont(d, 1, "title", &why) && why.find("already") != std::string::npos && d.scene().fonts[1].name == "font (2)");
    why.clear();
    CHECK(!renameFont(d, 1, "", &why) && !why.empty() && d.historySize() == 5);
    CHECK(!renameFont(d, 0, "title", &why));  // unchanged: no step
    CHECK(uniqueFontName(d.scene().fonts, "font (2)") == "font (4)" && fontUses(d.scene(), "title") == 2);
    // Delete: the text elements that used it go back to the system font.
    CHECK(deleteFont(d, 0) && d.scene().fonts.size() == 2 && d.scene().fonts[0].name == "font (2)");
    CHECK(d.scene().canvases[0].elements.back().font.empty() && d.scene().canvases[1].elements.back().font.empty());
    CHECK(d.undo() && d.scene().fonts.size() == 3 && d.scene().fonts[0].name == "title" &&
          d.scene().canvases[0].elements.back().font == "title" && d.scene().canvases[1].elements.back().font == "title");
    CHECK(!deleteFont(d, 5));
    // Undo puts the selection back too, and a UI element edit keeps the fonts.
    CHECK(editElement(d, 0, 0, [](splash::UIElement& e) { e.name = "b2"; }) && d.scene().fonts.size() == 3);
    CHECK(d.undo() && d.scene().fonts.size() == 3 && d.scene().fonts[0].name == "title");

    // The sheets: a rename, an edit and a delete-then-add under the same name show the new font.
    const std::filesystem::path src = SPLASHEDIT_SOURCE_DIR;
    const std::filesystem::path project = g_outDir / "uifonts";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);
    std::filesystem::copy_file(src / "assets" / "fonts" / "Inter-Regular.ttf", project / "inter.ttf");
    editor::Document e;
    splash::Scene es = uiScene();
    es.canvases[0].elements.push_back(t0);
    e.reset(es);
    Assets assets;
    assets.refresh(project, 0);
    CHECK(addFont(e, "inter.ttf") == 0);
    CHECK(editFont(e, 0, [](splash::UIFont& f) { f.size = 12; }));
    const splash::FontSheet* a = assets.font(e.scene(), "font");
    CHECK(a != nullptr);
    const int h12 = a ? a->glyphHeight : 0;
    CHECK(editFont(e, 0, [](splash::UIFont& f) { f.size = 32; }));
    const splash::FontSheet* b = assets.font(e.scene(), "font");
    CHECK(b && b->glyphHeight > h12);
    CHECK(renameFont(e, 0, "big") && assets.font(e.scene(), "font") == nullptr && assets.font(e.scene(), "big") != nullptr);
    CHECK(e.scene().canvases[0].elements.back().font == "big");
    CHECK(deleteFont(e, 0) && addFont(e, "inter.ttf") == 0 && renameFont(e, 0, "big"));
    const splash::FontSheet* c = assets.font(e.scene(), "big");
    CHECK(c && e.scene().fonts[0].size == 16);
    CHECK(c && b && c->glyphHeight < 32 && c->glyphHeight != b->glyphHeight);
    std::string err;
    CHECK(editFont(e, 0, [](splash::UIFont& f) { f.source = "missing.ttf"; }) && !assets.font(e.scene(), "big", &err) && !err.empty());
    // The sample line: drawn by render(), as wide as the text, some of it lit.
    CHECK(editFont(e, 0, [](splash::UIFont& f) { f.source = "inter.ttf"; }));
    Screen size;
    std::vector<uint32_t> px = renderSample(e.scene(), assets, "big", "AaBb", 0, &size);
    CHECK(size.w == textWidth(e.scene(), assets, "big", "AaBb") && size.w > 8 && px.size() == size_t(size.w) * size_t(size.h));
    CHECK(std::count(px.begin(), px.end(), 0xffffffffu) > 10);
}

// A canvas's default font (PSXCanvas.defaultFont): the scene file keeps it
// (null for the system font, never in the extras); a text element without a
// font of its own resolves to it; font rename and delete rewrite it in the
// same undo step as the elements; the preview draws with it; a default font
// the scene does not have is a canvas problem.
void testUiCanvasDefaultFont() {
    using namespace editor::uic;
    namespace fs = std::filesystem;
    // Scene file round trip.
    const fs::path dir = g_outDir / "cvfont";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::Scene rt = uiScene();
    rt.canvases[0].defaultFont = "font (2)";
    splash::saveScene(rt, dir / "rt.scene");
    std::string text;
    {
        std::ifstream in(dir / "rt.scene", std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("\"defaultFont\": \"font (2)\"") != std::string::npos);
    CHECK(text.find("\"defaultFont\": null") != std::string::npos);  // the second canvas: the system font
    const splash::Scene back = splash::loadScene(dir / "rt.scene");
    CHECK(back.canvases.size() == 2 && back.canvases[0].defaultFont == "font (2)" && back.canvases[1].defaultFont.empty());
    CHECK(back.canvases[0].extra.empty() && back.canvases[1].extra.empty());
    // An unknown key still goes to the extras, and back out.
    {
        std::string withExtra = text;
        const size_t at = withExtra.find("\"defaultFont\": \"font (2)\"");
        withExtra.insert(at, "\"futureKey\": 7, ");
        std::ofstream(dir / "extra.scene", std::ios::binary) << withExtra;
        const splash::Scene ex = splash::loadScene(dir / "extra.scene");
        CHECK(ex.canvases[0].defaultFont == "font (2)" && ex.canvases[0].extra.size() == 1 &&
              ex.canvases[0].extra[0].first == "futureKey");
    }

    // The resolver.
    splash::UICanvas cv;
    splash::UIElement el;
    el.type = splash::UIElementType::Text;
    CHECK(splash::effectiveFont(cv, el).empty());
    cv.defaultFont = "font (2)";
    CHECK(splash::effectiveFont(cv, el) == "font (2)");
    el.font = "font";
    CHECK(splash::effectiveFont(cv, el) == "font");

    // Rename and delete.
    editor::Document d;
    splash::Scene sc = uiScene();
    splash::UIElement t0 = uiBox("t0", 0, 0, 50, 16);
    t0.type = splash::UIElementType::Text;
    t0.text = "Hi";
    sc.canvases[0].elements.push_back(t0);
    d.reset(sc);
    CHECK(addFont(d, "fonts/a.ttf") == 0 && addFont(d, "fonts/b.ttf") == 1 && d.scene().fonts[1].name == "font (2)");
    CHECK(editCanvas(d, 0, [](splash::UICanvas& c) { c.defaultFont = "font (2)"; }) && d.scene().canvases[0].defaultFont == "font (2)");
    const size_t h0 = d.historySize();
    CHECK(fontUses(d.scene(), "font (2)") == 1 && fontUses(d.scene(), "font") == 0);
    CHECK(d.undo() && d.scene().canvases[0].defaultFont.empty() && d.redo() && d.scene().canvases[0].defaultFont == "font (2)");
    CHECK(renameFont(d, 1, "body") && d.scene().canvases[0].defaultFont == "body" && d.scene().canvases[1].defaultFont.empty());
    CHECK(d.historySize() == h0 + 1);
    CHECK(d.undo() && d.scene().canvases[0].defaultFont == "font (2)" && d.scene().fonts[1].name == "font (2)");
    CHECK(d.redo() && d.scene().canvases[0].defaultFont == "body");
    CHECK(canvasProblems(d, 0).empty());
    CHECK(deleteFont(d, 1) && d.scene().canvases[0].defaultFont.empty() && d.historySize() == h0 + 2);
    CHECK(d.undo() && d.scene().canvases[0].defaultFont == "body" && d.scene().fonts.size() == 2);
    // A default font the scene does not have: the canvas says so.
    CHECK(editCanvas(d, 0, [](splash::UICanvas& c) { c.defaultFont = "gone"; }));
    bool reported = false;
    for (const Problem& p : canvasProblems(d, 0)) reported |= p.title == "No such default font" && p.body.find("'gone'") != std::string::npos;
    CHECK(reported);
    CHECK(canvasProblems(d, 1).empty());

    // The preview draws a fontless text element with its canvas's default.
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    fs::copy_file(src / "assets" / "fonts" / "Inter-Regular.ttf", dir / "inter.ttf");
    Assets assets;
    assets.refresh(dir, 0);
    splash::Scene ps;
    splash::UIFont uf;
    uf.name = "inter";
    uf.source = "inter.ttf";
    uf.size = 16;
    ps.fonts.push_back(uf);
    splash::UICanvas pc;
    pc.name = "p";
    splash::UIElement pt = uiBox("t", 0, 0, 200, 20);
    pt.type = splash::UIElementType::Text;
    pt.text = "Hello Wq";
    pt.color = {1, 1, 1};
    pc.elements.push_back(pt);
    ps.canvases.push_back(pc);
    Screen s;
    const std::vector<uint32_t> sys = render(ps, {true}, s, assets, 0);
    ps.canvases[0].defaultFont = "inter";
    const std::vector<uint32_t> viaDefault = render(ps, {true}, s, assets, 0);
    ps.canvases[0].defaultFont.clear();
    ps.canvases[0].elements[0].font = "inter";
    const std::vector<uint32_t> own = render(ps, {true}, s, assets, 0);
    CHECK(sys != own);  // the control: the font draws differently from the system font
    CHECK(viaDefault == own);
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

// The loader pack (scene_<i>.loading) read back the way psxsplash reads it:
// LoadingScreen::load / uploadTextures and UISystem::loadFromSplashpack.
void testLoaderPack() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "loading";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "examples" / "courtyard", project, fs::copy_options::recursive);
    fs::copy_file(src / "assets" / "fonts" / "Inter-Regular.ttf", project / "inter.ttf");
    splash::Image img;
    img.width = img.height = 16;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            img.pixels.push_back({float((x / 4) & 1), float((y / 4) & 1), 0.5f, 1});
    splash::savePng(img, project / "logo.png");

    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    splash::UIFont font;
    font.name = "inter";
    font.source = "inter.ttf";
    font.size = 12;
    scene.fonts.push_back(font);
    splash::UICanvas hud;
    hud.name = "hud";
    hud.elements.push_back(uiBox("hp", 4, 4, 40, 8));
    scene.canvases.push_back(hud);
    splash::UICanvas ld;
    ld.name = "Loading";
    ld.elements.push_back(uiBox("bg", 0, 0, 320, 240));
    ld.elements.back().color = {0.1f, 0.2f, 0.6f};
    splash::UIElement bar = uiBox("loading", 40, 200, 240, 8);
    bar.type = splash::UIElementType::Progress;
    ld.elements.push_back(bar);
    splash::UIElement text = uiBox("title", 40, 100, 200, 16);
    text.type = splash::UIElementType::Text;
    text.text = "Loading...";
    text.font = "inter";
    ld.elements.push_back(text);
    splash::UIElement im = uiBox("logo", 150, 40, 16, 16);
    im.type = splash::UIElementType::Image;
    im.texture = "logo.png";
    im.bitDepth = splash::BitDepth::Bpp4;
    ld.elements.push_back(im);
    scene.canvases.push_back(ld);
    scene.settings.loadingCanvas = "Loading";

    // Scene file round trip keeps the name; an unset one is not written.
    splash::saveScene(scene, project / "loading.scene");
    CHECK(splash::loadScene(project / "loading.scene").settings.loadingCanvas == "Loading");
    CHECK(readAll(project / "loading.scene").find("loadingCanvas") != std::string::npos);
    splash::Scene plain = scene;
    plain.settings.loadingCanvas.clear();
    splash::saveScene(plain, project / "plain.scene");
    CHECK(readAll(project / "plain.scene").find("loadingCanvas") == std::string::npos);

    const fs::path out = project / "scene_0.loading";
    splash::ExportResult r = splash::exportLoaderPack(scene, project, out);
    for (const std::string& e : r.errors) std::fprintf(stderr, "loader pack: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(out);
    CHECK(b.size() > 16);
    if (!r.ok() || b.size() <= 16) return;
    auto in = [&](size_t off, size_t n) { return off != 0 && off % 2 == 0 && off + n <= b.size(); };
    auto fb = [](int x, int y, int w, int h) {
        auto hit = [&](int ax, int ay, int aw, int ah) { return ax < x + w && x < ax + aw && ay < y + h && y < ay + ah; };
        return hit(0, 0, 320, 240) || hit(0, 256, 320, 240);
    };
    CHECK(b[0] == 'L' && b[1] == 'P' && rd16(b, 2) == 2);
    const size_t fontCount = uint8_t(b[4]), canvasCount = uint8_t(b[5]);
    CHECK(fontCount == 1 && canvasCount == 1 && rd16(b, 6) == 320 && rd16(b, 8) == 240);
    const size_t atlasCount = uint8_t(b[10]), clutCount = uint8_t(b[11]);
    const size_t table = size_t(rd32(b, 12));
    CHECK(atlasCount == 1 && clutCount == 1);
    CHECK(in(table, fontCount * 112 + canvasCount * 12) && table % 4 == 0);
    int atlasX = -1, atlasY = -1, atlasW = 0;
    for (size_t i = 0; i < atlasCount; i++) {
        const size_t at = 16 + 12 * i;
        const size_t off = size_t(rd32(b, at));
        const int w = rd16(b, at + 4), h = rd16(b, at + 6), x = rd16(b, at + 8), y = rd16(b, at + 10);
        CHECK(in(off, size_t(w) * size_t(h) * 2));
        CHECK(w > 0 && h > 0 && x + w <= 1024 && y + h <= 512 && !fb(x, y, w, h));
        atlasX = x, atlasY = y, atlasW = w;
    }
    int clutX = -1, clutY = -1;
    for (size_t i = 0; i < clutCount; i++) {
        const size_t at = 16 + 12 * atlasCount + 12 * i;
        const size_t off = size_t(rd32(b, at));
        clutX = rd16(b, at + 4), clutY = rd16(b, at + 6);
        const int len = rd16(b, at + 8);
        CHECK(len >= 4 && len <= 16 && in(off, size_t(len) * 2));  // 4bpp, padded to 4 entries
        CHECK(clutX * 16 + len <= 1024 && clutY < 512 && !fb(clutX * 16, clutY, len, 1));
    }
    // Fonts: the sheet's 4bpp texels at dataOffset, 64 words x textureH.
    const std::vector<uint8_t> sheet = splash::buildFont(font, project).packed4bpp();
    for (size_t i = 0; i < fontCount; i++) {
        const size_t at = table + 112 * i;
        const int vx = rd16(b, at + 2), vy = rd16(b, at + 4), th = rd16(b, at + 6);
        const size_t off = size_t(rd32(b, at + 8)), size = size_t(rd32(b, at + 12));
        CHECK(off != 0);
        CHECK(off == 0 || (in(off, size) && size == sheet.size() && size == size_t(th) * 128 &&
              std::memcmp(b.data() + off, sheet.data(), sheet.size()) == 0));
        CHECK(!fb(vx, vy, 64, th) && vx + 64 <= 1024 && vy + th <= 464);
    }
    // The canvas descriptor follows the font descriptors with no gap.
    const size_t cv = table + fontCount * 112;
    const size_t elems = size_t(rd32(b, cv)), nameLen = uint8_t(b[cv + 4]), count = uint8_t(b[cv + 6]);
    const size_t nameOff = size_t(rd32(b, cv + 8));
    CHECK(nameLen == 7 && count == 4 && in(elems, count * 48) && (uint8_t(b[cv + 7]) & 1) &&
          nameOff + 8 <= b.size() && b.compare(nameOff, 8, std::string("Loading\0", 8)) == 0);
    if (!in(elems, count * 48)) return;
    auto nameOf = [&](size_t e) {
        const size_t off = size_t(rd32(b, e + 4)), len = uint8_t(b[e + 2]);
        return off + len < b.size() ? b.substr(off, len) : std::string();
    };
    int found = 0;
    for (size_t k = 0; k < count; k++) {
        const size_t e = elems + 48 * k;
        const std::string name = nameOf(e);
        if (name == "loading") {
            found++;
            CHECK(uint8_t(b[e]) == 3);
            CHECK(rdi16(b, e + 8) == 40 && rdi16(b, e + 10) == 200 && rdi16(b, e + 12) == 240 && rdi16(b, e + 14) == 8);
        } else if (name == "title") {
            const size_t t = size_t(rd32(b, e + 40));
            CHECK(uint8_t(b[e]) == 2 && uint8_t(b[e + 24]) == 1 && uint8_t(b[e + 24]) <= fontCount);
            CHECK(t + 11 <= b.size() && b.compare(t, 11, std::string("Loading...\0", 11)) == 0);
        } else if (name == "logo") {
            // 4bpp: the texture page and CLUT the atlas and CLUT records give.
            CHECK(uint8_t(b[e]) == 0 && uint8_t(b[e + 34]) == 0);
            CHECK(uint8_t(b[e + 24]) == atlasX / 64 && uint8_t(b[e + 25]) == atlasY / 256 && atlasW > 0);
            CHECK(rd16(b, e + 26) == clutX && rd16(b, e + 28) == clutY);
        }
    }
    CHECK(found == 1);

    // The splashpack leaves the loading canvas (and its image) out: one
    // canvas fewer, and the same pack and .vram as a scene without it.
    splash::ExportResult sr = splash::exportSplashpack(scene, project, project / "with.splashpack");
    CHECK(sr.ok());
    CHECK(rd16(readAll(project / "with.splashpack"), 92) == scene.canvases.size() - 1);
    splash::Scene without = scene;
    without.canvases.pop_back();
    without.settings.loadingCanvas.clear();
    CHECK(splash::exportSplashpack(without, project, project / "without.splashpack").ok());
    CHECK(readAll(project / "with.vram") == readAll(project / "without.vram"));
    // Without a loading canvas every canvas stays in the pack.
    CHECK(splash::exportSplashpack(plain, project, project / "plain.splashpack").ok());
    CHECK(rd16(readAll(project / "plain.splashpack"), 92) == scene.canvases.size());

    // A name that matches no canvas is an error from both exports.
    splash::Scene missing = scene;
    missing.settings.loadingCanvas = "Nope";
    CHECK(hasError(splash::exportLoaderPack(missing, project, project / "nope.loading"), "'Nope'"));
    CHECK(hasError(splash::exportSplashpack(missing, project, project / "nope.splashpack"), "'Nope'"));
    CHECK(!fs::exists(project / "nope.loading"));

    // Play writes scene_0.loading with the pack, and removes it once unset.
    const fs::path dir = g_outDir / "loadingplay";
    fs::remove_all(dir);
    splash::ExportResult pr = editor::exportForPlay(scene, project, dir);
    CHECK(pr.ok() && readAll(dir / "scene_0.loading") == b);
    pr = editor::exportForPlay(plain, project, dir);
    CHECK(pr.ok() && fs::exists(dir / "scene_0.splashpack") && !fs::exists(dir / "scene_0.loading"));
}

// A canvas's default font in the export: a text element with no font of its
// own gets its canvas's default font's index, its own font wins, a default
// font the scene does not have is an error naming the canvas; the loader pack
// carries the default font of its canvas.
void testExportCanvasDefaultFont() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "cvfontexport";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "examples" / "courtyard", project, fs::copy_options::recursive);
    fs::copy_file(src / "assets" / "fonts" / "Inter-Regular.ttf", project / "inter.ttf");

    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    scene.canvases.clear();
    scene.settings.loadingCanvas.clear();
    splash::UIFont font;
    font.source = "inter.ttf";
    font.size = 12;
    font.name = "font";
    scene.fonts.push_back(font);
    font.name = "font (2)";
    font.size = 14;
    scene.fonts.push_back(font);
    auto textEl = [](const char* name, const char* f) {
        splash::UIElement t = uiBox(name, 8, 8, 100, 16);
        t.type = splash::UIElementType::Text;
        t.text = "Hi";
        t.font = f;
        return t;
    };
    splash::UICanvas hud;
    hud.name = "hud";
    hud.defaultFont = "font (2)";
    hud.elements.push_back(textEl("dflt", ""));
    hud.elements.push_back(textEl("own", "font"));
    scene.canvases.push_back(hud);
    splash::UICanvas plain;
    plain.name = "plain";
    plain.elements.push_back(textEl("sys", ""));
    scene.canvases.push_back(plain);

    // The font byte (element offset 24) of each text element, by name.
    auto fontBytes = [](const std::string& b) {
        std::map<std::string, int> out;
        if (b.size() < 100) return out;
        const size_t canvases = rd16(b, 92), fonts = uint8_t(b[94]), table = size_t(rd32(b, 96));
        for (size_t c = 0; c < canvases; c++) {
            const size_t cv = table + fonts * 112 + 12 * c;
            if (cv + 12 > b.size()) return out;
            const size_t elems = size_t(rd32(b, cv)), count = uint8_t(b[cv + 6]);
            for (size_t k = 0; k < count && elems + 48 * (k + 1) <= b.size(); k++) {
                const size_t e = elems + 48 * k;
                const size_t off = size_t(rd32(b, e + 4)), len = uint8_t(b[e + 2]);
                if (uint8_t(b[e]) == 2 && off + len <= b.size()) out[b.substr(off, len)] = uint8_t(b[e + 24]);
            }
        }
        return out;
    };
    splash::ExportResult r = splash::exportSplashpack(scene, project, project / "cv.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "canvas default font: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(project / "cv.splashpack");
    CHECK(b.size() > 100 && uint8_t(b[94]) == 2);
    std::map<std::string, int> fb = fontBytes(b);
    CHECK(fb.size() == 3);
    CHECK(fb.count("dflt") && fb["dflt"] == 2);  // "font (2)", the canvas's default
    CHECK(fb.count("own") && fb["own"] == 1);    // its own font wins
    CHECK(fb.count("sys") && fb["sys"] == 0);    // no default: the system font

    // A default font the scene does not have: an error naming the canvas.
    splash::Scene missing = scene;
    missing.canvases[0].defaultFont = "gone";
    splash::ExportResult mr = splash::exportSplashpack(missing, project, project / "missing.splashpack");
    CHECK(!mr.ok() && hasError(mr, "canvas 'hud'") && hasError(mr, "'gone'") && hasError(mr, "default font"));

    // The loader pack carries the fonts its canvas draws with, its default too.
    splash::Scene ld = scene;
    ld.canvases[1].defaultFont = "font (2)";
    ld.settings.loadingCanvas = "plain";
    splash::ExportResult lr = splash::exportLoaderPack(ld, project, project / "cv.loading");
    CHECK(lr.ok());
    const std::string l = readAll(project / "cv.loading");
    CHECK(l.size() > 16 && uint8_t(l[4]) == 1 && uint8_t(l[5]) == 1);
    if (l.size() > 16) {
        const size_t cv = size_t(rd32(l, 12)) + uint8_t(l[4]) * 112;
        const size_t e = size_t(rd32(l, cv));
        CHECK(e + 48 <= l.size() && uint8_t(l[e]) == 2 && uint8_t(l[e + 24]) == 1);
    }
    splash::Scene ldMissing = ld;
    ldMissing.canvases[1].defaultFont = "gone";
    CHECK(hasError(splash::exportLoaderPack(ldMissing, project, project / "missing.loading"), "canvas 'plain'"));
}

namespace disctest {

namespace fs = std::filesystem;

std::vector<uint8_t> bytesOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// The six accepted licence sizes, built from the psxsplash licence: 12
// sectors as the file has them, 16 with sectors 0-3 again as 12-15.
std::vector<std::vector<uint8_t>> fixtures() {
    const std::vector<uint8_t>& lic = splash::psxsplashLicence();
    std::vector<std::vector<uint8_t>> out;
    for (size_t count : {size_t(12), size_t(16)}) {
        std::vector<uint8_t> a, b, c;
        for (size_t i = 0; i < count; i++) {
            const uint8_t* s = lic.data() + (i < 12 ? i : i - 12) * 2336;
            a.insert(a.end(), s, s + 2336);
            uint8_t head[16] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 2, uint8_t(i), 2};
            b.insert(b.end(), head, head + 16);
            b.insert(b.end(), s, s + 2336);
            c.insert(c.end(), s + 8, s + 8 + 2048);
        }
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    }
    return out;
}

bool sameSectors(const std::vector<splash::LicenceSector>& a, const std::vector<splash::LicenceSector>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::memcmp(a[i].sub, b[i].sub, 8) != 0 || std::memcmp(a[i].data, b[i].data, sizeof a[i].data) != 0)
            return false;
    return true;
}

// A stand-in CD-ROM engine: a PS-X EXE header and the loader's file names.
std::vector<uint8_t> cdEngine() {
    std::vector<uint8_t> e(2048, 0);
    std::memcpy(e.data(), "PS-X EXE", 8);
    const char* marker = "SCENE%d/SCENE_%d.SPK;1";
    e.insert(e.end(), marker, marker + std::strlen(marker) + 1);
    e.resize(e.size() + 100, 0);
    return e;
}

const uint8_t* rawSector(const std::vector<uint8_t>& bin, uint32_t lba) { return bin.data() + size_t(lba) * 2352; }

}  // namespace disctest

// Licence files of every accepted size give the same sectors; another size is
// an error that names it.
void testDiscLicenceParse() {
    using namespace disctest;
    const std::vector<uint8_t>& lic = splash::psxsplashLicence();
    CHECK(lic.size() == 28032);
    std::vector<std::vector<uint8_t>> f = fixtures();
    CHECK(f.size() == 6);
    const size_t sizes[6] = {28032, 28224, 24576, 37376, 37632, 32768};
    std::vector<std::vector<splash::LicenceSector>> parsed;
    for (size_t k = 0; k < f.size(); k++) {
        CHECK(f[k].size() == sizes[k]);
        std::string error;
        parsed.push_back(splash::parseLicence(f[k], error));
        CHECK(error.empty());
        CHECK(parsed[k].size() == (k < 3 ? 12u : 16u));
    }
    // Each sector: the file's subheader (00 00 08 00 x2) and its 2048 data
    // bytes; past them zero.
    bool payloads = true;
    for (size_t k = 0; k < parsed.size(); k++)
        for (size_t i = 0; i < parsed[k].size(); i++) {
            const uint8_t* s = lic.data() + (i < 12 ? i : i - 12) * 2336;
            const splash::LicenceSector& p = parsed[k][i];
            bool tailZero = std::all_of(p.data + 2048, p.data + sizeof p.data, [](uint8_t v) { return v == 0; });
            if (std::memcmp(p.sub, s, 8) != 0 || std::memcmp(p.data, s + 8, 2048) != 0 || !tailZero) {
                std::fprintf(stderr, "licence fixture %zu (%zu bytes) sector %zu differs\n", k, f[k].size(), i);
                payloads = false;
            }
        }
    CHECK(payloads);
    CHECK(sameSectors(parsed[0], parsed[1]) && sameSectors(parsed[0], parsed[2]));
    CHECK(sameSectors(parsed[3], parsed[4]) && sameSectors(parsed[3], parsed[5]));

    // A Form 2 sector keeps all 2324 data bytes and its subheader verbatim.
    std::vector<uint8_t> form2 = f[3];
    const uint8_t sub2[8] = {1, 2, 0x24, 3, 1, 2, 0x24, 3};
    std::memcpy(&form2[13 * 2336], sub2, 8);
    form2[13 * 2336 + 8 + 2323] = 0x5a;
    std::string error;
    std::vector<splash::LicenceSector> p2 = splash::parseLicence(form2, error);
    CHECK(p2.size() == 16 && std::memcmp(p2[13].sub, sub2, 8) == 0 && p2[13].data[2323] == 0x5a);

    // Any other size is refused, by size.
    std::string bad;
    CHECK(splash::parseLicence(std::vector<uint8_t>(1000, 0), bad).empty());
    CHECK(bad.find("1000") != std::string::npos && bad.find("28032") != std::string::npos &&
          bad.find("2048") != std::string::npos);
    std::string none;
    CHECK(splash::parseLicence({}, none).empty() && none.find(" 0 bytes") != std::string::npos);
    std::string big;
    CHECK(splash::parseLicence(std::vector<uint8_t>(28032 + 1, 0), big).empty() && big.find("28033") != std::string::npos);
}

// disc.json, and resolving a licence by name, by relative path and by a bad file.
void testDiscSettings() {
    using namespace disctest;
    const fs::path dir = g_outDir / "discsettings";
    fs::remove_all(dir);
    fs::create_directories(dir / "lic");

    CHECK(splash::readDiscSettings(dir / "disc.json").volume.empty());
    CHECK(splash::readDiscSettings({}).licence.empty());
    CHECK(!splash::writeDiscSettings({}, {}));
    splash::DiscSettings s;
    s.volume = "My Game";
    s.licence = "lic/system.sys";
    CHECK(splash::writeDiscSettings(dir / "disc.json", s));
    splash::DiscSettings back = splash::readDiscSettings(dir / "disc.json");
    CHECK(back.volume == "My Game" && back.licence == "lic/system.sys");
    std::ofstream(dir / "odd.json", std::ios::binary) << R"({"volume": 5, "licence": "psxsplash"})";
    splash::DiscSettings odd = splash::readDiscSettings(dir / "odd.json");
    CHECK(odd.volume.empty() && odd.licence == "psxsplash");
    std::ofstream(dir / "junk.json", std::ios::binary) << "not json";
    CHECK(splash::readDiscSettings(dir / "junk.json").licence.empty());

    std::string error;
    CHECK(splash::resolveLicence("", dir, error).empty() && error.empty());
    CHECK(splash::resolveLicence("psxsplash", dir, error).size() == 12 && error.empty());
    writeBytes(dir / "lic" / "system.sys", fixtures()[5]);
    CHECK(splash::resolveLicence("lic/system.sys", dir, error).size() == 16 && error.empty());
    writeBytes(dir / "lic" / "short.dat", std::vector<uint8_t>(1000, 0));
    CHECK(splash::resolveLicence("lic/short.dat", dir, error).empty());
    CHECK(error.find("short.dat") != std::string::npos && error.find("1000") != std::string::npos);
    error.clear();
    CHECK(splash::resolveLicence("lic/missing.dat", dir, error).empty() && error.find("missing.dat") != std::string::npos);

    // The volume name rule the editor's Disc section shows and types by.
    CHECK(splash::volumeName("my game-1_x") == "MYGAME1_X");
    CHECK(splash::volumeName(" -.").empty());
    CHECK(splash::volumeName(std::string(40, 'a')) == std::string(31, 'A'));
    CHECK(splash::discVolume("", "courtyard") == "COURTYARD");
    CHECK(splash::discVolume("disc 2", "courtyard") == "DISC2");
    CHECK(splash::discVolume("", "-.-") == "PSXSPLASH");

    // The editor's wrappers on <project>/disc.json.
    CHECK(editor::loadDiscSettings({}).volume.empty());
    CHECK(!editor::saveDiscSettings({}, s));
    CHECK(editor::loadDiscSettings(dir).licence == "lic/system.sys");
    splash::DiscSettings next;
    next.licence = "psxsplash";
    CHECK(editor::saveDiscSettings(dir, next));
    back = splash::readDiscSettings(dir / "disc.json");
    CHECK(back.volume.empty() && back.licence == "psxsplash");

    // Boot logo: on unless the file says false; written only when off.
    CHECK(back.bootLogo && splash::DiscSettings().bootLogo);
    CHECK(splash::readDiscSettings(dir / "missing.json").bootLogo);
    std::ofstream(dir / "logo.json", std::ios::binary) << R"({"volume": "", "licence": "", "bootLogo": "no"})";
    CHECK(splash::readDiscSettings(dir / "logo.json").bootLogo);
    std::ofstream(dir / "logo.json", std::ios::binary | std::ios::trunc) << R"({"bootLogo": false})";
    CHECK(!splash::readDiscSettings(dir / "logo.json").bootLogo);
    next.bootLogo = false;
    next.volume = "LOGOLESS";
    CHECK(editor::saveDiscSettings(dir, next));
    back = editor::loadDiscSettings(dir);
    CHECK(!back.bootLogo && back.volume == "LOGOLESS" && back.licence == "psxsplash");
    {
        std::ifstream in(dir / "disc.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"bootLogo\": false") != std::string::npos);
    }
    next.bootLogo = true;
    CHECK(editor::saveDiscSettings(dir, next));
    {
        std::ifstream in(dir / "disc.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("bootLogo") == std::string::npos);
    }
    CHECK(editor::loadDiscSettings(dir).bootLogo);
}

// Sectors 0-15 of the written image: unchanged without a licence, the
// licence then blank Form 2 with one.
void testDiscLicenceImage() {
    using namespace disctest;
    const fs::path dir = g_outDir / "disclicence";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const char* cnf = "BOOT = cdrom:\\PSX.EXE;1\r\n";
    std::vector<splash::DiscFile> files = {{"SYSTEM.CNF", std::vector<uint8_t>(cnf, cnf + std::strlen(cnf))}};

    // No licence: the bytes the writer has always put there (FNV-1a of
    // sectors 0-15 from the writer before licences, commit 61fba34).
    splash::writeDiscImage(files, "PLAIN", dir / "plain.bin");
    std::vector<uint8_t> plain = bytesOf(dir / "plain.bin");
    CHECK(plain.size() >= 16 * 2352 && fnv(plain.data(), 16 * 2352) == 0x618f843db4644257ull);

    std::string error;
    std::vector<splash::LicenceSector> lic = splash::resolveLicence("psxsplash", {}, error);
    splash::writeDiscImage(files, "LIC", dir / "lic.bin", lic);
    std::vector<uint8_t> bin = bytesOf(dir / "lic.bin");
    CHECK(bin.size() == plain.size());
    if (bin.size() < 18 * 2352) return;
    // LBA 0-11: Form 1 of the file's data with its 00 00 08 00 subheader.
    const std::vector<uint8_t>& file = splash::psxsplashLicence();
    uint8_t want[2352];
    bool form1 = true;
    for (uint32_t lba = 0; lba < 12; lba++) {
        splash::encodeForm1(want, lba, file.data() + lba * 2336 + 8, 0x08);
        form1 = form1 && std::memcmp(rawSector(bin, lba), want, 2352) == 0;
    }
    CHECK(form1);
    const std::string text(reinterpret_cast<const char*>(rawSector(bin, 4) + 24), 64);
    CHECK(text.find("Created wit") != std::string::npos && text.find("psxsplash") != std::string::npos);
    // LBA 12-15: blank Form 2, subheader 00 00 20 00 x2, EDC at 2348.
    const uint8_t sub2[8] = {0, 0, 0x20, 0, 0, 0, 0x20, 0};
    for (uint32_t lba = 12; lba < 16; lba++) {
        const uint8_t* s = rawSector(bin, lba);
        CHECK(s[16 + 2] == 0x20);
        CHECK(std::memcmp(s + 16, sub2, 8) == 0);
        CHECK(std::all_of(s + 24, s + 2348, [](uint8_t v) { return v == 0; }));
        CHECK(s[2348] | s[2349] | s[2350] | s[2351]);
        splash::encodeForm2(want, lba, nullptr, 0x20);
        CHECK(std::memcmp(s, want, 2352) == 0);
    }
    // From LBA 16 on the image is the one without a licence (dates aside).
    CHECK(std::memcmp(rawSector(bin, 17), rawSector(plain, 17), 2352) == 0);

    // A 16-sector licence fills 12-15 from the file, its Form 2 sector as Form 2.
    std::vector<uint8_t> f16 = fixtures()[3];
    const uint8_t xa[8] = {1, 0, 0x24, 0, 1, 0, 0x24, 0};
    std::memcpy(&f16[13 * 2336], xa, 8);
    std::vector<splash::LicenceSector> l16 = splash::parseLicence(f16, error);
    splash::writeDiscImage(files, "LIC", dir / "lic16.bin", l16);
    std::vector<uint8_t> b16 = bytesOf(dir / "lic16.bin");
    if (b16.size() < 16 * 2352) return;
    splash::encodeForm1(want, 12, file.data() + 8, 0x08);
    CHECK(std::memcmp(rawSector(b16, 12), want, 2352) == 0);
    CHECK(std::memcmp(rawSector(b16, 13) + 16, xa, 8) == 0);
    CHECK(std::memcmp(rawSector(b16, 13) + 24, file.data() + 1 * 2336 + 8, 2048) == 0);
    // Form 2: no ECC, so the EDC sits at 2348 and Form 1's EDC slot is data.
    CHECK(std::all_of(rawSector(b16, 13) + 24 + 2048, rawSector(b16, 13) + 2348, [](uint8_t v) { return v == 0; }));
    CHECK(rawSector(b16, 13)[2348] | rawSector(b16, 13)[2349] | rawSector(b16, 13)[2350] | rawSector(b16, 13)[2351]);
}

// exportDisc with disc settings: volume override, licence, a bad licence.
void testDiscExportSettings() {
    using namespace disctest;
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    const fs::path dir = g_outDir / "discexport";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeBytes(dir / "cd.ps-exe", cdEngine());
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");

    auto volumeOf = [](const std::vector<uint8_t>& b) {
        return b.size() < 17 * 2352 ? std::string() : std::string(reinterpret_cast<const char*>(rawSector(b, 16) + 24 + 40), 32);
    };
    splash::DiscResult plain = splash::exportDisc(scene, project, dir / "cd.ps-exe", dir / "plain.bin", "courtyard");
    CHECK(plain.exported.ok());
    std::vector<uint8_t> pb = bytesOf(dir / "plain.bin");
    CHECK(volumeOf(pb).rfind("COURTYARD ", 0) == 0);
    CHECK(pb.size() >= 16 * 2352 && fnv(pb.data(), 16 * 2352) == 0x618f843db4644257ull);

    splash::DiscSettings ds;
    ds.volume = "my game!";
    ds.licence = "psxsplash";
    splash::DiscResult r = splash::exportDisc(scene, project, dir / "cd.ps-exe", dir / "lic.bin", "courtyard", {}, ds);
    CHECK(r.exported.ok());
    std::vector<uint8_t> b = bytesOf(dir / "lic.bin");
    CHECK(volumeOf(b) == "MYGAME" + std::string(26, ' '));
    CHECK(b.size() == pb.size());
    if (b.size() >= 16 * 2352) {
        CHECK(std::string(reinterpret_cast<const char*>(rawSector(b, 4) + 24), 64).find("Created wit") != std::string::npos);
        CHECK(rawSector(b, 12)[16 + 2] == 0x20 && rawSector(b, 15)[16 + 2] == 0x20);
    }
    // A volume with nothing usable in it falls back to the title.
    ds.volume = "!!";
    splash::exportDisc(scene, project, dir / "cd.ps-exe", dir / "fallback.bin", "courtyard", {}, ds);
    CHECK(volumeOf(bytesOf(dir / "fallback.bin")).rfind("COURTYARD ", 0) == 0);

    // A bad licence is an error and writes no image.
    writeBytes(dir / "short.dat", std::vector<uint8_t>(1000, 0));
    ds.licence = (dir / "short.dat").string();
    splash::DiscResult bad = splash::exportDisc(scene, project, dir / "cd.ps-exe", dir / "bad.bin", "courtyard", {}, ds);
    CHECK(!bad.exported.ok() && !fs::exists(dir / "bad.bin"));
    CHECK(!bad.exported.errors.empty() && bad.exported.errors[0].find("1000") != std::string::npos);
}

namespace fs = std::filesystem;

// The size helpers the inspector estimates with match what export makes.
void testAudioSizes() {
    // 84 and 196 samples fill 3 and 7 blocks, where the trap block alone
    // crosses the 64-byte padding.
    for (size_t n : {size_t(0), size_t(1), size_t(27), size_t(28), size_t(29), size_t(84), size_t(196), size_t(1000)})
        for (bool loop : {true, false}) {
            std::vector<int16_t> pcm(n);
            for (size_t i = 0; i < n; i++) pcm[i] = int16_t((i * 977) % 20000) - 10000;
            CHECK(splash::spuAdpcmBytes(n, loop) == splash::encodeSpuAdpcm(pcm, loop).size());
        }
    struct Case {
        size_t n;
        int from, to;
    };
    for (Case c : {Case{0, 44100, 22050}, Case{1000, 22050, 22050}, Case{1000, 44100, 22050}, Case{1001, 44100, 11025},
                   Case{777, 8000, 44100}, Case{12345, 32000, 22050}}) {
        splash::MonoAudio a;
        a.rate = c.from;
        a.samples.assign(c.n, 0.25f);
        CHECK(splash::resampledCount(c.n, c.from, c.to) == splash::resample(a, c.to).samples.size());
    }
}

// A 16-bit PCM WAV with `frames` frames of `channels` channels at `rate`.
static void writeWav16(const fs::path& p, int rate, int channels, uint32_t frames) {
    std::ofstream out(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.put(char((v >> (8 * i)) & 0xff)); };
    auto u16 = [&](uint16_t v) { out.put(char(v & 0xff)); out.put(char(v >> 8)); };
    const uint32_t data = frames * uint32_t(channels) * 2;
    out.write("RIFF", 4);
    u32(36 + data);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 2));
    u16(uint16_t(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(data);
    for (uint32_t i = 0; i < frames * uint32_t(channels); i++) u16(uint16_t(int16_t((i * 131) % 8000 - 4000)));
}

// audioFileInfo reads a WAV header; the document caches it for audio clips.
void testWavInfo() {
    const fs::path dir = g_outDir / "wavinfo";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeWav16(dir / "st.wav", 32000, 2, 4321);
    splash::AudioFileInfo w = splash::audioFileInfo(dir / "st.wav");
    CHECK(w.rate == 32000 && w.channels == 2 && w.frames == 4321);
    { std::ofstream(dir / "bad.wav", std::ios::binary) << "not a wav"; }
    bool threw = false;
    try {
        splash::audioFileInfo(dir / "bad.wav");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    splash::Scene s;
    splash::Object o = named("Sound");
    o.audio.emplace();
    o.audio->clip = "st.wav";
    s.objects.push_back(o);
    o.name = "Bad";
    o.audio->clip = "bad.wav";
    s.objects.push_back(o);
    o.name = "Gone";
    o.audio->clip = "gone.wav";
    s.objects.push_back(o);
    editor::Document d;
    d.reset(s, dir);
    const editor::AudioInfo* ai = d.audio("st.wav");
    CHECK(ai && ai->status == editor::AssetStatus::Ok && ai->rate == 32000 && ai->channels == 2 && ai->frames == 4321);
    CHECK(d.audio("bad.wav") && d.audio("bad.wav")->status == editor::AssetStatus::Unreadable);
    CHECK(d.audio("gone.wav") && d.audio("gone.wav")->status == editor::AssetStatus::Missing);
    CHECK(d.fileExists("st.wav"));
    // A rewritten file is re-read.
    writeWav16(dir / "st.wav", 11025, 1, 100);
    d.refreshAssets({"st.wav"});
    ai = d.audio("st.wav");
    CHECK(ai && ai->rate == 11025 && ai->channels == 1 && ai->frames == 100);
    CHECK(d.fileExists("st.wav"));
}

// AIFF, MP3, Ogg Vorbis and FLAC clips decode to the same tone as the WAV they
// were encoded from (tests/audio/formats), are typed Audio in the asset browser
// and are cached by the document; a broken one is Unreadable.
void testAudioFormats() {
    const fs::path src = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "audio" / "formats";
    const splash::MonoAudio ref = splash::loadAudioMono(src / "tone.wav");
    CHECK(ref.rate == 22050 && ref.samples.size() == 5512);
    // Best normalised correlation over lags 0..2400 (an MP3 encoder adds a delay).
    auto match = [&](const splash::MonoAudio& a) {
        double best = 0;
        for (size_t lag = 0; lag <= 2400 && lag < a.samples.size(); lag++) {
            double xy = 0, xx = 0, yy = 0;
            for (size_t i = 0; i < ref.samples.size() && i + lag < a.samples.size(); i++) {
                const double x = ref.samples[i], y = a.samples[i + lag];
                xy += x * y;
                xx += x * x;
                yy += y * y;
            }
            if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
        }
        return best;
    };
    struct Case {
        const char* file;
        double minMatch;
        bool exactLength;
    };
    for (const Case& c : {Case{"tone.aiff", 0.9999, true}, Case{"tone.flac", 0.9999, true}, Case{"tone.ogg", 0.99, true},
                          Case{"tone.mp3", 0.98, false}}) {
        const fs::path f = src / c.file;
        CHECK(splash::isAudioFile(f));
        const splash::MonoAudio a = splash::loadAudioMono(f);
        const double m = match(a);
        const bool lengthOk = c.exactLength ? a.samples.size() == 5512 : a.samples.size() >= 5512 && a.samples.size() <= 5512 + 2400;
        if (!(a.rate == 22050 && lengthOk && m >= c.minMatch))
            std::printf("  %s: rate %d samples %zu match %.5f\n", c.file, a.rate, a.samples.size(), m);
        CHECK(a.rate == 22050 && lengthOk && m >= c.minMatch);
        const splash::AudioFileInfo info = splash::audioFileInfo(f);
        CHECK(info.rate == 22050 && info.channels == 1 && info.frames == a.samples.size());
    }
    const splash::AudioFileInfo st = splash::audioFileInfo(src / "tone-st44.flac");
    CHECK(st.rate == 44100 && st.channels == 2 && st.frames == 11025);
    const splash::MonoAudio stMono = splash::loadAudioMono(src / "tone-st44.flac");
    CHECK(stMono.rate == 44100 && stMono.samples.size() == 11025);

    CHECK(splash::isAudioFile("a/B.OGG") && splash::isAudioFile("x.Mp3") && splash::isAudioFile("y.aif"));
    CHECK(!splash::isAudioFile("notes.txt") && !splash::isAudioFile("tex.png") && !splash::isAudioFile("wav"));
    using T = editor::AssetType;
    for (const char* n : {"m.ogg", "m.mp3", "m.flac", "m.aiff", "m.aif", "m.WAV"}) CHECK(editor::classifyAsset(n, false) == T::Audio);

    const fs::path dir = g_outDir / "audioformats";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(src / "tone.ogg", dir / "music.ogg");
    { std::ofstream(dir / "bad.ogg", std::ios::binary) << "OggS but not really"; }
    bool threw = false;
    try {
        splash::loadAudioMono(dir / "bad.ogg");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("Ogg Vorbis") != std::string::npos;
    }
    CHECK(threw);
    splash::Scene s;
    splash::Object o = named("Music");
    o.audio.emplace();
    o.audio->clip = "music.ogg";
    s.objects.push_back(o);
    o.name = "Bad";
    o.audio->clip = "bad.ogg";
    s.objects.push_back(o);
    editor::Document d;
    d.reset(s, dir);
    const editor::AudioInfo* ai = d.audio("music.ogg");
    CHECK(ai && ai->status == editor::AssetStatus::Ok && ai->rate == 22050 && ai->channels == 1 && ai->frames == 5512);
    CHECK(d.audio("bad.ogg") && d.audio("bad.ogg")->status == editor::AssetStatus::Unreadable);
    // A re-read goes through the audio path, never the texture one.
    fs::copy_file(src / "tone-st44.flac", dir / "music.ogg", fs::copy_options::overwrite_existing);
    d.refreshAssets({"music.ogg"});
    ai = d.audio("music.ogg");
    CHECK(ai && ai->status == editor::AssetStatus::Unreadable && d.fileExists("music.ogg"));
    fs::copy_file(src / "tone.mp3", dir / "music.mp3");
    d.refreshAssets({"music.mp3"});
    ai = d.audio("music.mp3");
    CHECK(ai && ai->status == editor::AssetStatus::Ok && ai->rate == 22050 && d.fileExists("music.mp3"));
}

// loadCdAudio, writeDiscImage with music tracks, and exportDisc's music errors.
void testDiscMusic() {
    using namespace disctest;
    const fs::path src = fs::path(SPLASHEDIT_SOURCE_DIR) / "tests" / "audio" / "formats";

    // Settings round-trip carries the track list; a missing key reads as empty.
    const fs::path dir = g_outDir / "discmusic";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::DiscSettings s;
    s.tracks = {"music/one.wav", "two.flac"};
    CHECK(splash::writeDiscSettings(dir / "disc.json", s));
    splash::DiscSettings back = splash::readDiscSettings(dir / "disc.json");
    CHECK(back.tracks.size() == 2 && back.tracks[0] == "music/one.wav" && back.tracks[1] == "two.flac");
    std::ofstream(dir / "old.json", std::ios::binary) << R"({"volume": "V", "licence": ""})";
    CHECK(splash::readDiscSettings(dir / "old.json").tracks.empty());
    std::ofstream(dir / "mixed.json", std::ios::binary) << R"({"tracks": ["a.wav", 7, "b.wav"]})";
    splash::DiscSettings mixed = splash::readDiscSettings(dir / "mixed.json");
    CHECK(mixed.tracks.size() == 2 && mixed.tracks[0] == "a.wav" && mixed.tracks[1] == "b.wav");

    // loadCdAudio: interleaved stereo 44100; a mono clip has L == R and the
    // resampled frame count.
    const std::vector<int16_t> cd = splash::loadCdAudio(src / "tone.wav");
    const size_t frames = splash::resampledCount(5512, 22050, 44100);
    CHECK(cd.size() == frames * 2);
    bool lr = true;
    for (size_t i = 0; i < frames; i++) lr = lr && cd[2 * i] == cd[2 * i + 1];
    CHECK(lr);
    // A 44100 stereo clip is passed through at its own rate (frames unchanged).
    const std::vector<int16_t> st = splash::loadCdAudio(src / "tone-st44.flac");
    CHECK(st.size() == 11025 * 2);

    // writeDiscImage with two small tracks: the cue text, the DiscInfo, zero
    // pregaps, the PCM bytes at the track start.
    const char* cnf = "BOOT = cdrom:\\PSX.EXE;1\r\n";
    std::vector<splash::DiscFile> files = {{"SYSTEM.CNF", std::vector<uint8_t>(cnf, cnf + std::strlen(cnf))}};
    std::vector<int16_t> a(1000), b(600);  // one sector and change, one short of a sector
    for (size_t i = 0; i < a.size(); i++) a[i] = int16_t((i * 131) % 40000 - 20000);
    for (size_t i = 0; i < b.size(); i++) b[i] = int16_t((i * 977) % 40000 - 20000);
    splash::DiscInfo info = splash::writeDiscImage(files, "MUSIC", dir / "m.bin", {}, {a, b});
    std::vector<uint8_t> bin = bytesOf(dir / "m.bin");

    CHECK(info.tracks.size() == 2);
    CHECK(info.tracks[0].start == info.dataSectors + splash::kAudioPregap);
    const uint32_t secA = uint32_t((a.size() * 2 + 2351) / 2352);
    const uint32_t secB = uint32_t((b.size() * 2 + 2351) / 2352);
    CHECK(info.tracks[0].sectors == secA && info.tracks[1].sectors == secB);
    CHECK(info.tracks[1].start == info.tracks[0].start + secA + splash::kAudioPregap);
    CHECK(info.sectors == info.tracks[1].start + secB);
    CHECK(bin.size() == size_t(info.sectors) * 2352);

    // The pregap before each track is 150 zero sectors.
    bool pregapZero = true;
    for (uint32_t lba = info.dataSectors; lba < info.tracks[0].start; lba++)
        pregapZero = pregapZero && std::all_of(rawSector(bin, lba), rawSector(bin, lba) + 2352, [](uint8_t v) { return v == 0; });
    CHECK(pregapZero);

    // The PCM, little-endian, starts at INDEX 01.
    bool pcmOk = true;
    const uint8_t* p = rawSector(bin, info.tracks[0].start);
    for (size_t i = 0; i < a.size(); i++) {
        uint16_t v = uint16_t(uint8_t(p[2 * i]) | (uint8_t(p[2 * i + 1]) << 8));
        pcmOk = pcmOk && int16_t(v) == a[i];
    }
    CHECK(pcmOk);
    // The last sector of track 1 is zero-padded past the PCM.
    const uint8_t* last = rawSector(bin, info.tracks[0].start + secA - 1);
    CHECK(last[2351] == 0 || a.size() * 2 % 2352 == 0);

    // The cue text, exact.
    std::ifstream cf(dir / "m.cue", std::ios::binary);
    std::string cue((std::istreambuf_iterator<char>(cf)), {});
    std::string want = "FILE \"m.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n";
    want += "  TRACK 02 AUDIO\r\n    INDEX 00 " + splash::cueTime(info.dataSectors) + "\r\n    INDEX 01 " +
            splash::cueTime(info.tracks[0].start) + "\r\n";
    want += "  TRACK 03 AUDIO\r\n    INDEX 00 " + splash::cueTime(info.tracks[0].start + secA) + "\r\n    INDEX 01 " +
            splash::cueTime(info.tracks[1].start) + "\r\n";
    CHECK(cue == want);

    // Empty tracks: the image is the one without music (its pinned hash).
    splash::writeDiscImage(files, "PLAIN", dir / "plain.bin", {});
    std::vector<uint8_t> plain = bytesOf(dir / "plain.bin");
    CHECK(plain.size() >= 16 * 2352 && fnv(plain.data(), 16 * 2352) == 0x618f843db4644257ull);

    // exportDisc: a missing music track is "music track 2: ..." and no image.
    const fs::path project = fs::path(SPLASHEDIT_SOURCE_DIR) / "examples" / "courtyard";
    writeBytes(dir / "cd.ps-exe", cdEngine());
    splash::Scene scene = splash::loadScene(project / "courtyard.scene");
    splash::DiscSettings ds;
    ds.tracks = {"nope.wav"};
    splash::DiscResult r = splash::exportDisc(scene, project, dir / "cd.ps-exe", dir / "miss.bin", "courtyard", {}, ds);
    CHECK(!r.exported.ok() && !fs::exists(dir / "miss.bin"));
    CHECK(!r.exported.errors.empty() && r.exported.errors[0].find("music track 2") != std::string::npos);
}

// Mesh colours on a mesh without vertex colours is a warning, fixed by baking.
void testMeshColours() {
    const fs::path dir = g_outDir / "meshcolours";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::Mesh tri;
    tri.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    tri.normals.assign(3, {0, 0, 1});
    tri.submeshes = {{0, 1, 2}};
    splash::saveMesh(tri, dir / "plain.mesh");
    tri.colors.assign(3, {1, 0, 0, 1});
    splash::saveMesh(tri, dir / "painted.mesh");

    splash::Scene s;
    splash::Object o = named("Plain");
    o.mesh.emplace();
    o.mesh->mesh = "plain.mesh";
    o.mesh->vertexColors = splash::VertexColorMode::Mesh;
    s.objects.push_back(o);
    o.name = "Painted";
    o.mesh->mesh = "painted.mesh";
    s.objects.push_back(o);
    editor::Document d;
    d.reset(s, dir);
    const editor::MeshInfo* plain = d.mesh("plain.mesh");
    const editor::MeshInfo* painted = d.mesh("painted.mesh");
    CHECK(plain && plain->status == editor::AssetStatus::Ok && !plain->hasColors);
    CHECK(painted && painted->status == editor::AssetStatus::Ok && painted->hasColors);
    CHECK(d.hasWarning(*d.object({0})));
    CHECK(!d.hasWarning(*d.object({1})));
    d.edit({0}, [](splash::Object& ob) { ob.mesh->vertexColors = splash::VertexColorMode::Baked; });
    CHECK(!d.hasWarning(*d.object({0})));
    d.undo();
    CHECK(d.hasWarning(*d.object({0})));
}

// couldBe4bpp over every material: true only when each textured material's
// texture reads and fits 16 colours, and together they shrink at 4 bpp.
void testMaterials4bpp() {
    const fs::path dir = g_outDir / "materials4bpp";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto png = [&](const char* name, int colours) {
        splash::Image img;
        img.width = img.height = 32;
        img.pixels.resize(32 * 32);
        for (size_t i = 0; i < img.pixels.size(); i++)
            img.pixels[i] = splash::Image::Px{float(i % size_t(colours)) / float(colours), 0.25f, 0.5f, 1};
        splash::savePng(img, dir / name);
    };
    png("two.png", 2);
    png("four.png", 4);
    png("many.png", 64);
    splash::Mesh quad;
    quad.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    quad.normals.assign(4, {0, 0, 1});
    quad.submeshes = {{0, 1, 2}, {1, 3, 2}};
    splash::saveMesh(quad, dir / "quad.mesh");

    auto mesh = [](std::vector<std::string> textures) {
        splash::Object o = named("M");
        o.mesh.emplace();
        o.mesh->mesh = "quad.mesh";
        o.mesh->bitDepth = splash::BitDepth::Bpp16;
        for (const std::string& t : textures) o.mesh->materials.push_back({t, {1, 1, 1, 1}, {}});
        return o;
    };
    splash::Scene s;
    s.objects.push_back(mesh({"two.png", "four.png"}));   // 0: both fit
    s.objects.push_back(mesh({"two.png", "many.png"}));   // 1: the second has 64 colours
    s.objects.push_back(mesh({"two.png", "gone.png"}));   // 2: the second is missing
    s.objects.push_back(mesh({"two.png", ""}));           // 3: one textured, one plain
    s.objects.push_back(mesh({"", ""}));                  // 4: none textured
    s.objects.push_back(mesh({"many.png", "two.png"}));   // 5: the first fails
    editor::Document d;
    d.reset(s, dir);
    const editor::MeshInfo* mi = d.mesh("quad.mesh");
    CHECK(mi && mi->status == editor::AssetStatus::Ok && mi->submeshes == 2 && mi->triangles == 2);
    CHECK(d.texture("many.png") && d.texture("many.png")->colors15 > 16);
    CHECK(d.couldBe4bpp(*d.object({0})->mesh));
    CHECK(!d.couldBe4bpp(*d.object({1})->mesh));
    CHECK(!d.couldBe4bpp(*d.object({2})->mesh));
    CHECK(d.couldBe4bpp(*d.object({3})->mesh));
    CHECK(!d.couldBe4bpp(*d.object({4})->mesh));
    CHECK(!d.couldBe4bpp(*d.object({5})->mesh));
    CHECK(d.hasWarning(*d.object({0})));
    d.edit({0}, [](splash::Object& ob) { ob.mesh->bitDepth = splash::BitDepth::Bpp4; });
    CHECK(!d.couldBe4bpp(*d.object({0})->mesh));
}

// hashSceneId matches Unity's HashSceneId: FNV-1a over the low byte of each
// UTF-16 unit. Expected values from a Python run of that formula on
// str.encode('utf-16-le').
void testSceneNetworkId() {
    CHECK(splash::hashSceneId("") == 0);
    CHECK(splash::hashSceneId("lobby") == 0x1c5a0ed3u);
    CHECK(splash::hashSceneId("\xc3\xa9t\xc3\xa9") == 0xa39e1b7bu);  // "été": one unit per letter
    CHECK(splash::hashSceneId("a\xf0\x9f\x98\x80") == 0x2cb0f1f9u);  // U+1F600: a surrogate pair
    editor::Document d;
    d.editSettings([](splash::SceneSettings& st) { st.networkId = "lobby"; });
    CHECK(d.scene().settings.networkId == "lobby");
    CHECK(d.undo());
    CHECK(d.scene().settings.networkId.empty());
}

// uvTextureSlot numbers a mesh's distinct textures in material order (an
// untextured material has none, a repeated image keeps its first number), the
// numbering the exporter compares uvOffsetMaterial against.
void testUvOffsetSlots() {
    splash::MeshComponent mc;
    auto mat = [](const char* t) {
        splash::Material m;
        m.texture = t;
        return m;
    };
    mc.materials = {mat(""), mat("a.png"), mat("b.png"), mat("a.png")};
    CHECK(splash::uvTextureSlot(mc, 0) == -1);
    CHECK(splash::uvTextureSlot(mc, 1) == 0);
    CHECK(splash::uvTextureSlot(mc, 2) == 1);
    CHECK(splash::uvTextureSlot(mc, 3) == 0);
    CHECK(splash::uvTextureSlot(mc, 4) == -1);
    CHECK((splash::uvOffsetMaterials(mc, 0) == std::vector<size_t>{1, 3}));
    CHECK((splash::uvOffsetMaterials(mc, 1) == std::vector<size_t>{2}));
    CHECK(splash::uvOffsetMaterials(mc, 2).empty());
    CHECK(splash::uvOffsetMaterials(mc, -1).empty());
    mc.materials = {mat(""), mat("")};
    CHECK(splash::uvOffsetMaterials(mc, 0).empty());
}

// Mesh textures with texels under half alpha export cutout: the hole is VRAM
// word 0 (what the GPU skips); an image without one exports as before.
void testMeshAlpha() {
    namespace fs = std::filesystem;
    auto image = [](int w, int h, auto px) {
        splash::Image img;
        img.width = w, img.height = h;
        img.pixels.resize(size_t(w) * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) img.pixels[size_t(y) * w + x] = px(x, h - 1 - y);  // top row first
        return img;
    };
    // Top-left texel a hole, the next one opaque black, the rest a gradient.
    const splash::Image holed = image(16, 16, [](int x, int y) {
        if (x == 0 && y == 0) return splash::Image::Px{1, 1, 1, 0};
        if (x == 1 && y == 0) return splash::Image::Px{0, 0, 0, 1};
        return splash::Image::Px{float(x) / 15.f, float(y) / 15.f, 0.5f, 1};
    });
    const splash::Image opaque = image(16, 16, [](int x, int y) {
        return splash::Image::Px{float(x) / 15.f, float(y) / 15.f, 0.5f, x == 0 && y == 0 ? 0.5f : 1.f};  // 0.5 is not a hole
    });
    CHECK(splash::hasCutoutTexels(holed));
    CHECK(!splash::hasCutoutTexels(opaque));
    splash::Image almost = opaque;
    almost.pixels[7].a = 0.49f;
    CHECK(splash::hasCutoutTexels(almost));

    const fs::path dir = g_outDir / "meshalpha";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "out", ec);
    fs::copy_file(fs::path(SPLASHEDIT_SOURCE_DIR) / "examples/courtyard/meshes/crate.mesh", dir / "crate.mesh", ec);
    splash::savePng(holed, dir / "holed.png");
    splash::savePng(opaque, dir / "opaque.png");

    auto exportOne = [&](const char* tex, splash::BitDepth depth, splash::ExportStats::TexturePlacement& t, std::vector<uint16_t>& vram) {
        splash::Scene scene;
        splash::Object o = named("Quad");
        splash::MeshComponent m;
        m.mesh = "crate.mesh";
        m.materials.push_back({tex, {1, 1, 1, 1}, {}});
        m.bitDepth = depth;
        o.mesh = m;
        scene.objects.push_back(o);
        splash::ExportResult r = splash::exportSplashpack(scene, dir, dir / "out" / "scene.splashpack");
        CHECK(r.ok() && r.stats.textures.size() == 1);
        if (!r.ok() || r.stats.textures.size() != 1) return false;
        t = r.stats.textures[0];
        vram = vramFromFile(dir / "out" / "scene.vram");
        CHECK(!vram.empty());
        return !vram.empty();
    };
    splash::ExportStats::TexturePlacement t;
    std::vector<uint16_t> vram;
    auto at = [&](int x, int y) { return vram[size_t(y) * 1024 + x]; };

    if (exportOne("holed.png", splash::BitDepth::Bpp16, t, vram)) {
        CHECK(t.cutout);
        CHECK(at(t.x, t.y) == 0x0000);      // the hole
        CHECK(at(t.x + 1, t.y) == 0x8000);  // opaque black stays drawn
        int zeros = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) zeros += at(t.x + x, t.y + y) == 0;
        CHECK(zeros == 1);
    }
    if (exportOne("holed.png", splash::BitDepth::Bpp8, t, vram)) {
        CHECK(t.cutout);
        CHECK(at(t.clutX, t.clutY) == 0);           // CLUT entry 0 is the transparent word
        CHECK((at(t.x, t.y) & 0xff) == 0);          // the hole's index
        CHECK(((at(t.x, t.y) >> 8) & 0xff) != 0);   // black is a real colour
        CHECK(at(t.clutX + ((at(t.x, t.y) >> 8) & 0xff), t.clutY) == 0x8000);
    }
    // No hole: byte for byte what convertTexture(img, depth, false) makes.
    for (splash::BitDepth depth : {splash::BitDepth::Bpp4, splash::BitDepth::Bpp8, splash::BitDepth::Bpp16}) {
        if (!exportOne("opaque.png", depth, t, vram)) continue;
        CHECK(!t.cutout);
        const splash::PsxTexture want = splash::convertTexture(splash::loadImage(dir / "opaque.png"), depth, false);
        bool same = true;
        for (int y = 0; y < want.height; y++)
            for (int x = 0; x < want.quantizedWidth; x++) same &= at(t.x + x, t.y + y) == want.word(x, y);
        for (size_t i = 0; i < want.palette.size(); i++) same &= at(t.clutX + int(i), t.clutY) == want.palette[i];
        CHECK(same);
        if (!same) std::fprintf(stderr, "mesh alpha: an opaque texture changed at %d bpp\n", int(depth));
    }
}

// Sprite sheets: the scene JSON round trip, the exact sprite table of the
// tests/sprite fixture (2 sheets, 3 animations), each error export refuses
// with, and the inspector's edits as undo steps.
// A HUD sprite: a UI Image backed by a cell of a scene sprite sheet. JSON keys
// "sheet"/"cell" (only on a sheet-backed image), the exact 16 type-data bytes
// psxsplash's uisystem.cpp reads (texture page, CLUT, the cell's UVs and the
// grid UI.SetFrame steps through), one VRAM copy of the sheet, the export's
// two errors, the --stats list, the canvas preview's cell and the edits.
void testUiSprite() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "uisprite";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "tests" / "sprite", project, fs::copy_options::recursive);
    fs::copy(src / "examples" / "courtyard" / "textures" / "crate.png", project / "crate.png");
    splash::Scene scene = splash::loadScene(project / "sprite.scene");
    for (splash::Object& o : scene.objects)
        if (o.mesh) {
            o.mesh->mesh = (src / "examples" / "courtyard" / "meshes" / "crate.mesh").string();
            for (splash::Material& m : o.mesh->materials) m.texture = (src / "examples" / "courtyard" / "textures" / "crate.png").string();
        }
    splash::UICanvas hud;
    hud.name = "hud";
    splash::UIElement icon;
    icon.type = splash::UIElementType::Image;
    icon.name = "icon";
    icon.rect = {10, 20, 16, 16};
    icon.color = {0.5f, 0.5f, 0.5f};
    icon.sheet = "hero";
    icon.cell = 6;  // column 2, row 1 of hero's 4 x 2
    splash::UIElement plain = icon;
    plain.name = "plain";
    plain.sheet.clear();
    plain.cell = 0;
    plain.texture = "crate.png";
    plain.bitDepth = splash::BitDepth::Bpp8;
    plain.rect = {100, 20, 32, 32};
    hud.elements = {icon, plain};
    scene.canvases.push_back(hud);

    // JSON: sheet and cell come back; a plain image writes neither key; a
    // second save is byte for byte the first.
    splash::saveScene(scene, project / "rt1.scene");
    {
        const splash::Scene back = splash::loadScene(project / "rt1.scene");
        CHECK(back.canvases.size() == 1 && back.canvases[0].elements.size() == 2);
        if (back.canvases.size() == 1 && back.canvases[0].elements.size() == 2) {
            const auto& a = back.canvases[0].elements[0];
            const auto& b = back.canvases[0].elements[1];
            CHECK(a.sheet == "hero" && a.cell == 6 && b.sheet.empty() && b.cell == 0 && b.texture == "crate.png");
            CHECK(a.extra.empty() && b.extra.empty());
        }
        splash::saveScene(back, project / "rt2.scene");
        CHECK(readAll(project / "rt1.scene") == readAll(project / "rt2.scene"));
        const std::string text = readAll(project / "rt1.scene");
        size_t n = 0;
        for (size_t at = text.find("\"sheet\""); at != std::string::npos; at = text.find("\"sheet\"", at + 1)) n++;
        CHECK(n == 1 && text.find("\"cell\": 6") != std::string::npos);
    }

    // The type-data bytes.
    splash::ExportResult r = splash::exportSplashpack(scene, project, project / "u.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "uisprite export: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(project / "u.splashpack");
    if (r.ok() && b.size() > 100) {
        CHECK(rd16(b, 92) == 1 && uint8_t(b[94]) == 0);
        const size_t table = rd32(b, 96);
        const size_t data = rd32(b, table);
        CHECK(uint8_t(b[table + 6]) == 2);  // element count
        int heroCopies = 0;
        splash::ExportStats::TexturePlacement hero, crate;
        for (const auto& t : r.stats.textures) {
            if (fs::path(t.source).filename() == "hero.png") hero = t, heroCopies++;
            if (fs::path(t.source).filename() == "crate.png" && t.bitDepth == 8) crate = t;
        }
        CHECK(heroCopies == 1);  // the image shares the sprite table's texture
        CHECK(crate.width > 0 && crate.height > 0);
        std::string want;
        auto u8 = [&](int v) { want += char(uint8_t(v)); };
        auto u16 = [&](int v) { u8(v & 255), u8((v >> 8) & 255); };
        const int baseU = (hero.x % 64) * 4, baseV = hero.y % 256;
        const int u0 = baseU + 2 * 16, v0 = baseV + 1 * 16;
        u8(hero.x / 64), u8(hero.y / 256), u16(hero.clutX / 16), u16(hero.clutY), u8(u0), u8(v0), u8(u0 + 15), u8(v0 + 15), u8(0),
            u8(16), u8(16), u8(4), u8(baseU), u8(baseV);
        const std::string got = b.substr(data + 24, 16);
        CHECK(got == want);
        if (got != want)
            for (size_t i = 0; i < 16; i++) std::fprintf(stderr, "  icon byte %zu: got %d want %d\n", i, uint8_t(got[i]), uint8_t(want[i]));
        // Colour 0.5 (#808080) is the texture as drawn: 128.
        CHECK(uint8_t(b[data + 20]) == 128 && uint8_t(b[data + 21]) == 128 && uint8_t(b[data + 22]) == 128);
        // The plain image: its own texture, the whole of it, bytes 11..15 zero.
        want.clear();
        const int pu = (crate.x % 64) * 2, pv = crate.y % 256;
        u8(crate.x / 64), u8(crate.y / 256), u16(crate.clutX / 16), u16(crate.clutY), u8(pu), u8(pv), u8(pu + crate.width - 1),
            u8(pv + crate.height - 1), u8(1), u8(0), u8(0), u8(0), u8(0), u8(0);
        const std::string pgot = b.substr(data + 48 + 24, 16);
        CHECK(pgot == want);
        if (pgot != want)
            for (size_t i = 0; i < 16; i++) std::fprintf(stderr, "  plain byte %zu: got %d want %d\n", i, uint8_t(pgot[i]), uint8_t(want[i]));
        CHECK(pgot.substr(11) == std::string(5, '\0'));
        CHECK(uint8_t(b[data + 48 + 20]) == 128);
        // --stats: both images, in canvas order.
        CHECK(r.stats.uiImages.size() == 2);
        if (r.stats.uiImages.size() == 2) {
            const auto& i0 = r.stats.uiImages[0];
            const auto& i1 = r.stats.uiImages[1];
            CHECK(i0.canvas == "hud" && i0.element == "icon" && i0.sheet == "hero" && i0.cell == 6);
            CHECK(i1.element == "plain" && i1.sheet.empty() && i1.cell == 0);
        }
        // A sheet used only by images is still in the sprite table (and its 16-sheet limit).
        CHECK(rd16(b, 132) == 2);
    }

    // The errors: no such sheet, a cell past the sheet's 8; the last cell exports.
    auto errorsWith = [&](const std::string& sheet, int cell) {
        splash::Scene s = scene;
        s.canvases[0].elements[0].sheet = sheet;
        s.canvases[0].elements[0].cell = cell;
        return splash::exportSplashpack(s, project, project / "e.splashpack").errors;
    };
    auto has = [](const std::vector<std::string>& es, const std::string& what) {
        return std::any_of(es.begin(), es.end(), [&](const std::string& e) { return e.find(what) != std::string::npos; });
    };
    const std::vector<std::string> noSheet = errorsWith("nope", 0);
    CHECK(has(noSheet, "canvas 'hud', element 'icon': no sprite sheet named 'nope'"));
    const std::vector<std::string> pastEnd = errorsWith("hero", 8);
    CHECK(has(pastEnd, "canvas 'hud', element 'icon': cell 8 is outside sprite sheet 'hero', which has 8 cells"));
    CHECK(errorsWith("hero", 7).empty());
    CHECK(errorsWith("fx", 7).empty() && has(errorsWith("fx", 8), "outside sprite sheet 'fx'"));  // fx: 48x16 in 12x8 cells, 4 x 2
    {
        // A bad cell in the scene file is refused on load.
        std::string text = readAll(project / "rt1.scene");
        const size_t at = text.find("\"cell\": 6");
        text.replace(at, 9, "\"cell\": 300");
        std::ofstream(project / "bad.scene", std::ios::binary) << text;
        bool threw = false;
        try {
            splash::loadScene(project / "bad.scene");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // On the loading screen (a loader pack has no sprite table) the image gets
    // its own copy of the sheet's texture, with the grid based at that copy.
    {
        splash::Scene ls = scene;
        ls.settings.loadingCanvas = "hud";
        ls.canvases[0].elements.pop_back();  // just the sheet-backed image
        splash::ExportResult lr = splash::exportLoaderPack(ls, project, project / "u.loading");
        for (const std::string& e : lr.errors) std::fprintf(stderr, "uisprite loader: %s\n", e.c_str());
        CHECK(lr.ok());
        const std::string lb = readAll(project / "u.loading");
        if (lr.ok() && lb.size() > 16) {
            CHECK(lb[0] == 'L' && lb[1] == 'P' && uint8_t(lb[4]) == 0 && uint8_t(lb[5]) == 1 && uint8_t(lb[10]) == 1);
            const size_t ltable = rd32(lb, 12), ldata = rd32(lb, ltable);
            const std::string td = lb.substr(ldata + 24, 16);
            auto at = [&](int i) { return int(uint8_t(td[size_t(i)])); };
            CHECK(at(10) == 0 && at(11) == 16 && at(12) == 16 && at(13) == 4);
            CHECK(at(6) == at(14) + 32 && at(7) == at(15) + 16 && at(8) == at(6) + 15 && at(9) == at(7) + 15);
            const int lx = rd16(lb, 16 + 8), ly = rd16(lb, 16 + 10);  // the one atlas: the sheet's copy
            CHECK(at(0) == lx / 64 && at(1) == ly / 256 && at(14) == (lx % 64) * 4 && at(15) == ly % 256);
        }
        ls.canvases[0].elements[0].sheet = "nope";
        CHECK(has(splash::exportLoaderPack(ls, project, project / "u2.loading").errors, "no sprite sheet named 'nope'"));
    }

    // The canvas preview draws only that cell (tint 128: the texels as they are).
    {
        editor::uic::Assets assets;
        assets.refresh(project, 1);
        const editor::uic::Screen s;
        splash::Scene only = scene;
        only.canvases[0].elements[1].visible = false;
        const std::vector<uint32_t> px = editor::uic::render(only, {true}, s, assets, 0);
        const auto& t = assets.texture("hero.png", splash::BitDepth::Bpp4, true);
        CHECK(t.error.empty() && t.width == 64 && t.height == 32);
        int bad = 0, drawn = 0;
        auto ex = [](int c5) { return uint32_t((c5 << 3) | (c5 >> 2)); };
        for (int y = 0; y < 16 && t.error.empty(); y++)
            for (int x = 0; x < 16; x++) {
                const uint16_t w = t.words[size_t(16 + y * 15 / 16) * 64 + size_t(32 + x * 15 / 16)];
                const uint32_t want = w ? 0xff000000u | (ex((w >> 10) & 31) << 16) | (ex((w >> 5) & 31) << 8) | ex(w & 31) : 0u;
                const uint32_t got = px[size_t(20 + y) * size_t(s.w) + size_t(10 + x)];
                bad += got != want;
                drawn += got != 0;
            }
        CHECK(bad == 0 && drawn > 0);
    }

    // The edits: each one undo step; a new sheet starts at cell 0; Snap size
    // sets the rect to one cell; None makes it a plain image again.
    {
        using namespace editor::uic;
        const Screen s;
        editor::Document d;
        splash::Scene ds = scene;
        ds.canvases[0].elements[0].sheet.clear();
        ds.canvases[0].elements[0].cell = 0;
        ds.canvases[0].elements[0].rect = {10, 20, 40, 30};
        d.reset(ds);
        auto el = [&]() -> const splash::UIElement& { return d.scene().canvases[0].elements[0]; };
        CHECK(setImageSheet(d, 0, 0, "hero") && el().sheet == "hero" && el().cell == 0 && d.historySize() == 1);
        CHECK(!setImageSheet(d, 0, 0, "hero") && d.historySize() == 1);
        CHECK(setImageCell(d, 0, 0, 5) && el().cell == 5 && d.historySize() == 2);
        CHECK(!setImageCell(d, 0, 0, -1) && !setImageCell(d, 0, 0, 256) && !setImageCell(d, 0, 1, 2));  // plain image: no cell
        CHECK(imageSheet(d.scene(), el()) == &d.scene().spriteSheets[0]);
        CHECK(snapImageToCell(d, 0, 0, s) && el().rect == (std::array<int, 4>{10, 20, 16, 16}) && d.historySize() == 3);
        CHECK(!snapImageToCell(d, 0, 0, s));
        CHECK(setImageSheet(d, 0, 0, "fx") && el().cell == 0);
        CHECK(setImageSheet(d, 0, 0, "") && el().sheet.empty() && imageSheet(d.scene(), el()) == nullptr);
        d.undo();
        d.undo();
        CHECK(el().sheet == "hero" && el().cell == 5);
        d.undo();
        CHECK(el().rect == (std::array<int, 4>{10, 20, 40, 30}));
        // A missing sheet is a problem on the element.
        CHECK(setImageSheet(d, 0, 0, "gone"));
        const auto probs = elementProblems(d, 0, 0, s);
        CHECK(std::any_of(probs.begin(), probs.end(), [](const Problem& p) { return p.title == "No such sprite sheet"; }));
    }

    // Renaming a sheet renames the images that show it, in every canvas, in
    // the same undo step; undo puts both back. Deleting a sheet leaves them.
    {
        editor::Document d;
        splash::Scene ds = scene;
        splash::UICanvas second;
        second.name = "pause";
        second.elements = {scene.canvases[0].elements[0]};  // hero
        second.elements[0].name = "icon2";
        second.elements.push_back(scene.canvases[0].elements[0]);
        second.elements[1].name = "spark";
        second.elements[1].sheet = "fx";
        ds.canvases.push_back(second);
        d.reset(ds);
        auto sheetOf = [&](int c, int e) { return d.scene().canvases[size_t(c)].elements[size_t(e)].sheet; };
        CHECK(editor::editSpriteSheet(d, 0, [](splash::SpriteSheet& s) { s.name = "hero2"; }));
        CHECK(d.historySize() == 1 && d.scene().spriteSheets[0].name == "hero2");
        CHECK(sheetOf(0, 0) == "hero2" && sheetOf(1, 0) == "hero2" && sheetOf(1, 1) == "fx" && sheetOf(0, 1).empty());
        d.undo();
        CHECK(d.scene().spriteSheets[0].name == "hero" && sheetOf(0, 0) == "hero" && sheetOf(1, 0) == "hero");
        d.redo();
        CHECK(sheetOf(0, 0) == "hero2" && sheetOf(1, 0) == "hero2");
        // Typed a key at a time (one merge key): one step, the images follow each name.
        CHECK(editor::editSpriteSheet(d, 1, [](splash::SpriteSheet& s) { s.name = "f"; }, "name"));
        CHECK(editor::editSpriteSheet(d, 1, [](splash::SpriteSheet& s) { s.name = "fire"; }, "name"));
        d.endMerge();
        CHECK(sheetOf(1, 1) == "fire" && d.historySize() == 2);
        d.undo();
        CHECK(sheetOf(1, 1) == "fx" && d.scene().spriteSheets[1].name == "fx");
        // Deleting a sheet keeps the images' names.
        CHECK(editor::deleteSpriteSheet(d, 0) && sheetOf(0, 0) == "hero2" && sheetOf(1, 0) == "hero2");
    }
}

void testSpriteSheets() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "sprites";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "tests" / "sprite", project, fs::copy_options::recursive);
    splash::Scene scene = splash::loadScene(project / "sprite.scene");
    for (splash::Object& o : scene.objects)
        if (o.mesh) {
            o.mesh->mesh = (src / "examples" / "courtyard" / "meshes" / "crate.mesh").string();
            for (splash::Material& m : o.mesh->materials) m.texture = (src / "examples" / "courtyard" / "textures" / "crate.png").string();
        }
    CHECK(scene.spriteSheets.size() == 2 && scene.spriteSheets[0].anims.size() == 2 && scene.spriteSheets[1].anims.size() == 1);

    // JSON: what was read comes back, a second save is byte for byte the
    // first, and a scene without sheets writes no spriteSheets key.
    {
        splash::Scene s = scene;
        s.spriteSheets[1].anims[0].extra.push_back({"futureAnim", "7"});
        splash::saveScene(s, project / "rt1.scene");
        splash::Scene back = splash::loadScene(project / "rt1.scene");
        CHECK(back.spriteSheets == s.spriteSheets);
        splash::saveScene(back, project / "rt2.scene");
        CHECK(readAll(project / "rt1.scene") == readAll(project / "rt2.scene"));
        s.spriteSheets.clear();
        splash::saveScene(s, project / "rt3.scene");
        CHECK(readAll(project / "rt3.scene").find("spriteSheets") == std::string::npos);
        CHECK(splash::loadScene(project / "rt3.scene").spriteSheets.empty());
    }

    // The table, byte for byte.
    splash::ExportResult r = splash::exportSplashpack(scene, project, project / "s.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "sprite export: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(project / "s.splashpack");
    if (b.size() > 136 && r.ok()) {
        const size_t off = size_t(rd32(b, 128));
        CHECK(off % 4 == 0 && off > 136 && rd16(b, 132) == 2 && rd16(b, 134) == 3);
        auto placed = [&](const char* file) {
            for (const auto& t : r.stats.textures)
                if (fs::path(t.source).filename() == file) return t;
            CHECK(!"sprite texture placed");
            return splash::ExportStats::TexturePlacement{};
        };
        const auto hero = placed("hero.png"), fx = placed("fx.png");
        CHECK(hero.cutout && fx.cutout && hero.bitDepth == 4 && fx.bitDepth == 8);
        std::string want;
        auto u8 = [&](int v) { want += char(uint8_t(v)); };
        auto u16 = [&](int v) { u8(v & 255), u8((v >> 8) & 255); };
        auto u32 = [&](size_t v) { u16(int(v & 0xffff)), u16(int(v >> 16)); };
        const size_t names = off + 2 * 20 + 3 * 12;
        // name offset, texpage x/y, u0, v0, clut x (16-word units)/y, cell, grid, bpp index, pads
        u32(names), u8(hero.x / 64), u8(hero.y / 256), u8((hero.x % 64) * 4), u8(hero.y % 256), u16(hero.clutX / 16),
            u16(hero.clutY), u8(16), u8(16), u8(4), u8(2), u8(0), u8(0), u16(0);
        u32(names + 5), u8(fx.x / 64), u8(fx.y / 256), u8((fx.x % 64) * 2), u8(fx.y % 256), u16(fx.clutX / 16), u16(fx.clutY),
            u8(12), u8(8), u8(4), u8(2), u8(1), u8(0), u16(0);
        // name offset, sheet, first cell, frames, vsyncs, loop, pads
        u32(names + 8), u8(0), u8(0), u8(4), u8(8), u8(1), u8(0), u16(0);
        u32(names + 13), u8(0), u8(4), u8(4), u8(4), u8(0), u8(0), u16(0);
        u32(names + 18), u8(1), u8(2), u8(5), u8(3), u8(1), u8(0), u16(0);
        want += std::string("hero\0fx\0walk\0jump\0spark\0", 24);
        CHECK(b.size() >= off + want.size() && b.compare(off, want.size(), want) == 0);
        CHECK(r.stats.spriteTableBytes == 100 && r.stats.spriteAnims == 3 && r.stats.spriteSheets.size() == 2);
        if (r.stats.spriteSheets.size() == 2) {
            const auto& s0 = r.stats.spriteSheets[0];
            CHECK(s0.name == "hero" && s0.bitDepth == 4 && s0.cellW == 16 && s0.cols == 4 && s0.rows == 2 && s0.anims == 2);
        }
    }
    // Without sheets the header words stay zero.
    {
        splash::Scene none = scene;
        none.spriteSheets.clear();
        splash::ExportResult nr = splash::exportSplashpack(none, project, project / "none.splashpack");
        const std::string nb = readAll(project / "none.splashpack");
        CHECK(nr.ok() && nb.size() > 136 && rd32(nb, 128) == 0 && rd16(nb, 132) == 0 && rd16(nb, 134) == 0);
        CHECK(nr.stats.spriteSheets.empty() && nr.stats.spriteTableBytes == 0);
    }

    // Each error, from the check export and the inspector share.
    auto problems = [&](const std::function<void(std::vector<splash::SpriteSheet>&)>& edit) {
        std::vector<splash::SpriteSheet> sh = scene.spriteSheets;
        edit(sh);
        std::vector<std::string> errors, warnings;
        splash::checkSpriteSheets(
            sh,
            [](const splash::SpriteSheet& s) -> splash::SpriteTextureSize {
                if (s.texture == "hero.png") return {64, 32, {}};
                if (s.texture == "fx.png") return {48, 16, {}};
                return {0, 0, "cannot load image " + s.texture};
            },
            errors, warnings);
        std::string all;
        for (const std::string& e : errors) all += e + "\n";
        return all;
    };
    CHECK(problems([](auto&) {}).empty());
    CHECK(problems([](auto& sh) {
              for (int i = 0; i < 15; i++) {
                  splash::SpriteSheet c = sh[1];
                  c.name = "s" + std::to_string(i);
                  c.anims.clear();
                  sh.push_back(c);
              }
          }).find("17 sprite sheets") != std::string::npos);
    CHECK(problems([](auto& sh) {
              for (int i = 0; i < 62; i++) {
                  splash::SpriteAnim c;
                  c.name = "a" + std::to_string(i);
                  sh[1].anims.push_back(c);
              }
          }).find("65 sprite animations") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].name.clear(); }).find("sprite sheet 2 has no name") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].name = "hero"; }).find("two sprite sheets are named 'hero'") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].texture.clear(); }).find("'fx' has no texture") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].texture = "gone.png"; }).find("'fx': cannot load image gone.png") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[0].cellW = 12; }).find("64x32, not a whole number of 12x16 cells") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[0].cellH = 256; }).find("each side must be 1..255") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[0].cellW = 0; }).find("each side must be 1..255") != std::string::npos);
    CHECK(problems([](auto& sh) {
              sh[0].cellW = sh[0].cellH = 2;
              sh[0].anims.clear();
          }).find("32x16 = 512 cells") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].firstFrame = 6; }).find("cells 6..10 are outside the sheet's 8 cells") !=
          std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].firstFrame = -1; }).find("are outside the sheet") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].frameCount = 0; }).find("0 frames, it needs at least 1") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].frameDuration = 0; }).find("lasts 0 vsyncs, it must be 1..255") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].frameDuration = 256; }).find("it must be 1..255") != std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].name = "jump"; }).find("two sprite animations are named 'jump'") !=
          std::string::npos);
    CHECK(problems([](auto& sh) { sh[0].anims[1].name = "walk"; }).find("two sprite animations are named 'walk'") !=
          std::string::npos);
    CHECK(problems([](auto& sh) { sh[1].anims[0].name.clear(); }).find("animation 1 has no name") != std::string::npos);
    // The texture page: inside, and past its right and bottom edges.
    CHECK(splash::spriteSheetPageError("p", 192, 224, 64, 32).empty());
    CHECK(splash::spriteSheetPageError("p", 224, 0, 64, 32).find("run past the page") != std::string::npos);
    CHECK(splash::spriteSheetPageError("p", 0, 240, 64, 32).find("packed at 0,240") != std::string::npos);
    // Through export: refused, nothing written.
    {
        splash::Scene bad = scene;
        bad.spriteSheets[1].anims[0].name = "walk";
        fs::remove(project / "bad.splashpack");
        splash::ExportResult br = splash::exportSplashpack(bad, project, project / "bad.splashpack");
        CHECK(!br.ok() && hasError(br, "two sprite animations are named 'walk'"));
        bad = scene;
        bad.spriteSheets[0].texture = "missing.png";
        br = splash::exportSplashpack(bad, project, project / "bad.splashpack");
        CHECK(!br.ok() && hasError(br, "sprite sheet 'hero'") && hasError(br, "missing.png"));
    }

    // The inspector's edits, each one undo step.
    {
        editor::Document d;
        d.reset(scene, project);
        CHECK(editor::uniqueSpriteSheetName(d.scene().spriteSheets, "hero") == "hero (2)");
        CHECK(editor::uniqueSpriteAnimName(d.scene().spriteSheets, "spark") == "spark (2)");
        CHECK(editor::uniqueSpriteAnimName(d.scene().spriteSheets, "idle") == "idle");
        const size_t h0 = d.historySize();
        CHECK(editor::addSpriteSheet(d, "art/hero.png") == 2);
        const splash::SpriteSheet& added = d.scene().spriteSheets[2];
        CHECK(added.name == "hero (2)" && added.texture == "art/hero.png" && added.bitDepth == splash::BitDepth::Bpp4 &&
              added.cellW == 16 && added.cellH == 16 && added.anims.size() == 1 && added.anims[0].name == "idle" &&
              added.anims[0].frameDuration == 6 && added.anims[0].loop);
        CHECK(d.historySize() == h0 + 1);
        // A second sheet's default animation is still unique scene-wide.
        CHECK(editor::addSpriteSheet(d, "fx.png") == 3 && d.scene().spriteSheets[3].name == "fx (2)" &&
              d.scene().spriteSheets[3].anims[0].name == "idle (2)");
        CHECK(d.undo() && d.scene().spriteSheets.size() == 3);
        // A typed cell size merges into one step under one key.
        CHECK(editor::editSpriteSheet(d, 2, [](splash::SpriteSheet& s) { s.cellW = 8; }, "cell"));
        CHECK(editor::editSpriteSheet(d, 2, [](splash::SpriteSheet& s) { s.cellW = 32; }, "cell"));
        d.endMerge();
        CHECK(d.scene().spriteSheets[2].cellW == 32 && d.historySize() == h0 + 2);
        CHECK(!editor::editSpriteSheet(d, 2, [](splash::SpriteSheet& s) { s.cellW = 32; }));  // no change, no step
        CHECK(d.historySize() == h0 + 2);
        CHECK(editor::addSpriteAnim(d, 0, 8) == 2 && d.scene().spriteSheets[0].anims[2].name == "anim" &&
              d.scene().spriteSheets[0].anims[2].firstFrame == 0);  // walk 0..3 and jump 4..7 fill the 8 cells: wraps to 0
        CHECK(editor::addSpriteAnim(d, 1, 8) == 1 && d.scene().spriteSheets[1].anims[1].name == "anim (2)" &&
              d.scene().spriteSheets[1].anims[1].firstFrame == 7);  // after spark 2..6
        CHECK(editor::deleteSpriteAnim(d, 0, 0) && d.scene().spriteSheets[0].anims[0].name == "jump");
        CHECK(d.undo() && d.scene().spriteSheets[0].anims[0].name == "walk");
        CHECK(editor::deleteSpriteSheet(d, 0) && d.scene().spriteSheets[0].name == "fx");
        CHECK(d.undo() && d.scene().spriteSheets[0].name == "hero" && d.redo() && d.scene().spriteSheets[0].name == "fx");
        CHECK(!editor::deleteSpriteSheet(d, 9) && !editor::deleteSpriteAnim(d, 0, 9) && editor::addSpriteAnim(d, 9, 0) == -1);
        while (d.undo()) {
        }
        CHECK(d.scene().spriteSheets == scene.spriteSheets);
        // At 16 sheets Add refuses.
        d.editSpriteSheets([](std::vector<splash::SpriteSheet>& sh) { sh.resize(16); });
        CHECK(editor::addSpriteSheet(d, "x.png") == -1 && d.scene().spriteSheets.size() == 16);
    }

    // Grid readout and the preview's frame timing (spritemath::advanceAnim).
    {
        splash::SpriteSheet s;
        editor::SpriteGrid g = editor::spriteGrid(s, 64, 32);
        CHECK(g.whole && g.cols == 4 && g.rows == 2 && g.cells() == 8);
        s.cellW = 24;
        g = editor::spriteGrid(s, 64, 32);
        CHECK(!g.whole && g.cells() == 0 && g.cols == 2);
        splash::SpriteAnim a;
        a.frameCount = 4;
        a.frameDuration = 8;
        CHECK(editor::spriteAnimFrameAt(a, 0) == 0 && editor::spriteAnimFrameAt(a, 7) == 0 && editor::spriteAnimFrameAt(a, 8) == 1);
        CHECK(editor::spriteAnimFrameAt(a, 31) == 3 && editor::spriteAnimFrameAt(a, 32) == 0);  // loops
        a.loop = false;
        CHECK(editor::spriteAnimFrameAt(a, 32) == 3 && editor::spriteAnimFrameAt(a, 1000) == 3);  // holds the last
        a.frameDuration = 0;
        CHECK(editor::spriteAnimFrameAt(a, 2) == 2);  // 0 steps every vsync, as the engine does
    }
}

// The tilemap in the scene JSON: what was read comes back with unknown keys
// kept, a second save is byte for byte the first, the cells are rows of brush
// indices, and a scene without one writes no tilemap key.
void testTilemapModel() {
    using namespace roomtest;
    const fs::path dir = g_outDir / "tilemapmodel";
    fs::remove_all(dir);
    fs::create_directories(dir);
    splash::Scene s;
    splash::Tilemap t;
    t.tileset = "tiles";
    t.resize(3, 2);
    splash::TileBrush wall;
    wall.name = "wall";
    wall.cell = 1;
    wall.walkable = false;
    wall.color = {0.5f, 0.25f, 1};
    wall.extra.push_back({"futureBrush", "\"x\""});
    splash::TileBrush task;
    task.name = "task";
    task.cell = 2;
    task.kind = 3;
    task.id = 7;
    t.brushes = {splash::TileBrush{}, wall, task};
    t.kindNames = {"(none)", "vent", "spawn", "task"};
    t.cells = {0, 1, -1, 2, 0, 5};
    t.extra.push_back({"futureMap", "[1,2]"});
    s.tilemap = t;
    splash::saveScene(s, dir / "rt1.scene");
    const splash::Scene back = splash::loadScene(dir / "rt1.scene");
    CHECK(back.tilemap && *back.tilemap == t);
    splash::saveScene(back, dir / "rt2.scene");
    const std::string text = readAll(dir / "rt1.scene");
    CHECK(text == readAll(dir / "rt2.scene"));
    // Stable key order, cells as one string a row, right-aligned to the widest index.
    const size_t k0 = text.find("\"tileset\""), k1 = text.find("\"width\"", k0), k2 = text.find("\"brushes\"", k1),
                 k3 = text.find("\"kindNames\"", k2), k4 = text.find("\"cells\"", k3), k5 = text.find("\"futureMap\"", k4);
    CHECK(k0 != std::string::npos && k1 != std::string::npos && k2 != std::string::npos && k3 != std::string::npos &&
          k4 != std::string::npos && k5 != std::string::npos);
    const std::string rowsText = "\"cells\": [\n      \" 0  1 -1\",\n      \" 2  0  5\"\n    ],";
    CHECK(text.compare(k4, rowsText.size(), rowsText) == 0);
    {
        // One digit wide when no index needs more.
        splash::Scene one;
        one.tilemap = t;
        one.tilemap->cells = {0, 1, 2, 2, 0, 1};
        splash::saveScene(one, dir / "one.scene");
        CHECK(readAll(dir / "one.scene").find("\"0 1 2\",\n      \"2 0 1\"") != std::string::npos);
    }
    // Rows split on any whitespace; short rows read empty, long ones are cut;
    // the earlier arrays of numbers still read; a word is an error; the size is clamped.
    {
        std::ofstream(dir / "ws.scene") << R"({"format":"splashedit-ng/scene","version":1,"settings":{},"objects":[],)"
                                           R"("tilemap":{"width":3,"height":3,"cells":["\t1   -1\t 0  ","2","3 3 3 3"]}})";
        const splash::Scene ws = splash::loadScene(dir / "ws.scene");
        CHECK(ws.tilemap && ws.tilemap->cells == std::vector<int>({1, -1, 0, 2, -1, -1, 3, 3, 3}));
        std::ofstream(dir / "word.scene") << R"({"format":"splashedit-ng/scene","version":1,"settings":{},"objects":[],)"
                                             R"("tilemap":{"width":2,"height":1,"cells":["1 x"]}})";
        bool threw = false;
        try {
            splash::loadScene(dir / "word.scene");
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("tilemap row 1: 'x' is not a brush index") != std::string::npos;
        }
        CHECK(threw);
        std::ofstream(dir / "short.scene") << R"({"format":"splashedit-ng/scene","version":1,"settings":{},"objects":[],)"
                                              R"("tilemap":{"tileset":"t","width":3,"height":2,"cells":[[1],[2,2,2,2]]}})";
        const splash::Scene sh = splash::loadScene(dir / "short.scene");
        CHECK(sh.tilemap && sh.tilemap->cells == std::vector<int>({1, -1, -1, 2, 2, 2}) && sh.tilemap->brushes.empty() &&
              sh.tilemap->kindNames == std::vector<std::string>({"(none)"}));
        std::ofstream(dir / "big.scene") << R"({"format":"splashedit-ng/scene","version":1,"settings":{},"objects":[],)"
                                            R"("tilemap":{"width":999,"height":0}})";
        const splash::Scene big = splash::loadScene(dir / "big.scene");
        CHECK(big.tilemap && big.tilemap->width == 256 && big.tilemap->height == 1 && big.tilemap->cells.size() == 256);
    }
    // Resize keeps the overlap and fills the rest empty.
    {
        splash::Tilemap r = t;
        r.resize(2, 3);
        CHECK(r.width == 2 && r.height == 3 && r.cells == std::vector<int>({0, 1, 2, 0, -1, -1}));
        r.resize(0, 999);
        CHECK(r.width == 1 && r.height == 256 && r.at(0, 0) == 0 && r.at(0, 1) == 2 && r.at(0, 2) == -1 && r.at(1, 0) == -1);
    }
    s.tilemap.reset();
    splash::saveScene(s, dir / "rt3.scene");
    CHECK(readAll(dir / "rt3.scene").find("tilemap") == std::string::npos);
    CHECK(!splash::loadScene(dir / "rt3.scene").tilemap);
}

// The tilemap chunk, byte for byte as psxsplash's tilesystem.cpp reads it
// (SplashEdit's PSXTilemapExporter.Flatten and PSXSceneWriter), and each
// problem export refuses it for.
void testTilemapExport() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "tilemap";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "tests" / "sprite", project, fs::copy_options::recursive);
    splash::Scene scene = splash::loadScene(project / "sprite.scene");
    for (splash::Object& o : scene.objects)
        if (o.mesh) {
            o.mesh->mesh = (src / "examples" / "courtyard" / "meshes" / "crate.mesh").string();
            for (splash::Material& m : o.mesh->materials) m.texture = (src / "examples" / "courtyard" / "textures" / "crate.png").string();
        }
    // fx.png: 48x16 in 12x8 cells, so 8 cells; the second sheet, so index 1.
    splash::Tilemap t;
    t.tileset = "fx";
    t.resize(4, 3);
    auto brush = [](const char* name, int cell, bool walkable, int kind, int id) {
        splash::TileBrush b;
        b.name = name, b.cell = cell, b.walkable = walkable, b.kind = kind, b.id = id;
        return b;
    };
    t.brushes = {brush("floor", 0, true, 0, -1), brush("wall", 1, false, 0, -1), brush("task", 2, true, 3, -1),
                 brush("hole", -1, true, 5, -1), brush("vent", 7, false, 2, 9)};
    t.cells = {0, 1, 2, -1,  //
               2, 3, 4, 0,   //
               1, 1, 2, -1};
    scene.tilemap = t;

    // Flatten: holes and empty cells draw nothing and are solid; a hole is no
    // object; auto ids count each kind in reading order, a set id is kept.
    {
        const splash::FlatTilemap ft = splash::flattenTilemap(t);
        const std::vector<uint8_t> cells = {0, 1, 1, 0, 2, 1, 0xFF, 0, 2, 1, 0xFF, 0, 7, 0, 0, 1, 1, 0, 1, 0, 2, 1, 0xFF, 0};
        CHECK(ft.cells == cells);
        const std::vector<splash::TileObjectRecord> objs = {{3, 1, 2, 0}, {3, 2, 0, 1}, {2, 9, 2, 1}, {3, 3, 2, 2}};
        CHECK(ft.objects == objs);
    }

    splash::ExportResult r = splash::exportSplashpack(scene, project, project / "t.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "tilemap export: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(project / "t.splashpack");
    if (r.ok() && b.size() > 144) {
        // The header word (v23's former reserved word, after the scene hash).
        const size_t off = size_t(rd32(b, 140));
        CHECK(off != 0 && off % 4 == 0 && b.size() >= off + 68);
        CHECK(rd16(b, off) == 4 && rd16(b, off + 2) == 3);                          // width, height
        CHECK(uint8_t(b[off + 4]) == 12 && uint8_t(b[off + 5]) == 8);                // tile size: the sheet's cell
        CHECK(uint8_t(b[off + 6]) == 1 && uint8_t(b[off + 7]) == 0);                 // tileset sheet, pad
        CHECK(rd16(b, off + 8) == 4 && rd16(b, off + 10) == 0);                      // object count, pad
        CHECK(size_t(rd32(b, off + 12)) == off + 20 && size_t(rd32(b, off + 16)) == off + 44);  // absolute offsets
        // A cell: (2,1) is the vent, tile 7, solid.
        CHECK(uint8_t(b[off + 20 + (1 * 4 + 2) * 2]) == 7 && uint8_t(b[off + 20 + (1 * 4 + 2) * 2 + 1]) == 0);
        // A wall cell, and an empty one.
        CHECK(uint8_t(b[off + 20 + 2]) == 1 && uint8_t(b[off + 20 + 3]) == 0);
        CHECK(uint8_t(b[off + 20 + 6]) == 0xFF && uint8_t(b[off + 20 + 7]) == 0);
        // An object: the third, the vent with its own id 9 at 2,1.
        const size_t o2 = off + 44 + 2 * 6;
        CHECK(uint8_t(b[o2]) == 2 && uint8_t(b[o2 + 1]) == 9 && rd16(b, o2 + 2) == 2 && rd16(b, o2 + 4) == 1);
        CHECK(r.stats.tilemap.width == 4 && r.stats.tilemap.height == 3 && r.stats.tilemap.tileW == 12 && r.stats.tilemap.tileH == 8 &&
              r.stats.tilemap.sheet == 1 && r.stats.tilemap.objects == 4 && r.stats.tilemap.bytes == 20 + 24 + 24);
    }
    // Without objects the objects offset is 0; without a tilemap the word is.
    {
        splash::Scene s = scene;
        for (splash::TileBrush& br : s.tilemap->brushes) br.kind = 0;
        splash::ExportResult nr = splash::exportSplashpack(s, project, project / "noobj.splashpack");
        const std::string nb = readAll(project / "noobj.splashpack");
        const size_t off = nb.size() > 144 ? size_t(rd32(nb, 140)) : 0;
        CHECK(nr.ok() && off && rd16(nb, off + 8) == 0 && rd32(nb, off + 16) == 0 && nr.stats.tilemap.bytes == 20 + 24);
        s.tilemap.reset();
        nr = splash::exportSplashpack(s, project, project / "none.splashpack");
        const std::string zb = readAll(project / "none.splashpack");
        CHECK(nr.ok() && zb.size() > 144 && rd32(zb, 140) == 0 && nr.stats.tilemap.bytes == 0);
    }
    // Each problem: export refuses, saying what to fix.
    auto refused = [&](const std::function<void(splash::Tilemap&)>& edit, const std::string& part) {
        splash::Scene s = scene;
        edit(*s.tilemap);
        const splash::ExportResult br = splash::exportSplashpack(s, project, project / "bad.splashpack");
        if (!hasError(br, part))
            for (const std::string& e : br.errors) std::fprintf(stderr, "tilemap error: %s\n", e.c_str());
        return !br.ok() && hasError(br, part);
    };
    CHECK(refused([](splash::Tilemap& m) { m.tileset.clear(); }, "The tilemap has no tileset"));
    CHECK(refused([](splash::Tilemap& m) { m.tileset = "gone"; }, "tileset 'gone' is not one of the scene's sprite sheets"));
    CHECK(refused([](splash::Tilemap& m) { m.brushes[4].cell = 8; },
                  "Tile brush 'vent' draws tileset cell 8, but 'fx' has 8 (0..7)"));
    CHECK(refused([](splash::Tilemap& m) { m.brushes[0].cell = -2; }, "Tile brush 'floor' draws tileset cell -2: use -1 for a hole"));
    CHECK(refused([](splash::Tilemap& m) { m.cells[3] = 5, m.cells[11] = 6; },
                  "2 tilemap cells are painted with a brush that no longer exists"));
    CHECK(refused(
        [](splash::Tilemap& m) {
            m.resize(16, 9);
            std::fill(m.cells.begin(), m.cells.end(), 2);
        },
        "144 tilemap object tiles, psxsplash holds at most 128"));
    {
        // 128 is still fine.
        splash::Scene s = scene;
        s.tilemap->resize(16, 8);
        std::fill(s.tilemap->cells.begin(), s.tilemap->cells.end(), 2);
        CHECK(splash::exportSplashpack(s, project, project / "max.splashpack").stats.tilemap.objects == 128);
    }
}

// The inspector's and the Tilemap tab's edits (editor/tilemap.hh), each one undo step.
void testTilemapEdits() {
    splash::Scene scene;
    splash::SpriteSheet sheet;
    sheet.name = "tiles";
    sheet.texture = "tiles.png";
    scene.spriteSheets = {sheet};
    editor::Document d;
    d.reset(scene, g_outDir);
    const size_t h0 = d.historySize();
    CHECK(editor::addTilemap(d, "tiles", 8) && d.historySize() == h0 + 1);
    CHECK(!editor::addTilemap(d, "tiles", 8));  // one a scene
    {
        const splash::Tilemap& t = *d.scene().tilemap;
        CHECK(t.tileset == "tiles" && t.width == 20 && t.height == 15 && t.cells.size() == 300 &&
              std::all_of(t.cells.begin(), t.cells.end(), [](int c) { return c == -1; }));
        CHECK(t.brushes.size() == 2 && t.brushes[0].name == "floor" && t.brushes[0].cell == 0 && t.brushes[0].walkable &&
              t.brushes[1].name == "wall" && t.brushes[1].cell == 1 && !t.brushes[1].walkable);
    }
    // A stroke: several paints under one key are one undo step; the next stroke is another.
    CHECK(editor::paintTiles(d, {{0, 0}}, 0, "s1"));
    CHECK(editor::paintTiles(d, editor::tileLine(0, 0, 3, 0), 0, "s1"));
    CHECK(!editor::paintTiles(d, {{2, 0}}, 0, "s1"));  // nothing changed
    d.endMerge();
    CHECK(d.historySize() == h0 + 2 && d.scene().tilemap->at(3, 0) == 0 && d.scene().tilemap->at(4, 0) == -1);
    CHECK(editor::paintTiles(d, {{1, 0}, {1, 1}, {-1, 0}, {99, 99}}, 1, "s2"));  // off-map cells skipped
    d.endMerge();
    CHECK(d.historySize() == h0 + 3 && d.scene().tilemap->at(1, 0) == 1 && d.scene().tilemap->at(1, 1) == 1);
    CHECK(d.undo() && d.scene().tilemap->at(1, 0) == 0 && d.scene().tilemap->at(1, 1) == -1);
    CHECK(d.undo() && d.scene().tilemap->at(0, 0) == -1 && d.redo() && d.scene().tilemap->at(3, 0) == 0);
    // A rectangle, either corner first, clipped to the map.
    CHECK(editor::fillTiles(d, 2, 3, 0, 1, 1) == 9 && d.scene().tilemap->at(0, 1) == 1 && d.scene().tilemap->at(2, 3) == 1);
    CHECK(editor::fillTiles(d, 18, 13, 25, 25, 0) == 4 && d.scene().tilemap->at(19, 14) == 0);
    CHECK(editor::fillTiles(d, 18, 13, 25, 25, 0) == 0);
    // The line has no gaps, either way round.
    CHECK(editor::tileLine(0, 0, 3, 1).size() == 4 && editor::tileLine(3, 1, 0, 0).front() == (std::array<int, 2>{3, 1}));
    CHECK(editor::tileLine(2, 2, 2, 2).size() == 1 && editor::tileLine(0, 0, 0, 5).size() == 6);
    // Resize keeps the overlap; a typed size merges under its key.
    CHECK(editor::resizeTilemap(d, 4, 2, "w") && editor::resizeTilemap(d, 3, 2, "w"));
    d.endMerge();
    {
        const splash::Tilemap& t = *d.scene().tilemap;
        CHECK(t.width == 3 && t.height == 2 && t.cells == std::vector<int>({0, 0, 0, 1, 1, 1}));
    }
    CHECK(d.undo() && d.scene().tilemap->width == 20 && d.scene().tilemap->at(19, 14) == 0 && d.redo());
    // Brushes: added after the last cell; deleting one empties its cells and moves the later ones down.
    CHECK(editor::addTileBrush(d, 8) == 2 && d.scene().tilemap->brushes[2].name == "brush" && d.scene().tilemap->brushes[2].cell == 2);
    CHECK(editor::addTileBrush(d, 3) == 3 && d.scene().tilemap->brushes[3].name == "brush (2)" && d.scene().tilemap->brushes[3].cell == 0);
    CHECK(editor::paintTiles(d, {{2, 1}}, 3, "s3"));
    d.endMerge();
    CHECK(editor::deleteTileBrush(d, 1));
    {
        const splash::Tilemap& t = *d.scene().tilemap;
        CHECK(t.brushes.size() == 3 && t.brushes[1].name == "brush" && t.cells == std::vector<int>({0, 0, 0, -1, -1, 2}));
    }
    CHECK(d.undo() && d.scene().tilemap->cells == std::vector<int>({0, 0, 0, 1, 1, 3}) && d.scene().tilemap->brushes.size() == 4);
    CHECK(!editor::deleteTileBrush(d, 9));
    // A sheet rename carries the tileset, in the same step.
    CHECK(editor::editSpriteSheet(d, 0, [](splash::SpriteSheet& s) { s.name = "floor tiles"; }));
    CHECK(d.scene().tilemap->tileset == "floor tiles");
    CHECK(d.undo() && d.scene().spriteSheets[0].name == "tiles" && d.scene().tilemap->tileset == "tiles");
    // Remove, and back.
    const splash::Tilemap kept = *d.scene().tilemap;
    CHECK(editor::removeTilemap(d) && !d.scene().tilemap && !editor::removeTilemap(d));
    CHECK(!editor::paintTiles(d, {{0, 0}}, 0, "s4") && editor::fillTiles(d, 0, 0, 1, 1, 0) == 0);
    CHECK(d.undo() && d.scene().tilemap && *d.scene().tilemap == kept);
    while (d.undo()) {
    }
    CHECK(!d.scene().tilemap);
}

// Direction sets: Sprite.SetFacingFromYaw(sprite, "walk_s", n, yaw) plays the
// animation facingFromYaw(yaw, n) places after walk_s in the exported table,
// so a set is n animations named in increasing-yaw order, one after another.
void testSpriteDirectionSets() {
    using namespace roomtest;
    const fs::path src = SPLASHEDIT_SOURCE_DIR;
    const fs::path project = g_outDir / "spritedirs";
    fs::remove_all(project);
    fs::create_directories(project);
    fs::copy(src / "tests" / "sprite", project, fs::copy_options::recursive);
    splash::Scene scene = splash::loadScene(project / "sprite.scene");
    for (splash::Object& o : scene.objects)
        if (o.mesh) {
            o.mesh->mesh = (src / "examples" / "courtyard" / "meshes" / "crate.mesh").string();
            for (splash::Material& m : o.mesh->materials) m.texture = (src / "examples" / "courtyard" / "textures" / "crate.png").string();
        }
    // hero: walk 0+4, jump 4+4 on 8 cells; fx: spark 2+5 on 8 cells.
    CHECK(scene.spriteSheets.size() == 2 && scene.spriteSheets[0].anims.size() == 2 && scene.spriteSheets[1].anims.size() == 1);

    // The suffixes, in the order yaw increases from 0.
    CHECK(splash::spriteDirectionSuffixes(4) == (std::vector<std::string>{"n", "e", "s", "w"}));
    CHECK(splash::spriteDirectionSuffixes(8) == (std::vector<std::string>{"n", "ne", "e", "se", "s", "sw", "w", "nw"}));
    CHECK(splash::spriteDirectionSuffixes(6).empty() && splash::spriteDirectionSuffixes(0).empty());

    editor::Document d;
    d.reset(scene, project);
    const size_t h0 = d.historySize();
    std::string err = "stale";
    // 4 directions of 2 frames on fx: zap_n 0-1, zap_e 2-3, zap_s 4-5, zap_w 6-7, in one step.
    editor::DirectionSetSpec four;
    four.base = "zap";
    four.directions = 4;
    four.framesPerDirection = 2;
    four.firstCell = 0;
    four.frameDuration = 5;
    four.loop = false;
    CHECK(editor::addDirectionSet(d, 1, four, 8, &err) == 1 && err.empty());
    CHECK(d.historySize() == h0 + 1);
    {
        const auto& fa = d.scene().spriteSheets[1].anims;
        const char* names[4] = {"zap_n", "zap_e", "zap_s", "zap_w"};
        CHECK(fa.size() == 5);
        for (int k = 0; k < 4 && fa.size() == 5; k++)
            CHECK(fa[size_t(1 + k)].name == names[k] && fa[size_t(1 + k)].firstFrame == 2 * k && fa[size_t(1 + k)].frameCount == 2 &&
                  fa[size_t(1 + k)].frameDuration == 5 && !fa[size_t(1 + k)].loop);
        const std::vector<editor::DirectionSet> sets = editor::directionSets(d.scene().spriteSheets[1]);
        CHECK(sets.size() == 1 && sets[0].base == "zap" && sets[0].count == 4 && sets[0].first == 1);
        CHECK(editor::directionSetOf(sets, 0) == nullptr && editor::directionSetOf(sets, 1) && editor::directionSetOf(sets, 4) &&
              editor::directionSetOf(sets, 5) == nullptr);
        if (!sets.empty()) CHECK(editor::facingLuaLine(sets[0]) == "Sprite.SetFacingFromYaw(sprite, \"zap_n\", 4, yaw)");
    }
    CHECK(d.undo() && d.scene().spriteSheets == scene.spriteSheets);  // one Ctrl+Z takes all four
    CHECK(d.redo() && d.scene().spriteSheets[1].anims.size() == 5);
    // 8 directions of 1 frame on hero, after walk and jump.
    editor::DirectionSetSpec eight;
    eight.base = "run";
    eight.framesPerDirection = 1;
    eight.firstCell = 0;
    CHECK(eight.directions == 8 && eight.frameDuration == 6 && eight.loop);
    CHECK(editor::addDirectionSet(d, 0, eight, 8, &err) == 2 && err.empty());
    {
        const auto& ha = d.scene().spriteSheets[0].anims;
        const char* names[8] = {"run_n", "run_ne", "run_e", "run_se", "run_s", "run_sw", "run_w", "run_nw"};
        CHECK(ha.size() == 10);
        for (int k = 0; k < 8 && ha.size() == 10; k++)
            CHECK(ha[size_t(2 + k)].name == names[k] && ha[size_t(2 + k)].firstFrame == k && ha[size_t(2 + k)].frameCount == 1);
        const std::vector<editor::DirectionSet> sets = editor::directionSets(d.scene().spriteSheets[0]);
        CHECK(sets.size() == 1 && sets[0].base == "run" && sets[0].count == 8 && sets[0].first == 2);
    }

    // Each refusal says why and leaves the scene and its history alone.
    auto refused = [&](int sheet, const editor::DirectionSetSpec& spec, int cells) {
        const size_t h = d.historySize();
        const std::vector<splash::SpriteSheet> before = d.scene().spriteSheets;
        std::string why;
        const int r = editor::addDirectionSet(d, sheet, spec, cells, &why);
        CHECK(r == -1 && d.historySize() == h && d.scene().spriteSheets == before);
        if (why.empty()) std::fprintf(stderr, "direction set refusal without a reason\n");
        return why;
    };
    editor::DirectionSetSpec s4 = four;
    s4.frameDuration = 6;
    CHECK(refused(0, s4, 8).find("'zap_n' is already an animation in sheet 'fx'") != std::string::npos);  // any sheet
    d.editSpriteSheets([](std::vector<splash::SpriteSheet>& sh) { sh[0].anims[0].name = "fly_s"; });
    s4.base = "fly";
    CHECK(refused(1, s4, 8).find("'fly_s' is already an animation in sheet 'hero'") != std::string::npos);  // not just _n
    CHECK(d.undo());
    s4.base = "hop";
    s4.firstCell = 1;
    CHECK(refused(0, s4, 8).find("4 x 2 frames from cell 1 end on cell 8, past the sheet's 8 cells (0..7)") != std::string::npos);
    s4.firstCell = 0;
    s4.framesPerDirection = 3;
    CHECK(refused(0, s4, 8).find("end on cell 11") != std::string::npos);
    s4.framesPerDirection = 2;
    CHECK(refused(0, s4, 0).find("cells are unknown") != std::string::npos);
    editor::DirectionSetSpec bad = s4;
    bad.directions = 6;
    CHECK(refused(0, bad, 8).find("4 or 8 directions") != std::string::npos);
    bad = s4;
    bad.base.clear();
    CHECK(refused(0, bad, 8).find("Type a base name") != std::string::npos);
    bad.base = std::string(22, 'x');  // xxx_ne would be 25
    CHECK(refused(0, bad, 8).find("too long") != std::string::npos);
    bad.base = std::string(21, 'x');  // 24 exactly fits
    CHECK(editor::addDirectionSet(d, 0, bad, 8, &err) >= 0 && d.undo());
    bad = s4;
    bad.framesPerDirection = 0;
    CHECK(refused(0, bad, 8).find("Frames per direction") != std::string::npos);
    bad = s4;
    bad.frameDuration = 256;
    CHECK(refused(0, bad, 8).find("frame time") != std::string::npos);
    bad = s4;
    bad.firstCell = -1;
    CHECK(refused(0, bad, 8).find("first cell") != std::string::npos);
    CHECK(editor::addDirectionSet(d, 7, s4, 8, &err) == -1 && !err.empty());
    // psxsplash's 64 animations: 15 now; 45 more make 60, then 4 fit and 8 do not.
    d.editSpriteSheets([](std::vector<splash::SpriteSheet>& sh) {
        for (int i = 0; i < 45; i++) {
            splash::SpriteAnim a;
            a.name = "pad" + std::to_string(i);
            sh[1].anims.push_back(a);
        }
    });
    eight.base = "dash";
    CHECK(refused(0, eight, 8).find("8 more animations would make 68, psxsplash loads at most 64") != std::string::npos);
    CHECK(editor::addDirectionSet(d, 0, s4, 8, &err) >= 0 && d.undo());  // 64 exactly
    CHECK(d.undo() && d.scene().spriteSheets[1].anims.size() == 5);

    // The form's defaults: a row of cells per direction, after the sheet's animations when the set fits there.
    {
        splash::SpriteSheet s;
        s.anims.push_back({});  // idle on cell 0
        editor::DirectionSetSpec def = editor::defaultDirectionSet(s, 4, 8, 8);
        CHECK(def.base == "walk" && def.directions == 4 && def.framesPerDirection == 8 && def.firstCell == 8);
        def = editor::defaultDirectionSet(s, 8, 8, 8);
        CHECK(def.directions == 8 && def.framesPerDirection == 8 && def.firstCell == 0);  // 8 + 64 > 64: from the top
        def = editor::defaultDirectionSet(s, 8, 0, 0);  // grid unknown
        CHECK(def.framesPerDirection == 1 && def.firstCell == 1);
    }

    // Recognised sets: exactly the suffixes in order, one after another.
    auto sheetOf = [](std::initializer_list<const char*> names) {
        splash::SpriteSheet s;
        s.name = "t";
        for (const char* n : names) {
            splash::SpriteAnim a;
            a.name = n;
            s.anims.push_back(a);
        }
        return s;
    };
    auto setsOf = [&](std::initializer_list<const char*> names) {
        std::string o;
        for (const editor::DirectionSet& ds : editor::directionSets(sheetOf(names)))
            o += ds.base + ":" + std::to_string(ds.count) + "@" + std::to_string(ds.first) + " ";
        return o;
    };
    CHECK(setsOf({"idle", "walk_n", "walk_e", "walk_s", "walk_w"}) == "walk:4@1 ");
    CHECK(setsOf({"a_n", "a_ne", "a_e", "a_se", "a_s", "a_sw", "a_w", "a_nw", "b_n", "b_e", "b_s", "b_w"}) == "a:8@0 b:4@8 ");
    CHECK(setsOf({"my_walk_n", "my_walk_e", "my_walk_s", "my_walk_w"}) == "my_walk:4@0 ");
    CHECK(setsOf({"walk_n", "walk_s", "walk_e", "walk_w"}).empty());           // out of order
    CHECK(setsOf({"walk_n", "walk_e", "walk_s"}).empty());                      // missing w
    CHECK(setsOf({"walk_n", "walk_e", "idle", "walk_s", "walk_w"}).empty());    // not one after another
    CHECK(setsOf({"walk_n", "walk_ne", "walk_e", "walk_se", "walk_s", "walk_sw", "walk_w"}).empty());  // 7 of 8
    CHECK(setsOf({"walk_n", "run_e", "run_s", "run_w"}).empty());               // bases differ
    CHECK(setsOf({"_n", "_e", "_s", "_w"}).empty());                            // no base

    // The Problems warning, from checkSpriteSheets (export and the inspector).
    auto warned = [&](std::vector<splash::SpriteSheet> sh) {
        std::vector<std::string> errors, warnings;
        splash::checkSpriteSheets(
            sh, [](const splash::SpriteSheet&) -> splash::SpriteTextureSize { return {64, 64, {}}; }, errors, warnings);
        std::string all;
        for (const std::string& w : warnings) all += w + "\n";
        return all;
    };
    CHECK(warned({sheetOf({"idle", "walk_n", "walk_e", "walk_s", "walk_w"})}).empty());
    CHECK(warned({sheetOf({"door_s", "idle"})}).empty());  // one direction-like name is just a name
    std::string w = warned({sheetOf({"walk_n", "walk_s", "walk_e", "walk_w"})});
    CHECK(w.find("sprite animations walk_*: listed in the order n s e w") != std::string::npos &&
          w.find("Sprite.SetFacingFromYaw(sprite, \"walk_n\", 4, yaw) would play the wrong animation: list them n e s w") !=
              std::string::npos);
    w = warned({sheetOf({"walk_n", "walk_e", "walk_s"})});
    CHECK(w.find("no walk_w, so Sprite.SetFacingFromYaw") != std::string::npos);
    w = warned({sheetOf({"walk_n", "walk_ne", "walk_e", "walk_s", "walk_sw", "walk_w", "walk_nw"})});
    CHECK(w.find("no walk_se") != std::string::npos && w.find("\", 8, yaw)") != std::string::npos);
    w = warned({sheetOf({"walk_n", "walk_e", "idle", "walk_s", "walk_w"})});
    CHECK(w.find("other animations sit between them") != std::string::npos && w.find("wrong animation") != std::string::npos);
    w = warned({sheetOf({"walk_n", "walk_e", "idle"}), sheetOf({"walk_s", "walk_w"})});
    CHECK(w.find("other animations sit between them in the table (they are split across sprite sheets)") != std::string::npos);
    {
        std::vector<splash::SpriteSheet> two = {sheetOf({"walk_n", "walk_e"}), sheetOf({"walk_s", "walk_w"})};
        two[1].name = "u";
        w = warned(two);
        CHECK(w.find("split across sprite sheets 't' and 'u'") != std::string::npos);
    }
    w = warned({sheetOf({"walk_n", "walk_e", "walk_e", "walk_s", "walk_w"})});
    CHECK(w.find("more than once") != std::string::npos);
    // What the editor adds never warns.
    CHECK(warned(d.scene().spriteSheets).empty());

    // Export keeps the editor's order: sheets as listed, each sheet's
    // animations as listed, so a set stays one after another in the table and
    // its table index + direction lands on the right name.
    splash::ExportResult r = splash::exportSplashpack(d.scene(), project, project / "dirs.splashpack");
    for (const std::string& e : r.errors) std::fprintf(stderr, "direction set export: %s\n", e.c_str());
    CHECK(r.ok());
    const std::string b = readAll(project / "dirs.splashpack");
    std::vector<std::pair<int, std::string>> want;  // (sheet, name) in editor order
    for (size_t i = 0; i < d.scene().spriteSheets.size(); i++)
        for (const splash::SpriteAnim& a : d.scene().spriteSheets[i].anims) want.push_back({int(i), a.name});
    CHECK(want.size() == 15);
    if (r.ok() && b.size() > 136) {
        const size_t off = size_t(rd32(b, 128));
        const int sheetsN = rd16(b, 132), animsN = rd16(b, 134);
        CHECK(sheetsN == 2 && animsN == 15);
        std::vector<std::pair<int, std::string>> got;
        std::vector<int> firsts;
        for (int k = 0; k < animsN && off + 40 + 12 * size_t(k) + 12 <= b.size(); k++) {
            const size_t rec = off + 20 * size_t(sheetsN) + 12 * size_t(k);
            const size_t nameAt = size_t(rd32(b, rec));
            got.push_back({int(uint8_t(b[rec + 4])), nameAt < b.size() ? std::string(b.c_str() + nameAt) : std::string()});
            firsts.push_back(int(uint8_t(b[rec + 5])));
        }
        CHECK(got == want);
        // The table index SetFacingFromYaw adds the direction to.
        auto indexOf = [&](const std::string& n) {
            for (size_t k = 0; k < got.size(); k++)
                if (got[k].second == n) return int(k);
            return -1;
        };
        const std::vector<std::string>& s8 = splash::spriteDirectionSuffixes(8);
        const std::vector<std::string>& s4n = splash::spriteDirectionSuffixes(4);
        const int run = indexOf("run_n"), zap = indexOf("zap_n");
        CHECK(run == 2 && zap == 11);
        for (int k = 0; k < 8 && run >= 0 && size_t(run + k) < got.size(); k++)
            CHECK(got[size_t(run + k)].second == "run_" + s8[size_t(k)] && got[size_t(run + k)].first == 0 &&
                  firsts[size_t(run + k)] == k);
        for (int k = 0; k < 4 && zap >= 0 && size_t(zap + k) < got.size(); k++)
            CHECK(got[size_t(zap + k)].second == "zap_" + s4n[size_t(k)] && got[size_t(zap + k)].first == 1 &&
                  firsts[size_t(zap + k)] == 2 * k);
    }
}

// Dragging a whole-number field moves it one per pixel at any size (#174):
// a 300 px wide element grows by 5 for 5 px, not by 15; Shift takes 10 px
// per step. Other fields keep growing with the value.
void testNumberScrub() {
    auto drag = [](float start, std::initializer_list<float> dxs, bool integer, bool fine) {
        float acc = start, shown = start;
        for (float dx : dxs) shown = ui::scrubTo(acc, dx, integer, fine);
        return shown;
    };
    CHECK(drag(300, {1, 1, 1, 1, 1}, true, false) == 305);
    CHECK(drag(300, {5}, true, false) == 305);
    CHECK(drag(300, {-3}, true, false) == 297);
    CHECK(drag(8, {1}, true, false) == 9);
    for (float start : {0.0f, 3.0f, 120.0f, 319.0f, 1000.0f}) {
        float acc = start, last = start;
        for (int px = 0; px < 40; px++) {
            const float v = ui::scrubTo(acc, 1, true, false);
            CHECK(v - last == 1);
            last = v;
        }
    }
    CHECK(drag(300, {1, 1, 1, 1, 1, 1, 1, 1, 1}, true, true) == 301);  // Shift: 0.9 px of travel rounds to 1
    CHECK(drag(300, {4}, true, true) == 300);
    CHECK(ui::scrubRate(300, true, false).step == 1);
    // Fractional fields: 1% of the value per pixel, rounded to a power of ten.
    CHECK(std::fabs(ui::scrubRate(300, false, false).perPixel - 3) < 1e-4f);
    CHECK(std::fabs(ui::scrubRate(0.5f, false, false).perPixel - 0.01f) < 1e-6f);
    CHECK(std::fabs(drag(2.0f, {1}, false, false) - 2.02f) < 1e-4f);
}

// A colour field's Copy / Paste text (#176): Copy writes what the field
// shows, Paste reads that back and the usual hand-typed forms, nothing else.
void testColourHex() {
    CHECK(ui::colourHex({1.0f, 0.5f, 0.0f}) == "#FF8000");
    CHECK(ui::colourHex({-1.0f, 2.0f, 0.2f}) == "#00FF33");
    auto is = [](const char* text, unsigned rgb) {
        const auto c = ui::parseColourHex(text);
        if (!c) return false;
        const unsigned want[3] = {(rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u};
        for (int i = 0; i < 3; i++)
            if (std::lround((*c)[size_t(i)] * 255.0f) != long(want[i])) return false;
        return true;
    };
    CHECK(is("#FF8000", 0xFF8000));
    CHECK(is("ff8000", 0xFF8000));
    CHECK(is("  #a1B2c3\n", 0xA1B2C3));
    CHECK(is("#F80", 0xFF8800));
    CHECK(is("#000", 0x000000));
    for (const char* bad : {"", "#", "F80", "#FF80", "#FF80000", "FF800G", "#GG0000", "red", "# FF8000", "0xFF8000", "#FF 8000"})
        CHECK(!ui::parseColourHex(bad));
    // Round trip: what Copy writes, Paste reads back to the same field text.
    for (unsigned rgb : {0x000000u, 0x123456u, 0xFFFFFFu, 0x7F807Fu}) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%06X", rgb);
        const auto c = ui::parseColourHex(hex);
        CHECK(c && ui::colourHex(*c) == hex);
    }
}

// One native file dialog at a time (#159): a second is refused while one is
// open, and the gate opens again once it answers.
void testDialogGate() {
    ui::DialogGate gate;
    CHECK(!gate.open());
    CHECK(gate.enter());
    CHECK(gate.open());
    CHECK(!gate.enter());  // a second dialog while the first is up
    CHECK(gate.open());
    gate.leave();
    CHECK(!gate.open());
    CHECK(gate.enter());
    // The answer comes on SDL's thread: two takers racing, one wins.
    gate.leave();
    std::atomic<int> won{0};
    std::thread a([&] { won += gate.enter(); }), b([&] { won += gate.enter(); });
    a.join();
    b.join();
    CHECK(won == 1);
}

// Borderless window chrome (#162): a maximized window has no resize edges, so
// the minimize/maximize/close buttons in the top band take their clicks, and
// the title bar still drags.
void testWindowChrome() {
    using ui::ChromeHit;
    const ui::ChromeRect bar{0, 0, 1600, 32};
    const std::vector<ui::ChromeRect> buttons{{1480, 0, 1600, 32}};
    // A point in the top band, over the close button.
    CHECK(ui::chromeHitTest(1600, 960, false, 1597, 2, bar, buttons) == ChromeHit::ResizeTopRight);
    CHECK(ui::chromeHitTest(1600, 960, true, 1597, 2, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::chromeHitTest(1600, 960, false, 1500, 2, bar, buttons) == ChromeHit::ResizeTop);
    CHECK(ui::chromeHitTest(1600, 960, true, 1500, 2, bar, buttons) == ChromeHit::Normal);
    // Every edge resizes only while restored.
    CHECK(ui::chromeHitTest(1600, 960, false, 2, 500, bar, buttons) == ChromeHit::ResizeLeft);
    CHECK(ui::chromeHitTest(1600, 960, false, 1597, 500, bar, buttons) == ChromeHit::ResizeRight);
    CHECK(ui::chromeHitTest(1600, 960, false, 800, 957, bar, buttons) == ChromeHit::ResizeBottom);
    CHECK(ui::chromeHitTest(1600, 960, false, 2, 957, bar, buttons) == ChromeHit::ResizeBottomLeft);
    CHECK(ui::chromeHitTest(1600, 960, true, 2, 500, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::chromeHitTest(1600, 960, true, 1597, 500, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::chromeHitTest(1600, 960, true, 800, 957, bar, buttons) == ChromeHit::Normal);
    // The title bar drags in both states, including its top band when maximized.
    CHECK(ui::chromeHitTest(1600, 960, false, 800, 16, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::chromeHitTest(1600, 960, true, 800, 16, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::chromeHitTest(1600, 960, true, 800, 1, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::chromeHitTest(1600, 960, true, 800, 500, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::chromeHitTest(1600, 960, true, 1500, 16, bar, buttons) == ChromeHit::Normal);
}

// UI scale on high-DPI monitors (#136): the part of the OS scale the window
// does not already carry as pixel density, the override, and the window
// chrome hit-tested in window coordinates for rectangles drawn in UI ones.
void testUiScale() {
    using ui::ChromeHit;
    // Windows / X11 at 150%: pixel-sized window, display asks for 1.5.
    CHECK(ui::contentScale(1.5f, 1.0f) == 1.5f);
    // macOS retina, Wayland at 200%: the backend already renders at density 2.
    CHECK(ui::contentScale(2.0f, 2.0f) == 1.0f);
    // Wayland fractional 150% with a 2x buffer, then scaled by the compositor.
    CHECK(ui::contentScale(3.0f, 2.0f) == 1.5f);
    // SDL could not answer: 1, never a division by zero or NaN.
    CHECK(ui::contentScale(0.0f, 1.0f) == 1.0f);
    CHECK(ui::contentScale(1.5f, 0.0f) == 1.5f);
    CHECK(ui::contentScale(std::nanf(""), 1.0f) == 1.0f);
    CHECK(ui::contentScale(1.0f, std::nanf("")) == 1.0f);
    CHECK(ui::contentScale(100.0f, 1.0f) == ui::kMaxUiScale);
    // The override wins over the display.
    CHECK(ui::contentScale(1.5f, 1.0f, 2.0f) == 2.0f);
    CHECK(ui::contentScale(2.0f, 2.0f, 0.75f) == 0.75f);
    float v = 1;
    CHECK(ui::parseUiScale("2", v) && v == 2.0f);
    CHECK(ui::parseUiScale("1.5", v) && v == 1.5f);
    CHECK(ui::parseUiScale("0.5", v) && v == 0.5f);
    CHECK(ui::parseUiScale("4", v) && v == 4.0f);
    v = 1;
    CHECK(!ui::parseUiScale("0.4", v) && v == 1);
    CHECK(!ui::parseUiScale("4.5", v) && v == 1);
    CHECK(!ui::parseUiScale("", v) && !ui::parseUiScale(nullptr, v) && !ui::parseUiScale("2x", v));
    CHECK(!ui::parseUiScale("nan", v) && !ui::parseUiScale("inf", v) && v == 1);
    CHECK(ui::toUi(300, 2) == 150 && ui::toWindow(150, 2) == 300);
    // A scripted point survives ImGui's floor in window coordinates and the
    // UI pass's division, landing on the same UI pixel.
    for (float c : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f})
        for (int x = 0; x < 400; ++x) CHECK(std::floor(std::floor(ui::scriptedWindowCoord((float)x, c)) / c) == x);
    // The window chrome at 2x: a 40-unit title bar is 80 pixels, the close
    // button's rectangle doubles, and the resize band is 12 pixels deep.
    const ui::ChromeRect bar{0, 0, 800, 40};
    const std::vector<ui::ChromeRect> buttons{{740, 0, 800, 40}};
    CHECK(ui::resizeEdge(1) == ui::kResizeEdge && ui::resizeEdge(2) == 2 * ui::kResizeEdge && ui::resizeEdge(0.1f) == 1);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 800, 60, 2, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 800, 85, 2, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 1550, 40, 2, bar, buttons) == ChromeHit::Normal);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 1400, 40, 2, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 800, 10, 2, bar, buttons) == ChromeHit::ResizeTop);
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 400, 10, 1, bar, buttons) == ChromeHit::Draggable);
    CHECK(ui::scaledChromeHitTest(1600, 960, true, 800, 10, 2, bar, buttons) == ChromeHit::Draggable);
    // Unscaled, the same point past the UI rectangles is the plain client area.
    CHECK(ui::scaledChromeHitTest(1600, 960, false, 400, 60, 1, bar, buttons) == ChromeHit::Normal);
    // The default window grows with the scale, but not past the usable area.
    const ui::WindowSize grown = ui::initialWindowSize(1600, 960, 1.5f, 3840, 2100);
    CHECK(grown.w == 2400 && grown.h == 1440);
    const ui::WindowSize capped = ui::initialWindowSize(1600, 960, 2.0f, 2560, 1400);
    CHECK(capped.w == 2560 && capped.h == 1400);
    const ui::WindowSize unknown = ui::initialWindowSize(1600, 960, 2.0f, 0, 0);
    CHECK(unknown.w == 3200 && unknown.h == 1920);
}

int main(int argc, char** argv) {
    std::error_code ec;
    std::filesystem::path exe = argc > 0 ? std::filesystem::absolute(argv[0], ec) : std::filesystem::path();
    g_outDir = (exe.has_parent_path() ? exe.parent_path() : std::filesystem::current_path()) / "editor_tests_out";
    testApplyUndoRedo();
    testMerge();
    testSettings();
    testSceneRendering();
    testRenderPeak();
    testMemCard();
    testDirtyAndSave();
    testComponentFieldsSaveUndo();
    testStructure();
    testMove();
    testPick();
    testTriggerWorldBox();
    testGizmoMath();
    testScaleClamp();
    testGizmoUndo();
    testCatalog();
    testExportStats();
    testObjectRamBudget();
    testScriptWithoutMesh();
    testWorldStreamPlan();
    testWorldStreamExport();
    testTriangleBudget();
    testNavBigWorld();
    testRoomsExport();
    testActiveInHierarchyExport();
    testBvhBoxes();
    testNavInfo();
    testNavPresets();
    testNavWalkoff();
    testAgent();
    testSkinInfo();
    testLogViewFilter();
    testLogViewClear();
    testLogViewRestart();
    testLogViewJoin();
    testRoomsErrors();
    testRoomsSceneRoundTrip();
    testRoomsEditor();
    testRoomRename();
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
    testSeqRenameScriptVars();
    testTimelineUiPositionStart();
    testTimelineUiStartValues();
    testTimelinePasteAddsTrack();
    testTimelineSkinEventProblem();
    testLuaTokenize();
    testLuaBuffer();
    testLuaComplete();
    testLuaDefinitions();
    testLuaScopes();
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
    testPlayOverlays();
    testPlayBootLogo();
    testBuildScenes();
    testBuildReport();
    testVramLayout();
    testProject();
    testProjectTemplates();
    testAdoptAsset();
    testNewScript();
    testExternalEditorSetup();
    testNewBlocksScript();
    testScriptDefinesCallback();
    testBlockHelp();
    testBlocks();
    testBlocksLayout();
    testBlocksHitTest();
    testBlocksEdits();
    testBlocksSnap();
    testBlocksHistory();
    testBlocksNewBlock();
    testBlocksVariables();
    testSeqRenameBlocks();
    testPlaceModel();
    testPlaceSkinnedModel();
    testDefaultClipLoop();
    testClipLoopImportAndEdit();
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
    testMeshAlpha();
    testAssetBrowser();
    testBlocksConvert();
    testUiLayout();
    testUiHitTest();
    testUiResize();
    testUiSnap();
    testUiZOrder();
    testUiUndo();
    testUiLoadingRename();
    testUiFonts();
    testUiCanvasDefaultFont();
    testUiClipboard();
    testUiMultiSelect();
    testUiProblems();
    testUiRender();
    testUiMatchesRuntime();
    testLoaderPack();
    testExportCanvasDefaultFont();
    testDiscLicenceParse();
    testDiscSettings();
    testDiscLicenceImage();
    testDiscExportSettings();
    testAudioSizes();
    testWavInfo();
    testAudioFormats();
    testDiscMusic();
    testMeshColours();
    testMaterials4bpp();
    testUvOffsetSlots();
    testSceneNetworkId();
    testSpriteSheets();
    testTilemapModel();
    testTilemapExport();
    testTilemapEdits();
    testSpriteDirectionSets();
    testUiSprite();
    testNumberScrub();
    testDialogGate();
    testColourHex();
    testWindowChrome();
    testUiScale();
    if (g_failures) {
        std::fprintf(stderr, "editor_tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("editor_tests: all %d checks passed\n", g_checks);
    return 0;
}
