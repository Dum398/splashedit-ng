#!/usr/bin/env python3
"""Prove the editor's PS1 texture preview matches what psxsplash uploads to VRAM.

The editor shows, next to a texture, what the console will draw (src/editor/
texture_preview.*: the same splash::convertTexture the exporter calls, decoded
back to colours). This test checks that preview against the real thing: it
exports a scene to a CD-ROM image, boots it in pcsx-redux headless, lifts the
GPU's VRAM out of a save state, reads each texture back through its atlas and
CLUT exactly as the GPU samples it, and compares the 15-bit texels with the
editor's `--dump-texture-preview` output for the same image, bit depth and
cutout. A match on every texel means the editor shows the console's pixels.

Needs the BIOS, so it is NOT in CI (tests/README.md says so). The editor-side
unit test (testTexturePreview in editor_tests) covers the same comparison
against the .vram file and IS in CI.

usage: quantpreviewcheck.py <splashedit> <splashpack-cli> <engine.ps-exe> \
           <pcsx-redux> <bios.bin> [scene] [frame]

  <engine.ps-exe> a CD-ROM build of psxsplash (make LOADER=cdrom)
  scene defaults to tests/quantpreview/quantpreview.scene; frame to 1500
exit 0 = every texel matches, 1 = a mismatch, 2 = could not run
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
# A length-delimited protobuf field 3 (GPUVRam) of exactly 0x100000 bytes:
# tag 0x1a, varint length 0x80 0x80 0x40. The save state is uncompressed, so
# this 4-byte signature locates the 1 MB VRAM blob.
VRAM_SIG = bytes([0x1A, 0x80, 0x80, 0x40])
VRAM_BYTES = 0x100000


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def export_textures(cli, scene, out):
    """Export the scene, return (stdout, [placement dicts]) from --stats."""
    r = run([cli, "export", str(scene), "-o", str(out), "--stats"])
    if r.returncode != 0:
        sys.exit(f"export failed:\n{r.stdout}")
    places = []
    for line in r.stdout.splitlines():
        m = re.match(r"texture bpp=(\d+) cutout=(\d+) size=(\d+)x(\d+) vram=(\d+),(\d+) "
                     r"clut=(\d+),(\d+),(\d+) (.+)", line)
        if m:
            g = m.groups()
            places.append(dict(bpp=int(g[0]), cutout=g[1] == "1", w=int(g[2]), h=int(g[3]),
                               x=int(g[4]), y=int(g[5]), cx=int(g[6]), cy=int(g[7]),
                               clen=int(g[8]), source=g[9]))
    return places


def boot_vram(redux, bios, cue, frame):
    """Boot the disc, return VRAM as a 512 x 1024 uint16 array."""
    with tempfile.TemporaryDirectory() as work:
        env = dict(os.environ, BOOT_OUT=work, BOOT_FRAME=str(frame), LIBGL_ALWAYS_SOFTWARE="1")
        log = Path(work) / "log.txt"
        with open(log, "w") as f:
            subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", redux,
                            "-testmode", "-stdout", "-lua_stdout", "-safe", "-bios", bios,
                            "-iso", str(cue), "-run", "-dofile", str(HERE / "boot" / "vramdump.lua")],
                           stdout=f, stderr=subprocess.STDOUT, env=env, timeout=400)
        text = log.read_text(errors="replace")
        if "STATE " not in text:
            sys.exit("no save state taken; emulator log:\n" + text[:4000])
        data = (Path(work) / "state.bin").read_bytes()
    i = data.find(VRAM_SIG)
    if i < 0:
        sys.exit("VRAM field not found in save state")
    return np.frombuffer(data[i + 4:i + 4 + VRAM_BYTES], dtype="<u2").reshape(512, 1024)


def sample_vram(vram, p):
    """The 5-bit RGB the GPU samples for placement p, as a h x w x 3 array."""
    x, y, cx, cy, bpp = p["x"], p["y"], p["cx"], p["cy"], p["bpp"]
    out = np.zeros((p["h"], p["w"], 3), np.uint16)
    for yy in range(p["h"]):
        for xx in range(p["w"]):
            if bpp == 16:
                w = int(vram[y + yy, x + xx])
            elif bpp == 8:
                idx = (int(vram[y + yy, x + xx // 2]) >> ((xx & 1) * 8)) & 0xFF
                w = int(vram[cy, cx * 16 + idx])
            else:
                idx = (int(vram[y + yy, x + xx // 4]) >> ((xx & 3) * 4)) & 0xF
                w = int(vram[cy, cx * 16 + idx])
            out[yy, xx] = [w & 31, (w >> 5) & 31, (w >> 10) & 31]
    return out


def preview_5bit(editor, image, bpp, cutout, out_png):
    """The editor's PS1 preview for image, as 5-bit RGB."""
    cmd = [editor, "--dump-texture-preview", str(image), str(bpp), str(out_png)]
    if cutout:
        cmd.append("--cutout")
    r = run(cmd)
    if r.returncode != 0 or not out_png.exists():
        sys.exit(f"preview dump failed for {image} at {bpp} bpp:\n{r.stdout}")
    rgb = np.asarray(Image.open(out_png).convert("RGB")).astype(int)
    return ((rgb * 31 + 127) // 255).astype(np.uint16)  # 8-bit back to 5-bit


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    editor, cli, engine, redux, bios = sys.argv[1:6]
    scene = Path(sys.argv[6]) if len(sys.argv) > 6 else HERE / "quantpreview" / "quantpreview.scene"
    frame = int(sys.argv[7]) if len(sys.argv) > 7 else 1500
    for tool in (editor, cli, engine, redux, bios):
        if not Path(tool).exists():
            sys.exit(f"missing: {tool}")

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        places = export_textures(cli, scene, work / "scene.splashpack")
        if not places:
            sys.exit("the scene has no textures to check")
        disc = work / "disc.bin"
        r = run([cli, "disc", str(scene), "--engine", engine, "-o", str(disc)])
        if r.returncode != 0:
            sys.exit(f"disc build failed:\n{r.stdout}")
        vram = boot_vram(redux, bios, work / "disc.cue", frame)

        total = diff = 0
        bad = False
        root = scene.parent
        for p in places:
            runtime = sample_vram(vram, p)
            preview = preview_5bit(editor, root / p["source"], p["bpp"], p["cutout"], work / "prev.png")
            if preview.shape != runtime.shape:
                sys.exit(f"{p['source']}: preview {preview.shape} vs VRAM {runtime.shape}")
            d = int((preview != runtime).any(-1).sum())
            n = p["w"] * p["h"]
            total += n
            diff += d
            mx = int(np.abs(preview.astype(int) - runtime.astype(int)).max())
            print(f"{Path(p['source']).name} {p['bpp']}bpp cutout={int(p['cutout'])}: "
                  f"{n} texels, {d} differ, max 5-bit channel diff {mx}")
            bad |= d != 0

    print(f"total {total} texels compared, {diff} differing")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
