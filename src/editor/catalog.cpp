#include "editor/catalog.hh"

#include <algorithm>
#include <cctype>

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
        {ComponentKind::Audio, "Audio clip", "sound sfx wav music", "A sound Lua can play by name."},
        {ComponentKind::Skin, "Skinned animation", "skeleton bones clips anim", "Plays animation clips on a skinned mesh."},
        {ComponentKind::Room, "Room", "portal culling interior zone visibility", "A box whose geometry draws only when it can be seen."},
        {ComponentKind::Portal, "Portal", "door doorway opening room link culling", "An opening between two Rooms the camera can see through."},
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
    }
    return false;
}

std::string cannotAdd(const splash::Object& o, ComponentKind k) {
    if (hasComponent(o, k)) return "Already added";
    // The exporter only reads colliders and skins off mesh objects.
    if ((k == ComponentKind::Collider || k == ComponentKind::Skin) && !o.mesh) return "Needs a Mesh";
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
        {"Audio clip", "sound sfx wav music", "A sound Lua can play by name.", {K::Audio}},
        {"Script", "lua code", "An object that only runs a Lua file.", {K::Script}},
        {"Navigation", "navmesh walkable", "Bakes walkable ground without a player.", {K::Navigation}},
        {"Room", "portal culling interior zone", "A box whose geometry draws only when it can be seen.", {K::Room}},
        {"Portal", "door doorway opening room link", "An opening between two Rooms the camera can see through.", {K::Portal}},
    };
    return list;
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
