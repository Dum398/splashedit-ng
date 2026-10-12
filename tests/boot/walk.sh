#!/bin/bash
# Walk the player through a streamed world and check that the camera moves and
# that walking loads regions standing still does not.
# usage: walk.sh <splashpack-cli> <pcsx-redux> <bios> <psxsplash.ps-exe> [frame]
# The engine must be a PCdrv build made with `make STREAMLOG=1`, which prints
# "WORLDSTREAM attach/detach" lines.
set -eu
cli=$1; redux=$2; bios=$3; exe=$4; frame=${5:-3000}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The courtyard with its floor stretched to 450 x 375 m and the player near a
# side: a big world at the default nav cell size, which used to bake no nav
# regions, and so no walking.
cp -r "$here/../../examples/courtyard" "$work/project"
python3 -P - "$work/project/courtyard.scene" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for o in d['objects']:
    for c in o.get('children', []):
        if c['name'] == 'Floor':
            c['transform']['scale'] = [25.0, 1.0, 25.0]
    if o['name'] == 'Player Start':  # near an edge, so walking crosses into regions not loaded at the start
        o['transform']['position'] = [-200.0, 0.0, -60.0]
d['settings']['streamWorld'] = True
d['settings']['fog'] = {'enabled': True, 'color': [0.5, 0.5, 0.6], 'density': 10}
json.dump(d, open(p, 'w'), indent=1)
PY
"$cli" export "$work/project/courtyard.scene" -o "$work/w.splashpack" --stats > "$work/stats.txt" 2>&1
nav=$(sed -n 's/^nav regions=\([0-9]*\).*/\1/p' "$work/stats.txt")
if [ -z "$nav" ] || [ "$nav" -eq 0 ]; then echo "FAIL: no nav regions"; cat "$work/stats.txt"; exit 1; fi
grep '^nav \|^stream ' "$work/stats.txt"

boot() {  # boot <dir> <hold>
    mkdir -p "$1"
    for ext in splashpack vram spu geo; do cp "$work/w.$ext" "$1/scene_0.$ext"; done
    BOOT_OUT="$1" BOOT_FRAME="$frame" BOOT_HOLD="$2" LIBGL_ALWAYS_SOFTWARE=1 timeout 400 \
      xvfb-run -a -s "-screen 0 1280x720x24" "$redux" -testmode -stdout -lua_stdout -safe \
      -bios "$bios" -loadexe "$exe" -run -pcdrv -pcdrvbase "$1" -dofile "$here/boot.lua" \
      > "$1/log.txt" 2>&1 || true
}
# Two still boots (they must match, or a difference proves nothing) and one
# holding Up from frame 200.
boot "$work/still" "" & boot "$work/still2" "" & boot "$work/walk" "4:200" & wait
for d in still still2 walk; do
    if ! grep -q '^SHOT ' "$work/$d/log.txt"; then echo "FAIL: $d took no screenshot"; head -c 4000 "$work/$d/log.txt"; exit 1; fi
done
attach() { grep -c '^WORLDSTREAM attach' "$1/log.txt" || true; }
s=$(attach "$work/still"); w=$(attach "$work/walk")
echo "regions attached: still $s, walking $w; detached while walking $(grep -c '^WORLDSTREAM detach' "$work/walk/log.txt" || true)"
fail=0
cmp -s "$work/still/shot.raw" "$work/still2/shot.raw" || { echo "FAIL: two still boots differ"; fail=1; }
cmp -s "$work/still/shot.raw" "$work/walk/shot.raw" && { echo "FAIL: holding Up did not move the camera"; fail=1; }
[ "$s" -gt 0 ] || { echo "FAIL: no WORLDSTREAM lines (engine not built with STREAMLOG=1?)"; fail=1; }
[ "$w" -gt "$s" ] || { echo "FAIL: walking loaded no new region"; fail=1; }
[ $fail -eq 0 ] && echo PASS
exit $fail
