# Tests

## Format check against Unity SplashEdit 2.4.0

The exporter is not a port of 2.4.0 and does not try to match its output where 2.4.0 does worse
(textures, see below). The 2.4.0 exports are kept as a check that the file layout has not drifted.

`unity/NgParity.cs` builds small scenes in Unity, exports them with 2.4.0 and dumps each one as a
scene file (`unity/README.md` has the setup). `splashpack-cli` exports the dump, and
`splashdiff.py` compares the two section by section, matching objects by name. It decodes v23 to
v27 by version, as the psxsplash reader does (2.4.0 writes v23, or v24 with point lights; this
exporter writes v27), including the room block (rooms, portals, room triangle refs, cells). Packs of
different versions are compared on the header fields both carry, and the version is reported as a
difference. Sections it does not decode are reported as not compared. `splashdiffcheck.py` checks it
on a scene with rooms and on a v23 pack (`splashdiff/v23.*`).

    splashpack-cli export <out>/<case>/ir/scene.scene -o cpp/<case>.splashpack \
        --order-from <out>/<case>/unity/scene.splashpack
    python3 splashdiff.py <out>/<case>/unity/scene.splashpack cpp/<case>.splashpack
    cmp <out>/<case>/unity/scene.vram cpp/<case>.vram

`--order-from` copies Unity's object order. Unity's order comes from `FindObjectsByType` and changes
between runs, and the BVH leaf order depends on it through an unstable sort. With the order copied,
the files can be compared byte for byte.

| case | .splashpack | .vram | .spu |
|---|---|---|---|
| c01 cube, flat colour | identical | identical | identical |
| c02 rotations, non-uniform scale, mesh colours, inactive object | identical | identical | identical |
| c03 textures 4/8/16 bpp | CLUT length field only | texels and CLUTs differ (new quantizer) | identical |
| c04 textures needing a palette search | CLUT length field only | texels and CLUTs differ (new quantizer) | identical |
| c05 colliders none/static/dynamic, one nav region | identical | identical | identical |
| c06 baked directional + point light, sphere BVH | identical | identical | identical |
| c07 two submeshes, textured + tinted | identical | CLUT differs (new quantizer) | identical |

## Texture quality

`splashpack-cli texstats` converts one image the way export does and scores the result against
the source: RGB PSNR, mean OKLab deltaE (x100) per texel, and the same deltaE after a 3x3 box
blur. `--out` writes what the console will show.

    splashpack-cli texstats image.png --bpp 4 [--cutout] [--out decoded.png]

The quantizer differs from 2.4.0's in these ways:

- 5-bit channels are rounded to nearest at every bit depth. 2.4.0 truncated with `(ushort)(c * 31)`.
- If the image fits the palette once rounded to 15 bits, the palette is exact and nothing is
  dithered.
- Otherwise the palette is a k-means in OKLab over the distinct colours, weighted by the square
  root of their texel counts, seeded farthest-point first (deterministic) and iterated until it
  settles. 2.4.0 ran 10 rounds of unweighted RGB k-means from evenly spaced picks in scan order.
- Texels are error-diffused onto the palette after it is rounded to 15 bits, in serpentine order,
  at 0.75 strength.
- Opaque black is 0x8000. 2.4.0 wrote (1,1,1) with the STP bit.
- The CLUT is padded to a multiple of 4 words. 2.4.0 always added 1 to 4 more.

Mean deltaE, 2.4.0 against this exporter, on the test textures from `unity/NgParity.cs` and
sprite and UI art from a psxsplash game (images with transparency measured with `--cutout`):

| image | bpp | deltaE 2.4.0 | deltaE now | PSNR 2.4.0 | PSNR now |
|---|---|---|---|---|---|
| Amogus | 4 | 8.556 | 0.281 | 30.22 | 41.83 |
| Amogus | 8 | 8.385 | 0.169 | 31.16 | 47.91 |
| Band | 4 | 3.609 | 1.096 | 28.25 | 32.89 |
| Band | 8 | 1.938 | 0.652 | 33.71 | 41.08 |
| crew | 4 | 1.596 | 1.026 | 35.05 | 38.74 |
| crew | 8 | 1.596 | 1.026 | 35.05 | 38.74 |
| Nic | 4 | 3.138 | 1.292 | 27.70 | 32.39 |
| Nic | 8 | 4.567 | 0.928 | 33.92 | 39.63 |
| t03_pal100_32x32 | 4 | 8.431 | 6.323 | 18.86 | 18.51 |
| t03_pal100_32x32 | 8 | 1.312 | 0.692 | 35.27 | 40.58 |
| t03_pal7_16x16 | 4 | 0.261 | 0.037 | 41.69 | 58.60 |
| t03_pal7_16x16 | 8 | 0.261 | 0.037 | 41.69 | 58.60 |
| t03_rgb_32x16 | 4 | 6.655 | 4.846 | 21.51 | 21.33 |
| t03_rgb_32x16 | 8 | 1.527 | 1.233 | 33.39 | 32.67 |
| t04_grad_64x64 | 4 | 14.846 | 6.522 | 14.71 | 18.85 |
| t04_grad_64x64 | 8 | 2.604 | 1.681 | 28.87 | 29.97 |
| t04_many_32x32 | 4 | 9.649 | 4.982 | 15.47 | 21.15 |
| t04_many_32x32 | 8 | 2.011 | 1.254 | 29.69 | 31.59 |
| tileset | 4 | 1.627 | 0.637 | 34.69 | 42.75 |
| tileset | 8 | 1.627 | 0.637 | 34.69 | 42.75 |

deltaE goes down on every row. RGB PSNR drops slightly on three small synthetic rows because the
palette minimises OKLab distance.

## Texture preview against the console

The inspector shows a texture's source next to what the console will draw (`src/editor/
texture_preview.*`: the same `splash::convertTexture` the exporter calls, decoded back to colour).
`quantpreviewcheck.py` proves that preview is what psxsplash actually uploads. It exports a scene
to a CD-ROM image, boots it in pcsx-redux headless (`boot/vramdump.lua` saves a state at a frame),
lifts the GPU's 1 MB VRAM out of the uncompressed save state, reads each texture back through its
atlas and CLUT exactly as the GPU samples it, and compares the 15-bit texels with the editor's
`splashedit --dump-texture-preview` output for the same image, bit depth and cutout.

    python3 quantpreviewcheck.py <splashedit> <splashpack-cli> <engine.ps-exe> <pcsx-redux> <bios.bin> [scene] [frame]

The engine is a CD-ROM build of psxsplash (`make LOADER=cdrom`); scene defaults to
`quantpreview/quantpreview.scene` (one crate per bit depth, textured with a dithering-heavy
gradient) and frame to 1500 (past the boot splash, once the scene's atlases are uploaded). On the
fixture every texel matches: 12288 compared, 0 differing, at 4, 8 and 16 bpp. Needs the BIOS, so it
is **not in CI**. The editor-side unit test (`testTexturePreview` in `editor_tests`) makes the same
comparison against the exported `.vram` file and **is** in CI.

## Boot check

`boot/boot.sh` boots psxsplash (a PCdrv build) on one exported scene in pcsx-redux, headless, and
saves the screen at a given frame (default 600). The three files (and `.geo` and `.loading` when
present) are copied to a temporary directory as `scene_0.*`, the names the PCdrv loader opens.
With a loading canvas the loading screen shows for a few frames after the boot logo (frames
260-266 with psxsplash f671202 on tests/ui/ui.scene plus a canvas).

    boot/boot.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> <dir>/scene shot.png [frame]

`boot/discboot.sh` does the same for a disc image from `splashpack-cli disc` (a CD-ROM build of
psxsplash, `make LOADER=cdrom`), booted from its .cue through the BIOS and SYSTEM.CNF.

    boot/discboot.sh <pcsx-redux> <openbios.bin> image.cue shot.png [frame]

Courtyard at frame 900 is pixel-identical between the disc and the PCdrv boot.

`disccheck.py` builds a disc of the courtyard example with stand-in engines and reads it back: sync,
BCD address, subheader, EDC on every sector, P/Q parity on a sample (recomputed in Python from the
Reed-Solomon generator), the ISO 9660 tree, SYSTEM.CNF in the first root sector, and the scene files
byte-equal to `export --lua-bytecode`. A PCdrv engine must be refused. A second disc of two scenes
(the courtyard, then a copy with fog on) must hold each under `SCENE<i>/SCENE_<i>.*`, byte-equal to
that scene's own export, and no `SCENE2`.

    python3 disccheck.py <splashpack-cli>

`bvhstatscheck.py` exports the courtyard example with `--stats` and reads the
`bvh nodes=N leaves=L depth=D refs=R` line: nodes and refs must match the splashpack header, refs the
triangle count, leaves (N + 1) / 2 (every inner node has two children) and the depth at least
log2(L). A copy without objects must print an empty BVH.

    python3 bvhstatscheck.py <splashpack-cli>

Without nav regions psxsplash does not attach the camera to the player, so the camera stays at the
origin facing +Z and the player start in the file has no effect. Put the objects at positive Z.
A scene with a Player always gets nav regions when it has ground to stand on: on big worlds the
nav bake uses larger cells (Recast indexes at most 2^24 spans and the bake is kept under a million),
and `export --stats` prints the cell size it used.

`boot/walk.sh` stretches the courtyard to 450 x 375 m, turns streaming on, and boots it three
times: twice standing still (the shots must match) and once holding Up from frame 200 (`BOOT_HOLD`
in `boot.lua`). Walking must change the shot and attach more streamed regions than standing still.
The engine has to be a PCdrv build made with `make STREAMLOG=1`, which prints `WORLDSTREAM`
attach/detach lines.

    boot/walk.sh <splashpack-cli> <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> [frame]

Standing still attaches 1 region and walking 2. With the exporter from before the nav fix the
camera never moves (zero nav regions on a world that size).

`boot/palcheck.py` checks one textured quad on the screenshot against `texstats --out`: every pixel
must be a palette colour, give or take the one 5-bit step the GPU dither adds.

    python3 boot/palcheck.py shot.png x0 y0 x1 y1 decoded.png

With c03 moved to z = 3 all three quads render, and the 4 and 8 bpp quads pass with 16-word
CLUT padding (half the pixels exact, half one dither step off). Against the wrong palette the
4 bpp quad fails with a distance of 90.

## Load and save

`roundtrip.py` resaves scene and mesh files with `splashpack-cli resave` and checks that known values
and unknown keys survive. Bad versions and malformed mesh arrays must fail to load. Sprite sheets
come back with their defaults filled in and their unknown keys, and a scene without them writes no
`spriteSheets` key.

    python3 roundtrip.py <splashpack-cli>

## Model import

`importcheck.py` builds small glTF files (a .glb with an embedded texture, a .gltf with a data-URI
buffer, an external texture, no normals and a mirrored node), an OBJ+MTL whose texture is found by
bare name in a Textures/ folder (and a missing one that only warns) and an ASCII FBX in Z-up
centimetres (texture found in tex/), imports them with
`splashpack-cli import` and checks the .mesh: X mirrored, winding reversed to match the courtyard
meshes, V flipped for glTF and kept for OBJ/FBX (their V already starts at the bottom), node
transforms baked, colours read, textures kept top row first and scaled to
256 when larger. The node tree is checked too, on a .gltf with a parent, two children with
colliding names and a matrix grandchild, OBJs with two `o` objects and two `g` groups, and the
FBX: each node's transform (X mirrored, rotation (x, y, z, w) -> (x, -y, -z, w)) composed down
the tree over its own models/<stem>/<node>.mesh must give the baked mesh within 1e-4. A scene
using the imports must export.

    python3 importcheck.py <splashpack-cli>

`blendcheck.py` imports a .blend through a mock Blender (a Python script on PATH or named by
SPLASHEDIT_BLENDER) that writes a GLB made from `skinimport/rig.gltf` where the export script
asks, and checks the import writes the same files as that GLB imported directly, that a sidecar
beside the .blend applies, and the errors when Blender fails, writes nothing or is not found. The
mock arms print SKIP on Windows, where a shebang script cannot run.

    python3 blendcheck.py <splashpack-cli>

`blendasynccheck.py` drops a .blend in the editor with SPLASHEDIT_BLENDER naming a mock Blender
that waits until the check lets it finish. Blender runs on a worker thread, so 60 frames after the
drop the import must still be running with the editor drawing frames (the screenshot then shows
"Importing crate.blend with Blender... N s" and Cancel in the status bar); once it finishes the
model is placed and saved, and Ctrl+Z takes it back in one step. Cancel in the status bar kills the
mock, places and adopts nothing and leaves no `splashedit-blend-*` folder. `editor_tests` checks
the same cancel at the core level (`splash::BlendExport`). Needs a display (CI runs it under
`xvfb-run` on Linux); prints SKIP on Windows. A second argument copies the mid-import screenshot
there.

    python3 blendasynccheck.py <splashedit> [screenshot.png]

`asyncimportcheck.py` does the same for every model format: it builds an 80000-triangle glTF grid
that takes a few seconds to import and drops it in the editor. The file is read and baked on a
worker thread into a `splashedit-import-*` temporary folder, so 3 frames after the drop the import
must still be running, and it reports the frames drawn meanwhile (at least 10); once it is built the
model is placed and saved. While it builds the status bar shows the share done beside the seconds
(the `--shot` report adds `import progress: N%`), and an import folder two hours old seeded in the
run's temp folder is swept at the first import while a folder of another name is left. Cancel in the status bar places, adopts and writes nothing, the saved
scene is byte for byte the one a run without the drop saves, and no temporary folder is left. Two
models dropped one after the other both land in the order dropped, the second waiting behind the
first though it alone would be done sooner (the screenshot shows "Importing terrain.glb... N s (1
more waiting)" and Cancel). `editor_tests` checks the queue itself (`editor::ModelImports`): order,
cancel, a failed file, a scene change and a cancelled reimport leaving the meshes on disk as they
were; and (`testModelImportsGaps`) that a cancelled 180000-triangle build stops within a quarter of
its full build time, that the next import never starts while a cancelled build is still stopping,
that a model whose name (file or baked mesh) appears in `models/` while it imports takes the next
free name, outputs and node folder following it, and that the stale-folder sweep removes only
`splashedit-import-*` folders older than an hour that no import holds. Needs a display (CI runs it under xvfb on Linux).

    python3 asyncimportcheck.py <splashedit> [screenshot.png]

## Lua compiler

`luacheck.py` compiles every `lua/*.lua` with `splashpack-cli luac` and compares the result with
`lua/expected/*.luac` byte for byte. The expected files come from psxsplash's `tools/luac_psx`
running in pcsx-redux, so CI checks the host compiler without an emulator. The script also checks
that a changed source gives different bytes and that a syntax error is reported.

    python3 luacheck.py <splashpack-cli>
    splashpack-cli luac in.lua -o out.luac

The files cover constant folding with 32-bit overflow, division and modulo by negative constants, hex, leading-zero and
oversized literals, strings with escapes and zero bytes, long strings, more than 256 constants,
closures, varargs, goto, methods, loops and a table constructor large enough to need an extra
SETLIST argument.

To regenerate the expected files, list the sources in a `manifest.txt` (source path, then output
path, one per line) in a directory, and boot `luac_psx.ps-exe` with that directory as the PCdrv
base. It writes `__done__` containing `OK` when it has finished.

    pcsx-redux -testmode -stdout -safe -bios openbios.bin -loadexe luac_psx.ps-exe -run \
        -pcdrv -pcdrvbase <dir>

## Audio

`audiocheck.py` encodes the WAVs in `audio/` at their own rate and compares the ADPCM with
`audio/expected/`, which psxavenc (`-t spu -f <rate>`, plus `-L` for the looped files) wrote from the
same 16-bit mono PCM. The encoder is libpsxav's, as in psxavenc, so these match byte for byte.
Resampling is our own windowed sinc; psxavenc's goes through ffmpeg, so resampled clips differ.

    python3 audiocheck.py <splashpack-cli>

`splashpack-cli audio <in.wav> -o <out> --rate <hz> [--loop] [--trim] --pcm <src.wav>` writes the
PCM it starts from, which is what was fed to psxavenc to make the expected files.

Clips can also be AIFF, MP3, Ogg Vorbis or FLAC. `audio/formats/` holds a 440 Hz tone as `tone.wav`
and the same tone encoded to each (sox, lame, oggenc, flac). `audioformatcheck.py` decodes each
through `splashpack-cli audio` and matches it against the WAV (the MP3 at its best lag, for the
encoder delay), and exports an Audio source with the Ogg clip; `audiofilecheck.py` checks the
inspector's Source row for Ogg and MP3 clips.

    python3 audioformatcheck.py <splashpack-cli>
    xvfb-run -a python3 audiofilecheck.py <splashedit> [ogg.png]

## Cutscenes

`cutscenecheck.py` exports `cutscene/cutscene.scene` with extra tracks and reads the cutscene table
back the way psxsplash's loader does: per-type key encodings, key and audio event order, the
`uiPosition` anchor correction, and that unknown targets and clips, too many tracks, repeated names
and frames past 8191 fail the export.

    python3 cutscenecheck.py <splashpack-cli>

`cutscene/cutscene.scene` booted with `boot/boot.sh` (its script plays the cutscene in a loop) moves
the crate right while the progress bar fills: at frames 300, 337 and 374 the crate's centre is at
x 127, 141 and 156.5 and the bar is 48, 96 and 141 px long.

## Skinned meshes

`skincheck.py` exports `skin/skin.scene` (a two-joint arm from `skin/make_arm.py`) and reads the
skinned-mesh table back the way psxsplash's loader does. It applies each baked bone matrix to the
exported vertices the way the renderer does and compares with the clip's pose computed separately,
for every frame, a child joint under a moving parent and a non-uniformly scaled object. It also
checks frame counts, the skinned flag, the bone per triangle vertex, and that a missing skeleton,
an unknown joint, no clips, a repeated or too-long clip name and fps 31 fail the export.
With `--stats` the `skins=N bytes=B`, `skin bones=... <object>` and `skin clip fps=F frames=K bytes=Y
loop=0|1 <object>/<clip>` lines must match that table: bone, triangle, clip and frame counts, each clip's
bytes from its name length byte through its last matrix, its loop flag, and a scene without skins prints
none.

    python3 skincheck.py <splashpack-cli>

`agentcheck.py` exports `skin/skin.scene` with Agent components on the skinned Arm, on two mesh-less
objects and on an inactive one, and reads the agent records back the way psxsplash's loader does
(agentCount at header offset 114, 28-byte records right after the interactables, then the waypoints).
Every field is compared with the conversions of `docs/splashpack-format.md` 2.7 computed in the check:
flags, fp12 speed and ranges, the half-cone cosine, state clips (none past the skin's clips or without
a skin), waypoints capped at 8 with Y negated, and the game object each record names (through the name
table: mesh-less agents are game objects, the inactive one is left out). It also checks the export's
warnings (more than 8 waypoints, field of view outside (0, 360], state clips without a skin or past its
clips, a held state (Idle, Patrol, Seek, Flee, Wander) on a clip that plays once, agents without nav
regions; none with the courtyard's nav bake), the `--stats` `agent object=G waypoints=N bytes=B
clips=State:index,...|- <object>` lines (the clip written per state, `-` for none),
refused bad values and that `resave` keeps the component.

    python3 agentcheck.py <splashpack-cli>

`skin/skin.scene` booted with `boot/boot.sh` (its script loops `bend`) shows the upper box swinging
about the elbow.

## Memory card

`memcardcheck.py` exports scenes with `settings.memCard` and reads the table back the way
psxsplash's `splashpack.cpp` does: the header word at 120 (0 when saves are off, and then the same
bytes as a scene without `memCard`), then the 464-byte record field by field at the loader's
offsets. The three icon frames (made by the script as PNGs) must decode through the one shared CLUT
to their source pixels, top row first and low nibble left, with transparent texels on word 0. It
also checks the defaults, a 32-character title, the refused values (a 33-character title, an
11-character product code, an empty product code, a one-letter region, a non-ASCII title, 4 frames,
a 32x32 icon, a missing icon) and that `resave` keeps the fields and leaves defaults out.

    python3 memcardcheck.py <splashpack-cli>

## Sprite sheets

`spritecheck.py` exports `sprite/sprite.scene` (two sheets made by `sprite/make_sheets.py`: `hero`
at 4 bpp in 16x16 cells, `fx` at 8 bpp in 12x8 cells; three animations) and reads the sprite table
back the way psxsplash's `SpriteSystem` does: the header words at 128 (offset, 4-aligned), 132
(sheets) and 134 (animations), the 20-byte sheet and 12-byte animation records in scene order and
the NUL-terminated names after them. Each sheet's texture page, u0/v0 and CLUT must match its
texture's VRAM placement (packed with cutout on), and every frame of every animation, fetched with
the engine's `frameToUV` from the `.vram` file through the sheet's CLUT, must be that cell of the
source PNG. A scene without sheets leaves the three words 0. Export refuses 17 sheets, 65
animations, an empty or repeated sheet name, no or a missing texture, a texture that is not a whole
number of cells, a cell side over 255, more than 255 cells, an animation outside the sheet, 0
frames, 0 vsyncs a frame and one animation name on two sheets (`Sprite.AnimIndex` searches every
sheet). With `--stats`:

    sprites sheets=N anims=M bytes=B
    sprite sheet bpp=4|8|16 cell=WxH grid=CxR anims=K <sheet>

`B` is the table from its first sheet record through the last name's NUL (20 N + 12 M + names);
`grid` is the texture's columns x rows of cells. A scene without sheets prints neither. Each UI
image (not the loading canvas's) prints, in canvas order:

    ui image sheet=<sheet>|- cell=N <canvas>/<element>

`uispritecheck.py` exports the same scene with a canvas of HUD sprites (UI images backed by a cell
of a sheet: hero cell 6, fx cell 5) and a plain image, and reads each image's 16 type-data bytes
back the way `uisystem.cpp` does: page, CLUT and bit depth must be the sheet's, cellW/cellH/cols
and baseU/baseV the sheet record's grid and u0/v0, u0..u1/v0..v1 the authored cell (inclusive last
texel), the cell recovered from them as the loader does (UI.GetFrame), and the texels the GPU
fetches there through the `.vram` file that cell of the PNG, also after UI.SetFrame's arithmetic for
another cell. Each sheet texture is in VRAM once; the plain image keeps bytes 11..15 zero; colour
0.5 is 128. Export refuses a sheet the scene does not have and a cell past the sheet's last.

    python3 uispritecheck.py <splashpack-cli>

    python3 spritecheck.py <splashpack-cli>

Booted on psxsplash (pcdrv, e.g. psxsplash's `tests/boot/boot.sh` on the exported `scene_0.*` with
token `SPRITE OK`), `sprite/sprite.lua` looks both sheets and the three animations up by name,
expects the indices the table gives them, makes a sprite from each sheet, plays an animation on each
and prints `SPRITE OK` when `Sprite.Count()` is 2, else `SPRITE FAIL` and `BOOT FAIL`.

## UI

`uicheck.py` exports `ui/ui.scene`, reads the UI table back the way `uisystem.cpp` does and resolves
each element with the engine's `resolveLayout` at 320x240: a bar anchored 0..1 must cover x 0-319
and a box anchored to the right edge must end at x 319. It also checks font indices, line endpoints
past 255, the font sheets in the .vram file (in the x = 960 column, one page each, clear of the
system font, row 0 blank) and that an unknown font, 4 fonts, a repeated font name or 25 canvases
fail the export.

    python3 uicheck.py <splashpack-cli>

`splashpack-cli font <font.ttf> --size <px> -o sheet.png` (or `<bitmap.png> --cell <w>x<h>`) writes
the glyph sheet export builds and prints its cell size and VRAM cost.

`ui/ui.scene` booted with `boot/boot.sh` at frame 300 shows both Inter fonts, the system font, the
progress bar, the image, a line ending at x 311 and the full-width bar, at the positions above.
That frame is kept as `ui/ui-runtime.png` (psxsplash 669fb6b): `editor_tests` renders the scene
with the UI tab's preview and requires the same pixels in 15-bit colour.

`uicanvascheck.py` drives the UI tab (the canvas editor) on a copy of `ui/ui.scene`, started with
`--ui`, which opens the tab and prints where the canvas and the element list are. It drags an
element on the canvas (and to 2 px from the screen edge, where it must snap onto it), resizes one
by its corner handle, nudges with the arrows (Shift: 8 px), brings one forward with Ctrl+],
reorders by dragging a list row, duplicates, deletes, hides the canvas with its eye and clicks
empty canvas to deselect, then saves and reads the scene back. One Ctrl+Z must take back one
whole drag. Needs a display (CI runs it under `xvfb-run` on Linux).

It also copies, cuts and pastes elements (Ctrl+C/X/V and the right-click menu on the canvas and
on a list row, whose rows the editor prints when it opens): a paste lands in front of the
selection 8 px down and right of the original (a second one past the first copy, a cut one back
in place) with a numbered name, and one Ctrl+Z takes it back. Then it zooms (Ctrl+wheel and the
plain wheel, about the pointer, which must stay over the same canvas pixel) and, zoomed in,
selects by clicking where the fit view has nothing, drags and resizes by a handle; pans with a
middle drag and with Space + drag (which must move no element), and Ctrl+0 fits again. At 100%
(Ctrl+1) the canvas in the screenshot must equal `ui/ui-runtime.png` in 15-bit colour wherever
the game drew, except the 1 px element outlines. The clipboard text is
`{"splashedit-ui-elements": 1, ...}`; `editor_tests` checks that it and the scene objects'
`splashedit-objects` text are never pasted as each other.

Multi-select: Ctrl+click on the canvas and on the list toggles an element, Shift+click on the list
takes a range, and a rubber band dragged from empty canvas takes every element it touches (with
Ctrl it adds to the selection); the inspector then says "2 elements" (the editor prints it with
`--ui`). Arrow keys, a drag on one of them, Delete, Ctrl+X, Ctrl+C then Ctrl+V and Ctrl+D act on
all of them and one Ctrl+Z takes each back; a plain click on one of several keeps only that one.
The scenes carry a scene object `Marker` (selected with `--select`): F2 and Edit > Rename (clicked
at its place in the Edit menu) must rename the selected UI element and leave the object alone.
Paste with the pointer over the canvas puts the pasted group's top-left under it (snapped like a
drag: 1 px from the screen edge lands on it), else (the pointer parked on the status bar, or
Edit > Paste) it keeps the 8 px offset. A canvas filled to 255 elements refuses a paste or a
duplicate that would pass it, changes nothing and prints `uicanvas: notice Cannot paste...` (the
status bar shows it); one that makes exactly 255 goes through. `editor_tests` covers the same in
the model (`testUiMultiSelect`).

    python3 uicanvascheck.py <splashedit>

`uispriteguicheck.py` drives the Image inspector's Sheet row on a project holding `sprite/`'s two
sheets and a canvas with one plain image: selected on the canvas, its Sheet menu (`ui.sheet`, items
`ui.sheet/None`, `ui.sheet/<sheet>`) picks `hero`, the cell picker drawn over the sheet's texture
(each cell a named control `ui.sheetcell/<n>`) takes cell 6 and Snap size (`ui.snapsize`) makes it
16x16. With `--ui` the editor prints `uicanvas: image sheet <sheet> cell <n> of <cells> grid <c>x<r>
size <w>x<h>` (or `none`) when that changes. The saved scene must have `sheet`/`cell` and the rect,
the canvas at 100% must show hero's cell 6 there, one Ctrl+Z takes back Snap size and another the
cell, and None saves a plain image without the keys.

    python3 uispriteguicheck.py <splashedit>

`uifontscheck.py` drives the canvas inspector's Scene fonts section on the same copy of
`ui/ui.scene`. With `--ui` the editor prints each font as `uicanvas: fonts: <i> <name> ttf <source>
<size>px vram <bytes>` (or `bitmap <png> <w>x<h>`, or `error <why>`), `uicanvas: fonts: count <n>
add enabled|disabled` and `uicanvas: fonts: rename refused: <why>`, each when it changes. It checks
both fonts' lines and that each sample line draws ink, changes a size (the line and its VRAM
follow), renames a font (the text element that uses it follows in the saved scene) and is refused
a name another font has, adds a third font (Add font then reports disabled and a click on it adds
nothing), deletes one (its text element goes back to the system font) and takes that back with
Ctrl+Z. `editor_tests` covers the model (`testUiFonts`).

    python3 uifontscheck.py <splashedit>

`cvfontcheck.py` drives the canvas inspector's Default font row on the same copy of `ui/ui.scene`.
With `--ui` the editor prints `uicanvas: default font <name|none>` and, for a selected text element,
`uicanvas: element font <shown> width <px>` (the text's width in the font it resolves to), each when it changes. It checks the row and its menu (the system
font and each scene font), picks `title` (reported, saved as the canvas's `defaultFont`, the text
elements' own fonts untouched), that the fontless text element `sys` then shows `Canvas default
(title)` and is measured in that font, and its Font menu offers that instead of the system font (and, without a default, shows
and offers the system font), picks System again (saved as null) and takes a pick back with Ctrl+Z.
`editor_tests` covers the model and the export (`testUiCanvasDefaultFont`, `testExportCanvasDefaultFont`).

    python3 cvfontcheck.py <splashedit>

`uicanvassmallcheck.py` opens the same UI tab at a small editor window (`--size 1024x768`) and reads
the canvas header's layout, which the editor prints with `--ui` (each item as `x0-x1`, or `-` when it
gives way). At 1024x768 the resolution and the hint text step aside so the toolbar (fit, zoom in and
out, the zoom label, the snap switch) still fits, and the check asserts the buttons are present and
that no two header items overlap; it also requires the screen to fit at better than 25%. Widened back
to 1600x960, the resolution and the hint must both show again. Needs a display (CI runs it under
`xvfb-run`).

    python3 uicanvassmallcheck.py <splashedit>

`uitexpreviewcheck.py` checks the UI tab's inspector shows an Image element's texture as the mesh
inspector does: Source next to the PS1 image at the element's bit depth and cutout. The scene is a
copy of `ui/ui.scene` whose image uses a generated 32 x 32 RGBA texture (a transparent left quarter,
a gradient elsewhere) at 16 bpp with cutout on. With `--ui` the editor prints
`uicanvas: texpreview ...` whenever the preview changes (size, bit depth, cutout, PS1 colours and
the panes' screen rectangles). Selecting the image must show it, selecting the progress bar must
not; picking 4 bpp in the Bit depth menu must re-quantize the PS1 pane (at most 16 colours, its
pixels change, the Source pane's do not), and turning Cutout off must change the transparent quarter
of the PS1 pane; the saved scene must carry both. Needs a display (CI runs it under `xvfb-run`).

    python3 uitexpreviewcheck.py <splashedit>

`meshalphacheck.py` checks a mesh texture with an alpha hole keeps it. It builds a temp project with
one 4 x 4 quad on a green background, textured by a generated 32 x 32 red image whose middle 16 x 16
texels are alpha 0, at 16 bpp. The viewport centre must show green through the hole and red must show
around it; the mesh inspector must print `texture: holes yes <path>` and draw the `obj.texholes`
caption; `splashpack-cli export --stats` must report the texture cutout, with every hole texel VRAM
word 0x0000 and no other. The same quad with an opaque texture is the control (`texture: holes no`,
no caption, red in the middle, no word 0). Needs a display (CI runs it under `xvfb-run`).

    python3 meshalphacheck.py <splashedit> <splashpack-cli> [screenshot.png]

`statusbudgetcheck.py` checks the status bar's Triangles meter and what the meters show when the
live export fails. It builds a temp project with one flat grid mesh: 1700 triangles must print
`statusbar: tris 1700/2000 warn` (the most triangles in view against the psxsplash docs' 2000 per
frame) and 2100 `statusbar: tris 2100/2000 bad`, with a `statusbar: vram` line as the control. Then
it clicks Enable saves on a scene whose memory card product code is empty, which fails the export:
the bar must print `statusbar: export failed, meters from last good` and keep the tris line.
Needs a display (CI runs it under `xvfb-run`).

    python3 statusbudgetcheck.py <splashedit> [screenshot dir]

`gamekeys.sh` starts Play against a stand-in pcsx-redux (`emulator_tests` with
`SPLASHEDIT_FAKE_REDUX` set publishes an empty display block), then presses arrows and Enter in
the Game view. They must reach the game only: the check fails if an editor menu or picker is
open afterwards. Needs a display (CI runs it under `xvfb-run`).

    sh gamekeys.sh <splashedit> <emulator_tests>

`scripteditorcheck.py` opens the courtyard example's `game_logic.lua` with Edit script, types a
line and presses F5, then Ctrl+F5, with the script editor holding the keyboard. Each must save the
tab before building, so the file on disk starts with the typed line; typing alone must not. Then
New Lua, in the scene inspector and on an object whose Script component has no file: it writes
`scripts/<name>.lua` (numbered when taken), sets it, undoes in one step and opens the file; on a
Trigger box it writes `scripts/<name>_trigger.lua` with `onTriggerEnter`/`onTriggerExit`. Then
the editor itself:

- Replace (Ctrl+H): Ctrl+Alt+Enter and the Replace all button replace every match, Match case
  leaves other cases alone, one Ctrl+Z undoes the lot, Enter replaces one match at a time.
  With Regex (`.*`) on, `zzq(\d)` -> `v$1` puts each digit back; Preserve case (`AB`) turns
  `zzq = Zzq + ZZQ` into `yy = Yy + YY`; In selection (`Sel`) replaces only inside the selection.
- Typing `(`, `"` and the rest inserts the closer, typing the closer steps over it, Backspace in
  an empty pair deletes both, and `don't` gets no second quote.
- `--copy-file <from>@<to>` stands in for another program writing the open script. With no edits
  the tab reloads within a second; with edits the bar comes up (its colour in the screenshot),
  Reload takes the file, Keep mine keeps the edits (typing goes on where it was) and saves over
  it, and without an answer the file is left alone.
- `--delete-file <path>` stands in for another program deleting it: the tab reads `(deleted)`
  (its strip differs from a run without), a Save / Close bar comes up, Save writes the text
  again and Close drops the tab without writing anything.
- With completion open at the far right of a line, the inspector's pixels match a run without it:
  the list and its docs card stay inside the editor panel.

Needs a display (CI runs it under `xvfb-run` on Linux).

    python3 scripteditorcheck.py <splashedit>

`exportflipcheck.py` lets the viewport draw the courtyard example, then exports a disc with
Ctrl+B and compares its `SCENE_0.{SPK,VRM,SPU}` with those of `splashpack-cli disc` byte for
byte. The viewport loads its textures bottom row first; export must still decode them top row
first. Needs a display (CI runs it under `xvfb-run` on Linux).

    python3 exportflipcheck.py <splashedit> <splashpack-cli>

`memcardeditcheck.py` edits the courtyard example's memory card settings in the scene inspector:
the Enable saves switch, then the Save title field (Enter or clicking away commits, Escape
cancels), saves with Ctrl+S and reads `settings.memCard` back from the scene. Saving alone must
write no `memCard`, and one Ctrl+Z must take the title back and leave saves on. It folds the
sections above and clicks the controls by name (`--click @s_render`, `@scene.mcenable`). Needs a display.

    python3 memcardeditcheck.py <splashedit>

`spritesheetcheck.py` drives the scene inspector's Sprite sheets section (its last) on copies of the
courtyard example with `sprite/hero.png` added to its textures: Add sheet picks the image and makes
sheet `hero` (4 bpp, 16x16 cells, one `idle` animation); a typed cell width, an added animation and
its typed frames and frame time land in the saved scene's `spriteSheets` and in the section's
`scene: sprite <i> <name> texture ... cell WxH grid CxR anims <name>:<first>+<count>@<vsyncs>[L]`
line. Each of those edits goes back in one Ctrl+Z (Add sheet's too, leaving no `spriteSheets` key).
The preview's `scene: sprite preview <sheet>/<anim> cell N` lines after the last edit must step through 1, 2, 3 and
wrap; a 24 px cell on the 64 px texture must show in `scene: sprite problems` and fail the live
export; the sheet's trash deletes it. Controls are checked to be on screen. A second argument saves
the screenshot of the edited section.

    python3 spritesheetcheck.py <splashedit> [shot.png]

`scenefogcheck.py` drives the scene inspector's Rendering section on copies of the courtyard example:
the Fog switch, Density typed (and clamped to 10), GTE scaling typed, Scene type picked from its
menu (`@scenetype/Interior`), then Ctrl+S, reading `settings.fog`, `gteScaling` and `sceneType` back
from the saved scene and the inspector's `scene: fog ...` line from its output. One Ctrl+Z each must
take back the scene type, the typed density and the switch. With the background set to #20C040 the
viewport must clear to it with fog off as well as on (psxsplash clears to it every frame), and the
`culling` caption must read `frustum` with no Room, `rooms 2` with two, and not count a switched-off
Room. Controls are clicked by name. Needs a display.

    python3 scenefogcheck.py <splashedit>

`materialscheck.py` drives the mesh inspector on copies of the courtyard example. A one-material
Crate keeps its Texture row (`obj.texture`), with a Colour row (`obj.matcol.0`) only when it has no
texture. Given three materials (crate.png, a missing textures/nope.png, untextured) it must draw a
row per material (`obj.texture.<i>`) inside `obj.panel`, the File not found card `obj.fixtex.1`, the
caption and a Colour row for the third only, and print `materials: 3 textured 2` and a
`material <i>: texture <name|none> colour #RRGGBB` line each. A colour picked in the third's
picker (`@obj.matcolpick.2`) is saved on that material only and one Ctrl+Z puts it back; brick.png
dragged from the Project panel onto Material 3 sets that material only. An optional PNG path keeps
the three-material screenshot.

    python3 materialscheck.py <splashedit> [shot.png]

`loadingscreencheck.py` drives the scene inspector's Loading screen section on copies of the courtyard
example given two UI canvases, Title (a progress bar named `loading`) and Bare (no bar, a Line): picks
each from the Canvas menu (`@scene.loading/Title`), then None, saves with Ctrl+S and reads
`settings.loadingCanvas` back, and reads the inspector's `scene: loading ...` and `scene: loading
warnings ...` lines (Bare: `bar,line`; Title: none). One Ctrl+Z takes a pick back. Two optional PNG
paths keep the Title and Bare screenshots. Needs a display.

    python3 loadingscreencheck.py <splashedit> [title.png [bare.png]]

`buildscenescheck.py` drives the scene inspector's Build scenes section on copies of the courtyard example
with a second scene, `level2.scene`, beside it: adds both from the Add menu (`@build.add/level2.scene`),
moves level2 up (`@build.up.1`) and removes the other (`@build.remove.1`), checking the editor's
`build: 0 a.scene | 1 b.scene` line after each change, `scenes.list` and what the Add menu still offers;
then adds level2 to an empty list, which puts the open scene first, and checks Ctrl+Z leaves the list and
the scene unchanged. Controls are clicked by name. Needs a display.

    python3 buildscenescheck.py <splashedit>

`newprojectcheck.py` drives File > New Project (Ctrl+N): checks the modal draws a card per template
(`@newproj.tmpl.empty`, `@newproj.tmpl.first-person`, `@newproj.tmpl.third-person`) and its name,
location, Choose folder..., Create and Cancel controls; that Create does nothing without a name and
Cancel closes it; then, for each template, types a name and a temp location and clicks Create, checking
the `newproject: created <template id> <scene>` line, the scene on disk and opened, and for First and
Third person a `.lua` and a `.blocks` script copied (template.json left behind). A folder that already
holds a scene prints `newproject: failed ...` and keeps the modal open; with unsaved changes the Save
changes prompt comes first. The optional PNG is the modal with Third person picked. With the
Lamplighter sample fetched (`tools/fetch-samples.sh templates`) it also checks its card and that a
project made from it prints `newproject: created lamplighter title.scene` (the first entry of its
`scenes.list`), holds its four scenes, and that each exports with the `cli/splashpack-cli` beside the
editor; unfetched, that fails unless `--allow-missing` is given. `--sample-shots <dir>` writes the modal
with Lamplighter picked and the project it made. Needs a display.

    python3 newprojectcheck.py <splashedit> [modal.png] [--allow-missing] [--sample-shots <dir>]

`templatecheck.py` checks the shipped templates (`templates/<id>/`, made by `make_templates`): each
template.json has a name, description and order, and each scene exports with the CLI without warnings,
has a Player and nav regions, and attaches at least one Lua and one blocks script, the blocks
converting to Lua; Third person's Lua camera script takes the camera over.

    python3 templatecheck.py <splashpack-cli>

`samplecheck.py` checks the sample games pinned in `templates/samples.lock`, as `tools/fetch-samples.sh`
copies them into a templates folder (the tree's by default): Lamplighter's template.json is one New
Project lists, by the name Lamplighter; its four scenes are at the top and its `scenes.list` starts on
title.scene; each exports with the CLI and leaves the template untouched. A sample not fetched fails
unless `--allow-missing` is given.

    python3 samplecheck.py <splashpack-cli> [<templates-dir>] [--allow-missing]

`buildproblemscheck.py` presses F5 on courtyard copies whose `scenes.list` adds `level2.scene` (memory
card warnings), `level3.scene` (memory card errors) or `zbroken.scene` (not a scene), with pcsx-redux and
the engine set to `/bin/true`. It checks the editor's `build report: Play scenes N errors E warnings W`,
`problems: <pill>` and `playstatus: <status line>` lines: other scenes' problems are counted, the open
scene's own are counted once, the status line leads with the error count and stops short of the pill,
the Problems popup gets its `problems.build` heading, and a second build replaces the report. `--wait-play` holds the script while the build runs and
`--wait-export` until the live export lands, so the pill has its count before the click opens the popup.
Needs a display.

    python3 buildproblemscheck.py <splashedit> [screenshot.png]

`vramlayoutcheck.py` drives the scene inspector's VRAM layout section on copies of the courtyard example:
Add area (`@vram.add`, a 64 x 64 area at 320,0), types a width into `@vram.w.0`, drags on the map
(`--drag @vram.map,dx0,dy0,dx1,dy1`, offsets from the control's centre) to draw a second area snapped to
8, and removes the first (`@vram.remove.0`), checking the editor's `vram: x,y,w,h | x,y,w,h` line after
each change, `vram.json`, that it is no scene edit, and that the screenshot's map shows the area left in
red (the map is the live export's, so this proves the live export packs with `vram.json`). Needs a display.

    python3 vramlayoutcheck.py <splashedit>

`scripttabscheck.py` clicks the centre panel's tabs by name (`@tab.scene`, `@tab.ui`, `@tab.0`) on copies of
the courtyard example: back to Scene from an open script and from UI, and UI, the script and Scene in turn.
The editor prints `scripttabs: active <tab>` each time a click brings a tab to the front. Needs a display.

    python3 scripttabscheck.py <splashedit>

`blockseditorcheck.py` drives the block editor on copies of the courtyard example whose Game Logic runs
an empty block script (`blocks/editor/empty.blocks`): Edit script opens it in a blocks tab, "on_interact"
is dragged from the palette onto the canvas and "hide" (after the Objects chip) under it, where it must
snap; its field is typed to "Door" and Ctrl+S saves. The file must read exactly as the expected JSON and
`splashpack-cli blocks` must make the expected Lua. Then Ctrl+Z / Ctrl+Y, undoing back to the empty
script, Delete on a selected block, and a drop away from the hat that must make a second stack. Blocks
are aimed at by name (`@blocks.palette.<op>`, `@blocks.block.<path>`, `@blocks.field.<path>`); a first
run reads where they are drawn. Last, Convert to Lua from the blocks tab's right-click menu: Convert
must print `blocks: converted scripts/door.blocks -> scripts/door.lua`, write the Lua `splashpack-cli
blocks` makes and remove the `.blocks`, the tab becoming a Lua tab whose menu has no Convert row;
Cancel leaves the file alone; an errored script is refused (`blocks: convert refused: ...`). Needs a
display.

    python3 blockseditorcheck.py <splashedit> [<splashpack-cli>]

`blockslightcheck.py` drives the light and Persist blocks on copies of the courtyard example with three
lights added (a runtime point light, a baked one, a runtime one on an inactive object): "turn light ... on"
from the Objects section must offer only the active runtime light in its field's list (what Light.Find can
reach), "remember ... as ..." from Variables gets its name typed, and the saved file must make the expected
Lua through `splashpack-cli blocks`, counted by the Lua panel's scripted line. A light name with no runtime
light stays in the list. The Light section's Runtime toggle (`obj.lruntime`) puts a light in the picker and
Ctrl+Z takes it out; with no runtime lights the picker's one row says to turn Runtime on; with 18 runtime
lights the inspector warns (`obj.lruntimemax`, scripted line `light: ...`) and its fix turns the light's off.
With `--shot <png>` it also saves a picture of `blocks/beacon.blocks` in the tab, Beacon Light selected.

    python3 blockslightcheck.py <splashedit> [<splashpack-cli>] [--shot <png>]

`newscriptcheck.py` drives New blocks and New Lua on copies of the courtyard example: New blocks in the
scene inspector, on Game Logic's Script component with no file (game_logic.lua takes the stem, so it
writes `game_logic_2.blocks`) and on a Trigger box writes `scripts/<name>.blocks` with the starter's
hats, the saved scene names it and a blocks tab opens; New Lua still writes a `.lua`. The Project
panel's background menu writes `new_script.blocks` / `new_script.lua`. Then block help: a palette
block under the pointer, a canvas block under it and a selected one must show in the status line
(the editor prints `blocks: help <op>`). Needs a display.

    python3 newscriptcheck.py <splashedit>

`compfieldscheck.py` drives the object inspector's component rows on copies of the courtyard example
(components are added to the copied scene before launch): Interactable Button (picked from its menu),
Repeatable, Cooldown, Show prompt and Prompt canvas; Player Sprint speed, Gravity, Step height, Max
slope, Cell size and Cell height, and the same nav rows on a Navigation component (the voxel-count
caption is compared against float32 arithmetic, and `build/cli/splashpack-cli export --stats` on the
saved scene must report the edited cell size as the one the nav bake was asked for); Audio Name, Sample rate, Loop and Trim silence; Mesh Smooth normals; Spot light Inner angle. Each
run saves with Ctrl+S and reads the fields back from the saved scene; Ctrl+Z must take back the last
edit and keep the rest. Controls are clicked by name. Needs a display.

    python3 compfieldscheck.py <splashedit>

`roomscheck.py` splits a copy of the courtyard example into two Rooms (West, East) with a Door portal
between them. It adds a Room and a Portal through the Add component picker, picks the portal's Room A
and Room B from their dropdowns (`obj.pa`, `pa/West`), types its width, and reads the saved scene and
the `portal: Door a=... b=... WxH [problems N]` lines the editor prints under scripted runs; one Ctrl+Z
takes the width back. With the portal linked, West's `room: West N triangles, M cells, K portals` line
must follow a first-frame placeholder: `SPLASHEDIT_EXPORT_DELAY_MS` stretches the live export so a fast
build still shows the placeholder, and `--wait-export` holds the script until the export matches the
scene. A Room switched off (East), and a Room that is on under an off parent (Wing > Annex), are
listed `<name>  (off)` in the pickers; Annex's hierarchy label is as dim as Wing's (screenshot pixels); selecting Annex draws the inspector caption `obj.inheritedoff`
inside `obj.panel`, prints `object: Annex off via Wing` once, and its room line names Wing once the
export lands. Last, the scene view is shot with the overlay toggle (`vp.gizmos`)
on and off and the room boxes and portal opening are counted by colour, as in `gizmocheck.py`.

    python3 roomscheck.py <splashedit> [keep.png]

`bvhcheck.py` opens a copy of the courtyard example, folds the Scene inspector's sections above BVH
and opens BVH (`s_bvh`, closed by default). The editor prints `bvh: nodes N leaves L depth D` once the
live export lands, and `bvh: hidden` or `bvh: showing depth K boxes B` for what the viewport draws:
`bvh.show` must draw all N boxes at depth D, typing 0 into `bvh.depth` must leave one box, and the
toggle again hides them. The scene view shot with the boxes hidden and shown must differ.

    python3 bvhcheck.py <splashedit> [keep.png]

`inspectorscrollcheck.py` gives two objects on a copy of the courtyard example a spot Light, an
Interactable and an Audio clip, so their inspector is taller than the panel. Add component starts below
the panel; the wheel over the inspector brings it into view and a click on it opens the picker; and
inspecting the other object, as tall, starts at the top again. Rows are found by name (the scene
tree's rows are `tree.<object name>`). Needs a display.

    python3 inspectorscrollcheck.py <splashedit>

`gizmocheck.py` adds a Trigger (Gate) and an Interactable (Lever) to a copy of the courtyard example
and shoots the scene view twice, with the viewport's overlay toggle (`vp.gizmos`) on and off. Inside the
viewport (`vp.view`), each pixel the "on" shot moved towards an overlay's colour counts for that
overlay: the Trigger box, the Interactable's reach ring and the export's nav regions must each show up.

    python3 gizmocheck.py <splashedit> [keep.png]

`walkoffcheck.py` adds a Nav walk-off zone from the Add component picker to an empty object (Ledge) over
the courtyard floor's front edge and types its Size (`obj.wsize.x/.y/.z`). The editor prints
`walkoff: Ledge edges N` from the live export: the default box floats above the floor (0, with the
warning `obj.wstats` inside the section), 20 x 4 x 1 straddles the edge (N > 0), 20 x 1 x 1 floats again
(0). The saved scene keeps the size, and the scene view draws the box in orange only with `vp.gizmos` on.
The saved scenes are exported again with `splashpack-cli export --stats`, whose `walkoff` lines give
each zone's edges and the walk-off bits in the written nav regions (`mask_edges`): the box must add
exactly the edges the inspector reported.

    python3 walkoffcheck.py <splashedit> [keep.png]

`agenteditcheck.py` adds an Agent from the Add component picker to an empty object (Guard, turned 45
degrees) on a copy of the courtyard example, types Move speed (`obj.agspeed`), adds waypoints with
`agaddobj` (the object's world position) and `agaddview` (under the view centre, at the object's
height), types `obj.agwp1.x`, then reorders (`obj.agwp1.up`), deletes (`obj.agwp0.del`) and undoes.
The editor prints `agent: <name> enabled on speed ... waypoints N clips ... bytes B warnings K`; the
saved scene must hold each edit. Dragging the waypoint dots in the viewport (`vp.agwp<i>`) moves them on
their own height, the first one even where it sits on the move gizmo's centre (the Guard stays put).
The viewport draws the vision cone, hearing square and patrol in pink only with `vp.gizmos` on.

    python3 agenteditcheck.py <splashedit> [keep.png]

`cliploopcheck.py` opens a copy of `tests/skin` (bend.anim loops, raise.anim plays once) with an Agent
on Arm whose Patrol plays raise. The Skinned animation section's switch per clip
(`obj.skinloop.<i>`) rewrites the clip's `.anim` loop as one undo step: the file must say so, the
export bake raise again as a loop (another frame count, `skin: clip raise frames F bytes N loops`), and
Ctrl+Z put it all back. The Agent section's "Clip plays once" card (`obj.agonce<i>`, printed as
`agent: Arm plays once: raise`) shows while a held state plays a one-shot clip and goes once the clip
loops, by the switch or by the card's fix.

    python3 cliploopcheck.py <splashedit> [shot.png]

`timelinecheck.py` adds a cutscene to a copy of the courtyard example and opens it in the Timeline
panel (`--timeline intro`, and once through the scene inspector's Edit in timeline). It scrubs the
ruler, double-clicks a lane, drags a key, deletes a selected key with Delete while an object is
selected, and undoes; each run saves with Ctrl+S and the check reads the saved scene: the new key
sits at the scrub frame with the value the track had there, the dragged key keeps its value and
curve, and Delete takes the key, not the object. Needs a display.

    python3 timelinecheck.py <splashedit>

`timelinepreviewcheck.py` checks the Scene view's preview of the Timeline panel: on a copy of the
courtyard example with a cutscene that lifts and turns the Crate, it scrubs to frame 0 and to 75 and
compares the Scene view's pixels in the two screenshots, takes one more (`--shot-at 6`) on the very
frame the scrub click presses and expects the frame 75 pose already there, saves while previewing and
compares the file byte for byte with a save made without the timeline, and drags the Crate's move
gizmo: at frame 0 (the scene's own pose) the drag moves it, at frame 75 (posed) the gizmo is hidden
and the same drag writes nothing. Needs a display.

    python3 timelinepreviewcheck.py <splashedit>

`pasteoffsetcheck.py` copies the courtyard example's Crate and pastes it three times: each copy
must land one grid step (0.25 m) further along X, none on the original. A pasted group steps off
its original with its children in place; Cut then Paste keeps the object where it was; Ctrl+Z
takes a paste back. Reads the saved scene. Needs a display (CI runs it under `xvfb-run` on Linux).

    python3 pasteoffsetcheck.py <splashedit> [screenshot.png]

`importwarningscheck.py` drops `skinimport/rig.gltf` (blended skin weights and cubic-spline keys,
two import warnings) on the courtyard example's viewport. The status bar must count the warnings
and point at the inspector, and the inspector's Import settings must list both: the editor prints
the warnings it drew with the screenshot, and the screenshot must show their warning marks. Needs
a display (CI runs it under `xvfb-run` on Linux).

    python3 importwarningscheck.py <splashedit> [screenshot.png]
`timelineeditcheck.py` checks the Timeline's edits beyond keys on copies of the courtyard example
with two cutscenes: renaming one in the scene inspector (double-click the name) and from the
timeline's sequence picker, undo of a rename, a taken name and a 25-character one refused, the
rename rewriting the scene script's `Cutscene.Play("intro")` calls and a variable holding the name
that the script plays (an "intro" that is only dialogue left alone and listed, path:line, in the
status), deleting
one from the inspector and undoing it, pointing a track at another object from its lane menu (and
undoing that), each read back from the saved scene; the Scene view previewing a light track and a
UV offset track (pixels at frame 0 against frame 75) without touching the scene file; and, with
`tests/skin`'s arm added, a skin clip event naming a clip the arm does not have drawn red in the skin
lane while one naming its `raise` clip stays blue:

    python3 timelineeditcheck.py <splashedit>

`assetbrowsercheck.py` drives the Project panel on copies of the courtyard example. Searching
narrows the grid to the matching files from every folder. A model dragged from the panel onto the
viewport is imported and added as one object, and one Ctrl+Z takes it back; a .mesh dragged onto a
scene tree row becomes that object's child with the material of the objects already drawing it; a
texture dragged onto the inspector's Texture field is set on that object only. F2 on brick.png,
which the walls use, renames it and the walls follow, in the open scene and in a second scene
file on disk (`level2.scene`); one Ctrl+Z puts the file, the scene and `level2.scene` (byte for
byte) back; the rename alone, unsaved, leaves the document with unsaved changes. Renaming
`game_logic.lua` points the Game Logic object's Script at the new name. A file dragged onto a
folder of the panel's tree moves there with the same reference update, and Ctrl+Z moves it back;
a root file dragged onto the `textures` folder tile of the grid moves into it, the Back Wall's
reference along. Delete is refused on a used file and moves an unused one into the project's
`.trash/1/` after the prompt; Ctrl+Z brings it back. Opening a project empties a `.trash/<n>/`
older than 30 days (and keeps a newer one), saying so in the status bar. The chevron folds the
panel and View > Project opens it again. The Timeline tab is clicked low in the dock's tab strip,
which is 46 px high (the Timeline's header) with Project in front too. Needs a display (CI runs it
under `xvfb-run` on Linux).

    python3 assetbrowsercheck.py <splashedit>

`timelineselectcheck.py` checks the Timeline's selections and its audio lane on copies of the
courtyard example with two audio clips: a box dragged over empty lane space selecting keys on two
lanes (Ctrl adding a second box), one key dragged moving them all, Delete removing them all, one
Ctrl+Z taking each back; a box over a key and an audio event moving both; Ctrl+C / Ctrl+V pasting a
key at the scrub head; an audio event added from the lane's clip menu, dragged and deleted, each
undone; and auto-scroll: a key dragged past the lanes' right edge and held moving on past what the
edge alone reaches, a box held there reaching a key at frame 400 the fitted view does not show (Delete
removes it), and after Shift+wheel pans the view, a key held past the left edge following it back to
frame 0. All read back from the saved scene:

    python3 timelineselectcheck.py <splashedit>

`timelinekeyrowcheck.py` drives the Timeline's bottom row on copies of the courtyard example with
tests/skin's Arm and two audio clips. With a key selected: its X, Y and Z typed (one Ctrl+Z takes the
Y back), its CURVE picked (Step, Out, In-out then Ctrl+Z), its FRAME typed, an on/off key flipped
both ways, the row's x deleting it and a two-key selection. With a skin clip event: one added from the
lane's + menu (Arm > bend) and its FRAME typed, the red one whose clip is gone pointed at bend from
CLIP (one Ctrl+Z puts it back), LOOP, and the x. With an audio event: FRAME, CLIP, VOLUME and PAN.
Controls are clicked by name (`--click @timeline.kv1`, `--move @timeline.evclip/Arm`): the editor
aims at where the frame before drew the control, and lists every named control with the end
screenshot (`control <name> at x0,y0 x1,y1`). All read back from the saved scene:

    python3 timelinekeyrowcheck.py <splashedit>

`emulator_tests` (in ctest) stops a child that ignores being asked to close, the way pcsx-redux
without its UI ignores WM_CLOSE on Windows; `stop()` must still return.
