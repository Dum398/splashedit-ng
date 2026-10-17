# splashpack scene format (writer v25, reader accepts v20..v25)

Sources read (all via `git show origin/main:<path>`, never the working checkout):

- Writer repo `splashedit`, origin/main = `64785e3` ("Merge pull request #52 from psxsplash/fix-stream-hang")
- Reader repo `psxsplash`, origin/main = `c1df566` ("Merge pull request #55 from psxsplash/fix-heap-symbols")

Conventions: little-endian everywhere (.NET `BinaryWriter` on x86, MIPS LE reader). All offsets are absolute byte offsets from file start.
"W:" = writer file `Runtime/<file>.cs` (line numbers are origin/main line numbers, where I give them); "R:" = reader file under `src/`.
Quotes are verbatim from those files. Where I say "reader not located" I did not find the read site.

One scene export produces up to four files, all next to each other (writer: `PSXSceneWriter.Write`):

| file | extension | always? | contents |
|---|---|---|---|
| main pack | as given (`.splashpack`) | yes | everything in sections 1-2 below |
| VRAM | `ChangeExtension(path,".vram")` | yes | atlas pixels, CLUT palettes, font bitmaps |
| SPU | `ChangeExtension(path,".spu")` | yes | ADPCM audio |
| geo | `ChangeExtension(path,".geo")` | only if world streaming is active (else a stale one is deleted) | streamed Tri blobs |

A fifth, separate format `.loading` ("LP" magic, `PSXLoaderPackWriter`; ng `exportLoaderPack`) holds a scene's loading screen. It is NOT part of the splashpack; it reuses the UI table layout (font desc 112 B, canvas desc 12 B, element 48 B). See 4.6.

---

## 1. Header

Version written (W:PSXSceneWriter.cs:233): `writer.Write((ushort)(hasLights ? 24 : 23));`
So the writer emits exactly two versions: **23** when `pointLights.Length == 0`, **24** otherwise.
Header size: **144 bytes (v23)**, **148 bytes (v24)**, **156 bytes (v25)**. The writer always emits v25.
Reader (R:splashpack.cpp): `static_assert(sizeof(SPLASHPACKFileHeader) == 156 ...)`, `assert(header->version >= 20, ...)`, and
`splashpackHeaderSize`: `>=25 -> 156`, `>=24 -> 148`, `>=22 -> 144`, `>=21 -> 128`, else `120`. The reader assumes the header only ever grows by appending.
The doc comment at the top of W says "splashpack v16"; the Write() doc says "v20". Both are stale. The emitted number is 23/24.

Field table (writer order = reader struct order; `writer.Write('S')` writes the single byte 0x53 because BinaryWriter encodes a `char` as UTF-8):

| off | size | type | field | meaning / writer value |
|---|---|---|---|---|
| 0 | 2 | char[2] | magic | `'S','P'` |
| 2 | 2 | u16 | version | 23 or 24 |
| 4 | 2 | u16 | luaFileCount | unique LuaFile count |
| 6 | 2 | u16 | gameObjectCount | `scene.exporters.Length` |
| 8 | 2 | u16 | textureAtlasCount | `scene.atlases.Length` |
| 10 | 2 | u16 | clutCount | number of textures having a palette across atlases |
| 12 | 2 | u16 | colliderCount | exporters with `CollisionType==Dynamic` and a `MeshFilter.sharedMesh` |
| 14 | 2 | u16 | interactableCount | |
| 16 | 6 | i16[3] | playerStartPos | x, -y, z via `ConvertCoordinateToPSX` (4.12, **int16**, clamped) |
| 22 | 6 | i16[3] | playerStartRot | 2.4.0: euler.x/y/z (degrees) * Deg2Rad via `ConvertToFixed12` (4.12 int16). The engine reads `psyqo::Angle` (units of pi), see M17; this exporter writes degrees / 180 |
| 28 | 2 | u16 | playerHeight | `(ushort)ConvertCoordinateToPSX(playerHeight, gte)` |
| 30 | 2 | i16/u16 | sceneLuaFileIndex | index in lua list, `-1`(=0xFFFF) if none |
| 32 | 2 | u16 | bvhNodeCount | `Min(NodeCount, 65535)` |
| 34 | 2 | u16 | bvhTriangleRefCount | `Min(TriangleRefCount, 65535)` |
| 36 | 2 | u16 | sceneType | 0 Exterior, 1 Interior (`PSXSceneType`) |
| 38 | 2 | u16 | triggerBoxCount | (was pad0) |
| 40 | 2 | u16 | worldCollisionMeshCount | always 0 ("removed, kept for binary compat") |
| 42 | 2 | u16 | worldCollisionTriCount | always 0 |
| 44 | 2 | u16 | navRegionCount | |
| 46 | 2 | u16 | navPortalCount | |
| 48 | 2 | u16 | moveSpeed | fp12 per frame, see sec. 3 |
| 50 | 2 | u16 | sprintSpeed | fp12 per frame |
| 52 | 2 | u16 | jumpVelocity | fp12 |
| 54 | 2 | u16 | gravity | fp12 |
| 56 | 2 | u16 | playerRadius | `(ushort)ConvertCoordinateToPSX(playerRadius, gte)` |
| 58 | 2 | u16 | fogMaskClut4 | CLUT word `y << 6 \| x / 16` of the 4bpp fog mask CLUT, 0 = none (sec. 4.4) |
| 60 | 4 | u32 | nameTableOffset | backfilled; always written (object name table) |
| 64 | 2 | u16 | audioClipCount | |
| 66 | 2 | u16 | fogMaskClut8 | the same for the 8bpp fog mask CLUT, 0 = none |
| 68 | 4 | u32 | audioTableOffset | backfilled only if audioClipCount>0, else 0 |
| 72 | 1 | u8 | fogEnabled | 0/1 |
| 73 | 3 | u8[3] | fogR,G,B | `Clamp(RoundToInt(c*255),0,255)` |
| 76 | 1 | u8 | fogDensity | `Clamp(fogDensity,1,10)` |
| 77 | 1 | u8 | pad3 | 0 |
| 78 | 2 | u16 | roomCount | `roomCount>0 ? roomCount+1 : 0` (+1 = catch-all room) |
| 80 | 2 | u16 | portalCount | room portals |
| 82 | 2 | u16 | roomTriRefCount | total tri refs incl. catch-all |
| 84 | 2 | u16 | cutsceneCount | `scene.cutscenes.Length` (table is capped to 16; reader caps too) |
| 86 | 2 | u16 | roomCellCount | |
| 88 | 4 | u32 | cutsceneTableOffset | backfilled if cutsceneCount>0 |
| 92 | 2 | u16 | uiCanvasCount | |
| 94 | 1 | u8 | uiFontCount | |
| 95 | 1 | u8 | uiPad5 | 0 |
| 96 | 4 | u32 | uiTableOffset | backfilled if canvases or fonts exist |
| 100 | 4 | u32 | pixelDataOffset | always 0 ("no dead zone"). No use of this field found in R:splashpack.cpp |
| 104 | 2 | u16 | animationCount | `scene.animations.Length` (table capped to 16) |
| 106 | 2 | u16 | roomPortalRefCount | |
| 108 | 4 | u32 | animationTableOffset | backfilled if animationCount>0 |
| 112 | 2 | u16 | skinnedMeshCount | `bakedSkinData.Length` (table capped to 16) |
| 114 | 2 | u16 | agentCount | |
| 116 | 4 | u32 | skinTableOffset | backfilled if skinnedMeshCount>0 |
| 120 | 4 | u32 | memcardTableOffset | backfilled only if `memCardEnabled`, else 0 (splashedit-ng: `settings.memCard.enabled`, sec. 2.22) |
| 124 | 4 | u32 | streamTableOffset | writer calls it `reservedMemcard`; backfilled via `memcardTableOffsetPos + 4` only if streaming active |
| 128 | 4 | u32 | spriteTableOffset | backfilled only if >=1 sprite sheet |
| 132 | 2 | u16 | spriteSheetCount | |
| 134 | 2 | u16 | spriteAnimCount | |
| 136 | 4 | u32 | sceneHash | FNV-1a32 of `SceneNetworkId`, 0 if empty (sec. 3) |
| 140 | 4 | u32 | tilemapTableOffset | backfilled only if a tilemap exists |
| 144 | 4 | u32 | lightTableOffset | v24+; backfilled after the light chunk, 0 if no runtime lights |
| 148 | 4 | u32 | orderingTableSize | v25+; ordering table buckets, 0 = engine default |
| 152 | 4 | u32 | bumpAllocatorSize | v25+; bump allocator bytes per frame, 0 = engine default |

Reader gating (R:splashpack.cpp): agentCount only used if `version>=22`; skin table `version>=18`; memcard `>=21`; sprite/sceneHash `>=22`; tilemap `>=23`; lights `>=24`; render buffer sizes `>=25`; skin bind positions and joint names `>=26`; interactable facing cosine (32-byte records) `>=27`; stream table `>=21`; per-cutscene/animation skin events `>=19`. The writer emits v27, so all gates are satisfied.

---

## 2. Sections in file order (writer order)

Cursor model: the reader walks a cursor from `data + headerSize` through sections 2.1-2.10 **without any offset field**, aligning only where it says so. Everything after (2.11+) is reached by header offsets. The writer's `AlignToFourBytes` pads with zero bytes to the next multiple of 4 (`padding = (4 - pos%4) % 4`).

### 2.1 Lua file metadata (cursor, immediately after header)
`luaFileCount` x 8 bytes: `{ u32 dataOffset (backfilled at end, absolute), u32 length }`. Length = bytecode length if `CompiledLuaBytecode` has an entry keyed by the source text, else UTF-8 byte count of source. Reader: `struct LuaFile { union{u32 luaCodeOffset; const char* luaCode}; u32 length; }` (R:lua.h:12).
List order: unique `exporter.LuaFile` in exporter order, then `sceneLuaFile` if not present, then trigger-box lua files (uniqueness via `List.Contains`, i.e. Unity object equality).

### 2.2 GameObjects (cursor) - 92 bytes each, `gameObjectCount`
Reader: `static_assert(sizeof(GameObject) == 92`.

| off | size | field | writer |
|---|---|---|---|
| 0 | 4 | meshOffset (u32; union with `Tri*`) | placeholder, backfilled at end with the absolute offset of this object's tri array, or **0 if streamed** |
| 4 | 12 | position i32 x3 | `ConvertWorldToFixed12(pos.x / gte)`, `(-pos.y / gte)`, `(pos.z / gte)`; pos = `transform.localToWorldMatrix.GetPosition()` |
| 16 | 36 | rotation i32[9], row-major `[r][c]` | `ConvertRotationToPSXMatrix(transform.rotation)`; each value is an int16-range fp12 stored as `int32` (`writer.Write((int)rot[r, c])`) |
| 52 | 2 | polyCount u16 | `streamed ? 0 : exporter.Mesh.Triangles.Count` |
| 54 | 2 | luaFileIndex i16 | index into lua list or -1 |
| 56 | 4 | flags u32 | bit0 isActive; bit4 (0x10) skinned proxy; bit16 (0x10000) dynamicLit (only if hasLights); bit17 (0x20000) dynamicLitSmooth (only if hasLights) |
| 60 | 2 | interactableIndex u16 | index into interactable array, 0xFFFF none |
| 62 | 2 | uvOffset | 0 ("legacy healthIndex") |
| 64 | 4 | eventMask | 0 ("runtime-only, must be zero") |
| 68 | 24 | AABB i32 x6 | minX,minY,minZ,maxX,maxY,maxZ in PS1 space (Y negated, min/max swapped), world space from 8 transformed corners of `mesh.bounds`; all-zero if no MeshFilter mesh |

Reader flag semantics: R:gameobject.hh `kActive=0x01, kSkinned=0x10, DYNAMIC_LIT_BIT=0x10000, DYNAMIC_LIT_SMOOTH_BIT=0x20000`.

### 2.3 Colliders (cursor) - 32 bytes each, `colliderCount`
Only objects with `CollisionType == Dynamic` and a mesh. `{ i32 AABB x6 (of renderMesh.bounds, same transform as 2.2), u8 collisionType=1, u8 layerMask=0xFF, u16 gameObjectIndex, u32 0 }`. Reader: `SPLASHPACKCollider`, `static_assert == 32`.

### 2.4 Trigger boxes (cursor) - 32 bytes each, `triggerBoxCount`
`{ i32 minX = fp12(wMin.x/gte), minY = fp12(-wMax.y/gte), minZ = fp12(wMin.z/gte), maxX = fp12(wMax.x/gte), maxY = fp12(-wMin.y/gte), maxZ = fp12(wMax.z/gte); i16 luaFileIndex (-1 none); u16 0; u32 0 }`, bounds = `PSXTriggerBox.GetWorldBounds()` (AABB of the 8 rotated corners of a box `size` centred on `transform.position`). Reader: `SPLASHPACKTriggerBox`, 32 bytes.

### 2.5 BVH (cursor) - `AlignToFourBytes` first (W:522-523)
No counts in the section (they live in the header). `bvhNodeCount` x 32 B nodes, then `bvhTriangleRefCount` x 4 B refs.
Node: `{ i32 minX, minY, minZ, maxX, maxY, maxZ (same PS1-space convention, Y negated + swapped); u16 left (0xFFFF none); u16 right (0xFFFF none); u16 firstTriangle (0 if not a leaf); u16 triangleCount }`. Ref: `{ u16 objectIndex, u16 triangleIndex }`.
Reader: `BVHNode` 32 B, `TriangleRef` 4 B; only read `if (header->bvhNodeCount > 0)`. Reader does not align, relying on all previous record sizes being multiples of 4 (they are).
Builder facts needed for determinism (W:BVH.cs): max 64 tris/leaf, max depth 16, min 8 tris to split, 8-bin SAH on centroids, traversal/intersect cost 1.0; nodes numbered by BFS (queue), left before right; each leaf's refs are sorted with `List<T>.Sort` by `objectIndex` only (introsort, **unstable**) and appended in node order. Triangle source: `mesh.triangles` in index order of every **active** exporter with a mesh; `triangleIndex = i/3`. Vertices transformed with `localToWorldMatrix.MultiplyPoint3x4`. Triangle indices are indices into `mesh.triangles` (all submeshes concatenated), which equals the Tri order in the pack because `PSXMesh` iterates submeshes in order (but see sec. 7, item 9). This exporter builds the BVH over its exported Tris instead (in Tri order, corners in authored order for a Tri that was not cut), since cutting repeating textures breaks the one-to-one mapping.

### 2.6 Interactables (cursor) - `AlignToFourBytes` first; 32 bytes each (28 before v27)
`{ i32 radiusSquared = fp12(radius^2 / (gte*gte)); u8 interactButton; u8 flags (bit0 repeatable, bit1 showPrompt, bit2 requireLineOfSight); u16 cooldownFrames; u16 currentCooldown=0; u16 gameObjectIndex (0xFFFF if not found); char promptCanvasName[16]; i16 facingCosine = round(cos(facingAngle) * 4096); u16 reserved = 0 }`. Name: bytes `(byte)canvasName[i]` for i < min(len,15), rest zero. `facingCosine` (v27) is the cosine of the widest angle between the player's forward direction and the object that still passes requireLineOfSight; 0 is 90 degrees, and the reader fills 0 into records from older packs. Reader `Interactable`, `static_assert == 32`, `kInteractableSizeV26 = 28`.

### 2.7 Agents (cursor) - 28 bytes each, then packed waypoints
Pass 1: `agentCount` x `SPLASHPACKAgentV2` (28 B):
`+0 u16 gameObjectIndex; +2 u8 flags (0x01 start enabled, 0x02 vision (HasVision && range>0), 0x04 hearing, 0x08 patrol(waypointCount>0)); +3 u8 waypointCount (<=8); +4 u16 moveSpeed fp12 = Clamp(RoundToInt(MoveSpeed/30/gte*4096),0,65535); +6 u16 stopDistance = StopDistance/gte*4096; +8 u16 visionRange = VisionRange/gte*4096; +10 i16 visionCosAngle = Clamp(RoundToInt(cos(fov/2)*4096),-4096,4096); +12 u16 hearingRange; +14 u16 alertTimeout = Min(frames,65535); +16 u8 stateAnimClip[8] (0xFF none); +24 u8 visionRegionDepth (<=8); +25 u8 reserved[3]`.
Pass 2: for each agent in order, `Min(count,8)` waypoints x 3 x i32: `RoundToInt(worldPos.x / gte * 4096f)`, `.y`, `.z`. Unity 2.4 writes `.y` **not negated**; this exporter negates it like every other position (M2), because the engine keeps waypoints in its Y-down frame. Reader: `SPLASHPACKAgentV2` 28 B; waypoints `totalWaypoints * 3 * sizeof(int32_t)`, right after the records with no alignment (the records follow the interactables directly).

This exporter (the `agent` component, active objects only, in canonical object order): an agent object is always a game object, a mesh-less one shipping with no triangles like a script-only object (the engine's actor id is `gameObjectIndex + 1`, so 0xFFFF would wrap to the player). `stopDistance`, `visionRange`, `hearingRange` = `Clamp(RoundToInt(x / gte * 4096), 0, 65535)`; `fov` is clamped to 0..360 before the cosine; `stateAnimClip[s]` = the state's index into the object's own skin clips (the engine's `SkinAnimSet`, M1), 0xFF for none, for an object without a skin or for an index past its clips; `waypointCount` = the waypoints written. Warned at export: more than 8 waypoints, a field of view outside (0, 360], state clips without a skin or past its clips, a state the agent stays in (Idle, Patrol, Seek, Flee, Wander) on a skin clip that plays once (`loop` false: it stops on its last frame), and agents in a scene with no nav regions (psxsplash `tickAgents` returns without them). Agent objects never stream.

### 2.8 Nav regions (only if navRegionCount>0) - `AlignToFourBytes` first (W:`if (RegionCount > 0)`). Reader aligns itself too.
Header 8 B: `{ u16 regionCount, u16 portalCount, u16 startRegion, u16 0 }`.
Region 84 B (`GetBinarySize() => 8 + regions*84 + portals*20`):
`i32 vertsX[8] = fp12(vertsXZ[v].x/gte) (0 beyond vertCount); i32 vertsZ[8] = fp12(vertsXZ[v].y/gte); i32 planeA = fp12(-planeA); i32 planeB = fp12(-planeB); i32 planeD = fp12(-planeD/gte); u16 portalStart; u8 portalCount; u8 vertCount; u8 surfaceType (0 Flat <3deg, 1 Ramp <25deg, 2 Stairs); u8 roomIndex; u8 flags (bit0 platform); u8 walkoffEdgeMask`.
Portal 20 B: `i32 ax = fp12(a.x/gte), az = fp12(a.y/gte), bx, bz; u16 neighborRegion; i16 heightDelta = ConvertToFixed12(heightDelta/gte)`.
`boundaryEdgeMask` exists in `NavRegionExport` but is **not** written. Reader: `NavRegion` 84 B, `NavPortal` 20 B, `NavDataHeader` 8 B (R:navregion.hh), `initializeFromData` returns the advanced cursor.
Content is produced by a full DotRecast (Recast port) voxelisation pipeline on geometry of exporters with `CollisionType` Static (downward-facing world triangles `normal.y < 0` are dropped), parameters from `PSXNavigationSettings` or `PSXPlayer`; start region = region whose centroid (+plane Y) is closest to spawn.

Port in `core/navregion.cpp` (C++ recastnavigation, built from the upstream commit the 2.4.0
DotRecast build was synced to): geometry collection, the full Recast pipeline with the same
parameter conversions, region extraction and winding, plane fit, portals, room assignment,
platform flags (`collider.platform`), start region, and the binary layout above. Room assignment
(`AssignRoomsFromPSXRooms`): with Room objects in the scene, each region's `roomIndex` is the
smallest-volume room whose world box, 0.5 m larger per side, contains the region centroid (XZ mean
of its vertices, Y from its plane), else 0xFF; the room indices are the room block's (2.9). Only a
scene without Room objects falls back to numbering connectivity islands. Then
`ApplyWalkoffZones` (the `walkoff` component, active objects only): for each boundary edge j of each
region, the midpoint of its two XZ vertices at the region plane's Y; if any zone's world box (the
AABB of the 8 corners +-size/2 through the object's transform) contains it, inclusive, bit j is set
in `walkoffEdgeMask`. The multi-step float expressions in the builder's own code are evaluated
in double, as Unity's Mono does; DotRecast's internals are not, so float-order differences inside
Recast stay possible on sloped or rotated geometry. Checked against Unity on one flat region only.

### 2.9 Rooms/portals (only if roomCount>0) - `AlignToFourBytes` first
ng (`core/rooms.cpp`, written after nav and before the atlas metadata): the block exists iff the
scene has at least one exported, active Room object (`sceneType` is ignored; psxsplash renders
through rooms whenever roomCount>0 and then skips the BVH for static geometry). Rooms are the
Room objects in scene (flatten) order; a Room's box is the world AABB of the 8 corners of
`offset +- size/2` through the object's transform. A Portal names its two Rooms by object name;
center = object position, normal/right/up = its local Z/X/Y axes, halfW/halfH = width/height * 0.5
times the object's scale along right/up (Unity ignores the scale). The input triangles are the
BVH's: every exported Tri of every pack object, `triangleIndex` = Tri index. Export errors (never
clamped): a portal naming a missing, duplicate-named or identical room; more than 31 rooms
(psxsplash's visited mask only covers rooms < 32); more than 65535 rooms+1, portals, tri refs,
cells or portal refs; more than 255 portal refs in one room; rooms together with world streaming.
Sorts by objectIndex are stable.
Order: `(rooms+1)` x RoomData(36) | `portalCount` x PortalData(40) | `roomTriRefCount` x TriRef(4) | `roomCellCount` x RoomCell(28) | `roomPortalRefCount` x RoomPortalRef(4). Reader reads exactly in this order (cells and portal refs only if their counts >0).
- RoomData: `i32 aabb x6 (PS1 space, Y negated+swapped; room AABB from PSXRoom.GetWorldBounds); u16 firstTriRef; u16 triRefCount; u16 firstCell; u8 cellCount; u8 portalRefCount; u16 firstPortalRef; u16 pad=0`. Last entry is the catch-all room: AABB = fp12(-1000/gte) x3 .. fp12(1000/gte) x3 (note: min Y written as `-1000/gte`, max Y `+1000/gte`).
- PortalData: `u16 roomA; u16 roomB; i32 center x, -y, z (fp12(/gte)); i16 halfW = Clamp(RoundToInt(size.x*0.5/gte*4096),1,32767); i16 halfH likewise; i16 normal x, -y, z (RoundToInt(v*4096) clamp int16); i16 pad; i16 right x,-y,z; i16 up x,-y,z`.
- TriRef: `{u16 objectIndex, u16 triangleIndex}` rooms in order then catch-all. Triangle-to-room: by count of its 3 world vertices inside the room AABB expanded by `ROOM_MARGIN*2 = 1.0` total (0.5 per side), ties -> closest AABB centre to centroid; then portal-boundary triangles are duplicated into the neighbour room; each room list sorted by objectIndex (unstable `List.Sort`); then regrouped into 2x2x2 spatial cells (`CELLS_PER_AXIS=2`) by centroid, each cell's refs sorted by objectIndex again.
- RoomCell: `i32 aabb x6 (tight, PS1 space); u16 firstTriRef (global index into the flat array); u16 triRefCount`.
- RoomPortalRef: `{u16 portalIndex, u16 otherRoom}`.

### 2.10 Atlas metadata then CLUT metadata (cursor) - **content is vestigial for the reader**
Atlas: `atlasCount` x 12 B `{ u32 0 (offset backfilled to 0), u16 width, u16 256, u16 x, u16 y }` (`TextureAtlas.Height = 256`).
CLUT: for each atlas, for each contained texture with palette: 12 B `{ u32 0, u16 clutPackingX, u16 clutPackingY, u16 paletteCount, u16 0 }`.
Reader (R:splashpack.cpp): `cursor += sizeof(SPLASHPACKTextureAtlas)` / `sizeof(SPLASHPACKClut)` (12 each); contents are skipped. The real data is in the `.vram` file (sec. 4).

### 2.11 Lua data (by offset). Per lua file: `AlignToFourBytes`, offset recorded, then raw bytecode (or UTF-8 source). `luaOffset` backfilled at the very end (`BackfillOffsets(..., "lua")`).

`splashpack-cli export` writes the source by default. With `--lua-bytecode` it writes bytecode from
`core/luacompile.cpp`, which builds psxlua's compiler for the host and gives the same bytes as
`luac_psx` (Lua 5.2, 32-bit integer numbers, debug info stripped). The engine loads both with
`luaL_loadbuffer`, which treats data starting with `\x1bLua` as bytecode. An engine built with
`NOPARSER=1` has no parser and only accepts bytecode.

### 2.12 Mesh data (by per-object meshOffset). Per exporter in index order: `AlignToFourBytes`, offset recorded, then `Triangles.Count` x 52-byte `Tri`. Streamed objects emit nothing and `meshOffset = 0`. Tri record:

| off | size | field |
|---|---|---|
| 0 | 6 x3 | v0, v1, v2 positions i16 x,y,z |
| 18 | 6 | normal of v0 only (i16 x,y,z) |
| 24 | 4 x3 | colours r,g,b,0 for v0,v1,v2 |
| 36 | 2 x3 | uv (u,v) for v0,v1,v2 |
| 42 | 2 | padding (0) |
| 44 | 2 | tpage (TPageAttr info) or 0xFFFF if untextured |
| 46 | 2 | clutX (`ClutPackingX`, units of 16 px), 0 if untextured |
| 48 | 2 | clutY, 0 if untextured |
| 50 | 2 | flags: bit0 = `exporter.UVOffsetMaterial == tri.TextureIndex`; bit1 = paletted cutout texture whose depth has a fog mask CLUT |

Untextured: UVs all zero, `tpage = 0xFFFF`, clut = 0. Reader: `Tri`, `static_assert(sizeof(Tri) == 52`, `UNTEXTURED_TPAGE = 0xFFFF`, `ANIM_FLAG = 0x1`, `CUTOUT_FLAG = 0x2`. Same record is used in `.geo`.

### 2.13 Object name table (header nameTableOffset). `AlignToFourBytes`, then per exporter `{u8 len, bytes(UTF-8 of name truncated to 24 chars), 0}`. Reader walks `nameLen + 1` bytes per object.

### 2.14 Audio clip table (header audioTableOffset; only if audioClipCount>0). `AlignToFourBytes`, then `audioClipCount` x 16 B `{ u32 dataOffset=0, u32 sizeBytes, u16 sampleRate, u8 loop, u8 nameLen, u32 nameOffset }`, then all names (ASCII, NUL-terminated, name truncated to 255 chars) with nameOffset backfilled. Reader reads the same 16 B (R:splashpack.cpp "dataOff, size, rate, loop, nameLen, nameOff"); ADPCM data comes from `.spu`.

### 2.15 Cutscenes (header cutsceneTableOffset; only if cutsceneCount>0) - `PSXCutsceneExporter.ExportCutscenes`
Caps: 16 cutscenes, 8 tracks, 64 keyframes/track, 64 audio events, 16 skin-anim events, names 24 chars (writer consts, equal to R:cutscene.hh).
`AlignToFourBytes`; table of `n` x 12 B `{u32 dataOffset, u8 nameLen, u8[3] pad, u32 nameOffset}`; then per cutscene (each starting `AlignToFourBytes`):
- Header 20 B: `{ u16 durationFrames; u8 trackCount; u8 audioEventCount; u32 tracksOffset; u32 audioEventsOffset (0 if none); u8 skinAnimEventCount; u8[3] 0; u32 skinAnimEventsOffset (0 if none) }`
- (align4) tracks, 12 B each: `{ u8 trackType; u8 keyframeCount; u8 targetNameLen; u8 lightIndex (light tracks) or 0; u32 targetNameOffset (0 if no name); u32 keyframesOffset }`
- per track (align4) keyframes, 8 B each, sorted by frame (`List.Sort`, unstable): `{ u16 (interp<<13 | frame & 0x1FFF); i16 v[3] }`. Value encodings by `PSXTrackType`:

| id | type | v[0..2] |
|---|---|---|
| 0,2 | CameraPosition, ObjectPosition | `ConvertCoordinateToPSX(x,gte)`, `(-y)`, `(z)` |
| 1,3 | CameraRotation, ObjectRotation | `DegreesToAngleRaw(-x)`, `(y)`, `(-z)`; raw = `RoundToInt(deg*1024/180)` clamp int16 |
| 4,5,6,11 | ObjectActive, UICanvasVisible, UIElementVisible, RumbleSmall | `x>0.5?1:0`, 0, 0 |
| 7 | UIProgress | `Clamp(RoundToInt(x),0,100)`, 0, 0 |
| 8 | UIPosition | `RoundToInt(x)`, `RoundToInt(y)`, 0 (raw int16) |
| 9 | UIColor | `Clamp(RoundToInt(c),0,255)` x3 |
| 10 | CameraH | `Clamp(RoundToInt(x),1,1024)`, 0, 0 |
| 12 | RumbleLarge | `Clamp(RoundToInt(x),0,255)`, 0, 0 |
| 13 | ObjectUVOffset | `Clamp(RoundToInt(x),0,255)`, same for y, 0 |
| 14 | LightPosition | `ConvertCoordinateToPSX(x,gte)`, `(-y)`, `(z)` |
| 15 | LightColor | `ColorUnityToPSX(v)` x3 (0..255) |
| 16 | LightIntensity | `Clamp(RoundToInt(x*4096),0,32767)`, 0, 0 |
| 17 | LightRadius | `Max(0, ConvertCoordinateToPSX(x,gte))`, 0, 0 |
| 18 | LightEnabled | `x>0.5?1:0`, 0, 0 |

InterpMode ids: Linear 0, Step 1, EaseIn 2, EaseOut 3, EaseInOut 4.
- target-name strings (NUL-terminated UTF-8, <=24 chars) per track with non-empty name. Camera/vibration/light tracks have no name; UI element tracks use `"canvas/element"`, UI tracks `canvas`, others `ObjectName`.
- (align4) audio events, 8 B each, sorted by frame: `{u16 frame; u8 clipIndex (index in scene audio sources, first matching ClipName); u8 volume (0..128); u8 pan (0..127); u8[3] 0}`
- (align4) skin anim events, 8 B each, sorted by frame: `{u16 frame; u8 skinMeshIndex (index in skinnedExporters array); u8 clipIndex; u8 loop; u8[3] 0}`
- cutscene name (UTF-8, NUL-terminated, <=24), then all offsets are backfilled.
Reader: R:splashpack.cpp cutscene loop; `CutsceneKeyframe` 8 B, `CutsceneAudioEvent` 8 B, `CutsceneSkinAnimEvent` 8 B (static_asserts in R:cutscene.hh).

This exporter (`core/splashpack.cpp`, `writeSequences`) writes track types 0-18 with the same
encodings, except: keys and audio events are sorted stably, so keys on the same frame keep their
file order; `uiColor` and `lightColor` take 0..1; `uiPosition` keys get the same anchor rounding
correction as the element (sec. 2.18); a position or radius key that clamps to int16 is a warning,
and so is a light intensity key above 8. A light track names a runtime point light by object name
and the exporter writes its index in the light table (sec. 2.21) into the `lightIndex` byte. A track
target that is not an exported object, canvas, `canvas/element` or runtime light (a duplicate light
name too), an unknown audio clip, and
counts over the reader's caps fail the export: the reader would skip or drop them. The track
names are written before the audio events (the reader follows offsets). Skin events name an object
and one of its clips; the exporter writes that object's index in the skin table (sec. 2.17, not the
object index) and the clip's position in its skin component's `clips`, and fails the export on an
object without a skin or an unknown clip (2.4.0 skipped the event with a warning). A cutscene name
over 24 characters fails the export (2.4.0 cut it, and `Cutscene.Play` with the full name then
found nothing).

### 2.16 Animations (header animationTableOffset; only if animationCount>0) - `PSXAnimationExporter.ExportAnimations`
Same table entry (12 B) and track/keyframe/skin-event layouts as cutscenes. Differences: max 16 animations; camera position/rotation tracks are dropped (warning); no audio events; per-animation header is **16 B**: `{ u16 durationFrames; u8 trackCount; u8 0; u32 tracksOffset; u8 skinAnimEventCount; u8[3] 0; u32 skinAnimEventsOffset }`; the keyframe switch has **no CameraH case** (see sec. 7, M3). Order inside: header, tracks, keyframes, names, (align4) skin events, anim name.

This exporter (`core/splashpack.cpp`, `writeSequences`, shared with cutscenes) writes the same
layout with the cutscene key encodings and checks. Camera tracks, `cameraH` included, fail the
export instead of being dropped: psxsplash's animation player has no camera case and would ignore
them.

### 2.17 Skinned meshes (header skinTableOffset; only if skinnedMeshCount>0) - `PSXSkinnedMeshExporter.ExportSkinData`
`AlignToFourBytes`; table of `min(n,16)` x 12 B `{u32 dataOffset, u8 nameLen, u8[3] 0, u32 nameOffset}`; per mesh (each `AlignToFourBytes`):
`{ u16 gameObjectIndex; u8 boneCount (<=64); u8 clipCount (<=16) }`, then `boneIndices[polyCount*3]` (u8 per triangle vertex; `boneIndex0` of the vertex, clamped to 63; same winding fix as `PSXMesh`), `AlignToFourBytes`, then per clip: `u8 nameLen; name bytes; 0x00; u8 flags (bit0 loop); u8 fps; [pad 1 byte if position odd]; u16 frameCount; frameCount*boneCount x 24 B BakedBoneMatrix`. Then the object name (NUL-terminated) and table-entry backfill.
BakedBoneMatrix (24 B): `i16 r00,r01,r02,r10,r11,r12,r20,r21,r22 (4.12); i16 tx,ty,tz`. Reader: `animSet.boneIndices = skinPtr; skinPtr += polyCount * 3;` with `polyCount = setup.objects[gameObjectIndex]->polyCount`; aligns to 4; per clip aligns to 2 before frameCount. Frame count = `CeilToInt(clip.length * fps) + 1`, sample time = `frame/(frameCount-1) * clip.length`. Matrix = `objectInverse * bone.localToWorldMatrix * bindPose` (computed once before sampling), Y-flipped as `R_psx = F R F` with F=diag(1,-1,1): negate m01, m10, m12, m21; translation `tx = m03*uniformScale/gte*4096`, `ty = -m13*...`, `tz = m23*...` (uniformScale = `transform.lossyScale.x`).

This exporter writes the same layout with these differences: a looping clip has `round(length*fps)` frames over `[0, length)` (no repeated end frame); the bone matrix is `S M S^-1` with S the object's lossy scale, so non-uniform scale is exact (2.4.0 used `lossyScale.x` for translation only); clip names over 24 characters fail the export instead of being cut, since Lua could not play them.

From v26 each skin block carries a tail between the last clip and the object name: `[pad 1 byte if position odd]`, then `boneCount x {i16 x, y, z}`, each joint's bind-pose position in the vertices' space (lossy scale applied, Y negated, `/gte*4096`, the same conversion as a vertex), then `boneCount x {u8 nameLen; joint name (24 UTF-16 units max); 0x00}`. Older engines reach skin blocks by offset and never read it. psxsplash uses it for `SkinnedAnim.GetBone`.

### 2.18 UI (header uiTableOffset; if canvases or fonts exist) - `AlignToFourBytes` first
Order: font descriptors, canvas descriptors, then for each canvas with elements (each `AlignToFourBytes`) its element array followed by that canvas's strings, then all canvas-name strings.
- Font descriptor 112 B: `{u8 glyphW, u8 glyphH, u16 vramX, u16 vramY, u16 textureH, u32 dataOffset (=0), u32 dataSize, u8 advanceWidths[96]}`. Reader `UISystem::loadFromSplashpack`: reads the same offsets (0,1,2,4,6,8,12,16); font count capped to `UI_MAX_FONTS-1 = 3`. Exporter supports only 2 custom fonts (`fontPageStarts = {0,256}`), `VramX = 960`, `VramY = 0` or `256`.
- Canvas descriptor 12 B: `{u32 dataOffset (0 if no elements), u8 nameLen, u8 sortOrder, u8 elementCount, u8 flags(bit0 startVisible), u32 nameOffset}`.
- Element 48 B (writer comment at one place says "56", the code and reader are 48):

| off | size | field |
|---|---|---|
| 0 | 1 | type (Image 0, Box 1, Text 2, Progress 3, Line 4) |
| 1 | 1 | flags (bit0 startVisible) |
| 2 | 1 | nameLen |
| 3 | 1 | pad |
| 4 | 4 | nameOffset |
| 8 | 8 | x,y,w,h i16 |
| 16 | 4 | anchorMinX, anchorMinY, anchorMaxX, anchorMaxY (u8, 255 = 1.0) |
| 20 | 4 | colorR,G,B, pad |
| 24 | 16 | type data (below) |
| 40 | 4 | textOffset |
| 44 | 4 | pad |

Type data: Image `{u8 texpageX, texpageY; u16 clutX, clutY; u8 u0,v0,u1,v1; u8 bitDepthIndex (0=4bpp,1=8bpp,2=16bpp); u8 cellW, cellH, sheetCols, baseU, baseV}`; Progress `{bgR,bgG,bgB,value; 12 zero}`; Text `{fontIndex (0 system, 1+ custom); 15 zero}`; Line `{i16 x,y,w,h again; 8 zero}`; Box all zero. Reader reads the same bytes (R:uisystem.cpp).
Strings: for each element in order: text (only Text type and only if non-empty) then name (always, even empty); each NUL-terminated UTF-8; `nameOffset`/`textOffset` backfilled. The element's layout is baked by `PSXUILayout.Bake` (sec. 3).

This exporter (`core/splashpack.cpp`, `writeUi`) writes the same records with these differences:
- Fonts are stacked in the x = 960 column in scene order, first fit in rows 0-255 then 256-463
  (the system font is at 960,464), each inside one texture page, so 3 fonts fit where 2.4.0 placed
  at most 2. Glyph cells can be any width, not only 4/8/16/32 (the reader only needs
  `256 / glyphW` per row). Sheets come from `core/font.cpp`: TrueType glyphs are rendered at the
  subpixel offset that leaves the fewest half-covered pixels, then thresholded at 96/255.
- Anchors are still bytes read as `byte * res >> 8`, so 1.0 lands 2 px short at 320 wide. The
  rounding error of each anchor at the project resolution is added to x/w (y/h), and the element
  lands on the pixel the scene asks for: a bar anchored 0..1 at 320 wide resolves to 320 px, where
  2.4.0's bake gives 318 (tests/uicheck.py, which fails with the correction removed).
- Line endpoints are written as i16 in both the layout and the type data.
- More than 24 canvases, 256 elements, 255 elements in one canvas or 3 fonts fails the export; the
  loader would drop the rest.
- A sheet-backed image (scene `sheet`/`cell`, psxsplash PSXUISprite) shares its sprite sheet's
  texture, packed once for the sprite table (2.19): texpage and CLUT are the sheet's, baseU/baseV
  are the sheet record's u0/v0, u0 = baseU + (cell % cols) * cellW, v0 = baseV + (cell / cols) *
  cellH, u1/v1 = u0 + cellW - 1 / v0 + cellH - 1, then cellW, cellH, cols, baseU, baseV. The loader
  recovers the cell from u0/v0 and UI.SetFrame/GetFrame step through the grid; a plain image leaves
  those five bytes 0 (not sheet-backed). A loader pack (no sprite table) gets its own copy of the
  sheet's texture, with the grid based at that copy.

### 2.19 Sprite sheets + sprite anims (header spriteTableOffset; only if >=1 sheet) - `AlignToFourBytes` first
`spriteSheetCount` x 20 B `{u32 nameOffset; u8 texpageX, texpageY, u0, v0; u16 clutX, clutY; u8 cellW, cellH, cols, rows, bitDepthIndex, pad0; u16 pad1}` then `spriteAnimCount` x 12 B `{u32 nameOffset; u8 sheet, firstFrame, frameCount, frameDuration, loop, pad0; u16 pad1}`, then NUL-terminated names (<=24 chars) of all sheets then all anims. Reader: `SPLASHPACKSpriteSheet` 20 B / `SPLASHPACKSpriteAnim` 12 B (R:spritesystem.cpp), limits 16 sheets / 64 anims (asserts).

### 2.20 Tilemap (header tilemapTableOffset; only if present) - `AlignToFourBytes` first
`{u16 width,u16 height,u8 tileW,u8 tileH,u8 tilesetSheet,u8 0,u16 objectCount,u16 0,u32 cellsOffset,u32 objectsOffset}` (20 B), then `width*height*2` bytes of cells `{u8 tile (0xFF empty, else clamp 0..254), u8 flags (bit0 walkable)}` row-major, then `objectCount` x 6 B `{u8 kind,u8 id,u16 tileX,u16 tileY}` (reading order; auto ids per kind start at 1). `objectsOffset` is 0 if there are no objects. Max 128 objects. Reader: R:tilesystem.cpp (20/2/6-byte asserts).

### 2.21 Point lights (v24; header lightTableOffset) - `AlignToFourBytes` first
`{u16 count, u16 0}` then `count` x 28 B `{i32 x = fp12(pos.x/gte), y = fp12(-pos.y/gte), z; i32 radius = fp12(range/gte); u16 intensity = Clamp(RoundToInt(intensity*4096),0,65535); u8 r,g,b = ColorUnityToPSX; u8 flags (bit0 enabled); u16 pad; u32 nameOffset}`, then names (<=24 chars). Reader `SPLASHPACKPointLight` 28 B, `count` capped to `MAX_SCENE_LIGHTS = 16`, records at `table + 4`.

### 2.22 Memory card (header memcardTableOffset; only if enabled) - `AlignToFourBytes` first; fixed 464 B
`char region[2]; char product[10]; char title[32] (ASCII, `& 0x7F`, zero padded); u8 frameCount (1..3); u8 0; u16 0; u16 clut[16] (BGR555, from frame 0); u8 pixels[3][128] (16x16 4bpp, top row first, low nibble = left pixel)`. Icons are quantised with `PSXTexture2D.CreateFromTexture2D(tex, TEX_4BIT)`. Reader `SPLASHPACKMemcard`, 464 B.

Reader (R:splashpack.cpp at psxsplash origin/main `f671202`, behind `PSXSPLASH_FEATURE_MEMCARD`): only when `version >= 21 && memcardTableOffset != 0`. Field offsets: region +0x00, product +0x02, title +0x0C, iconFrameCount +0x2C, pads +0x2D/+0x2E, clut +0x30, iconPixels +0x50. It copies region (2) and product (10) and title (32), NUL-terminating each, clamps the frame count to 1..3 and copies the CLUT and all 3x128 pixel bytes into `MemoryCardConfig`, then `MemoryCardManager::setConfig`. With offset 0 nothing is set, so the previous scene's config (or the built-in BA / SLUS-00000 / "PSXSPLASH" / blank icon) stays. Use (R:memorycardmanager.cpp): `buildFilename` = region (up to its first NUL) + product (up to its first NUL) + Lua key, cut at 20 characters; `save` passes the title (or `MemCard.Save`'s 4th argument) to psyqo, whose default encoder is ASCII-only (each character to its full-width Shift-JIS form, 64 bytes; `" ' - \ ~` and non-ASCII become `?`), and the icon as `psyqo::MemoryCardFileSystem::Icon`.

splashedit-ng (`core/splashpack.cpp`, `memCardTable`; scene `settings.memCard`, docs/scene-format.md) writes the same record after the point light table and before the stream table, with two differences. The icon frames are quantised together (stacked into one 16x(16n) image) so every frame is drawn with the one CLUT the record holds, where 2.4 took frame 0's palette and quantised the others on their own. Palette entry 0 is the transparent word 0 only when some texel has alpha below 0.5. Values the record cannot hold (region not 2 characters, product empty or over 10, title over 32 or non-ASCII, over 3 frames, an icon not 16x16) fail the export instead of being cut or masked; `checkMemCard` (core/splashpack.hh) gives the same messages to the editor. Checked by tests/memcardcheck.py.

### 2.23 Stream table (header streamTableOffset @124; only if streaming plan writable) - `AlignToFourBytes` first
`{u16 regionCount, u16 objectRefCount, u16 slotCount, u16 0, u32 slotBytes, i32 loadRadius, i32 unloadRadius}` (20 B), `regionCount` x 32 B `{i32 minX, minZ, maxX, maxZ (fp12); u32 firstSector; u32 byteSize; u16 firstObjectRef; u16 objectRefCount; u32 0}`, then `objectRefCount` x 8 B `{u16 objectIndex; u16 polyCount; u32 offsetInRegionBlob}` (offset += count*52). `.geo` = per region the concatenated `Tri` arrays of its objects, zero padded to a 2048-byte multiple (`ByteSize`); `FirstSector` = cumulative `ByteSize/2048`. Reader: `SPLASHPACKStreamTable/Region/ObjectRef` (R:streamplanner.hh) 20/32/8. Planner (W:PSXWorldStreaming.cs): `BytesPerTri 52`, `SectorSize 2048`, uniform XZ grid, regions in `SortedDictionary<long,...>` key `cz*2^31+cx` order, auto cell size searched over k=1..96, slots <= 32, default draw distance `OrderingTableSize()` = 8192, fog => `min(draw, 20000/fogDensity)`, `load = draw*5/4`, `unload = load*3/2`. Eligible = no lua, not skinned, no interactable/agent, not Dynamic collider, not a cutscene/animation track target name, name not quoted in any Lua source text.

splashedit-ng (`core/worldstream.cpp`, scene setting `streamWorld`) writes the same table with two differences. An object wider than a region is cut by triangle centre into one part per region: the first part keeps the object's index and the others are appended after the scene's objects as copies (same name, transform and flags, their own AABB), so no object index moves; small objects go whole to the region holding their centre. With streaming on and no fog, the automatic draw distance (ordering table) is `min(scene diagonal, 8192)` rather than the diagonal, and the bump allocator counts streamed triangles as the most within the draw distance of one camera position instead of all of them.

### 2.24 Final backfills (order): pixelDataOffset=0, atlas/CLUT/audio/font data offsets=0, memcard offset, stream offset, then lua offsets, then mesh offsets. The pack ends at the last section written (no trailing pad guaranteed).

---

## 3. Fixed-point formats and coordinate conversion

`gte` = `PSXSceneExporter.GTEScaling` (default `100.0f`; world units per 1.0 GTE unit). Unity is Y-up, left-handed; PS1 data is Y-down: **every Y is negated** (positions, normals, AABBs with min/max swapped, rotation matrix, plane, portal vectors), except agent waypoints (sec. 7).

All of W:Utils.cs `PSXTrig`:

```csharp
public static short ConvertCoordinateToPSX(float value, float GTEScaling = 1.0f)
{
    int fixedValue = Mathf.RoundToInt((value / GTEScaling) * 4096.0f);
    return (short)Mathf.Clamp(fixedValue, -32768, 32767);
}
public static short ConvertToFixed12(float value)
{
    int fixedValue = Mathf.RoundToInt(value * 4096.0f);
    return (short)Mathf.Clamp(fixedValue, -32768, 32767);
}
public static int ConvertWorldToFixed12(float value)
{
    long fixedValue = (long)Mathf.RoundToInt(value * 4096.0f);
    return (int)Mathf.Clamp(fixedValue, int.MinValue, int.MaxValue);
}
```

- 4.12 int16 (`ConvertCoordinateToPSX`, `ConvertToFixed12`): range about +-8.0 GTE units = +-800 Unity units at gte=100. Used for: mesh vertices, normals (called with default gte=1), player position/rotation/height/radius, cutscene/animation positions, light-track values, bone translation.
- 20.12 int32 (`ConvertWorldToFixed12(v/gte)`): object positions, AABBs, BVH, rooms, portals, nav, lights, trigger boxes, stream bounds.
- All arithmetic is `float` (32-bit). `Mathf.RoundToInt` is Unity's rounding (documented as round-half-to-even; I did not verify that against engine source in this session). Float-to-int overflow is not guarded for `ConvertWorldToFixed12` (the `Clamp` is applied after the `int` conversion).
- Player position: `writer.Write(PSXTrig.ConvertCoordinateToPSX(scene.playerPos.x, gte));` and `(-scene.playerPos.y, gte)`, `(scene.playerPos.z, gte)` -> three int16.
- Player rotation (W:244): `writer.Write(PSXTrig.ConvertToFixed12(scene.playerRot.eulerAngles.x * Mathf.Deg2Rad));` x/y/z. Euler angles are Unity's 0..360 degree range, i.e. 0..6.28 rad, so the fp12 value tops out near 25736 and never clamps; a rotation of -10 degrees is stored as 350 degrees.
- Movement (W:~270-290), `const float fps = 30f`:
  `movePerFrame = moveSpeed / fps / gte` -> `Clamp(RoundToInt(movePerFrame * 4096f), 0, 65535)` (same for sprint);
  `jumpVel = Mathf.Sqrt(2f * gravity * jumpHeight) / gte` -> `*4096`, u16; `gravPsx = gravity / gte` -> `*4096`, u16; radius via `ConvertCoordinateToPSX`.
- Rotation matrix (W:Utils.cs ConvertRotationToPSXMatrix): standard quaternion->matrix then
  ```csharp
  float[,] fixedMatrix = new float[3, 3]
  { { m00, -m01, m02 }, { -m10, m11, -m12 }, { m20, -m21, m22 } };
  ```
  each element `ConvertToFixed12`, stored as int32. Matrix element `[r][c]`, rows first.
- Mesh vertex (W:PSXMesh.cs:171,253-264): position first scaled by object lossy scale only (no rotation/translation):
  `Vector3 v = Vector3.Scale(vertices[index], transform.lossyScale);`
  `vx = ConvertCoordinateToPSX(vertex.x, GTEScaling), vy = ConvertCoordinateToPSX(-vertex.y, GTEScaling), vz = ConvertCoordinateToPSX(vertex.z, GTEScaling)`
  normal (raw `mesh.normals[index]`, object space, NOT smoothed, NOT scaled; only v0's is written): `nx = ConvertCoordinateToPSX(normal.x)`, `ny = ConvertCoordinateToPSX(-normal.y)`, `nz = ConvertCoordinateToPSX(normal.z)`.
  UV: `u = (byte)Mathf.Clamp(uv.x * (width - 1), 0, 255)`, `v = (byte)Mathf.Clamp((1.0f - uv.y) * (height - 1), 0, 255)` where width/height = the source texture size; no wrapping, UV outside [0,1] clamps; `(byte)` truncates. This exporter instead cuts a textured triangle whose UVs span more than one copy of the texture along the integer U and V lines, shifts each piece into [0,1] (position, normal, colour and UV interpolated; a skinned piece's corner takes the bone of the nearest authored corner), and only then converts, so repeating textures draw as authored and the Tri count grows.
  Colour: `Utils.ColorUnityToPSX(v) => (byte)(Mathf.Clamp(v * 255, 0, 255))` (truncation, not rounding).
  Winding: `if (Vector3.Dot(faceNormal, normals[vid0]) < 0) (vid1, vid2) = (vid2, vid1);` (face normal from object-space cross product). The Y negation is NOT compensated by an extra swap; the reader's rasteriser is assumed to expect this.
- Vertex colour: `VertexColorMode` per object: BakedLighting (default; `PSXLightingBaker.ComputeLighting`, sum over all enabled scene lights (directional: `color*intensity*max(0,N.L)`; point: `.../max(d^2,1e-4)`; spot with cone), clamp each channel to 0..0.8), FlatColor (`flatVertexColor`), MeshVertexColors (`mesh.colors` or 0.5 grey). Untextured triangles multiply by material `_BaseColor`/`_Color`. Baked-light input normal = `transform.TransformDirection(smoothNormal).normalized` where smooth normals average `mesh.normals` over vertices with identical position (`Dictionary<Vector3,...>`, exact float equality).
- Angle (cutscene rotation): `raw = degrees * 1024.0f / 180.0f` -> int16 ("pi-units": 1024 = 180 deg).
- Interactable radius: `ConvertWorldToFixed12(radiusSq / (gte * gte))`.
- Nav plane: A,B dimensionless `ConvertWorldToFixed12(-A)`, `(-B)`; D `ConvertWorldToFixed12(-D / gte)`.
- UI layout (W:PSXUILayout.Bake): `scaleX = resolution.x / canvasW`, `scaleY = resolution.y / canvasH` (canvas `rect`, fallback to resolution if <=0). Anchors: `AnchorMinX = Clamp(RoundToInt(anchorMin.x*255),0,255)`, `AnchorMinY = ...((1f - anchorMax.y) * 255f)`, `AnchorMaxX = ...(anchorMax.x*255)`, `AnchorMaxY = ...((1f - anchorMin.y) * 255f)`. If anchorMin==anchorMax (Mathf.Approximately): `px = anchoredPosition.x*scaleX; py = -anchoredPosition.y*scaleY; pw = rect.width*scaleX; ph = rect.height*scaleY; px -= pivot.x*pw; py -= (1f - pivot.y)*ph;` X/Y = RoundToInt, W/H = `Max(1, RoundToInt)`. Else stretched: `X = RoundToInt(offsetMin.x*scaleX)`, `Y = RoundToInt(-offsetMax.y*scaleY)`, `W = RoundToInt(rightOff - leftOff)`, `H = RoundToInt(bottomOff - topOff)`. Line elements store raw `(byte)RoundToInt(Point.x/y)`.
- Scene hash (W:PSXSpriteExporter.HashSceneId): FNV-1a 32 over `(byte)c` of each UTF-16 char (low byte only), offset basis 2166136261, prime 16777619; empty -> 0; result 0 -> 1.
- Fog colour/intensity bytes: `Clamp(RoundToInt(x*255),0,255)`. Tints/UI colours same.

---

## 4. Textures and VRAM

### 4.1 Quantisation (W:PSXTexture2D.cs `CreateFromTexture2D`, W:ImageProcessing.cs)
- Bit depth per object (`PSXObjectExporter.bitDepth`, default 8 bit) or per UI image/sprite sheet. `PSXBPP`: `TEX_4BIT = 4, TEX_8BIT = 8, TEX_16BIT = 15` (note 15). `QuantizedWidth` = width/4 (4bpp), width/2 (8bpp), width (16bpp): width in 16-bit VRAM words.
- Source pixels: `Texture2D.GetPixels()` (bottom row first, floats from the import-processed texture), `GetPixel(x, height-y-1)` for 16bpp.
- 16bpp: `R = (ushort)(pixel.r * 31)` etc. (truncation), no dithering. If `Pack() == 0x0000` and `pixel.a > 0` -> (1,1,1) with bit15 set. Cutout (`cutout:true`, sprites only): alpha < 0.5 -> 0x0000.
- 4/8bpp (`maxColors` = 16/256): if distinct `(r,g,b)` colours <= maxColors the palette is the colours in first-appearance order (bottom-row-first scan), indices exact, no dithering (`ConvertToOutput`). Otherwise: k-means over distinct colours (`k` initial centroids `colors[i*count/k]`, exactly 10 iterations, nearest by squared distance, ties by `OrderBy` stability), then a k-d tree (median split on `List.Sort`, **unstable**) nearest-index search, with Floyd-Steinberg dithering (weights 7/16, 3/16, 5/16, 1/16 at (+1,0),(-1,+1),(0,+1),(+1,+1)) over `pixels` in bottom-up row order, error accumulated in float on the working copy.
- Cutout: palette[0] = black (transparent), opaque colours use `maxColors-1` entries; exact match via dictionary if few enough, else k-means + k-d tree + dithering to opaque pixels only.
- Palette padding quirk (W:PSXTexture2D.cs:205-210): `int targetCount = 4 - (result.Palette.Count % 4); if (targetCount != 0) { ... result.Palette.AddRange(new Vector3[targetCount]); }` -> always appends 1..4 black entries (4 when already a multiple of 4), so a full 16-colour palette becomes 20 entries and a 256-colour one 260.
- Palette to BGR555: `VRAMPixel { R = (ushort)(pixel.r * 31), ... }`; `Pack() = (SemiTransparent ? 1<<15 : 0) | (b << 10) | (g << 5) | r`. Non-transparent entries that pack to 0 become (1,1,1)+bit15. Cutout entry 0 stays 0x0000.
- Pixel data to words: for each row y (source row y, bottom-up) writes to `ImageData[group, Height - y - 1]` (so VRAM row 0 = top of image). 8bpp: `packed = (index2 << 8) | index1` for pixels (2g, 2g+1); 4bpp: `(idx4<<12)|(idx3<<8)|(idx2<<4)|idx1` for pixels (4g..4g+3). The 16-bit word is stored in a `VRAMPixel` via `Unpack(packed)` (round-trips through `Pack()`).
- Editor importer pre-step `Utils.SetTextureImporterFormat` (called inside `CreateFromTexture2D`): max size clamped to 256 (`MaxTextureSize => 256`), readable, npotScale None, uncompressed, no mipmaps, point filter, alphaIsTransparency; reimports the asset.

### 4.2 Packing (W:TexturePacker.cs `VRAMPacker`)
- VRAM is 1024x512 (16-bit words). Reserved: ProhibitedAreas from `PSXData.asset` + framebuffer(s) from `Utils.BufferForResolution(resolution, verticalLayout)` (second buffer always reserved: `_reservedAreas.Add(framebuffers[1])` is unconditional, so `dualBuffering=false` still throws... see risk list) + font column `Rect(960, 0, 64, 512)`.
- Collect textures: all `exporter.Textures` in exporter order, then UI images, then sprite sheets/tileset. Group by bit depth, groups ordered descending by `PSXBPP` value (16bit, 8bit, 4bit). Atlas width: 16bpp 256, 8bpp 128, 4bpp 64 (words); atlas height 256. Within a group, textures sorted `OrderByDescending(QuantizedWidth*Height)` (stable LINQ).
- Dedupe key `(OriginalTexture.GetInstanceID(), BitDepth, Cutout)`; duplicates inherit packing/texpage/CLUT coordinates after placement and obj.Textures/Tri.TextureIndex are rewritten to the deduped list.
- In-atlas placement: first-fit scan y=0..(256-h), x=0..(atlasWidth-QW), `byte` loop counters, `Rect.Overlaps` (touching is allowed). `PackingX/PackingY` are `byte`: **PackingX is in 16-bit words**, PackingY in rows. New atlas of same depth if it does not fit.
- Atlas placement in VRAM (`ArrangeAtlasesInVRAM`): order 16bpp atlases, then 8bpp, then 4bpp; for y in {0, 256}, x in 0,64,128,... first position where the 256-row rect does not overlap other atlases, reserved areas or CLUTs. An atlas is treated as "unplaced" while `PositionX==0 && PositionY==0`.
- Texpage: `TexpageX = (byte)(atlas.PositionX / 64)`, `TexpageY = (byte)(atlas.PositionY / 256)`, stored per texture.
- CLUT (`AllocateCLUTs`): for each texture in finalised-atlas order, width = `ColorPalette.Count`, height 1; scan `x = 0,16,32,...` outer, `y = 0..511` inner, first spot passing `IsPlacementValid`. Stored as `ClutPackingX = (ushort)(x/16)` (units of 16 px) and `ClutPackingY = y`; the reader multiplies back: `clutPackingX * 16`.
- `BuildVram` copies `ImageData[x,y]` into `atlas.vramPixels[x+PackingX, y+PackingY]` (atlas-local, no VRAM-wide copy is written to disk); CLUT words are in `ColorPalette`.

### 4.3 Where coordinates are stored
- Per triangle (`WriteTri`, W:1644-1704): `expander = 16 / (int)tex.BitDepth` (4bpp -> 4, 8bpp -> 2, 16bpp "15" -> 1); `u = (byte)(v.u + t.PackingX * expander)`, `v = (byte)(v.v + t.PackingY)`; tpage:
  ```csharp
  tpage.SetPageX(tex.TexpageX); tpage.SetPageY(tex.TexpageY);
  tpage.Set(tex.BitDepth.ToColorMode()); tpage.SetDithering(true);
  writer.Write((ushort)tpage.info); writer.Write((ushort)tex.ClutPackingX); writer.Write((ushort)tex.ClutPackingY);
  ```
  `TPageAttr.info` bits: 0-3 pageX (`TexpageX & 0xF`), bit 4 pageY (`& 1`), bits 5-6 semi-trans (not set, 0), bits 7-8 colour mode (0 4bit, 1 8bit, 2 16bit), bit 9 dithering (set). Page X is `PositionX/64` (0..15).
- UI images / sprite sheets: `TexpageX/Y`, `ClutX = ClutPackingX`, `ClutY = ClutPackingY`, `U0 = PackingX*expander`, `V0 = PackingY`, `U1 = U0 + Width - 1`, `V1 = V0 + Height - 1` (bytes; U wraps past 255), `BitDepthIndex` 0/1/2. Sprite sheet cell UVs: `u0 = baseU + (cell % cols) * CellWidth`.
- Atlas rectangles `(PositionX, PositionY, Width, 256)` are in the `.vram` file (and, redundantly, in 2.10).

### 4.4 `.vram` file
`{ 'V','R'; u16 atlasCount; u16 clutCount; u8 fontCount; u8 0 }` (8 B), then per atlas `{u16 x, u16 y, u16 width, u16 256}` + `width*256` u16 pixels (row-major, `vramPixels[x,y]` iterated y outer) + pad to 4; per CLUT `{u16 clutPackingX, u16 clutPackingY, u16 length, u16 0}` + `length` u16 + pad to 4 (the texture CLUTs, then the fog mask CLUTs, so `clutCount` here can be up to 2 more than the header's); per font `{u8 glyphW, u8 glyphH, u16 vramX, u16 vramY, u16 textureH, u32 dataSize}` + 4bpp bitmap (`256/2 * textureH` bytes, two texels per byte, low nibble = left, row 0 = top, 1 = alpha>0.5) + pad to 4. Reader (R:scenemanager.cpp `uploadVramData`) uploads atlas `width x height` at `(x,y)`, CLUT at `(clutPackingX*16, clutPackingY)`, `length x 1`, font `64 x textureH` at `(vramX,vramY)`, then overwrites 2x1 words at the same `(vramX,vramY)` with a white CLUT `{0x0000,0x7FFF}`.

Fog mask CLUTs: when fog is on and a mesh uses a 4bpp (or 8bpp) cutout texture, one 16 (or 256) entry CLUT `{0x0000, 0xFFFF, ...}` is placed after the texture CLUTs and its position written to the header. psxsplash draws the fog pass over such a triangle as the same textured triangle through that CLUT, additive: entry 0 is the transparent texel, so the holes get no fog. 16bpp textures, and packs without the CLUTs, keep the untextured fog pass.

### 4.5 `.spu` file
`{ 'S','A'; u16 clipCount }`, then per clip `{u32 sizeBytes, u16 sampleRate, u8 loop, u8 0}` + ADPCM + pad to 4. Clip bytes come from `AudioConvertDelegate(AudioClip, sampleRate, loop, trimLeadingSilence)` (registered by an editor-only `PSXAudioConverter`, source not part of the files read).

### 4.6 `.loading` loader pack (psxsplash `LoadingScreen`, `loadingscreen.hh`)
Written when the scene's `settings.loadingCanvas` names one of its canvases; that canvas is then left out of the splashpack's UI table and `.vram`. psxsplash opens `scene_<i>.loading` (PCdrv) or `SCENE<i>/SCENE_<i>.LDG;1` (CD), draws it once into both framebuffers (y 0 and 256) before every scene load, then only redraws the Progress element named `loading` with the engine's load percentage. Line elements are not drawn. A missing file or other version shows a black screen.

| off | size | type | field |
|---|---|---|---|
| 0 | 2 | char[2] | `'L','P'` |
| 2 | 2 | u16 | version = 2 |
| 4 | 1 | u8 | fontCount |
| 5 | 1 | u8 | canvasCount = 1 |
| 6 | 4 | u16 x2 | resW, resH (320, 240) |
| 10 | 1 | u8 | atlasCount |
| 11 | 1 | u8 | clutCount |
| 12 | 4 | u32 | tableOffset (UI table) |

Then `atlasCount` x 12 B `{u32 pixelDataOffset, u16 width (VRAM words), u16 height (256), u16 x, u16 y}`, then `clutCount` x 12 B `{u32 clutDataOffset, u16 clutX (16-word units), u16 clutY, u16 length, u16 0}`. Then the atlas pixels, CLUT entries and font sheets (4bpp, as in `.vram`), each 4-aligned, at the absolute offsets the records give. Atlases are packed with the project's VRAM settings (framebuffers, font column and prohibited areas free). At `tableOffset` the UI table is identical to the splashpack's (2.18), except that each font descriptor's `dataOffset` is the absolute offset of its sheet in this file (the engine skips a font with 0) and `dataSize` its byte size. Only fonts the canvas's text uses are included.

---

## 5. Output dependencies on Unity-only state (make these deterministic)

No GUIDs, timestamps, random numbers or asset paths are written into any output file. (grep over the listed writer files for `guid|GetAssetPath|DateTime|Random|Time.|GetHashCode` found only: a `GetHashCode()` in a log message at PSXSceneWriter.cs:730; `AssetDatabase.GetAssetPath` in Utils.cs:390 and PSXSkinnedMeshExporter.cs:193,339,446 used for importer configuration only.) What does vary:

1. **Object iteration order.** All of these use `FindObjectsByType<T>(FindObjectsSortMode.None)` (unordered): `PSXObjectExporter` (-> gameObject indices, tri-ref objectIndex, mesh order, name table, lua file order, texture list order -> VRAM placement tie order), `PSXSkinnedObjectExporter`, `PSXInteractable`, `PSXAudioClip` (audio clip indices; cutscene audio events bind to first same `ClipName`), `PSXTriggerBox`, `PSXAgent`, `PSXRoom` (room indices), `PSXPortalLink`, `PSXUIImage`, `PSXSprite`, `PSXUISprite`, `PSXNavWalkoffZone`, `PSXCanvas`, `Light`. Skinned proxies are appended at the end in `HashSet` enumeration order. `PSXPlayer` / `PSXNavigationSettings` use `FirstOrDefault()` of an unordered array. The reimplementation must define one canonical order (e.g. hierarchy path) for each.
2. **Sprite sheet order** = `refs.Select(Sheet).Concat(uiRefs...).Distinct()` over unordered arrays; sheet and anim indices are written to the pack and referenced by tilemap and Lua.
3. **Point lights**: `IsRuntime` uses editor-only `light.lightmapBakeType != Baked` (non-editor: `true`); order `OrderBy(HierarchyPath, Ordinal)` (ties unordered), truncated to 16. Light index is also referenced by light tracks via `FindIndex(l.gameObject.name == track.ObjectName)`.
4. **Baked vertex colours** depend on every enabled `Light` in the scene (`bakePointLights` excludes runtime point lights for dynamically-lit meshes); float sum order over unordered lights.
5. **Unstable sorts** (`List<T>.Sort` with a key that can tie): BVH leaf refs by `objectIndex`, room and cell tri-refs by `objectIndex`, cutscene/animation keyframes by `Frame`, audio and skin events by `Frame`, k-d tree build by axis value.
6. **Texture identity**: dedupe by `Texture2D.GetInstanceID()` (only equality, but first-wins depends on exporter order and area sort); source pixels depend on Unity importer state (forced to max 256, uncompressed, no mips, point filter, npot None) and on `material.mainTexture` (`Texture2D` or a `RenderTexture` read via `ReadPixels`); `material.HasProperty("_BaseColor")/"_Color"` for untextured tint.
7. **Project settings asset** `Assets/PSXData.asset` (`PSXData`): `OutputResolution`, `DualBuffering`, `VerticalBuffering`, `ProhibitedAreas` (shape the VRAM layout), `MemCardEnabled/Region/Product/Title/Icons`.
8. **Scene-level inspector settings** on `PSXSceneExporter`: `GTEScaling` (100 default), `SceneType`, fog, `SceneNetworkId`, `Cutscenes[]`, `Animations[]` order (these are indices), `StreamWorldGeometry/RegionSize/LoadDistance`, `SceneLuaFile`.
9. **Player**: `PSXPlayer.FindNavmesh()` does `Physics.Raycast(transform.position, Vector3.down, out hit, 100f)` (needs scene colliders) and uses `hit.point + (0, playerHeight, 0)`; otherwise `transform.position + (0, playerHeight, 0)`; without a player: `PSXNavigationSettings.SpawnPoint` (or origin) and fixed defaults (height 1.8 / radius 0.5 / move 3 / sprint 8 / jump 2 / gravity 20).
10. **Mesh state**: `mesh.RecalculateNormals()` and `mesh.uv = new Vector2[...]` are applied to the shared mesh if normals/uv are missing; normals/vertices are whatever Unity's mesh importer produced (weld, smoothing angle); `mesh.subMeshCount` order; material slot clamp `Min(submeshIndex, materials.Length - 1)`; submesh triangles `GetTriangles(i)`. Per-object `Textures` list = non-null `mainTexture` materials in slot order (null slots skipped), so `TextureIndex` indexes that compacted list, not the material slot.
11. **Dynamic lighting flags** (`IsDynamicLit`): from `PSXPointLightExporter.Resolve` (range-reaches-AABB test using `Bounds.ClosestPoint`).
12. **Nav mesh**: DotRecast version and parameters (`PSXNavigationSettings`/`PSXPlayer` fields: cell size 0.05, cell height 0.025, min region 8, merge 20, simplify 1.3, edge length 12, Watershed default ...); which objects participate (`CollisionType` Static + mesh); platform exporters; walkoff zones. Region order and polygon vertex order are Recast outputs.
13. **Skinned bake**: Unity `Animator`/`AnimationMode.SampleAnimationClip` or `AnimationClip.SampleAnimation`, `skinExp.TargetFPS`, `AnimationClips[]` order, `SkinnedMeshRenderer.bones/sharedMesh.bindposes/boneWeights`, humanoid auto-reimport (`EnsureHumanoidImportIfNeeded`), proxy GameObject creation. Sample time `frame/(frameCount-1)*clip.length`.
14. **UI**: Unity `RectTransform` (anchors, pivot, offsets, `rect`), `Canvas`, hierarchy order from `GetComponentsInChildren<Transform>(true)` (depth-first, sibling order), unique font list order (first use, max 3, 2 supported), `PSXFontAsset` (pre-rendered `fontTexture`, stored advance widths, `glyphWidth` in {4,8,16,32}).
15. **Lua**: file identity = Unity `LuaFile` object; `CompiledLuaBytecode` supplied by an external `luac_psx` run (keyed by source string); `LuaScript` text used by the streaming planner's name search.
16. **Names**: `gameObject.name` (truncated to 24 chars) is the runtime key for cutscene/animation targets and skin-event targets (`skinExp.gameObject.name == evt.TargetObjectName`); duplicates resolve to first.
17. **Audio**: `PSXAudioClip.ClipName`, `SampleRate`, `Loop`, `TrimLeadingSilence`, converter output.
18. **Streaming**: `PSXWorldStreamPlanner.OrderingTableSize` is a static delegate (default 2048*4).

---

## 6. Unity types / APIs on the writer path (what the C++ side must supply)

Data/model types:
`MonoBehaviour` components: `PSXSceneExporter`, `PSXObjectExporter`, `PSXSkinnedObjectExporter`, `PSXInteractable`, `PSXAgent`, `PSXTriggerBox`, `PSXRoom`, `PSXPortalLink`, `PSXPlayer`, `PSXNavigationSettings`, `PSXNavWalkoffZone`, `PSXAudioClip`, `PSXCanvas`, `PSXUIImage/Box/Line/Text/ProgressBar/Sprite`, `PSXSprite`, `PSXTilemapRenderer`; ScriptableObjects `PSXCutsceneClip`, `PSXAnimationClip`, `PSXSpriteSheet`, `PSXTilemap`, `PSXFontAsset`, `PSXData`, `LuaFile`.
Core: `Transform` (`localToWorldMatrix`, `worldToLocalMatrix`, `TransformPoint`, `TransformDirection`, `lossyScale`, `rotation`, `position`, `forward`, `parent`), `GameObject.name`, `Matrix4x4` (`MultiplyPoint3x4`, `GetPosition`), `Quaternion` (`x,y,z,w`, `eulerAngles`), `Vector2/3`, `Bounds` (`min`, `max`, `center`, `extents`, `size`, `Encapsulate`, `Expand`, `Contains`, `ClosestPoint`), `Rect.Overlaps`, `Plane`, `Color`/`Color32`.
Mesh/render: `MeshFilter.sharedMesh`, `Renderer.sharedMaterials`, `Material.mainTexture`, `Material.HasProperty/GetColor/color`, `Mesh` (`vertices`, `normals`, `uv`, `colors`, `triangles`, `GetTriangles(int)`, `subMeshCount`, `vertexCount`, `bounds`, `bindposes`, `boneWeights`, `RecalculateNormals`), `SkinnedMeshRenderer` (`sharedMesh`, `bones`, `rootBone`), `Light` (`type`, `color`, `intensity`, `range`, `spotAngle`, `innerSpotAngle`, `enabled`, `lightmapBakeType`, `transform`).
Texture: `Texture2D` (`width`, `height`, `GetPixels`, `GetPixel`, `ReadPixels`, `Apply`, `GetInstanceID`), `RenderTexture.active`, `TextureImporter` (+ `AssetDatabase`, `AssetImporter`), `AudioClip`.
Physics/anim: `Physics.Raycast`, `Animator`, `AnimationClip` (`length`, `isLooping`, `legacy`, `isHumanMotion`, `SampleAnimation`), `AnimationMode.SampleAnimationClip`, `AnimationUtility.CalculateTransformPath`, Avatar/humanoid import.
UI: `RectTransform` (`anchorMin/Max`, `anchoredPosition`, `pivot`, `rect`, `offsetMin/Max`), `Canvas`.
Scene queries: `Object.FindObjectsByType<T>`, `FindObjectsInactive.Include`, `GetComponent`, `GetComponentsInChildren<T>(true)`, `Object.FindFirstObjectByType`.
Math: `Mathf` (`RoundToInt`, `Clamp`, `Min/Max`, `Sqrt`, `Cos`, `Atan`, `Deg2Rad`, `Rad2Deg`, `CeilToInt`, `Approximately`, `Pow`, `Clamp01`).
Editor: `EditorUtility`, `AssetDatabase`, `DataStorage` (`Assets/PSXData.asset`), `Debug`.
Third party: DotRecast (`Recast`, `RcRecast`, `RcAreaModification`...).
.NET: `BinaryWriter/FileStream/Seek`, `Encoding.UTF8/ASCII`, LINQ (`GroupBy`, `OrderBy`, `OrderByDescending`, `Distinct`, `Sort`), `HashSet`, `SortedDictionary`, `Dictionary<Vector3,...>`.

Minimum data a non-Unity front end must provide: per object: world matrix, name, active flag, lua id, collision type, platform flag, dyn-lighting mode, bit depth, uvOffset material, mesh (positions, normals, uvs, optional colours, submeshes, per-submesh material + texture + tint), local bounds; per texture: RGBA pixels + size; scene: lights, player, nav settings, rooms/portals, triggers, interactables, agents, audio (ADPCM), canvases, fonts, sheets, tilemap, cutscene/animation clips, skinned meshes (bind poses, bones, weights, sampled bone transforms), project VRAM settings.

---

## 7. Writer-vs-reader mismatches and oddities

Re-checked 2026-10-03 against splashedit 64785e3 and psxsplash c1df566. All still hold. "Fix side"
says what has to change; WRITER items are fixed in this exporter as each feature is ported.

| item | fix side | effect |
|---|---|---|
| M1 | WRITER | agent state clips index the scene animation table, the engine reads a per-object skin clip index; any agent with state clips plays the wrong clip or none. This exporter writes the skin clip index |
| M2 | WRITER | patrol waypoint Y has the opposite sign to every other position; this exporter negates it |
| M3 | WRITER | a `CameraH` track on an animation (not reachable from the editor UI) writes 2-byte keyframes the reader steps as 8 |
| M4 | none | extra CLUT words; this exporter pads to a multiple of 4 instead |
| M5 | READER | the font CLUT is read from the bitmap origin, so the white CLUT replaces 8 texels of row 0: the space glyph, plus `!` when glyphs are 4 wide. Engine side is psxsplash#56; this exporter keeps row 0 of a TrueType sheet blank and warns when a bitmap font has ink there |
| M6 | none | unused fields |
| M7, M8, M10 | none | comments and names |
| M9, M16 | WRITER (guard) | cannot happen with the current eligibility rules |
| M11 | WRITER | counts over 65535 truncate while every record is written |
| M12 | WRITER | non-ASCII names mangled; name caps count UTF-16 characters against a UTF-8 byte length. This exporter writes every UI name and text as UTF-8 and cuts text at 63 bytes on a character boundary |
| M13 | WRITER | line endpoints wrap past 255 px. Fixed in this exporter (endpoints are i16) |
| M14 | WRITER | a third font, or a skipped one, shifts or dangles font indices. Fixed in this exporter: text names its font, an unknown name or a font that does not fit fails the export, and 3 fonts are supported |
| M15 | WRITER | single buffering throws in the packer |
| M17 | WRITER | player start rotation is written in radians and read in units of pi, so a start yaw of 90 degrees faces about -77 degrees. Measured with tests/boot on c05 plus a player; fixed in this exporter |

M1. **Agent per-state clip index space differs.** Writer indexes the scene's `PSXAnimationClip[]` table:
`if (scene.animations[c] == clip) { clipIndices[s] = (byte)(c < 255 ? c : 0xFE); ...` (W:PSXSceneWriter.cs ~585-600).
Reader treats it as a skinned-mesh clip index: `R:splashpack.hh:71 uint8_t stateAnimClip[8]; ///< Skinned-mesh clip index per AgentState` and `R:scenemanager.cpp:1865-1869 uint8_t clipIndex = ...stateAnimClip[...]; ... if (clipIndex >= set.clipCount) return; state.currentClip = clipIndex;` where `set` is the object's `SkinAnimSet`.
M2. **Agent waypoint Y not negated**, unlike every other position: `writer.Write((int)Mathf.RoundToInt(worldPos.y / gte * 4096f));` (W:PSXSceneWriter.cs, agent pass 2) vs `R:scenemanager.cpp:1161 agent.waypoints[w].y.value = waypointCursor[w * 3 + 1];` (raw copy). Positions elsewhere are `ConvertWorldToFixed12(-pos.y / gte)`. A writer bug: the engine compares a waypoint's y with Y-down nav floor heights (`findActorPathToPosition` -> `NavRegionSystem::findRegionNearest` -> `findRegionClosest`, which skips a region when `y - 32 > floorY`) and Lua's `Agent.AddWaypoint`/`Actor.GetPosition` use the same Y-down frame, so an un-negated waypoint above the floor resolves to no region and falls back to the nearest centroid. Fixed in this exporter (negated).
M3. **Animation keyframes for `CameraH` tracks are written with no payload.** `PSXAnimationExporter.ExportAnimations` filters only `CameraPosition`/`CameraRotation` (`if (track.TrackType == PSXTrackType.CameraPosition || track.TrackType == PSXTrackType.CameraRotation) ... continue;`) and its keyframe `switch` has no `CameraH` case, so only the 2-byte `frameAndInterp` is emitted per keyframe, while the reader indexes `CutsceneKeyframe` (8 B, `static_assert(sizeof(CutsceneKeyframe) == 8`) with the same stride for animation tracks. (Cutscene exporter does have the `CameraH` case.)
M4. **Palette padding always adds 1-4 entries** (W:PSXTexture2D.cs:205-210, quoted in sec. 4.1); the reader just uploads `length` words (`R:scenemanager.cpp uploadVramData`, `renderer.VramUpload(..., clutPackingX * 16, clutPackingY, length, 1)`). A 256-colour palette becomes a 260-word CLUT; `AllocateCLUTs` allocates that many words. Not a size mismatch, but surprising.
M5. **Font CLUT upload overwrites font bitmap words.** Writer puts font bitmap at `(VramX, VramY)` (W:PSXUIExporter.cs:135-136, `VramX = 960`); reader writes the white CLUT to the same coordinates: `R:scenemanager.cpp renderer.VramUpload(whiteCLUT, (int16_t)fontVramX, (int16_t)fontVramY, 2, 1);` replacing 2 words (8 texels) of bitmap row 0. Harmless only if those texels are blank.
M6. **Fields written but not consumed by the reader**: header `pixelDataOffset` (always 0; no use found in R:splashpack.cpp), `uiPad5`, `pad3`; atlas metadata and CLUT metadata records (skipped by size only, 2.10); `boundaryEdgeMask` has no field of its own and is ORed into `walkoffEdgeMask`; collider records are read into `setup.colliders` (consumption elsewhere not checked); `worldCollisionMeshCount/TriCount` written 0 (reader has a legacy skip path for non-zero).
M7. **Reader comments with stale sizes** (behaviour correct, layout is sequential): `R:splashpack.cpp "SPLASHPACKCutscene: 12 bytes at dataOffset"` vs actual 20 (2+1+1+4+4 then +1+3+4 for v19+); `"SPLASHPACKAnimation: 8 bytes"` vs actual 16. Writer comment `"Phase 2: Write element records (56 bytes each)"` vs 48 actual / reader 48. Writer class doc says "v16"/"v20".
M8. **Header field naming**: writer's `reservedMemcard` (offset 124) is the reader's `streamTableOffset`; writer's sprite-block comment names the same words differently. Same bytes, no layout mismatch.
M9. **Skin bone-index count depends on `polyCount` of the object record**: reader `polyCount = setup.objects[gameObjectIndex]->polyCount; skinPtr += polyCount * 3;`. The writer sets `polyCount = 0` for streamed objects; this is safe only because `FindEligible` excludes skinned proxies (`inp.Skinned.Contains(e)`), not because of any check at the write site.
M10. **BVH/room `triangleIndex` vs Tri order**: refs use `i/3` over `mesh.triangles` (all submeshes concatenated, W:BVH.cs ExtractTriangles; W:PSXRoom.cs same) while Tri order is submesh-by-submesh via `mesh.GetTriangles(submesh)` (W:PSXMesh.cs). These agree for normal meshes; they differ only if a mesh's `triangles` concatenation is not the submesh order (not expected).
M11. **Count truncations**: `uint16` header counts for BVH/room/portal/tri-ref are `Min(...,65535)` while all records are still written (writer logs an error); `PSXMesh` triangle `TextureIndex`/tri counts are `ushort`.
M12. **Interactable name** written byte-by-byte `(byte)canvasName[ci]` (low byte of UTF-16), audio names written ASCII (`'?'` for non-ASCII), all other names UTF-8; name lengths for objects/canvases/cutscene tracks are `Length` caps of 24 *chars* but stored length is UTF-8 *bytes*.
M13. **Line element coordinates** are `(byte)RoundToInt(point)` cast into `short` fields (W:PSXUIExporter.cs:503-504), so lines beyond 255 px wrap; the reader reads them as `uint16`.
M14. **UI font index mapping**: `fontIndex = uniqueFonts.IndexOf(font) + 1`, but fonts skipped for null bitmap or past 2 pages are not removed from `uniqueFonts` before indexing, so the index can name a font that was not exported (W:PSXUIExporter.cs:72-190 vs reader `UI_MAX_FONTS-1 = 3`).
M15. **VRAM packer reserves `framebuffers[1]` unconditionally** (`_reservedAreas.Add(framebuffers[1]);` W:TexturePacker.cs ~58) while `PSXSceneExporter.PackTextures` only adds it when `dualBuffering`; with single buffering `framebuffers[1]` throws `ArgumentOutOfRange`.
M16. **ArrangeAtlasesInVRAM "unplaced" test**: `if (atlas.PositionX == 0 && atlas.PositionY == 0)`: an atlas legitimately placed at (0,0) would be re-placed; in practice (0,0) is a framebuffer.

---

## 8. Commands run

```
git -C splashedit fetch -q origin && git -C splashedit log -1 --oneline origin/main
git -C psxsplash fetch -q origin; git -C ... log -1 --oneline origin/main
git -C psxsplash grep -n -i 'splashpack' origin/main -- src
git show origin/main:Runtime/{PSXSceneWriter,Utils,PSXMesh,BVH,PSXObjectExporter,PSXSceneExporter,PSXTexture2D,ImageProcessing,TexturePacker,PSXNavRegionBuilder,PSXRoom,PSXCanvasData,PSXUIExporter,PSXUILayout,PSXUIElementType,PSXFontAsset,PSXCutsceneExporter,PSXTrackType,PSXInterpMode,PSXPointLightExporter,PSXLightingBaker,PSXAnimationExporter,PSXSkinnedMeshExporter,PSXSpriteExporter,PSXTilemapExporter,PSXWorldStreaming,PSXLoaderPackWriter(header+greps),PSXInteractable,PSXAgent,PSXTriggerBox,PSXPlayer,PSXAudioClip}.cs   (sed/grep slices)
git show origin/main:src/{splashpack.hh,splashpack.cpp,gameobject.hh,mesh.hh,bvh.hh,interactable.hh,navregion.hh,navregion.cpp,cutscene.hh,uisystem.hh,uisystem.cpp,spritesystem.cpp,tilesystem.cpp,streamplanner.hh,worldstreamer.cpp,lua.h,scenemanager.cpp(slices),skinmesh.hh,lightmath.hh}
grep -n -i 'guid|GetAssetPath|DateTime|Random|GetHashCode|Time\.|Environment\.' over the writer files
```

Not read or only grepped: `PSXSkinnedObjectExporter`, `CreateProxy` body, `PSXRoom` cell/portal-duplication internals (summarised from comments), the nav Recast pipeline internals (lines ~131-400 of PSXNavRegionBuilder), `PSXCollisionExporter` (not referenced by the writer), `PSXPortalLink`/`PSXRoom` component bodies, `PSXAudioConverter`, `PSXSpriteSheet`/`PSXTilemap` validation, `SceneMemoryAnalyzer/Report`, engine renderer consumption of Tri/tpage/AABB fields, the `.loading` reader (`loadingscreen.cpp`).
