#!/bin/sh
# One step on the PC (Git Bash): build, keep the debug DLL, deploy to the game folder, verify the hash, commit.
# Usage: sh tools/ship.sh b95 "b95: what changed"
# Needs the compiler from tools/setup-toolchain.sh (or TOOLCHAIN=<llvm-mingw>/bin). The game must not be running
# (it locks the DLL). Update docs/NOTES.md ("Deployed") and docs/PLAN.md BEFORE calling: they go into the commit.
# Written 2026-10-09 and NOT yet run on the PC.
set -e
cd "$(dirname "$0")/.."
ID="$1"; MSG="$2"
[ -n "$ID" ] && [ -n "$MSG" ] || { echo "usage: sh tools/ship.sh <build id> \"<commit message>\""; exit 2; }
[ -e "re/builds/$ID" ] && { echo "build id $ID is already used (re/builds/$ID exists): take the next one"; exit 2; }
: "${TOOLCHAIN:=$PWD/re/toolchain/bin}"; export TOOLCHAIN
G="${SWBF_GAMEDATA:-/c/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront (Classic 2004)/GameData}"
[ -d "$G" ] || { echo "game folder not found: $G (set SWBF_GAMEDATA)"; exit 1; }
sh tools/build.sh "$ID"
mkdir -p "re/builds/$ID" && cp build/dinput8.debug.dll "re/builds/$ID/"
cp build/dinput8.dll "$G/dinput8.dll" || { echo "deploy failed: is the game still running?"; exit 1; }
A=$(sha256sum build/dinput8.dll | cut -c1-64); B=$(sha256sum "$G/dinput8.dll" | cut -c1-64)
[ "$A" = "$B" ] || { echo "hash mismatch after deploy: built $A, game folder has $B"; exit 1; }
grep -qvE "^[[:space:]]*(#.*)?$" "$G/swbf-vr-script.txt" 2>/dev/null && echo "warning: GameData/swbf-vr-script.txt is not empty"
[ -e "$G/swbf-vr-autorun.txt" ] && echo "warning: GameData/swbf-vr-autorun.txt is pending"
S=$(wc -c < docs/NOTES.md); [ "$S" -gt 12000 ] && echo "warning: docs/NOTES.md is $S bytes (keep it under 12000; history belongs in docs/RUNS.md)"
grep -q "$ID" docs/NOTES.md || echo "warning: docs/NOTES.md does not mention $ID (section Deployed)"
git add -A && git commit -q -m "$MSG" && git log --oneline | head -1
echo "shipped $ID  sha256 $A"
