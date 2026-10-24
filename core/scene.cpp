#include "scene.hh"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <charconv>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace splash {

// Floats parse with strtof and print as the shortest 32-bit round trip, so a
// value goes file -> float without passing through double (no double rounding).
using json = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, std::int64_t,
                                  std::uint64_t, float>;
namespace fs = std::filesystem;

const char* const kAgentStateNames[kAgentStates] = {"Idle", "Patrol", "Seek", "Flee", "Attack", "Wander", "Investigate", "Custom"};

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

json readJson(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) fail("cannot open " + file.string());
    try {
        return json::parse(in);
    } catch (const json::exception& e) {
        fail(file.string() + ": " + e.what());
    }
}

float f(const json& j) { return j.get<float>(); }
Vec3 vec3(const json& j) { return {f(j.at(0)), f(j.at(1)), f(j.at(2))}; }
json vec3(Vec3 v) { return json::array({v.x, v.y, v.z}); }

template <typename E>
E enumFrom(const json& j, std::initializer_list<std::pair<const char*, E>> table) {
    std::string s = j.get<std::string>();
    for (auto& [name, value] : table)
        if (s == name) return value;
    fail("unknown value '" + s + "'");
}
template <typename E>
const char* enumTo(E v, std::initializer_list<std::pair<const char*, E>> table) {
    for (auto& [name, value] : table)
        if (v == value) return name;
    return "?";
}

const std::initializer_list<std::pair<const char*, VertexColorMode>> kColorModes = {
    {"baked", VertexColorMode::Baked},
    {"flat", VertexColorMode::Flat},
    {"mesh", VertexColorMode::Mesh},
    {"painted", VertexColorMode::Painted}};
const std::initializer_list<std::pair<const char*, ColliderKind>> kColliderKinds = {
    {"none", ColliderKind::None}, {"static", ColliderKind::Static}, {"dynamic", ColliderKind::Dynamic}};
const std::initializer_list<std::pair<const char*, LightKind>> kLightKinds = {
    {"directional", LightKind::Directional}, {"point", LightKind::Point}, {"spot", LightKind::Spot}};
const std::initializer_list<std::pair<const char*, DynamicLighting>> kDynamicLighting = {
    {"auto", DynamicLighting::Auto},
    {"on", DynamicLighting::On},
    {"off", DynamicLighting::Off},
    {"smooth", DynamicLighting::Smooth}};
const std::initializer_list<std::pair<const char*, ShadowMode>> kShadowModes = {{"off", ShadowMode::Off},
                                                                                  {"blob", ShadowMode::Blob}};
const std::initializer_list<std::pair<const char*, SceneType>> kSceneTypes = {
    {"exterior", SceneType::Exterior}, {"interior", SceneType::Interior}};
const std::initializer_list<std::pair<const char*, PathMode>> kPathModes = {
    {"once", PathMode::Once}, {"loop", PathMode::Loop}, {"pingPong", PathMode::PingPong}};
const std::initializer_list<std::pair<const char*, NavPartition>> kPartitions = {
    {"watershed", NavPartition::Watershed}, {"monotone", NavPartition::Monotone}, {"layer", NavPartition::Layer}};

void readNav(const json& c, NavBakeSettings& n) {
    n.maxStepHeight = c.value("maxStepHeight", n.maxStepHeight);
    n.walkableSlopeAngle = c.value("walkableSlopeAngle", n.walkableSlopeAngle);
    n.cellSize = c.value("navCellSize", n.cellSize);
    n.cellHeight = c.value("navCellHeight", n.cellHeight);
    n.minRegionArea = c.value("navMinRegionArea", n.minRegionArea);
    n.mergeRegionArea = c.value("navMergeRegionArea", n.mergeRegionArea);
    n.maxSimplifyError = c.value("navMaxSimplifyError", n.maxSimplifyError);
    n.maxEdgeLength = c.value("navMaxEdgeLength", n.maxEdgeLength);
    if (c.contains("navPartitionMethod")) n.partition = enumFrom(c["navPartitionMethod"], kPartitions);
    n.detailSampleDist = c.value("navDetailSampleDist", n.detailSampleDist);
    n.detailMaxError = c.value("navDetailMaxError", n.detailMaxError);
    n.maxPlaneError = c.value("navMaxPlaneError", n.maxPlaneError);
}

void writeNav(json& j, const NavBakeSettings& n) {
    j["maxStepHeight"] = n.maxStepHeight;
    j["walkableSlopeAngle"] = n.walkableSlopeAngle;
    j["navCellSize"] = n.cellSize;
    j["navCellHeight"] = n.cellHeight;
    j["navMinRegionArea"] = n.minRegionArea;
    j["navMergeRegionArea"] = n.mergeRegionArea;
    j["navMaxSimplifyError"] = n.maxSimplifyError;
    j["navMaxEdgeLength"] = n.maxEdgeLength;
    j["navPartitionMethod"] = enumTo(n.partition, kPartitions);
    j["navDetailSampleDist"] = n.detailSampleDist;
    j["navDetailMaxError"] = n.detailMaxError;
    j["navMaxPlaneError"] = n.maxPlaneError;
}

// Keys of `in` that the writer does not produce for the value read from it.
ExtraKeys extrasOf(const json& in, const json& written) {
    ExtraKeys out;
    for (auto& [k, v] : in.items())
        if (!written.contains(k)) out.push_back({k, v.dump()});
    return out;
}

void putExtras(json& j, const ExtraKeys& extra) {
    for (auto& [k, v] : extra)
        if (!j.contains(k)) j[k] = json::parse(v);
}

json writeMaterial(const Material& mat) {
    json mj;
    mj["texture"] = mat.texture.empty() ? json(nullptr) : json(mat.texture);
    mj["color"] = json::array({mat.color[0], mat.color[1], mat.color[2], mat.color[3]});
    putExtras(mj, mat.extra);
    return mj;
}

json writeMesh(const MeshComponent& m) {
    json mats = json::array();
    for (const Material& mat : m.materials) mats.push_back(writeMaterial(mat));
    json j = {{"type", "mesh"},
              {"mesh", m.mesh},
              {"materials", mats},
              {"bitDepth", int(m.bitDepth)},
              {"vertexColors", enumTo(m.vertexColors, kColorModes)},
              {"flatColor", json::array({m.flatColor[0], m.flatColor[1], m.flatColor[2]})},
              {"smoothNormals", m.smoothNormals},
              {"uvOffsetMaterial", m.uvOffsetMaterial},
              {"dynamicLighting", enumTo(m.dynamicLighting, kDynamicLighting)}};
    // Written only when not the default, so scenes without shadows keep their bytes.
    const MeshComponent def;
    if (m.shadow != def.shadow) j["shadow"] = enumTo(m.shadow, kShadowModes);
    if (m.shadowSize != def.shadowSize) j["shadowSize"] = m.shadowSize;
    if (m.shadowDarkness != def.shadowDarkness) j["shadowDarkness"] = m.shadowDarkness;
    // The painted layer, rrggbb hex per vertex (encodeVertexPaint); only once painted.
    if (!m.paint.empty()) j["paintColors"] = encodeVertexPaint(m.paint);
    putExtras(j, m.extra);
    return j;
}

json writeSkin(const SkinComponent& sk) {
    json j = {{"type", "skin"}, {"clips", sk.clips}, {"fps", sk.fps}};
    // Only when set, so scenes from before it keep their bytes.
    if (!sk.clipSources.empty()) j["clipSources"] = sk.clipSources;
    putExtras(j, sk.extra);
    return j;
}

json writeCollider(const ColliderComponent& c) {
    json j = {{"type", "collider"}, {"kind", enumTo(c.kind, kColliderKinds)}, {"platform", c.platform}};
    putExtras(j, c.extra);
    return j;
}

json writeScript(const ScriptComponent& sc) {
    json j = {{"type", "script"}, {"lua", sc.lua}};
    putExtras(j, sc.extra);
    return j;
}

json writeLight(const LightComponent& l) {
    json j = {{"type", "light"},
              {"kind", enumTo(l.kind, kLightKinds)},
              {"color", json::array({l.color[0], l.color[1], l.color[2]})},
              {"intensity", l.intensity},
              {"range", l.range},
              {"spotAngle", l.spotAngle},
              {"innerSpotAngle", l.innerSpotAngle},
              {"enabled", l.enabled},
              {"runtime", l.runtime}};
    putExtras(j, l.extra);
    return j;
}

json writePlayer(const PlayerComponent& pc) {
    json j = {{"type", "player"},
              {"playerHeight", pc.playerHeight},
              {"playerRadius", pc.playerRadius},
              {"moveSpeed", pc.moveSpeed},
              {"sprintSpeed", pc.sprintSpeed}};
    writeNav(j, pc.nav);
    j["jumpHeight"] = pc.jumpHeight;
    j["gravity"] = pc.gravity;
    putExtras(j, pc.extra);
    return j;
}

json writeNavigation(const NavigationComponent& nc) {
    json j = {{"type", "navigation"}, {"agentHeight", nc.agentHeight}, {"agentRadius", nc.agentRadius}};
    writeNav(j, nc.nav);
    j["spawnAnchor"] = nc.spawnAnchor.empty() ? json(nullptr) : json(nc.spawnAnchor);
    putExtras(j, nc.extra);
    return j;
}

json writeTrigger(const TriggerComponent& t) {
    json j = {{"type", "trigger"}, {"size", vec3(t.size)}, {"lua", t.lua.empty() ? json(nullptr) : json(t.lua)}};
    putExtras(j, t.extra);
    return j;
}

json writeRoom(const RoomComponent& r) {
    json j = {{"type", "room"}, {"size", vec3(r.size)}, {"offset", vec3(r.offset)}};
    putExtras(j, r.extra);
    return j;
}

json writeWalkoff(const WalkoffComponent& w) {
    json j = {{"type", "walkoff"}, {"size", vec3(w.size)}};
    putExtras(j, w.extra);
    return j;
}

json writePortal(const PortalComponent& p) {
    json j = {{"type", "portal"}, {"roomA", p.roomA}, {"roomB", p.roomB}, {"width", p.width}, {"height", p.height}};
    putExtras(j, p.extra);
    return j;
}

json writeInteractable(const InteractableComponent& it) {
    json j = {{"type", "interactable"},       {"radius", it.radius},
              {"button", it.button},           {"repeatable", it.repeatable},
              {"cooldownFrames", it.cooldownFrames}, {"showPrompt", it.showPrompt},
              {"promptCanvas", it.promptCanvas}, {"lineOfSight", it.lineOfSight},
              {"facingAngle", it.facingAngle}};
    putExtras(j, it.extra);
    return j;
}

json writeAgent(const AgentComponent& ag) {
    json wps = json::array();
    for (const Vec3& p : ag.waypoints) wps.push_back(vec3(p));
    json clips = json::object();
    for (int i = 0; i < kAgentStates; i++)
        if (ag.stateClips[size_t(i)] >= 0) clips[kAgentStateNames[i]] = ag.stateClips[size_t(i)];
    json j = {{"type", "agent"},
              {"startEnabled", ag.startEnabled},
              {"moveSpeed", ag.moveSpeed},
              {"stopDistance", ag.stopDistance},
              {"vision", ag.vision},
              {"visionRange", ag.visionRange},
              {"fov", ag.fov},
              {"visionRegionDepth", ag.visionRegionDepth},
              {"hearingRange", ag.hearingRange},
              {"alertTimeout", ag.alertTimeout},
              {"waypoints", wps},
              {"stateClips", clips}};
    putExtras(j, ag.extra);
    return j;
}

json writePath(const PathComponent& p) {
    json pts = json::array();
    for (const Vec3& v : p.points) pts.push_back(vec3(v));
    json j = {{"type", "path"},   {"points", pts},     {"closed", p.closed},
              {"speed", p.speed}, {"mode", enumTo(p.mode, kPathModes)}, {"autostart", p.autostart}};
    putExtras(j, p.extra);
    return j;
}

json writeFollowPath(const FollowPathComponent& f) {
    json j = {{"type", "followPath"}, {"path", f.path}, {"speed", f.speed}, {"startOffset", f.startOffset},
              {"faceAlong", f.faceAlong}};
    putExtras(j, f.extra);
    return j;
}

json writeTerrain(const TerrainComponent& t) {
    json j = {{"type", "terrain"}, {"data", t.data}, {"sizeX", t.sizeX}, {"sizeZ", t.sizeZ}, {"tileCells", t.tileCells}};
    putExtras(j, t.extra);
    return j;
}

json writeAudio(const AudioComponent& a) {
    json j = {{"type", "audio"},
              {"clip", a.clip.empty() ? json(nullptr) : json(a.clip)},
              {"clipName", a.clipName},
              {"sampleRate", a.sampleRate},
              {"loop", a.loop},
              {"defaultVolume", a.defaultVolume},
              {"trimLeadingSilence", a.trimLeadingSilence}};
    putExtras(j, a.extra);
    return j;
}

Object readObject(const json& j) {
    Object o;
    o.name = j.at("name").get<std::string>();
    o.active = j.value("active", true);
    o.startActive = j.value("startActive", true);
    if (j.contains("transform")) {
        const json& t = j["transform"];
        if (t.contains("position")) o.transform.position = vec3(t["position"]);
        if (t.contains("rotation")) {
            const json& r = t["rotation"];
            o.transform.rotation = {f(r.at(0)), f(r.at(1)), f(r.at(2)), f(r.at(3))};
        }
        if (t.contains("scale")) o.transform.scale = vec3(t["scale"]);
    }
    for (const json& c : j.value("components", json::array())) {
        std::string type = c.at("type").get<std::string>();
        if (type == "mesh") {
            MeshComponent m;
            m.mesh = c.at("mesh").get<std::string>();
            for (const json& mj : c.value("materials", json::array())) {
                Material mat;
                if (mj.contains("texture") && !mj["texture"].is_null()) mat.texture = mj["texture"].get<std::string>();
                if (mj.contains("color"))
                    for (int i = 0; i < 4; i++) mat.color[i] = f(mj["color"].at(i));
                mat.extra = extrasOf(mj, writeMaterial(mat));
                m.materials.push_back(mat);
            }
            int bd = c.value("bitDepth", 8);
            if (bd != 4 && bd != 8 && bd != 16) fail("bitDepth must be 4, 8 or 16");
            m.bitDepth = BitDepth(bd);
            if (c.contains("vertexColors")) m.vertexColors = enumFrom(c["vertexColors"], kColorModes);
            if (c.contains("flatColor"))
                for (int i = 0; i < 3; i++) m.flatColor[i] = c["flatColor"].at(i).get<uint8_t>();
            m.smoothNormals = c.value("smoothNormals", true);
            m.uvOffsetMaterial = c.value("uvOffsetMaterial", 0);
            if (c.contains("dynamicLighting")) m.dynamicLighting = enumFrom(c["dynamicLighting"], kDynamicLighting);
            if (c.contains("shadow")) m.shadow = enumFrom(c["shadow"], kShadowModes);
            m.shadowSize = c.value("shadowSize", m.shadowSize);
            m.shadowDarkness = c.value("shadowDarkness", m.shadowDarkness);
            if (c.contains("paintColors")) {
                std::optional<std::vector<std::array<uint8_t, 3>>> p = decodeVertexPaint(c["paintColors"].get<std::string>());
                if (!p) fail("paintColors must be rrggbb hex digits per vertex");
                m.paint = std::move(*p);
            }
            {
                // The shadow keys are the mesh's own even when they hold the default.
                json written = writeMesh(m);
                for (const char* k : {"shadow", "shadowSize", "shadowDarkness"}) written[k] = nullptr;
                m.extra = extrasOf(c, written);
            }
            o.mesh = m;
        } else if (type == "collider") {
            ColliderComponent cc;
            if (c.contains("kind")) cc.kind = enumFrom(c["kind"], kColliderKinds);
            cc.platform = c.value("platform", false);
            cc.extra = extrasOf(c, writeCollider(cc));
            o.collider = cc;
        } else if (type == "script") {
            ScriptComponent sc{c.at("lua").get<std::string>(), {}};
            sc.extra = extrasOf(c, writeScript(sc));
            o.script = sc;
        } else if (type == "light") {
            LightComponent l;
            if (c.contains("kind")) l.kind = enumFrom(c["kind"], kLightKinds);
            if (c.contains("color"))
                for (int i = 0; i < 3; i++) l.color[i] = f(c["color"].at(i));
            l.intensity = c.value("intensity", 1.f);
            l.range = c.value("range", 10.f);
            l.spotAngle = c.value("spotAngle", 30.f);
            l.innerSpotAngle = c.value("innerSpotAngle", 0.f);
            l.enabled = c.value("enabled", true);
            l.runtime = c.value("runtime", false);
            if (l.runtime && l.kind != LightKind::Point) fail("only point lights can be runtime lights");
            l.extra = extrasOf(c, writeLight(l));
            o.light = l;
        } else if (type == "player") {
            PlayerComponent pc;
            pc.playerHeight = c.value("playerHeight", pc.playerHeight);
            pc.playerRadius = c.value("playerRadius", pc.playerRadius);
            pc.moveSpeed = c.value("moveSpeed", pc.moveSpeed);
            pc.sprintSpeed = c.value("sprintSpeed", pc.sprintSpeed);
            readNav(c, pc.nav);
            pc.jumpHeight = c.value("jumpHeight", pc.jumpHeight);
            pc.gravity = c.value("gravity", pc.gravity);
            pc.extra = extrasOf(c, writePlayer(pc));
            o.player = pc;
        } else if (type == "navigation") {
            NavigationComponent nc;
            nc.agentHeight = c.value("agentHeight", nc.agentHeight);
            nc.agentRadius = c.value("agentRadius", nc.agentRadius);
            readNav(c, nc.nav);
            if (c.contains("spawnAnchor") && !c["spawnAnchor"].is_null())
                nc.spawnAnchor = c["spawnAnchor"].get<std::string>();
            nc.extra = extrasOf(c, writeNavigation(nc));
            o.navigation = nc;
        } else if (type == "trigger") {
            TriggerComponent t;
            if (c.contains("size")) t.size = vec3(c["size"]);
            if (c.contains("lua") && !c["lua"].is_null()) t.lua = c["lua"].get<std::string>();
            t.extra = extrasOf(c, writeTrigger(t));
            o.trigger = t;
        } else if (type == "room") {
            RoomComponent r;
            if (c.contains("size")) r.size = vec3(c["size"]);
            if (c.contains("offset")) r.offset = vec3(c["offset"]);
            r.extra = extrasOf(c, writeRoom(r));
            o.room = r;
        } else if (type == "walkoff") {
            WalkoffComponent w;
            if (c.contains("size")) w.size = vec3(c["size"]);
            w.extra = extrasOf(c, writeWalkoff(w));
            o.walkoff = w;
        } else if (type == "portal") {
            PortalComponent pc;
            pc.roomA = c.value("roomA", pc.roomA);
            pc.roomB = c.value("roomB", pc.roomB);
            pc.width = c.value("width", pc.width);
            pc.height = c.value("height", pc.height);
            pc.extra = extrasOf(c, writePortal(pc));
            o.portal = pc;
        } else if (type == "interactable") {
            InteractableComponent it;
            it.radius = c.value("radius", it.radius);
            it.button = c.value("button", it.button);
            if (it.button < 0 || it.button > 15) fail("object '" + o.name + "': interactable button must be 0..15");
            it.repeatable = c.value("repeatable", it.repeatable);
            it.cooldownFrames = c.value("cooldownFrames", it.cooldownFrames);
            it.showPrompt = c.value("showPrompt", it.showPrompt);
            it.promptCanvas = c.value("promptCanvas", it.promptCanvas);
            it.lineOfSight = c.value("lineOfSight", it.lineOfSight);
            it.facingAngle = c.value("facingAngle", it.facingAngle);
            if (!(it.facingAngle >= 0 && it.facingAngle <= 180))
                fail("object '" + o.name + "': interactable facingAngle must be 0..180");
            it.extra = extrasOf(c, writeInteractable(it));
            o.interactable = it;
        } else if (type == "audio") {
            AudioComponent a;
            if (c.contains("clip") && !c["clip"].is_null()) a.clip = c["clip"].get<std::string>();
            a.clipName = c.value("clipName", a.clipName);
            a.sampleRate = c.value("sampleRate", a.sampleRate);
            if (a.sampleRate < 1 || a.sampleRate > 65535) fail("object '" + o.name + "': audio sampleRate must be 1..65535");
            a.loop = c.value("loop", a.loop);
            a.defaultVolume = c.value("defaultVolume", a.defaultVolume);
            a.trimLeadingSilence = c.value("trimLeadingSilence", a.trimLeadingSilence);
            a.extra = extrasOf(c, writeAudio(a));
            o.audio = a;
        } else if (type == "agent") {
            AgentComponent ag;
            const std::string where = "object '" + o.name + "': agent ";
            ag.startEnabled = c.value("startEnabled", ag.startEnabled);
            ag.moveSpeed = c.value("moveSpeed", ag.moveSpeed);
            ag.stopDistance = c.value("stopDistance", ag.stopDistance);
            ag.vision = c.value("vision", ag.vision);
            ag.visionRange = c.value("visionRange", ag.visionRange);
            ag.fov = c.value("fov", ag.fov);
            ag.visionRegionDepth = c.value("visionRegionDepth", ag.visionRegionDepth);
            ag.hearingRange = c.value("hearingRange", ag.hearingRange);
            ag.alertTimeout = c.value("alertTimeout", ag.alertTimeout);
            if (!(ag.moveSpeed >= 0) || !(ag.stopDistance >= 0) || !(ag.visionRange >= 0) || !(ag.hearingRange >= 0))
                fail(where + "speeds and ranges must be 0 or more");
            if (ag.visionRegionDepth < 0 || ag.visionRegionDepth > 8) fail(where + "visionRegionDepth must be 0..8");
            if (ag.alertTimeout < 0 || ag.alertTimeout > 65535) fail(where + "alertTimeout must be 0..65535");
            for (const json& wj : c.value("waypoints", json::array())) ag.waypoints.push_back(vec3(wj));
            if (c.contains("stateClips")) {
                const json& sc = c["stateClips"];
                if (!sc.is_object()) fail(where + "stateClips must be an object");
                for (auto it = sc.begin(); it != sc.end(); ++it) {
                    int k = -1;
                    for (int i = 0; i < kAgentStates; i++)
                        if (it.key() == kAgentStateNames[i]) k = i;
                    if (k < 0) fail(where + "unknown state '" + it.key() + "' in stateClips");
                    const int v = it.value().get<int>();
                    if (v < -1 || v > 15) fail(where + "a state clip must be -1..15");
                    ag.stateClips[size_t(k)] = v;
                }
            }
            ag.extra = extrasOf(c, writeAgent(ag));
            o.agent = ag;
        } else if (type == "terrain") {
            TerrainComponent t;
            t.data = c.value("data", t.data);
            t.sizeX = c.value("sizeX", t.sizeX);
            t.sizeZ = c.value("sizeZ", t.sizeZ);
            t.tileCells = c.value("tileCells", t.tileCells);
            if (!(t.sizeX > 0) || !(t.sizeZ > 0)) fail("object '" + o.name + "': terrain size must be above 0");
            if (t.tileCells < 1 || t.tileCells > 256) fail("object '" + o.name + "': terrain tileCells must be 1..256");
            t.extra = extrasOf(c, writeTerrain(t));
            o.terrain = t;
        } else if (type == "path") {
            PathComponent pc;
            for (const json& pj : c.value("points", json::array())) pc.points.push_back(vec3(pj));
            pc.closed = c.value("closed", pc.closed);
            pc.speed = c.value("speed", pc.speed);
            if (c.contains("mode")) pc.mode = enumFrom(c["mode"], kPathModes);
            pc.autostart = c.value("autostart", pc.autostart);
            if (!(pc.speed >= 0)) fail("object '" + o.name + "': path speed must be 0 or more");
            pc.extra = extrasOf(c, writePath(pc));
            o.path = pc;
        } else if (type == "followPath") {
            FollowPathComponent fp;
            fp.path = c.value("path", fp.path);
            fp.speed = c.value("speed", fp.speed);
            fp.startOffset = c.value("startOffset", fp.startOffset);
            fp.faceAlong = c.value("faceAlong", fp.faceAlong);
            if (!(fp.speed >= 0)) fail("object '" + o.name + "': follow path speed must be 0 or more");
            if (!(fp.startOffset >= 0 && fp.startOffset <= 1))
                fail("object '" + o.name + "': follow path startOffset must be 0..1");
            fp.extra = extrasOf(c, writeFollowPath(fp));
            o.followPath = fp;
        } else if (type == "skin") {
            SkinComponent sk;
            for (const json& cj : c.value("clips", json::array())) sk.clips.push_back(cj.get<std::string>());
            for (const json& cj : c.value("clipSources", json::array())) sk.clipSources.push_back(cj.get<std::string>());
            sk.fps = c.value("fps", sk.fps);
            if (sk.fps < 1 || sk.fps > 30) fail("object '" + o.name + "': skin fps must be 1..30");
            sk.extra = extrasOf(c, writeSkin(sk));
            o.skin = sk;
        } else {
            o.unknownComponents.push_back(c.dump());
        }
    }
    for (const json& cj : j.value("children", json::array())) o.children.push_back(readObject(cj));
    if (j.contains("prefab") && !j["prefab"].is_null()) o.prefab = j["prefab"].get<std::string>();
    o.extra = extrasOf(j, json{{"name", 0}, {"active", 0}, {"startActive", 0}, {"transform", 0}, {"prefab", 0}, {"components", 0},
                               {"children", 0}});
    return o;
}

json writeObject(const Object& o) {
    json j;
    j["name"] = o.name;
    j["active"] = o.active;
    if (!o.startActive) j["startActive"] = false;
    j["transform"] = {{"position", vec3(o.transform.position)},
                      {"rotation", json::array({o.transform.rotation.x, o.transform.rotation.y,
                                                o.transform.rotation.z, o.transform.rotation.w})},
                      {"scale", vec3(o.transform.scale)}};
    // An instance is its link: the content comes from the prefab file.
    if (!o.prefab.empty()) {
        j["prefab"] = o.prefab;
        putExtras(j, o.extra);
        return j;
    }
    json comps = json::array();
    if (o.mesh) comps.push_back(writeMesh(*o.mesh));
    if (o.collider) comps.push_back(writeCollider(*o.collider));
    if (o.script) comps.push_back(writeScript(*o.script));
    if (o.light) comps.push_back(writeLight(*o.light));
    if (o.player) comps.push_back(writePlayer(*o.player));
    if (o.navigation) comps.push_back(writeNavigation(*o.navigation));
    if (o.trigger) comps.push_back(writeTrigger(*o.trigger));
    if (o.room) comps.push_back(writeRoom(*o.room));
    if (o.walkoff) comps.push_back(writeWalkoff(*o.walkoff));
    if (o.portal) comps.push_back(writePortal(*o.portal));
    if (o.interactable) comps.push_back(writeInteractable(*o.interactable));
    if (o.audio) comps.push_back(writeAudio(*o.audio));
    if (o.skin) comps.push_back(writeSkin(*o.skin));
    if (o.agent) comps.push_back(writeAgent(*o.agent));
    if (o.terrain) comps.push_back(writeTerrain(*o.terrain));
    if (o.path) comps.push_back(writePath(*o.path));
    if (o.followPath) comps.push_back(writeFollowPath(*o.followPath));
    for (const std::string& c : o.unknownComponents) comps.push_back(json::parse(c));
    j["components"] = comps;
    json kids = json::array();
    for (const Object& c : o.children) kids.push_back(writeObject(c));
    j["children"] = kids;
    putExtras(j, o.extra);
    return j;
}

void writeJson(const json& j, const fs::path& file) {
    std::ofstream out(file, std::ios::binary);
    if (!out) fail("cannot write " + file.string());
    out << j.dump(2) << '\n';
}

Quat mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

void flattenInto(const Object& o, const FlatObject* parent, std::vector<FlatObject>& out) {
    FlatObject fo;
    fo.object = &o;
    const Transform& t = o.transform;
    if (!parent) {
        fo.worldPosition = t.position;
        fo.worldRotation = t.rotation;
        fo.lossyScale = t.scale;
        fo.activeInHierarchy = o.active;
    } else {
        // Nested transforms are not yet checked against Unity's composition.
        fo.worldPosition = parent->localToWorld.point(t.position);
        fo.worldRotation = mul(parent->worldRotation, t.rotation);
        fo.lossyScale = scale(parent->lossyScale, t.scale);
        fo.activeInHierarchy = parent->activeInHierarchy && o.active;
    }
    fo.localToWorld = Mat34::trs(fo.worldPosition, fo.worldRotation, fo.lossyScale);
    out.push_back(fo);
    FlatObject self = out.back();
    for (const Object& c : o.children) flattenInto(c, &self, out);
}

// settings.memCard: each key only when it differs from MemCardSettings'
// default, and no object at all for an untouched default.
json writeMemCard(const MemCardSettings& mc) {
    const MemCardSettings d;
    json j = json::object();
    if (mc.enabled != d.enabled) j["enabled"] = mc.enabled;
    if (mc.region != d.region) j["region"] = mc.region;
    if (mc.product != d.product) j["product"] = mc.product;
    if (mc.title != d.title) j["title"] = mc.title;
    if (!mc.icons.empty()) j["icons"] = mc.icons;
    putExtras(j, mc.extra);
    return j;
}

MemCardSettings readMemCard(const json& j) {
    MemCardSettings mc;
    mc.enabled = j.value("enabled", mc.enabled);
    mc.region = j.value("region", mc.region);
    mc.product = j.value("product", mc.product);
    mc.title = j.value("title", mc.title);
    if (j.contains("icons"))
        for (const json& p : j["icons"]) mc.icons.push_back(p.get<std::string>());
    // Against every known key: one holding its default is not written back.
    mc.extra = extrasOf(j, json{{"enabled", 0}, {"region", 0}, {"product", 0}, {"title", 0}, {"icons", 0}});
    return mc;
}

json writeFog(const FogSettings& fog) {
    json j = {{"enabled", fog.enabled},
              {"color", json::array({fog.color[0], fog.color[1], fog.color[2]})},
              {"density", fog.density}};
    putExtras(j, fog.extra);
    return j;
}

json writeSettings(const SceneSettings& st) {
    json j = {{"gteScaling", st.gteScaling},
              {"sceneType", enumTo(st.sceneType, kSceneTypes)},
              {"fog", writeFog(st.fog)},
              {"networkId", st.networkId},
              {"script", st.script.empty() ? json(nullptr) : json(st.script)}};
    if (st.orderingTableSize) j["orderingTableSize"] = st.orderingTableSize;
    if (st.bumpAllocatorSize) j["bumpAllocatorSize"] = st.bumpAllocatorSize;
    if (st.streamWorld) j["streamWorld"] = true;
    if (!st.edgeFade) j["edgeFade"] = false;
    if (!st.loadingCanvas.empty()) j["loadingCanvas"] = st.loadingCanvas;
    if (json mc = writeMemCard(st.memCard); !mc.empty()) j["memCard"] = mc;
    if (!st.stringPrefixes.empty()) j["stringPrefixes"] = st.stringPrefixes;
    putExtras(j, st.extra);
    return j;
}

const std::initializer_list<std::pair<const char*, UIElementType>> kUITypes = {
    {"image", UIElementType::Image}, {"box", UIElementType::Box},  {"text", UIElementType::Text},
    {"progress", UIElementType::Progress}, {"line", UIElementType::Line}};

json rgb(const std::array<float, 3>& c) { return json::array({c[0], c[1], c[2]}); }
void readRgb(const json& j, const char* key, std::array<float, 3>& c) {
    if (j.contains(key))
        for (int i = 0; i < 3; i++) c[size_t(i)] = f(j[key].at(size_t(i)));
}
template <size_t N>
void readInts(const json& j, const char* key, std::array<int, N>& v) {
    if (j.contains(key))
        for (size_t i = 0; i < N; i++) v[i] = j[key].at(i).get<int>();
}

json writeFont(const UIFont& fo) {
    json j = {{"name", fo.name}};
    if (!fo.bitmap.empty()) {
        j["bitmap"] = fo.bitmap;
        j["glyphWidth"] = fo.glyphWidth;
        j["glyphHeight"] = fo.glyphHeight;
        if (!fo.advances.empty()) j["advances"] = fo.advances;
    } else {
        j["source"] = fo.source;
        j["size"] = fo.size;
    }
    putExtras(j, fo.extra);
    return j;
}

UIFont readFont(const json& j) {
    UIFont fo;
    fo.name = j.value("name", "");
    fo.source = j.value("source", "");
    fo.size = j.value("size", fo.size);
    fo.bitmap = j.value("bitmap", "");
    fo.glyphWidth = j.value("glyphWidth", fo.glyphWidth);
    fo.glyphHeight = j.value("glyphHeight", fo.glyphHeight);
    if (j.contains("advances")) fo.advances = j["advances"].get<std::vector<int>>();
    if (fo.source.empty() == fo.bitmap.empty()) fail("font '" + fo.name + "': set exactly one of source and bitmap");
    if (!fo.source.empty() && (fo.size < 4 || fo.size > 64)) fail("font '" + fo.name + "': size must be 4..64");
    if (!fo.bitmap.empty() && (fo.glyphWidth < 1 || fo.glyphWidth > 64 || fo.glyphHeight < 1 || fo.glyphHeight > 64))
        fail("font '" + fo.name + "': glyphWidth and glyphHeight must be 1..64");
    if (!fo.advances.empty() && fo.advances.size() != 96) fail("font '" + fo.name + "': advances needs 96 entries");
    fo.extra = extrasOf(j, writeFont(fo));
    return fo;
}

json writeSpriteAnim(const SpriteAnim& a) {
    json j = {{"name", a.name},
              {"firstFrame", a.firstFrame},
              {"frameCount", a.frameCount},
              {"frameDuration", a.frameDuration},
              {"loop", a.loop}};
    putExtras(j, a.extra);
    return j;
}

SpriteAnim readSpriteAnim(const json& j) {
    SpriteAnim a;
    a.name = j.value("name", a.name);
    a.firstFrame = j.value("firstFrame", a.firstFrame);
    a.frameCount = j.value("frameCount", a.frameCount);
    a.frameDuration = j.value("frameDuration", a.frameDuration);
    a.loop = j.value("loop", a.loop);
    a.extra = extrasOf(j, writeSpriteAnim(a));
    return a;
}

// Sheets read as written: out-of-range cells and frames load and are
// reported by the export, so the editor can show and fix them.
json writeSpriteSheet(const SpriteSheet& s) {
    json anims = json::array();
    for (const SpriteAnim& a : s.anims) anims.push_back(writeSpriteAnim(a));
    json j = {{"name", s.name},
              {"texture", s.texture},
              {"bitDepth", int(s.bitDepth)},
              {"cellWidth", s.cellW},
              {"cellHeight", s.cellH},
              {"anims", anims}};
    putExtras(j, s.extra);
    return j;
}

SpriteSheet readSpriteSheet(const json& j) {
    SpriteSheet s;
    s.name = j.value("name", "");
    s.texture = j.value("texture", "");
    int bd = j.value("bitDepth", int(s.bitDepth));
    if (bd != 4 && bd != 8 && bd != 16) fail("sprite sheet '" + s.name + "': bitDepth must be 4, 8 or 16");
    s.bitDepth = BitDepth(bd);
    s.cellW = j.value("cellWidth", s.cellW);
    s.cellH = j.value("cellHeight", s.cellH);
    for (const json& aj : j.value("anims", json::array())) s.anims.push_back(readSpriteAnim(aj));
    s.extra = extrasOf(j, writeSpriteSheet(s));
    return s;
}

json writeTileBrush(const TileBrush& b) {
    json j = {{"name", b.name}, {"cell", b.cell}, {"walkable", b.walkable}, {"kind", b.kind}, {"id", b.id}, {"color", rgb(b.color)}};
    putExtras(j, b.extra);
    return j;
}

TileBrush readTileBrush(const json& j) {
    TileBrush b;
    b.name = j.value("name", b.name);
    b.cell = j.value("cell", b.cell);
    b.walkable = j.value("walkable", b.walkable);
    b.kind = j.value("kind", b.kind);
    b.id = j.value("id", b.id);
    readRgb(j, "color", b.color);
    b.extra = extrasOf(j, writeTileBrush(b));
    return b;
}

// The cells as one string a row, top row first: the brush indices separated
// by spaces and right-aligned to the widest, so a map reads (and diffs) as a
// grid, " 0  0  1 -1". Read back split on any whitespace; the earlier array of
// number arrays still reads. Brush indices read as written (one past the
// brushes is reported by the export); the grid is made width x height, cells
// past the end of a row or the rows read empty.
json writeTilemap(const Tilemap& t) {
    json brushes = json::array(), kinds = json::array(), rows = json::array();
    for (const TileBrush& b : t.brushes) brushes.push_back(writeTileBrush(b));
    for (const std::string& k : t.kindNames) kinds.push_back(k);
    size_t w = 1;
    for (int c : t.cells) w = std::max(w, std::to_string(c).size());
    for (int y = 0; y < t.height; y++) {
        std::string row;
        for (int x = 0; x < t.width; x++) {
            const std::string v = std::to_string(t.at(x, y));
            row += (x ? " " : "") + std::string(w - v.size(), ' ') + v;
        }
        rows.push_back(row);
    }
    json j = {{"tileset", t.tileset}, {"width", t.width},     {"height", t.height},
              {"brushes", brushes},   {"kindNames", kinds}, {"cells", rows}};
    putExtras(j, t.extra);
    return j;
}

Tilemap readTilemap(const json& j) {
    Tilemap t;
    t.tileset = j.value("tileset", "");
    t.width = std::clamp(j.value("width", t.width), 1, Tilemap::kMaxSide);
    t.height = std::clamp(j.value("height", t.height), 1, Tilemap::kMaxSide);
    for (const json& bj : j.value("brushes", json::array())) t.brushes.push_back(readTileBrush(bj));
    if (j.contains("kindNames")) t.kindNames = j["kindNames"].get<std::vector<std::string>>();
    t.cells.assign(size_t(t.width * t.height), -1);
    const json rows = j.value("cells", json::array());
    for (int y = 0; y < t.height && y < int(rows.size()); y++) {
        const json& row = rows[size_t(y)];
        std::vector<int> v;
        if (row.is_string()) {
            std::istringstream in(row.get<std::string>());
            for (std::string tok; in >> tok;) {
                int n = 0;
                const auto [end, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), n);
                if (ec != std::errc() || end != tok.data() + tok.size())
                    fail("tilemap row " + std::to_string(y + 1) + ": '" + tok + "' is not a brush index");
                v.push_back(n);
            }
        } else {
            for (const json& c : row) v.push_back(c.get<int>());
        }
        for (int x = 0; x < t.width && x < int(v.size()); x++) t.cells[size_t(y * t.width + x)] = v[size_t(x)];
    }
    t.extra = extrasOf(j, writeTilemap(t));
    return t;
}

json writeElement(const UIElement& e) {
    if (!e.unknown.empty()) return json::parse(e.unknown);
    json j = {{"type", enumTo(e.type, kUITypes)}, {"name", e.name}, {"visible", e.visible}};
    if (e.type == UIElementType::Line) {
        j["from"] = json::array({e.from[0], e.from[1]});
        j["to"] = json::array({e.to[0], e.to[1]});
    } else {
        j["rect"] = json::array({e.rect[0], e.rect[1], e.rect[2], e.rect[3]});
        j["anchorMin"] = json::array({e.anchorMin[0], e.anchorMin[1]});
        j["anchorMax"] = json::array({e.anchorMax[0], e.anchorMax[1]});
    }
    j["color"] = rgb(e.color);
    switch (e.type) {
    case UIElementType::Text:
        j["text"] = e.text;
        j["font"] = e.font.empty() ? json(nullptr) : json(e.font);
        if (!e.textKey.empty()) j["textKey"] = e.textKey;
        break;
    case UIElementType::Progress:
        j["background"] = rgb(e.background);
        j["value"] = e.value;
        break;
    case UIElementType::Image:
        j["texture"] = e.texture.empty() ? json(nullptr) : json(e.texture);
        j["bitDepth"] = int(e.bitDepth);
        j["cutout"] = e.cutout;
        if (!e.sheet.empty()) {
            j["sheet"] = e.sheet;
            j["cell"] = e.cell;
        }
        break;
    default:
        break;
    }
    putExtras(j, e.extra);
    return j;
}

UIElement readElement(const json& j, const std::string& canvas) {
    UIElement e;
    std::string type = j.at("type").get<std::string>();
    if (std::none_of(kUITypes.begin(), kUITypes.end(), [&](const auto& t) { return type == t.first; })) {
        e.name = j.value("name", "");
        e.unknown = j.dump();
        return e;
    }
    e.type = enumFrom(j.at("type"), kUITypes);
    e.name = j.value("name", "");
    e.visible = j.value("visible", true);
    readInts(j, "rect", e.rect);
    if (j.contains("anchorMin"))
        for (size_t i = 0; i < 2; i++) e.anchorMin[i] = f(j["anchorMin"].at(i));
    if (j.contains("anchorMax"))
        for (size_t i = 0; i < 2; i++) e.anchorMax[i] = f(j["anchorMax"].at(i));
    for (size_t i = 0; i < 2; i++)
        if (e.anchorMin[i] < 0 || e.anchorMax[i] > 1 || e.anchorMin[i] > e.anchorMax[i])
            fail("canvas '" + canvas + "', element '" + e.name + "': anchors must satisfy 0 <= min <= max <= 1");
    readRgb(j, "color", e.color);
    e.text = j.value("text", "");
    if (j.contains("font") && !j["font"].is_null()) e.font = j["font"].get<std::string>();
    if (j.contains("textKey") && j["textKey"].is_string()) e.textKey = j["textKey"].get<std::string>();
    readInts(j, "from", e.from);
    readInts(j, "to", e.to);
    readRgb(j, "background", e.background);
    e.value = j.value("value", 0);
    if (e.value < 0 || e.value > 100) fail("canvas '" + canvas + "', element '" + e.name + "': value must be 0..100");
    if (j.contains("texture") && !j["texture"].is_null()) e.texture = j["texture"].get<std::string>();
    int bd = j.value("bitDepth", int(e.bitDepth));
    if (bd != 4 && bd != 8 && bd != 16) fail("canvas '" + canvas + "', element '" + e.name + "': bitDepth must be 4, 8 or 16");
    e.bitDepth = BitDepth(bd);
    e.cutout = j.value("cutout", e.cutout);
    if (e.type == UIElementType::Image && j.contains("sheet") && !j["sheet"].is_null()) {
        e.sheet = j["sheet"].get<std::string>();
        e.cell = j.value("cell", 0);
        if (e.cell < 0 || e.cell > 255) fail("canvas '" + canvas + "', element '" + e.name + "': cell must be 0..255");
    }
    e.extra = extrasOf(j, writeElement(e));
    return e;
}

json writeCanvas(const UICanvas& c) {
    json els = json::array();
    for (const UIElement& e : c.elements) els.push_back(writeElement(e));
    json j = {{"name", c.name}, {"visible", c.visible}, {"sortOrder", c.sortOrder},
              {"defaultFont", c.defaultFont.empty() ? json(nullptr) : json(c.defaultFont)}, {"elements", els}};
    putExtras(j, c.extra);
    return j;
}

UICanvas readCanvas(const json& j) {
    UICanvas c;
    c.name = j.value("name", "");
    c.visible = j.value("visible", true);
    c.sortOrder = j.value("sortOrder", 0);
    if (c.sortOrder < 0 || c.sortOrder > 255) fail("canvas '" + c.name + "': sortOrder must be 0..255");
    if (j.contains("defaultFont") && !j["defaultFont"].is_null()) c.defaultFont = j["defaultFont"].get<std::string>();
    for (const json& ej : j.value("elements", json::array())) c.elements.push_back(readElement(ej, c.name));
    c.extra = extrasOf(j, writeCanvas(c));
    return c;
}

const std::initializer_list<std::pair<const char*, TrackType>> kTrackTypes = {
    {"cameraPosition", TrackType::CameraPosition}, {"cameraRotation", TrackType::CameraRotation},
    {"objectPosition", TrackType::ObjectPosition}, {"objectRotation", TrackType::ObjectRotation},
    {"objectActive", TrackType::ObjectActive},     {"uiCanvasVisible", TrackType::UICanvasVisible},
    {"uiElementVisible", TrackType::UIElementVisible}, {"uiProgress", TrackType::UIProgress},
    {"uiPosition", TrackType::UIPosition},         {"uiColor", TrackType::UIColor},
    {"cameraH", TrackType::CameraH},               {"rumbleSmall", TrackType::RumbleSmall},
    {"rumbleLarge", TrackType::RumbleLarge},       {"objectUVOffset", TrackType::ObjectUVOffset},
    {"lightPosition", TrackType::LightPosition},   {"lightColor", TrackType::LightColor},
    {"lightIntensity", TrackType::LightIntensity}, {"lightRadius", TrackType::LightRadius},
    {"lightEnabled", TrackType::LightEnabled},     {"followPath", TrackType::FollowPath}};
const std::initializer_list<std::pair<const char*, Interp>> kInterps = {
    {"linear", Interp::Linear}, {"step", Interp::Step}, {"easeIn", Interp::EaseIn},
    {"easeOut", Interp::EaseOut}, {"easeInOut", Interp::EaseInOut}};

json writeTrack(const CutsceneTrack& t) {
    json keys = json::array();
    for (const Keyframe& k : t.keyframes) {
        json kj = {{"frame", k.frame}, {"value", json::array({k.value[0], k.value[1], k.value[2]})},
                   {"interp", enumTo(k.interp, kInterps)}};
        putExtras(kj, k.extra);
        keys.push_back(kj);
    }
    json j = {{"type", enumTo(t.type, kTrackTypes)}, {"target", t.target}, {"keyframes", keys}};
    if (t.type == TrackType::FollowPath) j["path"] = t.path;
    putExtras(j, t.extra);
    return j;
}

json writeSkinEvents(const std::vector<SkinAnimEvent>& evs) {
    json out = json::array();
    for (const SkinAnimEvent& e : evs) {
        json ej = {{"frame", e.frame}, {"object", e.object}, {"clip", e.clip}, {"loop", e.loop}};
        putExtras(ej, e.extra);
        out.push_back(ej);
    }
    return out;
}

json writeCutscene(const Cutscene& c) {
    json tracks = json::array(), audio = json::array();
    for (const CutsceneTrack& t : c.tracks) tracks.push_back(writeTrack(t));
    for (const CutsceneAudioEvent& a : c.audioEvents) {
        json aj = {{"frame", a.frame}, {"clip", a.clip}, {"volume", a.volume}, {"pan", a.pan}};
        putExtras(aj, a.extra);
        audio.push_back(aj);
    }
    json j = {{"name", c.name}, {"durationFrames", c.durationFrames}, {"tracks", tracks}, {"audioEvents", audio}};
    if (!c.skinEvents.empty()) j["skinEvents"] = writeSkinEvents(c.skinEvents);
    putExtras(j, c.extra);
    return j;
}

json writeAnimation(const Animation& a) {
    json tracks = json::array();
    for (const CutsceneTrack& t : a.tracks) tracks.push_back(writeTrack(t));
    json j = {{"name", a.name}, {"durationFrames", a.durationFrames}, {"tracks", tracks}};
    if (!a.skinEvents.empty()) j["skinEvents"] = writeSkinEvents(a.skinEvents);
    putExtras(j, a.extra);
    return j;
}

template <class Bad>
CutsceneTrack readTrack(const json& tj, const Bad& bad) {
    CutsceneTrack t;
    t.type = enumFrom(tj.at("type"), kTrackTypes);
    t.target = tj.value("target", "");
    if (t.type == TrackType::FollowPath) t.path = tj.value("path", "");
    for (const json& kj : tj.value("keyframes", json::array())) {
        Keyframe k;
        k.frame = kj.value("frame", 0);
        if (k.frame < 0 || k.frame > 8191) bad("keyframe frame must be 0..8191");
        if (kj.contains("value"))
            for (size_t i = 0; i < 3; i++) k.value[i] = f(kj["value"].at(i));
        if (kj.contains("interp")) k.interp = enumFrom(kj["interp"], kInterps);
        k.extra = extrasOf(kj, json{{"frame", 0}, {"value", 0}, {"interp", 0}});
        t.keyframes.push_back(k);
    }
    t.extra = extrasOf(tj, writeTrack(t));
    return t;
}

template <class Bad>
std::vector<SkinAnimEvent> readSkinEvents(const json& j, const Bad& bad) {
    std::vector<SkinAnimEvent> out;
    for (const json& ej : j.value("skinEvents", json::array())) {
        SkinAnimEvent e;
        e.frame = ej.value("frame", 0);
        e.object = ej.value("object", "");
        e.clip = ej.value("clip", "");
        e.loop = ej.value("loop", false);
        if (e.frame < 0 || e.frame > 65535) bad("skin event frame must be 0..65535");
        e.extra = extrasOf(ej, json{{"frame", 0}, {"object", 0}, {"clip", 0}, {"loop", 0}});
        out.push_back(std::move(e));
    }
    return out;
}

Cutscene readCutscene(const json& j) {
    Cutscene c;
    c.name = j.value("name", "");
    auto bad = [&](const std::string& what) { fail("cutscene '" + c.name + "': " + what); };
    c.durationFrames = j.value("durationFrames", c.durationFrames);
    if (c.durationFrames < 1 || c.durationFrames > 65535) bad("durationFrames must be 1..65535");
    for (const json& tj : j.value("tracks", json::array())) c.tracks.push_back(readTrack(tj, bad));
    for (const json& aj : j.value("audioEvents", json::array())) {
        CutsceneAudioEvent a;
        a.frame = aj.value("frame", 0);
        a.clip = aj.value("clip", "");
        a.volume = aj.value("volume", a.volume);
        a.pan = aj.value("pan", a.pan);
        if (a.frame < 0 || a.frame > 65535) bad("audio event frame must be 0..65535");
        if (a.volume < 0 || a.volume > 128) bad("audio event volume must be 0..128");
        if (a.pan < 0 || a.pan > 127) bad("audio event pan must be 0..127");
        a.extra = extrasOf(aj, json{{"frame", 0}, {"clip", 0}, {"volume", 0}, {"pan", 0}});
        c.audioEvents.push_back(std::move(a));
    }
    c.skinEvents = readSkinEvents(j, bad);
    json known = writeCutscene(c);
    known["skinEvents"] = 0;
    c.extra = extrasOf(j, known);
    return c;
}

Animation readAnimation(const json& j) {
    Animation a;
    a.name = j.value("name", "");
    auto bad = [&](const std::string& what) { fail("animation '" + a.name + "': " + what); };
    a.durationFrames = j.value("durationFrames", a.durationFrames);
    if (a.durationFrames < 1 || a.durationFrames > 65535) bad("durationFrames must be 1..65535");
    for (const json& tj : j.value("tracks", json::array())) a.tracks.push_back(readTrack(tj, bad));
    a.skinEvents = readSkinEvents(j, bad);
    json known = writeAnimation(a);
    known["skinEvents"] = 0;
    a.extra = extrasOf(j, known);
    return a;
}

// The version only changes when an older build would read a file wrong.
// Additions it can skip keep version 1, and their keys survive a save.
void checkVersion(const json& j, const fs::path& file) {
    if (!j.contains("version") || !j["version"].is_number_integer())
        fail(file.string() + ": missing or invalid \"version\"");
    auto v = j["version"].get<int64_t>();
    if (v < 1 || v > kFormatVersion)
        fail(file.string() + ": format version " + std::to_string(v) + ", this build reads up to " +
             std::to_string(kFormatVersion));
}

}  // namespace

void applyNavPreset(NavBakeSettings& n, NavPreset p) {
    struct V {
        float cs, ch;
        int minA, mergeA;
        float simp, edge;
        NavPartition part;
        float detDist, detErr, plane;
    };
    static const V kPresets[3] = {
        {0.05f, 0.025f, 8, 20, 1.3f, 12.f, NavPartition::Watershed, 6.f, 0.025f, 0.15f},
        {0.08f, 0.04f, 4, 6, 0.4f, 5.f, NavPartition::Monotone, 4.f, 0.02f, 0.1f},
        {0.05f, 0.02f, 6, 12, 0.8f, 8.f, NavPartition::Layer, 5.f, 0.02f, 0.12f}};
    const V& v = kPresets[int(p)];
    n.cellSize = v.cs;
    n.cellHeight = v.ch;
    n.minRegionArea = v.minA;
    n.mergeRegionArea = v.mergeA;
    n.maxSimplifyError = v.simp;
    n.maxEdgeLength = v.edge;
    n.partition = v.part;
    n.detailSampleDist = v.detDist;
    n.detailMaxError = v.detErr;
    n.maxPlaneError = v.plane;
}

int matchingNavPreset(const NavBakeSettings& n) {
    auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-4f; };
    for (int i = 0; i < 3; i++) {
        NavBakeSettings p = n;
        applyNavPreset(p, NavPreset(i));
        if (near(n.cellSize, p.cellSize) && near(n.cellHeight, p.cellHeight) &&
            n.minRegionArea == p.minRegionArea && n.mergeRegionArea == p.mergeRegionArea &&
            near(n.maxSimplifyError, p.maxSimplifyError) && near(n.maxEdgeLength, p.maxEdgeLength) &&
            n.partition == p.partition && near(n.detailSampleDist, p.detailSampleDist) &&
            near(n.detailMaxError, p.detailMaxError) && near(n.maxPlaneError, p.maxPlaneError))
            return i;
    }
    return -1;
}

void Tilemap::resize(int w, int h) {
    w = std::clamp(w, 1, kMaxSide);
    h = std::clamp(h, 1, kMaxSide);
    std::vector<int> next(size_t(w * h), -1);
    for (int y = 0; y < std::min(h, height); y++)
        for (int x = 0; x < std::min(w, width); x++) next[size_t(y * w + x)] = at(x, y);
    cells = std::move(next);
    width = w;
    height = h;
}

std::string encodeVertexPaint(const std::vector<std::array<uint8_t, 3>>& paint) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(paint.size() * 6);
    for (const std::array<uint8_t, 3>& c : paint)
        for (uint8_t v : c) s += kHex[v >> 4], s += kHex[v & 15];
    return s;
}

std::optional<std::vector<std::array<uint8_t, 3>>> decodeVertexPaint(const std::string& text) {
    if (text.size() % 6) return std::nullopt;
    auto nibble = [](char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::vector<std::array<uint8_t, 3>> out(text.size() / 6);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i / 6][(i / 2) % 3] = uint8_t(hi * 16 + lo);
    }
    return out;
}

int uvTextureSlot(const MeshComponent& mc, size_t mat) {
    if (mat >= mc.materials.size() || mc.materials[mat].texture.empty()) return -1;
    std::vector<const std::string*> seen;
    for (size_t i = 0; i <= mat; i++) {
        const std::string& t = mc.materials[i].texture;
        if (t.empty()) continue;
        auto it = std::find_if(seen.begin(), seen.end(), [&](const std::string* s) { return *s == t; });
        if (i == mat) return int(it - seen.begin());
        if (it == seen.end()) seen.push_back(&t);
    }
    return -1;
}

std::vector<size_t> uvOffsetMaterials(const MeshComponent& mc, int index) {
    std::vector<size_t> out;
    for (size_t i = 0; i < mc.materials.size(); i++)
        if (index >= 0 && uvTextureSlot(mc, i) == index) out.push_back(i);
    return out;
}

std::vector<FlatObject> flatten(const Scene& scene) {
    std::vector<FlatObject> out;
    for (const Object& o : scene.objects) flattenInto(o, nullptr, out);
    return out;
}

Scene loadScene(const fs::path& file) {
    json j = readJson(file);
    if (j.value("format", "") != "splashedit-ng/scene") fail(file.string() + ": not a splashedit-ng scene");
    checkVersion(j, file);
    Scene s;
    if (j.contains("settings")) {
        const json& st = j["settings"];
        s.settings.gteScaling = st.value("gteScaling", 100.f);
        if (st.contains("sceneType")) s.settings.sceneType = enumFrom(st["sceneType"], kSceneTypes);
        if (st.contains("fog")) {
            const json& fg = st["fog"];
            s.settings.fog.enabled = fg.value("enabled", false);
            if (fg.contains("color"))
                for (int i = 0; i < 3; i++) s.settings.fog.color[i] = f(fg["color"].at(i));
            s.settings.fog.density = fg.value("density", 5);
            s.settings.fog.extra = extrasOf(fg, writeFog(s.settings.fog));
        }
        s.settings.networkId = st.value("networkId", "");
        if (st.contains("script") && !st["script"].is_null()) s.settings.script = st["script"].get<std::string>();
        s.settings.orderingTableSize = st.value("orderingTableSize", 0);
        s.settings.bumpAllocatorSize = st.value("bumpAllocatorSize", 0);
        s.settings.streamWorld = st.value("streamWorld", false);
        s.settings.edgeFade = st.value("edgeFade", true);
        s.settings.loadingCanvas = st.value("loadingCanvas", "");
        if (st.contains("memCard")) s.settings.memCard = readMemCard(st["memCard"]);
        if (st.contains("stringPrefixes") && st["stringPrefixes"].is_array())
            for (const json& p : st["stringPrefixes"])
                if (p.is_string()) s.settings.stringPrefixes.push_back(p.get<std::string>());
        s.settings.extra = extrasOf(st, writeSettings(s.settings));
        // A memCard left at its defaults is not written back.
        std::erase_if(s.settings.extra, [](const auto& e) { return e.first == "memCard"; });
        // Nor is an explicit "edgeFade": true (the default).
        std::erase_if(s.settings.extra, [](const auto& e) { return e.first == "edgeFade"; });
    }
    for (const json& oj : j.value("objects", json::array())) s.objects.push_back(readObject(oj));
    for (const json& fj : j.value("fonts", json::array())) s.fonts.push_back(readFont(fj));
    for (const json& sj : j.value("spriteSheets", json::array())) s.spriteSheets.push_back(readSpriteSheet(sj));
    if (j.contains("tilemap") && !j["tilemap"].is_null()) s.tilemap = readTilemap(j["tilemap"]);
    for (const json& cj : j.value("canvases", json::array())) s.canvases.push_back(readCanvas(cj));
    for (const json& cj : j.value("cutscenes", json::array())) s.cutscenes.push_back(readCutscene(cj));
    for (const json& aj : j.value("animations", json::array())) s.animations.push_back(readAnimation(aj));
    s.extra = extrasOf(j, json{{"format", 0}, {"version", 0}, {"settings", 0}, {"objects", 0}, {"fonts", 0},
                               {"spriteSheets", 0}, {"tilemap", 0}, {"canvases", 0}, {"cutscenes", 0}, {"animations", 0}});
    return s;
}

void saveScene(const Scene& s, const fs::path& file) {
    json j;
    j["format"] = "splashedit-ng/scene";
    j["version"] = kFormatVersion;
    j["settings"] = writeSettings(s.settings);
    json objs = json::array();
    for (const Object& o : s.objects) objs.push_back(writeObject(o));
    j["objects"] = objs;
    if (!s.fonts.empty()) {
        json fonts = json::array();
        for (const UIFont& fo : s.fonts) fonts.push_back(writeFont(fo));
        j["fonts"] = fonts;
    }
    if (!s.spriteSheets.empty()) {
        json sheets = json::array();
        for (const SpriteSheet& sh : s.spriteSheets) sheets.push_back(writeSpriteSheet(sh));
        j["spriteSheets"] = sheets;
    }
    if (s.tilemap) j["tilemap"] = writeTilemap(*s.tilemap);
    if (!s.canvases.empty()) {
        json cvs = json::array();
        for (const UICanvas& c : s.canvases) cvs.push_back(writeCanvas(c));
        j["canvases"] = cvs;
    }
    if (!s.cutscenes.empty()) {
        json cs = json::array();
        for (const Cutscene& c : s.cutscenes) cs.push_back(writeCutscene(c));
        j["cutscenes"] = cs;
    }
    if (!s.animations.empty()) {
        json as = json::array();
        for (const Animation& a : s.animations) as.push_back(writeAnimation(a));
        j["animations"] = as;
    }
    putExtras(j, s.extra);
    writeJson(j, file);
}

namespace {
constexpr const char* kObjectsMarker = "splashedit-objects";
constexpr int kObjectsVersion = 1;
}  // namespace

std::string writeObjectsText(const std::vector<Object>& objects) {
    json j;
    j[kObjectsMarker] = kObjectsVersion;
    json objs = json::array();
    for (const Object& o : objects) objs.push_back(writeObject(o));
    j["objects"] = objs;
    return j.dump(2);
}

std::optional<std::vector<Object>> readObjectsText(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    auto marker = j.find(kObjectsMarker);
    if (marker == j.end() || !marker->is_number_integer() || marker->get<std::int64_t>() != kObjectsVersion) return std::nullopt;
    auto objs = j.find("objects");
    if (objs == j.end() || !objs->is_array() || objs->empty()) return std::nullopt;
    std::vector<Object> out;
    try {
        for (const json& oj : *objs) out.push_back(readObject(oj));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return out;
}


// --- prefabs ---

namespace {

constexpr const char* kPrefabFormat = "splashedit-ng/prefab";

Prefab readPrefabJson(const json& j, const std::string& where) {
    if (!j.is_object() || j.value("format", "") != kPrefabFormat) fail(where + ": not a splashedit-ng prefab");
    if (!j.contains("version") || !j["version"].is_number_integer()) fail(where + ": missing or invalid \"version\"");
    const auto v = j["version"].get<int64_t>();
    if (v < 1 || v > kPrefabVersion)
        fail(where + ": prefab version " + std::to_string(v) + ", this build reads up to " + std::to_string(kPrefabVersion));
    if (!j.contains("root") || !j["root"].is_object()) fail(where + ": the prefab has no root object");
    Prefab p;
    p.root = readObject(j["root"]);
    p.extra = extrasOf(j, json{{"format", 0}, {"version", 0}, {"root", 0}});
    return p;
}

json writePrefabJson(const Prefab& p) {
    json j;
    j["format"] = kPrefabFormat;
    j["version"] = kPrefabVersion;
    j["root"] = writeObject(p.root);
    putExtras(j, p.extra);
    return j;
}

// A prefab path as the cycle check and the cache compare it.
std::string prefabKey(const std::string& p) {
    std::string s = p;
    for (char& c : s)
        if (c == '\\') c = '/';
    return fs::path(s).lexically_normal().generic_string();
}

// Where an object sits in the world, composed the way flatten composes it.
struct WorldTRS {
    Vec3 pos;
    Quat rot;
    Vec3 scale{1, 1, 1};
};

WorldTRS childWorld(const WorldTRS* parent, const Transform& t) {
    if (!parent) return {t.position, t.rotation, t.scale};
    WorldTRS w;
    w.pos = Mat34::trs(parent->pos, parent->rot, parent->scale).point(t.position);
    w.rot = mul(parent->rot, t.rotation);
    w.scale = scale(parent->scale, t.scale);
    return w;
}

// Every agent waypoint in the tree, world position p, moved to
// world(inverse(from)(p)): from the prefab root's frame to the instance's.
void moveWaypoints(Object& o, const Transform& from, const WorldTRS& to) {
    if (o.agent) {
        const Mat34 m = Mat34::trs(to.pos, to.rot, to.scale);
        const Quat inv{-from.rotation.x, -from.rotation.y, -from.rotation.z, from.rotation.w};
        auto div = [](float a, float b) { return b != 0.f ? a / b : 0.f; };
        for (Vec3& p : o.agent->waypoints) {
            const Vec3 r = rotate(inv, p - from.position);
            p = m.point({div(r.x, from.scale.x), div(r.y, from.scale.y), div(r.z, from.scale.z)});
        }
    }
    for (Object& c : o.children) moveWaypoints(c, from, to);
}

struct Resolver {
    fs::path root;
    std::map<std::string, std::pair<std::optional<Prefab>, std::string>> cache;  // key -> prefab or why not
    std::vector<std::string> stack;  // prefab keys being filled in, outermost first
    std::vector<std::string> errors;

    const Prefab* load(const std::string& key, std::string* why) {
        auto it = cache.find(key);
        if (it == cache.end()) {
            std::pair<std::optional<Prefab>, std::string> e;
            const fs::path file = root / fs::path(key);
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) {
                e.second = "not found";
            } else {
                try {
                    e.first = loadPrefab(file);
                } catch (const std::exception& ex) {
                    e.second = std::string("could not be read: ") + ex.what();
                }
            }
            it = cache.emplace(key, std::move(e)).first;
        }
        *why = it->second.second;
        return it->second.first ? &*it->second.first : nullptr;
    }

    void clearContent(Object& o) {
        Object empty;
        empty.name = std::move(o.name);
        empty.active = o.active;
        empty.startActive = o.startActive;
        empty.transform = o.transform;
        empty.extra = std::move(o.extra);
        empty.prefab = std::move(o.prefab);
        o = std::move(empty);
    }

    // `names`: the object's path of names, for messages. `parent`: the
    // parent's world transform, null at the top of the tree being filled.
    void object(Object& o, const std::string& names, const WorldTRS* parent) {
        const WorldTRS world = childWorld(parent, o.transform);
        if (o.prefab.empty()) {
            for (Object& c : o.children) object(c, names + "/" + c.name, &world);
            return;
        }
        const std::string key = prefabKey(o.prefab);
        o.prefabResolved = false;
        if (std::find(stack.begin(), stack.end(), key) != stack.end()) {
            std::string chain;
            for (auto it = std::find(stack.begin(), stack.end(), key); it != stack.end(); ++it) chain += *it + " -> ";
            errors.push_back(names + ": prefab " + o.prefab + " includes itself (" + chain + key + ")");
            clearContent(o);
            return;
        }
        std::string why;
        const Prefab* p = load(key, &why);
        if (!p) {
            errors.push_back(names + ": prefab " + o.prefab + " " + why);
            clearContent(o);
            return;
        }
        // The prefab's root, its own nested instances filled in where it
        // sits in its file, then moved to where the instance sits.
        Object content = p->root;
        stack.push_back(key);
        const size_t before = errors.size();
        object(content, names, nullptr);
        stack.pop_back();
        if (errors.size() != before) {
            clearContent(o);
            return;
        }
        const Transform& from = p->root.transform;
        const bool same = from.position == world.pos && from.scale == world.scale && from.rotation.x == world.rot.x &&
                          from.rotation.y == world.rot.y && from.rotation.z == world.rot.z && from.rotation.w == world.rot.w;
        if (!same) moveWaypoints(content, from, world);
        content.name = std::move(o.name);
        content.active = o.active;
        content.startActive = o.startActive;
        content.transform = o.transform;
        content.extra = std::move(o.extra);
        content.prefab = std::move(o.prefab);
        content.prefabResolved = true;
        o = std::move(content);
    }
};

}  // namespace

Prefab loadPrefab(const fs::path& file) { return readPrefabJson(readJson(file), file.string()); }

void savePrefab(const Prefab& prefab, const fs::path& file) { writeJson(writePrefabJson(prefab), file); }

std::string writePrefabText(const Prefab& prefab) { return writePrefabJson(prefab).dump(2) + "\n"; }

Prefab readPrefabText(const std::string& text, const std::string& where) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        fail(where + ": " + e.what());
    }
    return readPrefabJson(j, where);
}

std::vector<std::string> resolvePrefabs(Scene& scene, const fs::path& projectRoot) {
    Resolver r;
    r.root = projectRoot;
    for (Object& o : scene.objects) r.object(o, o.name, nullptr);
    return r.errors;
}

std::vector<std::string> prefabNameWarnings(const Scene& scene) {
    // Every object's name, with the instance whose content it is (if any).
    struct Named {
        const Object* object;
        const Object* instance;  // the outermost instance root it is content of; null outside any
    };
    std::vector<Named> all;
    std::function<void(const Object&, const Object*)> walk = [&](const Object& o, const Object* inst) {
        all.push_back({&o, inst});
        const Object* in = inst ? inst : (o.prefab.empty() ? nullptr : &o);
        for (const Object& c : o.children) walk(c, in);
    };
    for (const Object& o : scene.objects) walk(o, nullptr);
    // What refers to a name, once each.
    std::map<std::string, std::vector<std::string>> refs;
    auto refer = [&](const std::string& name, const std::string& by) {
        if (name.empty()) return;
        auto& v = refs[name];
        if (std::find(v.begin(), v.end(), by) == v.end()) v.push_back(by);
    };
    for (const Named& n : all) {
        const Object& o = *n.object;
        if (o.portal) {
            refer(o.portal->roomA, "Portal '" + o.name + "'");
            refer(o.portal->roomB, "Portal '" + o.name + "'");
        }
        if (o.navigation) refer(o.navigation->spawnAnchor, "the navigation spawnAnchor");
    }
    for (const Cutscene& c : scene.cutscenes)
        for (const CutsceneTrack& t : c.tracks) refer(t.target, "cutscene '" + c.name + "'");
    for (const Animation& a : scene.animations)
        for (const CutsceneTrack& t : a.tracks) refer(t.target, "animation '" + a.name + "'");
    std::vector<std::string> out;
    for (const auto& [name, by] : refs) {
        std::vector<const Named*> holders;
        bool fromPrefab = false;
        for (const Named& n : all)
            if (n.object->name == name) {
                holders.push_back(&n);
                if (n.instance && n.instance != n.object) fromPrefab = true;
            }
        if (holders.size() < 2 || !fromPrefab) continue;
        std::string where, prefab;
        for (const Named* h : holders) {
            where += (where.empty() ? "" : ", ") + (h->instance && h->instance != h->object ? "in " + h->instance->name : std::string("in the scene"));
            if (prefab.empty() && h->instance && h->instance != h->object) prefab = h->instance->prefab;
        }
        std::string users;
        for (size_t i = 0; i < by.size(); ++i) users += (i ? (i + 1 == by.size() ? " and " : ", ") : "") + by[i];
        out.push_back("Prefab " + prefab + ": " + std::to_string(holders.size()) + " objects are named '" + name + "' (" + where +
                      ") and " + users + " refers to it by name, so only one of them is meant. Unpack an instance and rename it.");
    }
    return out;
}

namespace {
constexpr const char* kUIElementsMarker = "splashedit-ui-elements";
constexpr int kUIElementsVersion = 1;
}  // namespace

const std::string& effectiveFont(const UICanvas& canvas, const UIElement& e) {
    return e.font.empty() ? canvas.defaultFont : e.font;
}

std::string writeUIElementsText(const std::vector<UIElement>& elements) {
    json j;
    j[kUIElementsMarker] = kUIElementsVersion;
    json els = json::array();
    for (const UIElement& e : elements) els.push_back(writeElement(e));
    j["elements"] = els;
    return j.dump(2);
}

std::optional<std::vector<UIElement>> readUIElementsText(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    auto marker = j.find(kUIElementsMarker);
    if (marker == j.end() || !marker->is_number_integer() || marker->get<std::int64_t>() != kUIElementsVersion) return std::nullopt;
    auto els = j.find("elements");
    if (els == j.end() || !els->is_array() || els->empty()) return std::nullopt;
    std::vector<UIElement> out;
    try {
        for (const json& ej : *els) {
            if (!ej.is_object()) return std::nullopt;
            out.push_back(readElement(ej, "clipboard"));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return out;
}

Bounds Mesh::bounds() const {
    if (positions.empty()) return {};
    Vec3 mn = positions[0], mx = positions[0];
    for (const Vec3& p : positions) {
        mn = vmin(mn, p);
        mx = vmax(mx, p);
    }
    Bounds b;
    b.center = (mn + mx) * 0.5f;
    b.extents = (mx - mn) * 0.5f;
    return b;
}

Mesh loadMesh(const fs::path& file) {
    json j = readJson(file);
    if (j.value("format", "") != "splashedit-ng/mesh") fail(file.string() + ": not a splashedit-ng mesh");
    checkVersion(j, file);
    Mesh m;
    auto floats = [&](const char* key, size_t stride) {
        std::vector<float> v;
        if (j.contains(key))
            for (const json& x : j[key]) v.push_back(x.get<float>());
        if (v.size() % stride)
            fail(file.string() + ": \"" + key + "\" length is not a multiple of " + std::to_string(stride));
        return v;
    };
    auto p = floats("positions", 3), n = floats("normals", 3), uv = floats("uv", 2), c = floats("colors", 4);
    for (size_t i = 0; i + 2 < p.size(); i += 3) m.positions.push_back({p[i], p[i + 1], p[i + 2]});
    for (size_t i = 0; i + 2 < n.size(); i += 3) m.normals.push_back({n[i], n[i + 1], n[i + 2]});
    for (size_t i = 0; i + 1 < uv.size(); i += 2) m.uv.push_back({uv[i], uv[i + 1]});
    for (size_t i = 0; i + 3 < c.size(); i += 4) m.colors.push_back({c[i], c[i + 1], c[i + 2], c[i + 3]});
    for (const json& sm : j.value("submeshes", json::array())) m.submeshes.push_back(sm.get<std::vector<int>>());
    size_t nv = m.positions.size();
    if (!m.normals.empty() && m.normals.size() != nv) fail(file.string() + ": normals count mismatch");
    if (!m.uv.empty() && m.uv.size() != nv) fail(file.string() + ": uv count mismatch");
    if (!m.colors.empty() && m.colors.size() != nv) fail(file.string() + ": colors count mismatch");
    for (auto& sm : m.submeshes) {
        if (sm.size() % 3) fail(file.string() + ": submesh index count not a multiple of 3");
        for (int i : sm)
            if (i < 0 || size_t(i) >= nv) fail(file.string() + ": index out of range");
    }
    if (j.contains("skin")) {
        const json& sj = j["skin"];
        MeshSkin sk;
        for (const json& jj : sj.value("joints", json::array())) {
            Joint jt;
            jt.name = jj.at("name").get<std::string>();
            jt.parent = jj.value("parent", -1);
            if (jj.contains("position")) jt.position = vec3(jj["position"]);
            if (jj.contains("rotation")) {
                const json& r = jj["rotation"];
                jt.rotation = {f(r.at(0)), f(r.at(1)), f(r.at(2)), f(r.at(3))};
            }
            if (jj.contains("scale")) jt.scale = vec3(jj["scale"]);
            if (jt.parent < -1 || jt.parent >= int(sk.joints.size()))
                fail(file.string() + ": joint '" + jt.name + "' parent must be -1 or an earlier joint");
            sk.joints.push_back(std::move(jt));
        }
        size_t nj = sk.joints.size();
        if (nj == 0) fail(file.string() + ": skin has no joints");
        std::vector<float> ib;
        for (const json& x : sj.value("inverseBind", json::array())) ib.push_back(x.get<float>());
        if (!ib.empty()) {
            if (ib.size() != nj * 12) fail(file.string() + ": inverseBind needs 12 floats per joint");
            for (size_t i = 0; i < nj; i++) {
                std::array<float, 12> a;
                std::copy(ib.begin() + long(i * 12), ib.begin() + long(i * 12 + 12), a.begin());
                sk.inverseBind.push_back(a);
            }
        }
        std::vector<int> vj;
        std::vector<float> vw;
        for (const json& x : sj.value("vertexJoints", json::array())) vj.push_back(x.get<int>());
        for (const json& x : sj.value("vertexWeights", json::array())) vw.push_back(x.get<float>());
        if (vj.size() != nv * 4 || vw.size() != nv * 4)
            fail(file.string() + ": vertexJoints and vertexWeights need 4 values per vertex");
        for (size_t i = 0; i < nv; i++) {
            std::array<int, 4> a;
            std::array<float, 4> b;
            for (int k = 0; k < 4; k++) {
                a[k] = vj[i * 4 + size_t(k)];
                b[k] = vw[i * 4 + size_t(k)];
                if (a[k] < 0 || size_t(a[k]) >= nj) fail(file.string() + ": vertex joint index out of range");
            }
            sk.vertexJoints.push_back(a);
            sk.vertexWeights.push_back(b);
        }
        m.skin = std::move(sk);
    }
    return m;
}

void saveMesh(const Mesh& m, const fs::path& file) {
    json j;
    j["format"] = "splashedit-ng/mesh";
    j["version"] = kFormatVersion;
    json p = json::array(), n = json::array(), uv = json::array(), c = json::array();
    for (auto& v : m.positions) p.insert(p.end(), {v.x, v.y, v.z});
    for (auto& v : m.normals) n.insert(n.end(), {v.x, v.y, v.z});
    for (auto& v : m.uv) uv.insert(uv.end(), {v.x, v.y});
    for (auto& v : m.colors) c.insert(c.end(), {v[0], v[1], v[2], v[3]});
    j["positions"] = p;
    if (!m.normals.empty()) j["normals"] = n;
    if (!m.uv.empty()) j["uv"] = uv;
    if (!m.colors.empty()) j["colors"] = c;
    j["submeshes"] = m.submeshes;
    if (m.skin) {
        const MeshSkin& sk = *m.skin;
        json joints = json::array(), ib = json::array(), vj = json::array(), vw = json::array();
        for (const Joint& jt : sk.joints)
            joints.push_back({{"name", jt.name},
                              {"parent", jt.parent},
                              {"position", vec3(jt.position)},
                              {"rotation", json::array({jt.rotation.x, jt.rotation.y, jt.rotation.z, jt.rotation.w})},
                              {"scale", vec3(jt.scale)}});
        for (const auto& a : sk.inverseBind)
            for (float x : a) ib.push_back(x);
        for (const auto& a : sk.vertexJoints)
            for (int x : a) vj.push_back(x);
        for (const auto& a : sk.vertexWeights)
            for (float x : a) vw.push_back(x);
        json sj = {{"joints", joints}};
        if (!sk.inverseBind.empty()) sj["inverseBind"] = ib;
        sj["vertexJoints"] = vj;
        sj["vertexWeights"] = vw;
        j["skin"] = sj;
    }
    writeJson(j, file);
}

namespace {
const std::initializer_list<std::pair<const char*, AnimProperty>> kAnimProps = {
    {"position", AnimProperty::Position}, {"rotation", AnimProperty::Rotation}, {"scale", AnimProperty::Scale}};
const std::initializer_list<std::pair<const char*, AnimInterp>> kAnimInterps = {{"linear", AnimInterp::Linear},
                                                                                 {"step", AnimInterp::Step}};
}  // namespace

AnimClip loadAnim(const fs::path& file) {
    json j = readJson(file);
    if (j.value("format", "") != "splashedit-ng/anim") fail(file.string() + ": not a splashedit-ng anim");
    checkVersion(j, file);
    AnimClip c;
    c.name = j.at("name").get<std::string>();
    c.length = j.at("length").get<float>();
    if (!(c.length >= 0)) fail(file.string() + ": length must be >= 0");
    c.loop = j.value("loop", false);
    for (const json& cj : j.value("channels", json::array())) {
        AnimChannel ch;
        ch.joint = cj.at("joint").get<std::string>();
        ch.property = enumFrom(cj.at("property"), kAnimProps);
        if (cj.contains("interpolation")) ch.interp = enumFrom(cj["interpolation"], kAnimInterps);
        for (const json& x : cj.at("times")) ch.times.push_back(x.get<float>());
        for (const json& x : cj.at("values")) ch.values.push_back(x.get<float>());
        size_t stride = ch.property == AnimProperty::Rotation ? 4 : 3;
        if (ch.times.empty()) fail(file.string() + ": channel '" + ch.joint + "' has no keys");
        if (ch.values.size() != ch.times.size() * stride)
            fail(file.string() + ": channel '" + ch.joint + "' needs " + std::to_string(stride) + " values per key");
        for (size_t i = 1; i < ch.times.size(); i++)
            if (ch.times[i] < ch.times[i - 1]) fail(file.string() + ": channel '" + ch.joint + "' times not ascending");
        c.channels.push_back(std::move(ch));
    }
    return c;
}

void saveAnim(const AnimClip& c, const fs::path& file) {
    json j;
    j["format"] = "splashedit-ng/anim";
    j["version"] = kFormatVersion;
    j["name"] = c.name;
    j["length"] = c.length;
    j["loop"] = c.loop;
    json chs = json::array();
    for (const AnimChannel& ch : c.channels)
        chs.push_back({{"joint", ch.joint},
                       {"property", enumTo(ch.property, kAnimProps)},
                       {"interpolation", enumTo(ch.interp, kAnimInterps)},
                       {"times", ch.times},
                       {"values", ch.values}});
    j["channels"] = chs;
    writeJson(j, file);
}

bool defaultClipLoop(const std::string& name) {
    static const char* const kLooping[] = {"idle", "walk", "run", "jog", "loop", "cycle", "patrol", "swim", "fly", "breath", "hover"};
    auto check = [](const std::string& word) {
        for (const char* k : kLooping)
            if (word.rfind(k, 0) == 0) return true;
        return false;
    };
    std::string word;
    for (size_t i = 0; i <= name.size(); i++) {
        const unsigned char ch = i < name.size() ? static_cast<unsigned char>(name[i]) : 0;
        const bool letter = std::isalpha(ch) != 0;
        const bool camel = letter && std::isupper(ch) && i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]));
        if (!letter || camel) {
            if (check(word)) return true;
            word.clear();
        }
        if (letter) word += char(std::tolower(ch));
    }
    return false;
}

VramSettings::Area clampVramArea(VramSettings::Area a) {
    a.x = std::clamp(a.x, 0, kVramWidth - 1);
    a.y = std::clamp(a.y, 0, kVramHeight - 1);
    a.w = std::clamp(a.w, 1, kVramWidth - a.x);
    a.h = std::clamp(a.h, 1, kVramHeight - a.y);
    return a;
}

VramSettings readVramSettings(const fs::path& file) {
    VramSettings vs;
    if (file.empty()) return vs;
    std::ifstream in(file, std::ios::binary);
    if (!in) return vs;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("prohibited") || !j["prohibited"].is_array()) return vs;
    for (const json& e : j["prohibited"]) {
        if (!e.is_object()) continue;
        bool ok = true;
        int v[4] = {};
        const char* keys[4] = {"x", "y", "w", "h"};
        for (int i = 0; i < 4 && ok; i++) {
            auto it = e.find(keys[i]);
            if (it == e.end() || !it->is_number()) {
                ok = false;
                break;
            }
            double d = it->get<double>();
            if (!(d > -1e6 && d < 1e6)) ok = false;
            else v[i] = int(d);
        }
        if (ok) vs.prohibited.push_back(clampVramArea({v[0], v[1], v[2], v[3]}));
    }
    return vs;
}

bool writeVramSettings(const fs::path& file, const VramSettings& vs) {
    if (file.empty()) return false;
    json areas = json::array();
    for (const VramSettings::Area& a : vs.prohibited) {
        json e = json::object();
        e["x"] = a.x;
        e["y"] = a.y;
        e["w"] = a.w;
        e["h"] = a.h;
        areas.push_back(e);
    }
    json j = json::object();
    j["prohibited"] = areas;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2) << '\n';
    out.close();
    return !out.fail();
}

}  // namespace splash
