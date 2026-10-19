// Scene IR: what the editor edits and the exporter consumes. See
// docs/scene-format.md for the file format.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "unitymath.hh"

namespace splash {

enum class BitDepth : uint8_t { Bpp4 = 4, Bpp8 = 8, Bpp16 = 16 };
enum class VertexColorMode : uint8_t { Baked, Flat, Mesh };
enum class ColliderKind : uint8_t { None, Static, Dynamic };
enum class LightKind : uint8_t { Directional, Point, Spot };
enum class DynamicLighting : uint8_t { Auto, On, Off, Smooth };
enum class SceneType : uint8_t { Exterior = 0, Interior = 1 };
enum class NavPartition : uint8_t { Watershed = 0, Monotone = 1, Layer = 2 };

// Keys in a file that this build does not know: (key, value as JSON text) in
// file order. Saving writes them back after the known keys.
using ExtraKeys = std::vector<std::pair<std::string, std::string>>;

// Scene and mesh file format version this build reads and writes.
constexpr int kFormatVersion = 1;

struct Material {
    std::string texture;  // project-relative path, empty = untextured
    std::array<float, 4> color{1, 1, 1, 1};
    ExtraKeys extra;
};

// Skinned mesh: a named clip set played by SkinnedAnim.Play. Requires a mesh
// component whose .mesh carries a skeleton.
struct SkinComponent {
    std::vector<std::string> clips;  // project-relative .anim paths, at most 16
    int fps = 15;                    // bake rate, 1..30
    ExtraKeys extra;
};

struct MeshComponent {
    std::string mesh;  // project-relative path to a .mesh
    std::vector<Material> materials;
    BitDepth bitDepth = BitDepth::Bpp8;
    VertexColorMode vertexColors = VertexColorMode::Baked;
    std::array<uint8_t, 3> flatColor{128, 128, 128};
    bool smoothNormals = true;
    int uvOffsetMaterial = 0;
    // Auto: lit by runtime lights whose range reaches the mesh bounds.
    DynamicLighting dynamicLighting = DynamicLighting::Auto;
    ExtraKeys extra;
};

struct ColliderComponent {
    ColliderKind kind = ColliderKind::None;
    bool platform = false;
    ExtraKeys extra;
};

struct ScriptComponent {
    std::string lua;
    ExtraKeys extra;
};

struct LightComponent {
    LightKind kind = LightKind::Point;
    std::array<float, 3> color{1, 1, 1};
    float intensity = 1;
    float range = 10;
    float spotAngle = 30;
    float innerSpotAngle = 0;
    bool enabled = true;
    // Point lights only: computed on the console (and switchable from Lua)
    // instead of baked into vertex colours.
    bool runtime = false;
    ExtraKeys extra;
};

// Nav mesh bake parameters, shared by PSXPlayer and PSXNavigationSettings
// (SplashEdit 2.4 defaults).
struct NavBakeSettings {
    float maxStepHeight = 0.35f;
    float walkableSlopeAngle = 46.0f;
    float cellSize = 0.05f;
    float cellHeight = 0.025f;
    int minRegionArea = 8;
    int mergeRegionArea = 20;
    float maxSimplifyError = 1.3f;
    float maxEdgeLength = 12.0f;
    NavPartition partition = NavPartition::Watershed;
    float detailSampleDist = 6.0f;
    float detailMaxError = 0.025f;
    float maxPlaneError = 0.15f;
};

// SplashEdit's Indoor / Terrain / Multi-Level bake presets. They set the ten
// region-building fields (cell size through max plane error) and never touch
// step height or slope.
enum class NavPreset : uint8_t { Indoor = 0, Terrain = 1, MultiLevel = 2 };
void applyNavPreset(NavBakeSettings& n, NavPreset p);
// Index of the preset whose ten fields all match `n` (within 1e-4), else -1.
int matchingNavPreset(const NavBakeSettings& n);

// PSXPlayer
struct PlayerComponent {
    float playerHeight = 1.8f;
    float playerRadius = 0.5f;
    float moveSpeed = 3.0f;
    float sprintSpeed = 8.0f;
    NavBakeSettings nav;
    float jumpHeight = 2.0f;
    float gravity = 20.0f;
    ExtraKeys extra;
};

// PSXNavigationSettings: nav bake without a player.
struct NavigationComponent {
    float agentHeight = 1.8f;
    float agentRadius = 0.5f;
    NavBakeSettings nav;
    std::string spawnAnchor;  // object name; empty = this object
    ExtraKeys extra;
};

// PSXTriggerBox: an axis-aligned box (in world space, after the object's
// transform) whose Lua script gets enter/exit callbacks.
struct TriggerComponent {
    Vec3 size{1, 1, 1};
    std::string lua;  // empty = none
    ExtraKeys extra;
};

// PSXRoom: a box (offset +- size/2 in the object's local space) whose
// static triangles render as one room. Rooms and portals together replace
// the BVH for static rendering when at least one Room exists.
struct RoomComponent {
    Vec3 size{1, 1, 1};
    Vec3 offset{};
    ExtraKeys extra;
};

// PSXNavWalkoffZone: a box (+- size/2 in the object's local space). Nav
// boundary edges whose midpoint lies inside it let the player walk off.
struct WalkoffComponent {
    Vec3 size{1, 2, 0.5f};
    ExtraKeys extra;
};

// PSXPortalLink: a width x height rectangle in the object's local XY plane
// (normal = local +Z), joining two Room objects named by object name.
struct PortalComponent {
    std::string roomA, roomB;  // object names
    float width = 2.0f;
    float height = 3.0f;
    ExtraKeys extra;
};

// PSXAudioClip: an audio file (WAV, AIFF, MP3, Ogg Vorbis, FLAC) converted to SPU-ADPCM at export.
struct AudioComponent {
    std::string clip;      // project-relative audio file path, empty = no data
    std::string clipName;  // name Lua plays it by
    int sampleRate = 22050;
    bool loop = false;
    int defaultVolume = 100;
    bool trimLeadingSilence = false;
    ExtraKeys extra;
};

// PSXInteractable
struct InteractableComponent {
    float radius = 2.0f;
    int button = 14;  // pad bit, 14 = Cross
    bool repeatable = true;
    uint16_t cooldownFrames = 30;
    bool showPrompt = false;
    std::string promptCanvas;  // at most 15 bytes are stored
    bool lineOfSight = false;
    // Half-angle in degrees, 0..180, between the player's forward direction
    // and the object that still counts as facing it. Only used with lineOfSight.
    float facingAngle = 90.0f;
    ExtraKeys extra;
};

// PSXAgent: an NPC psxsplash moves along the nav regions (Agent.* in Lua).
// Waypoints are world positions; stateClips index the object's skin clips
// per AgentState (-1 = none).
constexpr int kAgentStates = 8;
constexpr int kAgentMaxWaypoints = 8;
extern const char* const kAgentStateNames[kAgentStates];  // Idle, Patrol, Seek, Flee, Attack, Wander, Investigate, Custom
struct AgentComponent {
    bool startEnabled = true;
    float moveSpeed = 2.0f;     // units per second
    float stopDistance = 0.5f;  // units
    bool vision = true;
    float visionRange = 8.0f;   // units, 0 = blind
    float fov = 90.0f;          // whole cone, degrees, (0, 360]
    int visionRegionDepth = 2;  // nav region hops, 0..8
    float hearingRange = 0.0f;  // units, 0 = deaf
    int alertTimeout = 90;      // frames, 0..65535
    std::vector<Vec3> waypoints;
    std::array<int, kAgentStates> stateClips{-1, -1, -1, -1, -1, -1, -1, -1};
    ExtraKeys extra;
};

struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1, 1, 1};
};

struct Object {
    std::string name;
    bool active = true;       // exported at all; off leaves the object out of the pack
    bool startActive = true;  // exported objects only: shown and colliding when the scene loads
    Transform transform;
    std::optional<MeshComponent> mesh;
    std::optional<ColliderComponent> collider;
    std::optional<ScriptComponent> script;
    std::optional<LightComponent> light;
    std::optional<PlayerComponent> player;
    std::optional<NavigationComponent> navigation;
    std::optional<TriggerComponent> trigger;
    std::optional<RoomComponent> room;
    std::optional<WalkoffComponent> walkoff;
    std::optional<PortalComponent> portal;
    std::optional<InteractableComponent> interactable;
    std::optional<AudioComponent> audio;
    std::optional<SkinComponent> skin;
    std::optional<AgentComponent> agent;
    std::vector<std::string> unknownComponents;  // components of an unknown type, as JSON text
    std::vector<Object> children;
    ExtraKeys extra;
};

struct FogSettings {
    bool enabled = false;
    std::array<float, 3> color{0.5f, 0.5f, 0.6f};
    int density = 5;
    ExtraKeys extra;
};

// Memory card saves (Unity: the PSXData MemCard* fields). psxsplash's
// MemoryCardManager names a save region + product + the Lua key, shows the
// title in the BIOS memory card manager, and draws the icon beside it. Off,
// the pack carries no table and the console keeps the configuration of the
// last scene that had one (or its built-in BA / SLUS-00000 / PSXSPLASH).
// Every scene of a game should carry the same values, or saves made in one
// scene are not found from another.
struct MemCardSettings {
    bool enabled = false;
    std::string region = "BA";           // exactly 2 ASCII characters: BA America, BE Europe, BI Japan
    std::string product = "SLUS-00000";  // 1..10 ASCII characters
    std::string title = "PSXSPLASH SAVE";  // printable ASCII, at most 32 characters
    // Project-relative 16x16 images, 1..3 frames (2 or 3 animate). The frames
    // share one 16-colour palette; alpha below 0.5 is transparent. Empty = a
    // blank icon.
    std::vector<std::string> icons;
    ExtraKeys extra;
};

constexpr size_t kMemCardRegionLen = 2, kMemCardProductMax = 10, kMemCardTitleMax = 32, kMemCardIconsMax = 3;
constexpr int kMemCardIconSize = 16;
// psxsplash's filename is region + product + key, cut at 20 characters.
constexpr size_t kMemCardFilenameMax = 20;

struct SceneSettings {
    float gteScaling = 100.f;
    SceneType sceneType = SceneType::Exterior;
    FogSettings fog;
    std::string networkId;
    std::string script;  // scene Lua file, empty = none
    // psxsplash render buffers: ordering table buckets and bump allocator
    // bytes per frame. 0 = computed by the exporter (ExportStats).
    int orderingTableSize = 0;
    int bumpAllocatorSize = 0;
    // Stream static scenery off the disc by region while playing (psxsplash
    // WorldStreamer), for worlds bigger than RAM. Regions are automatic.
    bool streamWorld = false;
    // Name of a UICanvas in this scene shown by psxsplash while the scene
    // loads (the loader pack, scene_<i>.loading). Empty = none. That canvas
    // is left out of the splashpack.
    std::string loadingCanvas;
    MemCardSettings memCard;
    ExtraKeys extra;
};

// Project-wide VRAM layout (Unity: Assets/PSXData.asset).
struct VramSettings {
    int resolutionX = 320, resolutionY = 240;
    bool dualBuffering = true;
    bool verticalBuffering = true;
    struct Area {
        int x, y, w, h;
    };
    std::vector<Area> prohibited;
};
// VRAM is 1024 x 512 16-bit words.
constexpr int kVramWidth = 1024, kVramHeight = 512;

// A project's VRAM settings file (vram.json): only the prohibited areas are
// stored, as {"prohibited": [{"x":..,"y":..,"w":..,"h":..}]}; resolution and
// buffering are psxsplash's fixed 320x240 dual vertical layout. A missing or
// unreadable file reads as the defaults; entries that are not objects with
// four numbers are skipped; areas are clamped into VRAM with w, h >= 1.
// writeVramSettings returns false when the file cannot be written.
VramSettings readVramSettings(const std::filesystem::path& file);
bool writeVramSettings(const std::filesystem::path& file, const VramSettings& vs);
// One area clamped as readVramSettings clamps it.
VramSettings::Area clampVramArea(VramSettings::Area a);

// A custom font for UI text, rasterised at export. Either `source` (a TTF/OTF
// rasterised at `size` pixels per em) or `bitmap` (a 256-wide PNG of glyph
// cells from 0x20 in ASCII order, ink = alpha above 0.5) is set.
struct UIFont {
    std::string name;    // what text elements refer to it by
    std::string source;  // project-relative TTF/OTF
    int size = 16;       // pixels per em
    std::string bitmap;  // project-relative PNG
    int glyphWidth = 8, glyphHeight = 16;  // bitmap cells
    // Bitmap fonts: per-character advance (0x20..0x7F). Empty = measured from
    // each cell's ink.
    std::vector<int> advances;
    ExtraKeys extra;
};

enum class UIElementType : uint8_t { Image = 0, Box = 1, Text = 2, Progress = 3, Line = 4 };

// One UI element. Layout is in screen pixels. Each axis is anchored to a
// fraction of the screen: with anchorMin == anchorMax on an axis, rect x/y is
// the offset of the top-left corner from that point and w/h the size; with
// them apart, the element stretches between the two points and x/y and w/h
// are the insets (left/top, and right minus left / bottom minus top).
struct UIElement {
    UIElementType type = UIElementType::Box;
    std::string name;
    bool visible = true;
    std::array<int, 4> rect{0, 0, 16, 16};  // x, y, w, h
    std::array<float, 2> anchorMin{0, 0}, anchorMax{0, 0};
    std::array<float, 3> color{1, 1, 1};
    // Text
    std::string text;
    std::string font;  // UIFont name, empty = system font
    // Line, in screen pixels (anchors and rect do not apply)
    std::array<int, 2> from{0, 0}, to{0, 0};
    // Progress
    std::array<float, 3> background{0.2f, 0.2f, 0.2f};
    int value = 0;  // 0..100
    // Image
    std::string texture;  // project-relative image
    BitDepth bitDepth = BitDepth::Bpp8;
    bool cutout = true;
    // Sheet-backed: the name of a scene sprite sheet (empty = a plain image)
    // and the cell of it shown, numbered as the sheet's. The sheet's texture,
    // bit depth and cutout then stand in for texture/bitDepth/cutout, and Lua's
    // UI.SetFrame steps through its cells.
    std::string sheet;
    int cell = 0;
    // An element of a type this build does not know, as JSON text: kept on
    // save, skipped on export.
    std::string unknown;
    ExtraKeys extra;
};

struct UICanvas {
    std::string name;
    bool visible = true;
    int sortOrder = 0;  // 0 = back, 255 = front
    std::string defaultFont;  // UIFont name, empty = system font
    std::vector<UIElement> elements;
    ExtraKeys extra;
};

// A sprite sheet: one texture cut into a grid of cellW x cellH cells,
// numbered row by row from the top left, and the animations that play runs
// of those cells. The splashpack carries only sheets and animations;
// psxsplash's sprites are made from Lua (Sprite.Create) and refer to them by name.
struct SpriteAnim {
    std::string name = "idle";  // looked up across every sheet, so unique in the scene
    int firstFrame = 0;         // absolute cell index in the sheet
    int frameCount = 1;
    int frameDuration = 6;  // vsyncs (1/60 s) each cell shows
    bool loop = true;
    ExtraKeys extra;
    bool operator==(const SpriteAnim&) const = default;
};

struct SpriteSheet {
    std::string name;
    std::string texture;  // project-relative image
    BitDepth bitDepth = BitDepth::Bpp4;
    int cellW = 16, cellH = 16;
    std::vector<SpriteAnim> anims;
    ExtraKeys extra;
    bool operator==(const SpriteSheet&) const = default;
};

// A tilemap brush: what a painted cell draws and how the world behaves there.
// Cells store brush indices, so changing a brush changes every cell painted with it.
struct TileBrush {
    std::string name = "floor";
    int cell = 0;           // tileset cell drawn, row by row from 0; -1 draws nothing (a hole)
    bool walkable = true;   // off: a wall
    int kind = 0;           // 0: a plain tile; else an object Lua finds with Tile.ObjectAt
    int id = -1;            // the object's id; -1 numbers each kind 1, 2, 3 in reading order
    std::array<float, 3> color{1, 1, 1};  // the painter's tint, not exported
    ExtraKeys extra;
    bool operator==(const TileBrush&) const = default;
};

// The scene's one screen-space tilemap (psxsplash's TileSystem): a grid of
// brush indices drawn from a sprite sheet's cells, the sheet's cell size being
// the tile size.
struct Tilemap {
    static constexpr int kMaxSide = 256;
    std::string tileset;  // a scene sprite sheet's name
    int width = 32, height = 32;  // 1..kMaxSide
    std::vector<TileBrush> brushes;
    std::vector<std::string> kindNames{"(none)"};  // labels for kind values, authoring only
    std::vector<int> cells = std::vector<int>(size_t(32 * 32), -1);  // row-major brush indices, -1 = empty
    ExtraKeys extra;
    bool operator==(const Tilemap&) const = default;
    // The brush index at x, y; -1 off the map (or past a short cell list).
    int at(int x, int y) const {
        const size_t i = size_t(y * width + x);
        return x < 0 || y < 0 || x >= width || y >= height || i >= cells.size() ? -1 : cells[i];
    }
    // A new size (each side clamped to 1..kMaxSide), keeping the cells both sizes share.
    void resize(int w, int h);
};

// The font a text element draws with: its own when it names one, else its
// canvas's default font; empty means the system font.
const std::string& effectiveFont(const UICanvas& canvas, const UIElement& e);

// Cutscene track types, numbered as psxsplash's TrackType.
enum class TrackType : uint8_t {
    CameraPosition = 0, CameraRotation = 1, ObjectPosition = 2, ObjectRotation = 3, ObjectActive = 4,
    UICanvasVisible = 5, UIElementVisible = 6, UIProgress = 7, UIPosition = 8, UIColor = 9, CameraH = 10,
    RumbleSmall = 11, RumbleLarge = 12, ObjectUVOffset = 13, LightPosition = 14, LightColor = 15,
    LightIntensity = 16, LightRadius = 17, LightEnabled = 18
};
enum class Interp : uint8_t { Linear = 0, Step = 1, EaseIn = 2, EaseOut = 3, EaseInOut = 4 };

// A key. `value` is in the track's own units: world units for positions,
// degrees for rotations, 0/1 for on/off tracks, 0..100 for progress, pixels
// for UI position and UV offset, 0..1 colour, raw projection H, 0..255 rumble;
// light tracks: world units for position and radius, 0..1 colour, the light's
// intensity, 0/1 enabled.
struct Keyframe {
    int frame = 0;  // 0..8191
    std::array<float, 3> value{0, 0, 0};
    Interp interp = Interp::Linear;
    ExtraKeys extra;
};

struct CutsceneTrack {
    TrackType type = TrackType::ObjectPosition;
    // Object tracks: object name. UI canvas tracks: canvas name. UI element
    // tracks: "canvas/element". Light tracks: the runtime light's object name.
    // Camera and rumble tracks: empty.
    std::string target;
    std::vector<Keyframe> keyframes;
    ExtraKeys extra;
};

struct CutsceneAudioEvent {
    int frame = 0;
    std::string clip;  // an audio component's clipName
    int volume = 100;  // 0..128
    int pan = 64;      // 0..127
    ExtraKeys extra;
};

// Starts one clip of a skinned mesh, as SkinnedAnim.Play would.
struct SkinAnimEvent {
    int frame = 0;
    std::string object;  // an object with a skin component
    std::string clip;    // the name of one of its clips
    bool loop = false;
    ExtraKeys extra;
};

struct Cutscene {
    std::string name;
    int durationFrames = 90;  // at 30 frames per second
    std::vector<CutsceneTrack> tracks;
    std::vector<CutsceneAudioEvent> audioEvents;
    std::vector<SkinAnimEvent> skinEvents;
    ExtraKeys extra;
};

// Played by Animation.Play. Unlike a cutscene, several run at once and they
// have no camera tracks and no audio.
struct Animation {
    std::string name;
    int durationFrames = 90;  // at 30 frames per second
    std::vector<CutsceneTrack> tracks;
    std::vector<SkinAnimEvent> skinEvents;
    ExtraKeys extra;
};

struct Scene {
    SceneSettings settings;
    std::vector<Object> objects;
    std::vector<UIFont> fonts;
    std::vector<SpriteSheet> spriteSheets;
    std::optional<Tilemap> tilemap;
    std::vector<UICanvas> canvases;
    std::vector<Cutscene> cutscenes;
    std::vector<Animation> animations;
    ExtraKeys extra;
};

// A flattened object: the tree walked depth-first pre-order, which is the
// canonical index order of everything the splashpack addresses by position.
struct FlatObject {
    const Object* object;
    Vec3 worldPosition;
    Quat worldRotation;
    Vec3 lossyScale;
    Mat34 localToWorld;
    bool activeInHierarchy;  // Unity's meaning: the object's own active flag and every ancestor's are on
};

// An object's Trigger box in world space: the axis-aligned bounds of the
// box after the object's transform, which is what the splashpack stores.
inline void triggerWorldBox(const FlatObject& fo, Vec3* mn, Vec3* mx) {
    const Vec3 half = fo.object->trigger ? fo.object->trigger->size * 0.5f : Vec3{};
    for (int i = 0; i < 8; i++) {
        const Vec3 c{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        const Vec3 p = fo.localToWorld.point(c);
        *mn = i ? vmin(*mn, p) : p;
        *mx = i ? vmax(*mx, p) : p;
    }
}

// An object's Room box in world space: the axis-aligned bounds of the
// 8 corners of offset +- size/2 after the object's transform.
inline void roomWorldBox(const FlatObject& fo, Vec3* mn, Vec3* mx) {
    const Vec3 half = fo.object->room ? fo.object->room->size * 0.5f : Vec3{};
    const Vec3 off = fo.object->room ? fo.object->room->offset : Vec3{};
    for (int i = 0; i < 8; i++) {
        const Vec3 c{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        const Vec3 p = fo.localToWorld.point(off + c);
        *mn = i ? vmin(*mn, p) : p;
        *mx = i ? vmax(*mx, p) : p;
    }
}

// An object's walk-off zone in world space: the axis-aligned bounds of the
// 8 corners of +- size/2 after the object's transform.
inline void walkoffWorldBox(const FlatObject& fo, Vec3* mn, Vec3* mx) {
    const Vec3 half = fo.object->walkoff ? fo.object->walkoff->size * 0.5f : Vec3{};
    for (int i = 0; i < 8; i++) {
        const Vec3 c{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        const Vec3 p = fo.localToWorld.point(c);
        *mn = i ? vmin(*mn, p) : p;
        *mx = i ? vmax(*mx, p) : p;
    }
}

// A Portal's rectangle in world space. normal/right/up are unit vectors
// along the object's local Z/X/Y axes; the half extents honour the
// object's scale along right/up (Unity exports the raw size instead).
struct PortalFrame {
    Vec3 center, normal, right, up;
    float halfW = 0, halfH = 0;
};
inline PortalFrame portalWorldFrame(const FlatObject& fo) {
    PortalFrame f;
    const Mat34& m = fo.localToWorld;
    const Vec3 ax{m.m[0][0], m.m[1][0], m.m[2][0]};
    const Vec3 ay{m.m[0][1], m.m[1][1], m.m[2][1]};
    const Vec3 az{m.m[0][2], m.m[1][2], m.m[2][2]};
    f.center = m.position();
    f.right = normalized(ax);
    f.up = normalized(ay);
    f.normal = normalized(az);
    const float w = fo.object->portal ? fo.object->portal->width : 0.f;
    const float h = fo.object->portal ? fo.object->portal->height : 0.f;
    f.halfW = w * 0.5f * magnitude(ax);
    f.halfH = h * 0.5f * magnitude(ay);
    return f;
}

// Material mat's texture index as the exporter numbers them: the object's
// distinct textures in material order, a repeated image keeping its first
// number. -1 when the material has no texture or does not exist.
int uvTextureSlot(const MeshComponent& mc, size_t mat);
// The materials whose triangles a UV offset track scrolls: the exporter flags a
// triangle when its texture index equals uvOffsetMaterial.
std::vector<size_t> uvOffsetMaterials(const MeshComponent& mc, int index);

std::vector<FlatObject> flatten(const Scene& scene);

Scene loadScene(const std::filesystem::path& file);
void saveScene(const Scene& scene, const std::filesystem::path& file);

// Objects with their children as a self-contained JSON document, in the same
// form the scene file stores them: {"splashedit-objects": 1, "objects": [...]}.
// Asset paths stay project-relative strings. For the editor's clipboard.
std::string writeObjectsText(const std::vector<Object>& objects);
// The objects in such a document; nullopt for anything else (plain text,
// other JSON, a later version, an empty list, a malformed object).
std::optional<std::vector<Object>> readObjectsText(const std::string& text);

// UI elements as a self-contained JSON document, the way the scene file
// stores them: {"splashedit-ui-elements": 1, "elements": [...]}. A marker of
// its own, so object and UI element clipboard text never pass for each other.
std::string writeUIElementsText(const std::vector<UIElement>& elements);
// The elements in such a document; nullopt for anything else (object
// clipboard text included), a later version, an empty list or a malformed element.
std::optional<std::vector<UIElement>> readUIElementsText(const std::string& text);

// Skeleton in mesh space. Joint transforms are local to the parent joint
// (parent -1 = mesh space) and give the rest pose.
struct Joint {
    std::string name;
    int parent = -1;
    Vec3 position;
    Quat rotation;
    Vec3 scale{1, 1, 1};
};

struct MeshSkin {
    std::vector<Joint> joints;
    // Per joint, mesh space -> joint space at bind time, 3x4 row-major.
    // Empty = the inverse of each joint's rest transform.
    std::vector<std::array<float, 12>> inverseBind;
    // Per vertex, up to four influences; unused slots have weight 0.
    std::vector<std::array<int, 4>> vertexJoints;
    std::vector<std::array<float, 4>> vertexWeights;
};

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uv;
    std::vector<std::array<float, 4>> colors;
    std::vector<std::vector<int>> submeshes;
    std::optional<MeshSkin> skin;
    // Mesh.bounds as Unity computes it from the vertices.
    Bounds bounds() const;
};

Mesh loadMesh(const std::filesystem::path& file);
void saveMesh(const Mesh& mesh, const std::filesystem::path& file);

// Skeletal animation clip (*.anim): keyframed joint channels, by joint name.
enum class AnimProperty { Position, Rotation, Scale };
enum class AnimInterp { Linear, Step };

struct AnimChannel {
    std::string joint;
    AnimProperty property = AnimProperty::Rotation;
    AnimInterp interp = AnimInterp::Linear;
    std::vector<float> times;   // seconds, ascending
    std::vector<float> values;  // 3 per key (position, scale) or 4 (rotation x,y,z,w)
};

struct AnimClip {
    std::string name;
    float length = 0;  // seconds
    bool loop = false;
    std::vector<AnimChannel> channels;
};

AnimClip loadAnim(const std::filesystem::path& file);
void saveAnim(const AnimClip& clip, const std::filesystem::path& file);
// The loop flag an imported clip starts with, from its name: true when a
// word of it (split at non-letters and lower-to-upper case changes, any case)
// starts with idle, walk, run, jog, loop, cycle, patrol, swim, fly, breath or
// hover, so "Walk", "IdleLong", "walk_cycle" and "Running" loop and "Attack",
// "Death" and "Prune" play once.
bool defaultClipLoop(const std::string& name);

}  // namespace splash
