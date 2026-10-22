#!/bin/bash
# Checks that the psxlua the editor compiles Lua with is the one the engine runs.
# usage: psxluapin.sh <psxsplash checkout, submodules in> <editor CMakeLists.txt>
# The engine's psxlua is the gitlink psxsplash HEAD records for third_party/nugget,
# then the one that nugget commit records for third_party/psxlua. The editor's is
# the GIT_TAG in CMakeLists.txt's FetchContent_Declare(psxlua ...) block. Exit 0 if
# they are the same commit, 1 if they differ, 2 if either cannot be read.
set -u
if [ $# -ne 2 ]; then echo "usage: $0 <psxsplash-dir> <CMakeLists.txt>" >&2; exit 2; fi
dir=$1; cml=$2
err() { echo "psxluapin: error: $*" >&2; exit 2; }
ishash() { [[ $1 =~ ^[0-9a-f]{40}$ ]]; }

[ -r "$cml" ] && [ -f "$cml" ] || err "cannot read $cml"
nugget=$(git -C "$dir" rev-parse --verify -q HEAD:third_party/nugget 2>/dev/null) ||
  err "no third_party/nugget gitlink at HEAD of $dir"
ishash "$nugget" || err "bad nugget gitlink in $dir: $nugget"
[ -d "$dir/third_party/nugget" ] || err "$dir/third_party/nugget is not checked out (submodules)"
engine=$(git -C "$dir/third_party/nugget" rev-parse --verify -q "$nugget:third_party/psxlua" 2>/dev/null) ||
  err "nugget $nugget is not in $dir/third_party/nugget or records no third_party/psxlua"
ishash "$engine" || err "bad psxlua gitlink in nugget $nugget: $engine"

# Only the GIT_TAG inside FetchContent_Declare(psxlua ...), up to its closing paren.
found=$(awk '
  !inb && /FetchContent_Declare[ \t]*\([ \t]*psxlua([ \t]|$)/ { inb = 1 }
  inb && match($0, /GIT_TAG[ \t]+[^ \t)]+/) {
    t = substr($0, RSTART, RLENGTH); sub(/GIT_TAG[ \t]+/, "", t); print NR, t; n++ }
  inb && /\)/ { inb = 0; blocks++ }
  END { if (blocks != 1 || n != 1) exit 1 }' "$cml") ||
  err "no single FetchContent_Declare(psxlua ...) block with one GIT_TAG in $cml"
line=${found%% *}; editor=${found#* }
ishash "$editor" || err "$cml:$line: psxlua GIT_TAG is not a full commit hash: $editor"

echo "engine psxlua: $engine (psxsplash $(git -C "$dir" rev-parse --short HEAD), nugget ${nugget:0:12})"
echo "editor psxlua: $editor ($cml:$line)"
if [ "$engine" = "$editor" ]; then echo "psxluapin: ok, the same psxlua"; exit 0; fi
echo "psxluapin: MISMATCH: the engine runs psxlua $engine but the editor compiles with $editor;" \
  "bump GIT_TAG at $cml:$line to $engine (and check tests/lua/expected)" >&2
exit 1
