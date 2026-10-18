#!/usr/bin/env bash
# Fetches the sample games pinned in templates/samples.lock into a templates
# folder: for each "<id> <repository> <commit>" line, that commit's project/
# folder becomes <templates-dir>/<id>/ (replacing what was there).
# usage: tools/fetch-samples.sh <templates-dir>
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <templates-dir>" >&2
    exit 2
fi
dest=$1
lock="$(cd "$(dirname "$0")/.." && pwd)/templates/samples.lock"
[ -f "$lock" ] || { echo "fetch-samples: no $lock" >&2; exit 1; }
mkdir -p "$dest"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

count=0
while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    read -r id repo sha rest <<< "$line"
    case "$id" in ''|'#'*) continue ;; esac
    if [ -z "$repo" ] || [ -z "$sha" ] || [ -n "$rest" ]; then
        echo "fetch-samples: bad line in samples.lock: $id $repo $sha $rest" >&2
        exit 1
    fi
    src="$work/$id"
    git init -q "$src"
    git -C "$src" fetch -q --depth 1 "$repo" "$sha"
    git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD
    got=$(git -C "$src" rev-parse HEAD)
    if [ "$got" != "$sha" ]; then
        echo "fetch-samples: $id: fetched $got, not $sha" >&2
        exit 1
    fi
    if [ ! -f "$src/project/template.json" ]; then
        echo "fetch-samples: $id at $sha has no project/template.json" >&2
        exit 1
    fi
    rm -rf "${dest:?}/$id"
    cp -R "$src/project" "$dest/$id"
    if [ ! -f "$dest/$id/template.json" ]; then
        echo "fetch-samples: $id: nothing copied to $dest/$id" >&2
        exit 1
    fi
    echo "fetch-samples: $id at $sha -> $dest/$id"
    count=$((count + 1))
done < "$lock"

if [ "$count" -eq 0 ]; then
    echo "fetch-samples: samples.lock lists no samples" >&2
    exit 1
fi
