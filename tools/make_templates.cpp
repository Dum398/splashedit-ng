// Writes the new-project templates the editor offers (templates/<id>/): a
// template.json (name, description, order) and a project tree for each.
// Both are a walled yard with a Player (so the camera follows it and the
// scene gets a nav mesh) and two levers that do the same thing, switching a
// lamp on and off and counting the pulls: one is a Lua script, the other a
// block script, so a beginner sees both forms side by side. Third person adds
// a visible body and a Lua camera script that keeps the camera behind it.
// The "empty" template is built into the editor and has no folder.
//
//   make_templates [templates dir]   (default: the source tree's templates/)
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "blocks.hh"
#include "content_kit.hh"
#include "scene.hh"
#include "texture.hh"

namespace fs = std::filesystem;
using namespace kit;

namespace {

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
    if (!out.flush()) throw std::runtime_error("cannot write " + p.string());
}

// The Lua lever. lever_blocks.blocks is the same logic as blocks.
std::string leverLua(const char* lamp) {
    return std::string("-- Lever, written in Lua. Walk up to the lever and press Square: each pull\n"
                       "-- switches the lamp next to it on or off and counts the pulls.\n"
                       "-- The other lever runs scripts/lever_blocks.blocks, the same thing as blocks.\n"
                       "\n"
                       "local lit = true\n"
                       "local pulls = 0\n"
                       "\n"
                       "-- Called when the player presses the Interactable's button near this object.\n"
                       "function onInteract(self)\n"
                       "    lit = not lit\n"
                       "    pulls = pulls + 1\n"
                       "    Entity.SetActive(Entity.Find(\"") +
           lamp +
           "\"), lit)\n"
           "    Debug.Log(\"Lua lever pulled \" .. pulls .. \" times\")\n"
           "end\n";
}

splash::Block blk(std::string op, std::string field = {}, std::vector<splash::Block> args = {},
                  std::vector<std::vector<splash::Block>> bodies = {}) {
    return splash::Block{std::move(op), std::move(field), std::move(args), std::move(bodies)};
}

// The block lever: the same variables and steps as leverLua.
std::string leverBlocks(const char* lamp) {
    splash::BlockScript s;
    s.variables = {{"lit", "true"}, {"pulls", "0"}};
    splash::BlockStack st;
    st.x = 40;
    st.y = 40;
    st.blocks = {
        blk("on_interact"),
        blk("set_var", "lit", {blk("not", {}, {blk("var", "lit")})}),
        blk("change_var", "pulls", {blk("number", "1")}),
        blk("if_else", {}, {blk("var", "lit")}, {{blk("show", lamp)}, {blk("hide", lamp)}}),
        blk("log", {}, {blk("join", {}, {blk("text", "Blocks lever pulled "), blk("join", {}, {blk("var", "pulls"), blk("text", " times")})})}),
    };
    s.stacks.push_back(st);
    std::string text = splash::saveBlocks(s);
    splash::parseBlocks(text, "lever_blocks.blocks");  // round-trips
    return text;
}

// Third person: psxsplash has no built-in third-person camera, so this
// script stops the camera following the player's eyes and, every frame,
// stands the body where the player is and puts the camera behind and above it.
const char* kCameraLua =
    "-- Third-person camera, on the Player Body object. Blocks have no camera\n"
    "-- blocks, so this one is Lua only.\n"
    "--\n"
    "-- psxsplash moves the player (its position is the player's eyes) and, by\n"
    "-- default, puts the camera there: first person. This script takes the camera\n"
    "-- over, and every frame stands this body where the player is and puts the\n"
    "-- camera behind and above it, looking at the player's head.\n"
    "--\n"
    "-- Positions are in GTE units: one metre is 1/100 at the scene's GTE scaling\n"
    "-- of 100. Y points down while the game runs, so up is -y.\n"
    "\n"
    "local DISTANCE = FixedPoint.new(3) / 100   -- 3 m behind the player\n"
    "local HEIGHT = FixedPoint.new(1) / 100     -- 1 m above the player's eyes\n"
    "local EYES = FixedPoint.new(18) / 1000     -- the Player's height, 1.8 m: eyes to feet\n"
    "\n"
    "function onCreate(self)\n"
    "    Camera.FollowPsxPlayer(false)\n"
    "end\n"
    "\n"
    "function onUpdate(self, dt)\n"
    "    local eyes = Player.GetPosition()\n"
    "    local turn = Player.GetRotation()\n"
    "    -- The body's origin is at its feet.\n"
    "    Entity.SetPosition(self, Vec3.new(eyes.x, eyes.y + EYES, eyes.z))\n"
    "    Entity.SetRotationY(self, turn.y)\n"
    "    -- Face the way the player faces, then step back along that direction.\n"
    "    Camera.SetRotation(Vec3.new(0, turn.y, 0))\n"
    "    local forward = Camera.GetForward()\n"
    "    Camera.SetPosition(Vec3.new(eyes.x - forward.x * DISTANCE, eyes.y - HEIGHT, eyes.z - forward.z * DISTANCE))\n"
    "    Camera.LookAt(eyes)\n"
    "end\n";

void writeTemplate(const fs::path& out, const std::string& id, const char* name, const char* description, int order,
                   bool thirdPerson) {
    fs::create_directories(out / "meshes");
    fs::create_directories(out / "textures");
    fs::create_directories(out / "scripts");
    {
        nlohmann::ordered_json j;
        j["name"] = name;
        j["description"] = description;
        j["order"] = order;
        writeText(out / "template.json", j.dump(2) + "\n");
    }

    splash::saveMesh(box({16, 0.2f, 16}, 0.5f), out / "meshes/floor.mesh");
    splash::saveMesh(box({16, 3, 0.3f}, 0.5f), out / "meshes/wall.mesh");
    splash::saveMesh(box({0.8f, 3, 0.8f}, 1), out / "meshes/pillar.mesh");
    splash::saveMesh(box({1, 1, 1}), out / "meshes/crate.mesh");
    {
        // A lever: a base plate with a handle sticking up, origin on the floor.
        splash::Mesh m;
        addBox(m, {-0.3f, 0, -0.2f}, {0.3f, 0.2f, 0.2f});
        addBox(m, {-0.06f, 0.2f, -0.06f}, {0.06f, 1.0f, 0.06f});
        splash::saveMesh(m, out / "meshes/lever.mesh");
    }
    splash::saveMesh(box({0.25f, 2, 0.25f}), out / "meshes/lamp_post.mesh");
    splash::saveMesh(box({0.5f, 0.5f, 0.5f}), out / "meshes/lamp.mesh");
    if (thirdPerson) {
        // The body: a torso block with a head on top, origin at the feet,
        // facing +Z (the head sits a little forward so the facing shows).
        splash::Mesh m;
        addBox(m, {-0.3f, 0, -0.2f}, {0.3f, 1.4f, 0.2f});
        addBox(m, {-0.18f, 1.4f, -0.12f}, {0.18f, 1.75f, 0.24f});
        splash::saveMesh(m, out / "meshes/body.mesh");
    }
    splash::savePng(crateTexture(), out / "textures/crate.png");
    splash::savePng(stoneTexture(), out / "textures/stone.png");
    splash::savePng(brickTexture(), out / "textures/brick.png");
    writeText(out / "scripts/lever_lua.lua", leverLua("Lamp (Lua)"));
    writeText(out / "scripts/lever_blocks.blocks", leverBlocks("Lamp (Blocks)"));
    if (thirdPerson) writeText(out / "scripts/third_person_camera.lua", kCameraLua);

    splash::Scene s;
    const std::array<float, 4> white{1, 1, 1, 1};

    splash::Object env = object("Environment", {});
    auto part = [&](const char* nm, Vec3 pos, float yaw, const char* mesh, const char* texture) {
        splash::Object o = object(nm, pos);
        o.transform.rotation = euler(0, yaw, 0);
        o.mesh = meshComp(mesh, white, texture);
        o.collider = staticCollider();
        env.children.push_back(o);
    };
    part("Floor", {0, -0.1f, 0}, 0, "meshes/floor.mesh", "textures/stone.png");
    part("Back Wall", {0, 1.5f, 8.15f}, 0, "meshes/wall.mesh", "textures/brick.png");
    part("Left Wall", {-8.15f, 1.5f, 0}, 90, "meshes/wall.mesh", "textures/brick.png");
    part("Right Wall", {8.15f, 1.5f, 0}, 90, "meshes/wall.mesh", "textures/brick.png");
    part("Pillar", {-5, 1.5f, -2}, 0, "meshes/pillar.mesh", "textures/stone.png");
    part("Pillar (2)", {5, 1.5f, -2}, 0, "meshes/pillar.mesh", "textures/stone.png");
    part("Crate", {5.5f, 0.5f, 3}, -15, "meshes/crate.mesh", "textures/crate.png");
    part("Crate (2)", {6.4f, 0.5f, 4.4f}, 10, "meshes/crate.mesh", "textures/crate.png");
    part("Lamp Post", {-4.4f, 1, 6.5f}, 0, "meshes/lamp_post.mesh", "");
    part("Lamp Post (2)", {4.4f, 1, 6.5f}, 0, "meshes/lamp_post.mesh", "");
    s.objects.push_back(env);

    // The two levers and the lamps they switch.
    auto lever = [&](const char* nm, Vec3 pos, const char* script) {
        splash::Object o = object(nm, pos);
        o.mesh = meshComp("meshes/lever.mesh", {0.75f, 0.2f, 0.15f, 1});
        splash::InteractableComponent in;
        in.radius = 2;
        in.button = 15;  // Square: Cross is jump
        o.interactable = in;
        o.script.emplace().lua = script;
        s.objects.push_back(o);
    };
    auto lamp = [&](const char* nm, Vec3 pos) {
        splash::Object o = object(nm, pos);
        o.mesh = meshComp("meshes/lamp.mesh", {1, 0.9f, 0.35f, 1});
        s.objects.push_back(o);
    };
    lever("Lever (Lua)", {-3, 0, 6}, "scripts/lever_lua.lua");
    lamp("Lamp (Lua)", {-4.4f, 2.25f, 6.5f});
    lever("Lever (Blocks)", {3, 0, 6}, "scripts/lever_blocks.blocks");
    lamp("Lamp (Blocks)", {4.4f, 2.25f, 6.5f});

    {
        splash::Object player = object("Player", {0, 0, -5});
        player.player.emplace();
        s.objects.push_back(player);
    }
    if (thirdPerson) {
        splash::Object body = object("Player Body", {0, 0, -5});
        body.mesh = meshComp("meshes/body.mesh", {0.25f, 0.45f, 0.85f, 1});
        body.script.emplace().lua = "scripts/third_person_camera.lua";
        s.objects.push_back(body);
    }
    // The courtyard example's key and fill lights.
    auto sun = [&](const char* nm, splash::Quat rot, std::array<float, 3> color, float intensity) {
        splash::Object o = object(nm, {0, 6, 0});
        o.transform.rotation = rot;
        splash::LightComponent l;
        l.kind = splash::LightKind::Directional;
        l.color = color;
        l.intensity = intensity;
        o.light = l;
        s.objects.push_back(o);
    };
    sun("Sun", {0.40821789f, -0.23456972f, 0.10938165f, 0.8754261f}, {1.0f, 0.95f, 0.85f}, 0.75f);
    sun("Fill", {0.07782839f, 0.92121983f, -0.2904595f, 0.24684011f}, {0.6f, 0.65f, 0.85f}, 0.35f);
    const fs::path scene = out / (id + ".scene");
    splash::saveScene(s, scene);

    // Read everything back so a writer/reader mismatch shows up here.
    splash::Scene back = splash::loadScene(scene);
    for (const auto& e : fs::directory_iterator(out / "meshes")) splash::loadMesh(e.path());
    std::printf("wrote %s (%zu top-level objects)\n", scene.string().c_str(), back.objects.size());
}

}  // namespace

int main(int argc, char** argv) {
    fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path(SPLASHEDIT_TEMPLATE_DIR);
    try {
        writeTemplate(out / "first-person", "first_person", "First person",
                      "A walled yard you walk around in first person. Two levers switch a lamp on and off "
                      "and count the pulls: one runs a Lua script, the other the same steps as blocks.",
                      10, false);
        writeTemplate(out / "third-person", "third_person", "Third person",
                      "The same yard seen from behind a visible player body. The camera that follows it is a "
                      "Lua script (blocks have no camera blocks); the two levers are one Lua, one blocks.",
                      20, true);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "make_templates: %s\n", e.what());
        return 1;
    }
    return 0;
}
