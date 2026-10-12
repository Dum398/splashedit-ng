#!/bin/bash
# Boot psxsplash on one exported scene in pcsx-redux and save what it shows.
# usage: boot.sh <pcsx-redux> <bios> <psxsplash.ps-exe> <scene prefix> <out.png> [frame]
# <scene prefix> names <prefix>.splashpack, <prefix>.vram and <prefix>.spu (and <prefix>.geo if streamed).
set -eu
redux=$1; bios=$2; exe=$3; scene=$4; png=$5; frame=${6:-600}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for ext in splashpack vram spu; do cp "$scene.$ext" "$work/scene_0.$ext"; done
# A streamed scene also reads its regions from the .geo file.
if [ -f "$scene.geo" ]; then cp "$scene.geo" "$work/scene_0.geo"; fi
BOOT_OUT="$work" BOOT_FRAME="$frame" BOOT_HOLD="${BOOT_HOLD:-}" LIBGL_ALWAYS_SOFTWARE=1 timeout 300 \
  xvfb-run -a -s "-screen 0 1280x720x24" "$redux" -testmode -stdout -lua_stdout -safe \
  -bios "$bios" -loadexe "$exe" -run -pcdrv -pcdrvbase "$work" -dofile "$here/boot.lua" \
  > "$work/log.txt" 2>&1 || true
if ! grep -q '^SHOT ' "$work/log.txt"; then
  echo "no screenshot taken; emulator log:" >&2; head -c 4000 "$work/log.txt" >&2; exit 1
fi
python3 - "$work" "$png" <<'PY'
import sys, numpy as np
from PIL import Image
work, png = sys.argv[1], sys.argv[2]
w, h = [int(x) for l in open(work + '/log.txt', errors='replace') if l.startswith('SHOT ') for x in l.split()[1:3]][:2]
raw = open(work + '/shot.raw', 'rb').read()
if len(raw) == w * h * 2:
    d = np.frombuffer(raw, dtype='<u2')[:w * h].reshape(h, w)
    rgb = (np.stack([d & 31, (d >> 5) & 31, (d >> 10) & 31], -1).astype(np.uint16) * 255 // 31).astype(np.uint8)
else:
    rgb = np.frombuffer(raw, dtype=np.uint8)[:w * h * 3].reshape(h, w, 3)
Image.fromarray(rgb).save(png)
print(png, w, h)
PY
