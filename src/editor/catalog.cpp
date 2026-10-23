#include "editor/catalog.hh"

#include <algorithm>
#include <cctype>

#include "terrain.hh"

namespace editor {

const std::vector<ComponentInfo>& components() {
    static const std::vector<ComponentInfo> list = {
        {ComponentKind::Mesh, "Mesh", "model geometry renderer", "Draws a model with its textures."},
        {ComponentKind::Collider, "Collider", "physics collision solid static dynamic", "Makes the mesh solid to the player."},
        {ComponentKind::Script, "Script", "lua code behaviour", "Runs a Lua file for this object."},
        {ComponentKind::Light, "Light", "lamp point spot directional", "Lights the meshes around it."},
        {ComponentKind::Player, "Player", "character controller spawn", "Where the player starts, and how it moves."},
        {ComponentKind::Navigation, "Navigation", "navmesh walkable bake", "Bakes walkable ground without a player."},
        {ComponentKind::Trigger, "Trigger box", "zone volume area enter exit", "Calls Lua when the player walks in or out."},
        {ComponentKind::Interactable, "Interactable", "button press use prompt", "Calls Lua when the player presses a button nearby."},
        {ComponentKind::Audio, "Audio clip", "sound sfx wav mp3 ogg music", "A sound Lua can play by name."},
        {ComponentKind::Skin, "Skinned animation", "skeleton bones clips anim", "Plays animation clips on a skinned mesh."},
        {ComponentKind::Room, "Room", "portal culling interior zone visibility", "A box whose geometry draws only when it can be seen."},
        {ComponentKind::Portal, "Portal", "door doorway opening room link culling", "An opening between two Rooms the camera can see through."},
        {ComponentKind::Walkoff, "Nav walk-off zone", "ledge pit drop edge fall navigation", "Lets the player walk off nav edges inside this box."},
        {ComponentKind::Agent, "Agent (NPC)", "npc enemy guard ai patrol waypoint vision hearing chase", "A character that walks the nav regions, patrols and senses the player."},
        {ComponentKind::Terrain, "Terrain", "landscape ground heightmap height field hills sculpt paint", "Ground you sculpt and paint in the Scene view."},
        {ComponentKind::Path, "Path", "curve route rail track spline waypoints moving platform patrol camera dolly", "A curve that objects, agents and the camera ride."},
        {ComponentKind::FollowPath, "Follow path", "ride move along curve route platform patrol", "Moves this object along a Path."},
    };
    return list;
}

const ComponentInfo& info(ComponentKind k) {
    for (const ComponentInfo& c : components())
        if (c.kind == k) return c;
    return components().front();
}

bool hasComponent(const splash::Object& o, ComponentKind k) {
    switch (k) {
        case ComponentKind::Mesh: return o.mesh.has_value();
        case ComponentKind::Collider: return o.collider.has_value();
        case ComponentKind::Script: return o.script.has_value();
        case ComponentKind::Light: return o.light.has_value();
        case ComponentKind::Player: return o.player.has_value();
        case ComponentKind::Navigation: return o.navigation.has_value();
        case ComponentKind::Trigger: return o.trigger.has_value();
        case ComponentKind::Interactable: return o.interactable.has_value();
        case ComponentKind::Audio: return o.audio.has_value();
        case ComponentKind::Skin: return o.skin.has_value();
        case ComponentKind::Room: return o.room.has_value();
        case ComponentKind::Portal: return o.portal.has_value();
        case ComponentKind::Walkoff: return o.walkoff.has_value();
        case ComponentKind::Agent: return o.agent.has_value();
        case ComponentKind::Terrain: return o.terrain.has_value();
        case ComponentKind::Path: return o.path.has_value();
        case ComponentKind::FollowPath: return o.followPath.has_value();
    }
    return false;
}

std::string cannotAdd(const splash::Object& o, ComponentKind k) {
    if (hasComponent(o, k)) return "Already added";
    // The exporter only reads colliders and skins off mesh objects.
    if ((k == ComponentKind::Collider || k == ComponentKind::Skin) && !o.mesh) return "Needs a Mesh";
    // A terrain builds its own mesh: not on an object that draws a model, or animates one.
    if (k == ComponentKind::Terrain && o.mesh && !o.mesh->mesh.empty()) return "Has a model mesh";
    if (k == ComponentKind::Terrain && o.skin) return "Has a skin";
    if (k == ComponentKind::Skin && o.terrain) return "Is a terrain";
    return {};
}

bool addComponent(splash::Object& o, ComponentKind k) {
    if (!cannotAdd(o, k).empty()) return false;
    switch (k) {
        case ComponentKind::Mesh: o.mesh.emplace(); break;
        case ComponentKind::Collider: o.collider.emplace().kind = splash::ColliderKind::Static; break;
        case ComponentKind::Script: o.script.emplace(); break;
        case ComponentKind::Light: o.light.emplace(); break;
        case ComponentKind::Player: o.player.emplace(); break;
        case ComponentKind::Navigation: o.navigation.emplace(); break;
        case ComponentKind::Trigger: o.trigger.emplace(); break;
        case ComponentKind::Interactable: o.interactable.emplace(); break;
        case ComponentKind::Audio: o.audio.emplace().clipName = o.name; break;
        case ComponentKind::Skin: o.skin.emplace(); break;
        case ComponentKind::Room: o.room.emplace(); break;
        case ComponentKind::Portal: o.portal.emplace(); break;
        case ComponentKind::Walkoff: o.walkoff.emplace(); break;
        case ComponentKind::Agent: o.agent.emplace(); break;
        case ComponentKind::Terrain: {
            // A flat grid, its layers in a mesh component's materials, solid and walkable.
            splash::TerrainComponent& t = o.terrain.emplace();
            t.data = defaultTerrainData(o.name);
            splash::initTerrain(t, 32);
            if (!o.mesh) o.mesh.emplace();
            if (o.mesh->materials.empty()) o.mesh->materials.emplace_back();
            if (!o.collider) o.collider.emplace();
            o.collider->kind = splash::ColliderKind::Static;
            break;
        }
        case ComponentKind::Path:
            // Something to see and drag: a 4 m line through the object.
            o.path.emplace().points = {{-2, 0, 0}, {2, 0, 0}};
            break;
        case ComponentKind::FollowPath: o.followPath.emplace(); break;
    }
    return true;
}

bool removeComponent(splash::Object& o, ComponentKind k) {
    if (!hasComponent(o, k)) return false;
    switch (k) {
        case ComponentKind::Mesh:
            o.mesh.reset();
            o.collider.reset();
            o.skin.reset();
            o.terrain.reset();  // its layers are the mesh's materials
            break;
        case ComponentKind::Collider: o.collider.reset(); break;
        case ComponentKind::Script: o.script.reset(); break;
        case ComponentKind::Light: o.light.reset(); break;
        case ComponentKind::Player: o.player.reset(); break;
        case ComponentKind::Navigation: o.navigation.reset(); break;
        case ComponentKind::Trigger: o.trigger.reset(); break;
        case ComponentKind::Interactable: o.interactable.reset(); break;
        case ComponentKind::Audio: o.audio.reset(); break;
        case ComponentKind::Skin: o.skin.reset(); break;
        case ComponentKind::Room: o.room.reset(); break;
        case ComponentKind::Portal: o.portal.reset(); break;
        case ComponentKind::Walkoff: o.walkoff.reset(); break;
        case ComponentKind::Agent: o.agent.reset(); break;
        case ComponentKind::Terrain: o.terrain.reset(); break;
        case ComponentKind::Path: o.path.reset(); break;
        case ComponentKind::FollowPath: o.followPath.reset(); break;
    }
    return true;
}

const std::vector<ObjectPreset>& presets() {
    using K = ComponentKind;
    static const std::vector<ObjectPreset> list = {
        {"Empty", "group folder parent blank", "An object with only a transform, for grouping.", {}},
        {"Mesh", "model geometry", "A model; pick its file in the inspector.", {K::Mesh}},
        {"Point light", "lamp light bulb", "Light that spreads from one point.", {K::Light}},
        {"Player", "spawn start character", "Where the player starts.", {K::Player}},
        {"Trigger box", "zone volume area", "Calls Lua when the player walks in or out.", {K::Trigger}},
        {"Interactable", "button use prompt", "Calls Lua when the player presses a button nearby.", {K::Interactable}},
        {"Audio clip", "sound sfx wav mp3 ogg music", "A sound Lua can play by name.", {K::Audio}},
        {"Script", "lua code", "An object that only runs a Lua file.", {K::Script}},
        {"Navigation", "navmesh walkable", "Bakes walkable ground without a player.", {K::Navigation}},
        {"Room", "portal culling interior zone", "A box whose geometry draws only when it can be seen.", {K::Room}},
        {"Portal", "door doorway opening room link", "An opening between two Rooms the camera can see through.", {K::Portal}},
        {"Nav walk-off zone", "ledge pit drop edge fall navigation", "Lets the player walk off nav edges inside this box.", {K::Walkoff}},
        {"Agent (NPC)", "npc enemy guard ai patrol waypoint", "A character that walks the nav regions, patrols and senses the player.", {K::Agent}},
        {"Terrain", "landscape ground heightmap hills sculpt paint", "Ground you sculpt and paint in the Scene view.", {K::Terrain}},
        {"Path", "curve route rail track spline moving platform patrol camera dolly", "A curve that objects, agents and the camera ride.", {K::Path}},
    };
    return list;
}

std::string defaultTerrainData(const std::string& objectName) {
    std::string n;
    for (char c : objectName) n += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    if (n.empty()) n = "terrain";
    return "terrain/" + n + ".terrain";
}

std::string uniqueName(const std::string& base, const std::vector<splash::Object>& siblings) {
    auto taken = [&](const std::string& n) {
        return std::any_of(siblings.begin(), siblings.end(), [&](const splash::Object& s) { return s.name == n; });
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + " (" + std::to_string(i) + ")";
        if (!taken(n)) return n;
    }
}

splash::Object makeObject(const ObjectPreset& p, const std::vector<splash::Object>& siblings) {
    splash::Object o;
    o.name = uniqueName(p.label, siblings);
    for (ComponentKind k : p.parts) addComponent(o, k);
    return o;
}

static std::string lower(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// True if `q` starts any space-separated word of `text`.
static bool wordPrefix(const std::string& text, const std::string& q) {
    for (size_t i = 0; i < text.size(); ++i)
        if ((i == 0 || text[i - 1] == ' ') && text.compare(i, q.size(), q) == 0) return true;
    return false;
}

static bool inOrder(const std::string& text, const std::string& q) {
    size_t at = 0;
    for (char c : q) {
        at = text.find(c, at);
        if (at == std::string::npos) return false;
        ++at;
    }
    return true;
}

int matchScore(const std::string& query, const char* label, const char* keywords) {
    std::string q = lower(query.c_str());
    q.erase(0, q.find_first_not_of(' '));
    q.erase(q.find_last_not_of(' ') + 1);
    if (q.empty()) return 1;
    std::string l = lower(label), k = lower(keywords);
    if (l.compare(0, q.size(), q) == 0) return 6;
    if (wordPrefix(l, q)) return 5;
    if (wordPrefix(k, q)) return 4;
    if (l.find(q) != std::string::npos) return 3;
    if (k.find(q) != std::string::npos) return 2;
    if (inOrder(l, q)) return 1;
    return 0;
}

std::vector<int> rank(const std::string& query, const std::vector<std::pair<const char*, const char*>>& items) {
    std::vector<std::pair<int, int>> scored;  // (score, index)
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
        if (int s = matchScore(query, items[i].first, items[i].second)) scored.push_back({s, i});
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (const auto& s : scored) out.push_back(s.second);
    return out;
}

}  // namespace editor
