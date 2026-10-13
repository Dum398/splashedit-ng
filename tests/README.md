# Tests

## Format check against Unity SplashEdit 2.4.0

The exporter is not a port of 2.4.0 and does not try to match its output where 2.4.0 does worse
(textures, see below). The 2.4.0 exports are kept as a check that the file layout has not drifted.

`unity/NgParity.cs` builds small scenes in Unity, exports them with 2.4.0 and dumps each one as a
scene file (`unity/README.md` has the setup). `splashpack-cli` exports the dump, and
`splashdiff.py` compares the two section by section, matching objects by name.

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
palette minimises OKLab distance. Not yet checked: these textures rendered
by psxsplash on the console or an emulator.

## Boot check

`boot/boot.sh` boots psxsplash (a PCdrv build) on one exported scene in pcsx-redux, headless, and
saves the screen at a given frame (default 600). The three files are copied to a temporary
directory as `scene_0.*`, the names the PCdrv loader opens.

    boot/boot.sh <pcsx-redux> <openbios.bin> <psxsplash.ps-exe> <dir>/scene shot.png [frame]

`boot/discboot.sh` does the same for a disc image from `splashpack-cli disc` (a CD-ROM build of
psxsplash, `make LOADER=cdrom`), booted from its .cue through the BIOS and SYSTEM.CNF.

    boot/discboot.sh <pcsx-redux> <openbios.bin> image.cue shot.png [frame]

Courtyard at frame 900 is pixel-identical between the disc and the PCdrv boot.

`disccheck.py` builds a disc of the courtyard example with stand-in engines and reads it back: sync,
BCD address, subheader, EDC on every sector, P/Q parity on a sample (recomputed in Python from the
Reed-Solomon generator), the ISO 9660 tree, SYSTEM.CNF in the first root sector, and the scene files
byte-equal to `export --lua-bytecode`. A PCdrv engine must be refused.

    python3 disccheck.py <splashpack-cli>

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
and unknown keys survive. Bad versions and malformed mesh arrays must fail to load.

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

    python3 skincheck.py <splashpack-cli>

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

`gamekeys.sh` starts Play against a stand-in pcsx-redux (`emulator_tests` with
`SPLASHEDIT_FAKE_REDUX` set publishes an empty display block), then presses arrows and Enter in
the Game view. They must reach the game only: the check fails if an editor menu or picker is
open afterwards. Needs a display (CI runs it under `xvfb-run`).

    sh gamekeys.sh <splashedit> <emulator_tests>

`scripteditorcheck.py` opens the courtyard example's `game_logic.lua` with Edit script, types a
line and presses F5, then Ctrl+F5, with the script editor holding the keyboard. Each must save the
tab before building, so the file on disk starts with the typed line; typing alone must not. Then
New script, in the scene inspector and on an object whose Script component has no file: it writes
`scripts/<name>.lua` (numbered when taken), sets it, undoes in one step and opens the file. Needs
a display (CI runs it under `xvfb-run` on Linux).

    python3 scripteditorcheck.py <splashedit>

`exportflipcheck.py` lets the viewport draw the courtyard example, then exports a disc with
Ctrl+B and compares its `SCENE_0.{SPK,VRM,SPU}` with those of `splashpack-cli disc` byte for
byte. The viewport loads its textures bottom row first; export must still decode them top row
first. Needs a display (CI runs it under `xvfb-run` on Linux).

    python3 exportflipcheck.py <splashedit> <splashpack-cli>

`memcardeditcheck.py` edits the courtyard example's memory card settings in the scene inspector:
the Enable saves switch, then the Save title field (Enter or clicking away commits, Escape
cancels), saves with Ctrl+S and reads `settings.memCard` back from the scene. Saving alone must
write no `memCard`, and one Ctrl+Z must take the title back and leave saves on. Needs a display.

    python3 memcardeditcheck.py <splashedit>

`timelinecheck.py` adds a cutscene to a copy of the courtyard example and opens it in the Timeline
panel (`--timeline intro`, and once through the scene inspector's Edit in timeline). It scrubs the
ruler, double-clicks a lane, drags a key, deletes a selected key with Delete while an object is
selected, and undoes; each run saves with Ctrl+S and the check reads the saved scene: the new key
sits at the scrub frame with the value the track had there, the dragged key keeps its value and
curve, and Delete takes the key, not the object. Needs a display.

    python3 timelinecheck.py <splashedit>

`emulator_tests` (in ctest) stops a child that ignores being asked to close, the way pcsx-redux
without its UI ignores WM_CLOSE on Windows; `stop()` must still return.
