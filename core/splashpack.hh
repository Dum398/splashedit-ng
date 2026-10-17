// Splashpack writer: Scene IR -> .splashpack + .vram + .spu, as SplashEdit 2.4
// writes them (splashpack v25). See docs/splashpack-format.md.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

struct ExportOptions {
    VramSettings vram;
    // Parity testing only: export objects in this name order instead of the
    // canonical tree order (to reproduce a Unity export, whose order is the
    // unordered FindObjectsByType result). Names not listed keep tree order
    // after the listed ones.
    std::vector<std::string> objectOrder;
    // Store Lua scripts as psxlua bytecode (compileLua) instead of source.
    bool luaBytecode = false;
    // Build everything but write no files; ExportResult::stats still fills in.
    bool dryRun = false;
};

// A world streaming problem, for the editor to show with its fix.
enum class StreamProblem : uint8_t { NothingStreams, TooManyObjects, LargeRegion, UnevenRegions, TooManyNear, NoSaving, CdMusic };
struct StreamWarning {
    StreamProblem kind;
    std::string title, body;
};

// What one export puts on the console, in bytes unless noted.
// Ordering table size limits. The low end is Renderer::WORLD_DEPTH_MIN: the
// table must reach past the 2D bands. The exporter clamps overrides to these.
constexpr uint32_t kOtMin = 256, kOtMax = 65536;

struct ExportStats {
    size_t splashpackBytes = 0, vramFileBytes = 0, spuFileBytes = 0;
    // VRAM: display and draw buffers, texture atlases (each width x 256 at
    // 16 bits per texel), CLUTs and font sheets.
    size_t framebufferBytes = 0, atlasBytes = 0, clutBytes = 0, fontBytes = 0;
    size_t vramBytes() const { return framebufferBytes + atlasBytes + clutBytes + fontBytes; }
    // SPU RAM end address after psxsplash uploads every clip: the engine
    // starts at 0x1010 and rounds each clip's address and size up to 16 bytes.
    size_t spuEnd = 0;
    int triangles = 0;
    // Most triangles that can be in view in one frame: every triangle of the
    // objects that do not stream plus streamDrawTris (the same worst case
    // bumpAllocatorNeed sizes the render buffer for).
    uint64_t trianglesInView = 0;
    // psxsplash render buffers. The *Need values are the exporter's worst case
    // for this scene; the plain ones are what the splashpack carries, which is
    // the scene's override when it has one.
    uint32_t orderingTableNeed = 0, bumpAllocatorNeed = 0;
    uint32_t orderingTableSize = 0, bumpAllocatorSize = 0;
    // Bytes psxsplash allocates for them: two ordering tables of size + 1
    // four-byte entries and two bump allocators.
    size_t rendererBytes() const { return 2 * (size_t(orderingTableSize) + 1) * 4 + 2 * size_t(bumpAllocatorSize); }
    // Triangle references in the BVH, as the header carries them. psxsplash
    // sizes its frustum culling output to this, one four-byte TriangleRef each
    // (Renderer::ReserveVisibleTriangles).
    uint32_t bvhTriangleRefs = 0;
    size_t visibleListBytes() const { return 4 * size_t(bvhTriangleRefs); }
    // The BVH's nodes for the viewport, in node order (breadth first, so the
    // root is first and a parent comes before its children): world space
    // bounds, Y up, in the same frame as navOutlines. depth is 0 at the root,
    // tris is the node's triangle count (non-zero on leaves only). bvhDepth is
    // the deepest node's depth, 0 with no nodes.
    struct BvhBox {
        Vec3 lo, hi;
        int depth = 0;
        bool leaf = false;
        int tris = 0;
    };
    std::vector<BvhBox> bvhBoxes;
    int bvhDepth = 0;
    // Where each packed texture lands in VRAM (deduplicated, placement
    // order). x and clutX are in 16-bit words, absolute.
    struct TexturePlacement {
        std::string source;
        int bitDepth = 0;
        bool cutout = false;
        int width = 0, height = 0;
        int x = 0, y = 0;
        int clutX = 0, clutY = 0, clutLength = 0;  // clutLength 0: no CLUT (16 bpp)
    };
    std::vector<TexturePlacement> textures;
    // What occupies VRAM, as packing laid it out, in 16-bit words and lines
    // (VRAM is 1024 x 512): the two framebuffers, the column at x 960 kept
    // for fonts, the project's prohibited areas, each placed atlas, each CLUT
    // (one line, 16 or 256 words) and each font sheet. Filled on dry runs too,
    // and also when packing fails (with what did fit).
    enum class VramKind : uint8_t { Framebuffer, Reserved, Prohibited, Atlas4, Atlas8, Atlas16, Clut, Font };
    struct VramRect {
        VramKind kind;
        int x = 0, y = 0, w = 0, h = 0;
        std::string label;
    };
    std::vector<VramRect> vramMap;
    // Game objects in the file, streaming's cut pieces included. psxsplash
    // allocates per object when the scene loads (objectLoadBytes, budget.hh).
    int gameObjects = 0;

    // Nav bake (Player or Navigation object). psxsplash only hands the camera
    // to the player when the scene has nav regions; with none it is a free
    // camera that ignores the pad.
    bool navWanted = false;            // the scene has a Player or Navigation object
    int navRegions = 0;
    int navPortals = 0;  // one per direction: a shared edge counts once for each side
    float navCellSize = 0, navCellSizeAsked = 0;  // metres
    float navMaxPlaneError = 0;  // the bake's Max plane error, metres
    float navExtentX = 0, navExtentZ = 0;         // walkable geometry, metres

    // World streaming (SceneSettings::streamWorld). All zero when the scene
    // does not stream. Distances and bounds are PS1 fixed point (4096 = one
    // GTE unit, gteScaling world units).
    int streamRegions = 0;
    int streamedObjects = 0;     // objects whose triangles live in the .geo file
    int streamSplitObjects = 0;  // extra game objects made by cutting big meshes at region edges
    int streamSlots = 0, streamSlotsWanted = 0;
    uint32_t streamSlotBytes = 0;
    int32_t streamLoadRadius = 0;
    int64_t streamRegionSize = 0;
    size_t streamedTriBytes = 0;  // what those triangles would take in RAM without streaming
    uint64_t streamDrawTris = 0;  // most of them within the draw distance at once (render buffer sizing)
    size_t streamTableBytes = 0, geoFileBytes = 0;
    std::vector<std::array<int32_t, 4>> streamRegionBounds;  // minX, minZ, maxX, maxZ
    std::vector<float> streamRegionFloor;  // per region, its lowest point in world units (Y up)
    // Each baked nav region's outline, in world metres (Y up), for the viewport.
    std::vector<std::vector<Vec3>> navOutlines;
    // Each baked nav region's facts for the inspector, in navOutlines order.
    struct NavPortalInfo {
        int neighbor = 0;
        float heightDelta = 0;  // metres
        Vec3 a, b;              // the shared edge, world metres, Y on this region's floor plane
    };
    struct NavRegionInfo {
        Vec3 centroid;  // XZ = vertex average, Y on the region's floor plane
        float floorY = 0;  // centroid.y
        // Index into rooms, -1 for none. When navIslands is set the scene has
        // no Room objects and this is the region's connectivity island.
        int room = -1;
        int surface = 0;  // 0 flat, 1 ramp, 2 stairs
        bool platform = false;
        // Bit j: the edge from outline vertex j to j+1 lets the player walk
        // off (a platform's boundary, or inside a walk-off zone).
        uint8_t walkoffEdges = 0;
        // Largest distance of the region's ground samples from its floor
        // plane, metres (NavRegion::maxPlaneDeviation).
        float planeDeviation = 0;
        std::vector<NavPortalInfo> portals;
    };
    std::vector<NavRegionInfo> navInfo;
    bool navIslands = false;
    // Nav walk-off zones on active objects, in flatten order: the boundary
    // edges each one opened (an edge inside two counts for the first).
    struct WalkoffInfo {
        std::string object;  // the object's name
        int flatIndex = 0;   // its index in flatten(scene)
        int edges = 0;
    };
    std::vector<WalkoffInfo> walkoffZones;

    // Skinned meshes, in skin table order. bytes is the skin's 12-byte table
    // entry plus its whole data block (alignment padding included); a clip's
    // bytes run from its name length byte through its last matrix.
    struct SkinClipInfo {
        std::string name;
        int fps = 0;
        int frames = 0;
        size_t bytes = 0;
    };
    struct SkinInfo {
        std::string object;
        int bones = 0;
        size_t triangles = 0;
        size_t bytes = 0;
        std::vector<SkinClipInfo> clips;
    };
    std::vector<SkinInfo> skins;
    std::vector<StreamWarning> streamWarnings;  // also in ExportResult::warnings, as one line each

    // Rooms and portals (Room objects). Empty when the scene has none, and
    // then psxsplash renders static geometry through the BVH.
    struct RoomSummary {
        std::string name;
        int triRefs = 0, cells = 0, portals = 0;
    };
    std::vector<RoomSummary> rooms;  // real rooms in room order; the catch-all is not listed
    int roomCatchAllTris = 0, roomPortals = 0;
    size_t roomBlockOffset = 0;  // byte offset of the room block in the splashpack, 0 = none
    size_t streamPoolBytes() const { return size_t(streamSlots) * streamSlotBytes; }
    // Negative when streaming costs more RAM than it frees.
    long long streamRamSaved() const {
        return (long long)streamedTriBytes - (long long)streamPoolBytes() - (long long)streamTableBytes;
    }
};

// With streaming on and no fog, the automatic draw distance (ordering table
// size), in GTE units: SplashEdit 2.4's default ordering table.
constexpr uint32_t kStreamDrawDistance = 8192;

struct ExportResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    ExportStats stats;
    bool ok() const { return errors.empty(); }
};

// The scene has a Player or Navigation object but the bake found nowhere to
// stand, so psxsplash leaves the camera at the origin and ignores the pad.
inline std::string navEmptyWarning() {
    return "The player can't walk: the nav bake found no ground to stand on, so the game keeps the camera at the origin "
           "and ignores the controller. Give the floor or terrain a Static collider, and make it flat enough and wider "
           "than the player.";
}

// Set when the bake ran with bigger cells than the scene asks for.
inline bool navCoarsened(const ExportStats& s) { return s.navCellSize > s.navCellSizeAsked * 1.001f; }

// Problems with a scene's memory card settings that export reports, one line
// each saying how to fix it, for the inspector to show as they are typed. Only
// checks an enabled config, and not the icon files (export loads those).
void checkMemCard(const MemCardSettings& mc, std::vector<std::string>& errors, std::vector<std::string>& warnings);

// Where a UI position track on `e` starts, in the keys' units (the scene
// file's): psxsplash's CutscenePlayer starts the track from the element's
// resolved screen position (UISystem::getPosition: the anchor's pixel plus the
// offset, clamped at 0), and writeSequences adds the anchor's rounding
// correction to every key, so this is that position less the correction.
std::array<int, 2> uiPositionTrackStart(const UIElement& e, const VramSettings& vs = {});

// `projectRoot` resolves the project-relative asset paths in the scene.
// `out` is the .splashpack path; .vram and .spu are written next to it.
ExportResult exportSplashpack(const Scene& scene, const std::filesystem::path& projectRoot,
                              const std::filesystem::path& out, const ExportOptions& options = {});

// The loader pack (psxsplash LoadingScreen, "LP" version 2): the canvas named
// by scene.settings.loadingCanvas with its image atlases, CLUTs and font
// sheets, drawn by psxsplash while the scene loads. `out` is the .loading
// path. With no loadingCanvas nothing is written and an old `out` is removed;
// a name that matches no canvas is an error. See docs/splashpack-format.md.
ExportResult exportLoaderPack(const Scene& scene, const std::filesystem::path& projectRoot,
                              const std::filesystem::path& out, const ExportOptions& options = {});

// The vertex colours the exporter writes for `object`'s mesh, in mesh vertex
// order, 0..1 per channel (the shipped byte is the value * 255, truncated).
// Material colour on untextured submeshes is not included. Baked meshes get
// every enabled light; on a runtime-lit mesh the console adds the runtime
// lights itself, which this approximates by baking them too.
std::vector<Vec3> exportVertexColors(const std::vector<FlatObject>& flat, const FlatObject& object,
                                     const Mesh& mesh);


// The scene's network id as the header stores it: FNV-1a over the low byte
// of each UTF-16 unit of `id` (what Unity writes), 0 when `id` is empty, so
// the engine derives one from the scene index and object count.
uint32_t hashSceneId(const std::string& id);

}  // namespace splash
