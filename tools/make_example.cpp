// Writes the bundled example project (examples/courtyard): subdivided box
// meshes, 64x64 crate, stone and brick textures, a Lua stub and
// courtyard.scene, all through splashcore's own writers.
//
//   make_example [output dir]   (default: the source tree's examples/courtyard)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "content_kit.hh"
#include "scene.hh"
#include "texture.hh"

namespace fs = std::filesystem;
using splash::Vec3;

using namespace kit;

int main(int argc, char** argv) {
    fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path(SPLASHEDIT_EXAMPLE_DIR);
    try {
        fs::create_directories(out / "meshes");
        fs::create_directories(out / "textures");
        fs::create_directories(out / "scripts");

        // The courtyard corner the viewport used to hard-code, in Unity's
        // left-handed space: the back wall is at +Z and the camera looks along
        // +Z, so the editor (like Unity) shows the side wall on the left. Each
        // mesh is centred on its object's origin.
        splash::saveMesh(box({18, 0.2f, 15}, 0.5f), out / "meshes/floor.mesh");
        splash::saveMesh(box({14, 4.5f, 0.3f}, 0.5f), out / "meshes/back_wall.mesh");
        splash::saveMesh(box({0.3f, 4.5f, 11}, 0.5f), out / "meshes/side_wall.mesh");
        splash::saveMesh(box({0.8f, 3.6f, 0.8f}, 1), out / "meshes/pillar.mesh");
        {
            // Platform plus the step in front of it (towards -Z).
            splash::Mesh m;
            addBox(m, {-2.5f, -0.4f, -0.7f}, {2.5f, 0.4f, 0.7f}, 1);
            addBox(m, {-1.5f, -0.4f, -1.3f}, {1.5f, 0.0f, -0.7f}, 1);
            splash::saveMesh(m, out / "meshes/platform.mesh");
        }
        splash::saveMesh(box({1, 1, 1}), out / "meshes/crate.mesh");
        splash::savePng(crateTexture(), out / "textures/crate.png");
        splash::savePng(stoneTexture(), out / "textures/stone.png");
        splash::savePng(brickTexture(), out / "textures/brick.png");
        {
            std::ofstream lua(out / "scripts/game_logic.lua", std::ios::binary);
            lua << "-- Courtyard demo: scene-wide game logic.\n\nfunction onCreate(self)\nend\n";
        }

        splash::Scene s;
        const std::array<float, 4> white{1, 1, 1, 1};

        splash::Object env = object("Environment", {});
        auto part = [&](const char* name, Vec3 pos, const char* mesh, const char* texture) {
            splash::Object o = object(name, pos);
            o.mesh = meshComp(mesh, white, texture);
            o.collider = staticCollider();
            env.children.push_back(o);
        };
        part("Floor", {0, -0.1f, -1.5f}, "meshes/floor.mesh", "textures/stone.png");
        part("Back Wall", {0, 2.25f, 6.15f}, "meshes/back_wall.mesh", "textures/brick.png");
        part("Side Wall", {-7.15f, 2.25f, 0.5f}, "meshes/side_wall.mesh", "textures/brick.png");
        part("Pillar", {-3.5f, 1.8f, 3.5f}, "meshes/pillar.mesh", "textures/stone.png");
        part("Pillar (2)", {3, 1.8f, 4}, "meshes/pillar.mesh", "textures/stone.png");
        part("Platform", {4, 0.4f, 5.3f}, "meshes/platform.mesh", "textures/stone.png");
        s.objects.push_back(env);

        splash::Object crates = object("Crates", {});
        auto crate = [&](const char* name, Vec3 pos, float yaw, float size) {
            splash::Object o = object(name, pos);
            o.transform.rotation = euler(0, yaw, 0);
            o.transform.scale = {size, size, size};
            o.mesh = meshComp("meshes/crate.mesh", white, "textures/crate.png");
            o.collider = staticCollider();
            crates.children.push_back(o);
        };
        crate("Crate", {0, 0.5f, 1}, -15, 1);
        crate("Crate (2)", {-1.7f, 0.5f, 1.9f}, 0, 1);
        crate("Crate (3)", {-1.65f, 1.35f, 1.85f}, 10, 0.7f);
        s.objects.push_back(crates);

        s.objects.push_back(object("Player Start", {1.6f, 0, -2.2f}));
        {
            splash::Object cam = object("Main Camera", {7.6f, 5.2f, -9.2f});
            cam.transform.rotation = euler(17, -37, 0);
            s.objects.push_back(cam);
        }
        {
            splash::Object torch = object("Torch Light", {-2.6f, 2.4f, 5.0f});
            splash::LightComponent l;
            l.kind = splash::LightKind::Point;
            l.color = {1.0f, 0.59f, 0.25f};
            l.intensity = 1.05f;
            l.range = 9;
            torch.light = l;
            s.objects.push_back(torch);
        }
        {
            splash::Object amb = object("Ambience", {-4.8f, 1.2f, -1.5f});
            amb.active = false;
            s.objects.push_back(amb);
        }
        {
            splash::Object logic = object("Game Logic", {});
            logic.script.emplace().lua = "scripts/game_logic.lua";
            s.objects.push_back(logic);
        }
        splash::saveScene(s, out / "courtyard.scene");

        // Read everything back so a writer/reader mismatch shows up here.
        splash::Scene back = splash::loadScene(out / "courtyard.scene");
        for (const char* m : {"floor", "back_wall", "side_wall", "pillar", "platform", "crate"})
            splash::loadMesh(out / "meshes" / (std::string(m) + ".mesh"));
        std::printf("wrote %s (%zu top-level objects)\n", (out / "courtyard.scene").string().c_str(), back.objects.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "make_example: %s\n", e.what());
        return 1;
    }
    return 0;
}
