#include "scene.hh"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace splash {

// Floats parse with strtof and print as the shortest 32-bit round trip, so a
// value goes file -> float without passing through double (no double rounding).
using json = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, std::int64_t,
                                  std::uint64_t, float>;
namespace fs = std::filesystem;

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
    {"baked", VertexColorMode::Baked}, {"flat", VertexColorMode::Flat}, {"mesh", VertexColorMode::Mesh}};
const std::initializer_list<std::pair<const char*, ColliderKind>> kColliderKinds = {
    {"none", ColliderKind::None}, {"static", ColliderKind::Static}, {"dynamic", ColliderKind::Dynamic}};
const std::initializer_list<std::pair<const char*, LightKind>> kLightKinds = {
    {"directional", LightKind::Directional}, {"point", LightKind::Point}, {"spot", LightKind::Spot}};
const std::initializer_list<std::pair<const char*, DynamicLighting>> kDynamicLighting = {
    {"auto", DynamicLighting::Auto},
    {"on", DynamicLighting::On},
    {"off", DynamicLighting::Off},
    {"smooth", DynamicLighting::Smooth}};
const std::initializer_list<std::pair<const char*, SceneType>> kSceneTypes = {
    {"exterior", SceneType::Exterior}, {"interior", SceneType::Interior}};
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
    putExtras(j, m.extra);
    return j;
}

json writeSkin(const SkinComponent& sk) {
    json j = {{"type", "skin"}, {"clips", sk.clips}, {"fps", sk.fps}};
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
            m.extra = extrasOf(c, writeMesh(m));
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
        } else if (type == "skin") {
            SkinComponent sk;
            for (const json& cj : c.value("clips", json::array())) sk.clips.push_back(cj.get<std::string>());
            sk.fps = c.value("fps", sk.fps);
            if (sk.fps < 1 || sk.fps > 30) fail("object '" + o.name + "': skin fps must be 1..30");
            sk.extra = extrasOf(c, writeSkin(sk));
            o.skin = sk;
        } else {
            o.unknownComponents.push_back(c.dump());
        }
    }
    for (const json& cj : j.value("children", json::array())) o.children.push_back(readObject(cj));
    o.extra = extrasOf(j, json{{"name", 0}, {"active", 0}, {"startActive", 0}, {"transform", 0}, {"components", 0}, {"children", 0}});
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
    if (!st.loadingCanvas.empty()) j["loadingCanvas"] = st.loadingCanvas;
    if (json mc = writeMemCard(st.memCard); !mc.empty()) j["memCard"] = mc;
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
        break;
    case UIElementType::Progress:
        j["background"] = rgb(e.background);
        j["value"] = e.value;
        break;
    case UIElementType::Image:
        j["texture"] = e.texture.empty() ? json(nullptr) : json(e.texture);
        j["bitDepth"] = int(e.bitDepth);
        j["cutout"] = e.cutout;
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
    {"lightEnabled", TrackType::LightEnabled}};
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
        s.settings.loadingCanvas = st.value("loadingCanvas", "");
        if (st.contains("memCard")) s.settings.memCard = readMemCard(st["memCard"]);
        s.settings.extra = extrasOf(st, writeSettings(s.settings));
        // A memCard left at its defaults is not written back.
        std::erase_if(s.settings.extra, [](const auto& e) { return e.first == "memCard"; });
    }
    for (const json& oj : j.value("objects", json::array())) s.objects.push_back(readObject(oj));
    for (const json& fj : j.value("fonts", json::array())) s.fonts.push_back(readFont(fj));
    for (const json& cj : j.value("canvases", json::array())) s.canvases.push_back(readCanvas(cj));
    for (const json& cj : j.value("cutscenes", json::array())) s.cutscenes.push_back(readCutscene(cj));
    for (const json& aj : j.value("animations", json::array())) s.animations.push_back(readAnimation(aj));
    s.extra = extrasOf(j, json{{"format", 0}, {"version", 0}, {"settings", 0}, {"objects", 0}, {"fonts", 0},
                               {"canvases", 0}, {"cutscenes", 0}, {"animations", 0}});
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
