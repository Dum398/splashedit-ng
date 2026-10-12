#!/bin/sh
# Play against a stand-in pcsx-redux, then the keys a player uses: arrows and
# Enter in the Game view must reach the game only, never open an editor menu.
# usage: tests/gamekeys.sh <splashedit> <emulator_tests>
set -u
editor=$1 fake=$2
out=$(mktemp -d)
SPLASHEDIT_FAKE_REDUX=1 SPLASHEDIT_REDUX=$fake SPLASHEDIT_PSXSPLASH=$fake "$editor" --screenshot "$out/keys.png" \
    --key f5 --wait 120 --click 760,700 --key up --key up --key down --key enter --wait 10 > "$out/log.txt" 2>&1
cat "$out/log.txt"
grep -q '^game attached: yes$' "$out/log.txt" || { echo "FAIL: the Game view never attached"; exit 1; }
grep -q '^popups open: no$' "$out/log.txt" || { echo "FAIL: arrows and Enter in the Game view opened an editor popup"; exit 1; }
echo "PASS: game keys stay in the game"
