#!/bin/sh
# Adds SplashEdit to the applications menu for this user, starting the editor
# from where this folder is now. Run it again after moving the folder.
set -e
here=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
for icon in "$here"/share/icons/hicolor/*/apps/splashedit.png; do
    size=$(basename "$(dirname "$(dirname "$icon")")")
    mkdir -p "$data/icons/hicolor/$size/apps"
    cp "$icon" "$data/icons/hicolor/$size/apps/splashedit.png"
done
# A quoted Exec value escapes " ` $ \ with a backslash, which is itself
# written \\, and % is written %%.
exe=$(printf '%s' "$here/splashedit" | sed -e 's/[\\"`$]/\\\\&/g' -e 's/%/%%/g')
mkdir -p "$data/applications"
while IFS= read -r line; do
    case $line in
        Exec=*) printf 'Exec="%s"\n' "$exe" ;;
        *) printf '%s\n' "$line" ;;
    esac
done < "$here/share/applications/splashedit.desktop" > "$data/applications/splashedit.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$data/applications" || true; fi
echo "SplashEdit is in the applications menu: $data/applications/splashedit.desktop"
