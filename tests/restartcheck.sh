#!/bin/sh
# Restart while a stand-in pcsx-redux runs: the title-bar Restart button and
# Ctrl+Shift+F5 each stop the game and start a fresh build in one step.
# usage: tests/restartcheck.sh <splashedit> <emulator_tests>
set -u
editor=$1 fake=$2
out=$(mktemp -d)
SPLASHEDIT_FAKE_REDUX=1 SPLASHEDIT_REDUX=$fake SPLASHEDIT_PSXSPLASH=$fake "$editor" --screenshot "$out/restart.png" \
    --key f5 --wait 120 --click @restart --wait 120 --key ctrl+shift+f5 --wait 120 > "$out/log.txt" 2>&1
cat "$out/log.txt"
n=$(grep -c '^play: started$' "$out/log.txt")
[ "$n" -eq 3 ] || { echo "FAIL: expected 3 launches (Play, Restart button, Ctrl+Shift+F5), got $n"; exit 1; }
grep -q '^game attached: yes$' "$out/log.txt" || { echo "FAIL: the Game view did not attach after the restarts"; exit 1; }
echo "PASS: Restart stops and relaunches"
