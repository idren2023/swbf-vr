#!/bin/sh
# Cross-compiles dinput8.dll for 32-bit Windows with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# Usage: TOOLCHAIN=/path/to/llvm-mingw/bin tools/build.sh [build id]
# Output: build/dinput8.dll (stripped, for the game folder) and build/dinput8.debug.dll (same code with symbols,
# for resolving "dinput8.dll+0x..." addresses from swbf-vr-crash.log with llvm-symbolizer).
set -e
cd "$(dirname "$0")/.."
T="${TOOLCHAIN:+$TOOLCHAIN/}"
ID="${1:-$(date -u +%Y%m%d-%H%M%S)}"
mkdir -p build/obj
MH=external/minhook
for f in $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/hde32.c; do
  "${T}i686-w64-mingw32-clang" -O2 -g -c "$f" -I$MH/include -o "build/obj/$(basename "$f" .c).o"
done
# Sources in case-insensitive order, so that renaming a file to PascalCase (CODE-STYLE step 5) keeps the link order
# and the code layout: a pure rename then builds to the same bytes.
# the stock pieces (exteriors, first-person cockpits, the speeder's mirror) are built at game start from the player's
# own level files (src/StockTables.cpp); the build only needs their labels (our data: tools/lvllabels.json)
mkdir -p build/gen
rm -f build/gen/SpeederStockMirror.inc build/gen/StarfighterExteriors.inc build/gen/StarfighterStockCockpits.inc
python tools/lvllabels_inc.py >/dev/null || exit 1 # build/gen/LvlLabels.inc
# The arm, the player body and the seated trooper: src/data/*.inc, in git, hold only our data (bones, weights,
# triangles, our own meshes, poses, grips); the game-derived parts (texture pixels, the game-derived meshes' positions,
# normals and uvs) are left out (strip_texels.py, strip_meshes.py) and made at start from the player's own game files
# (GameTextures, GameMeshes). Where the full includes exist (re/body, the dev PC: tools/body_export.py,
# tools/body_export_player.py, tools/seated_trooper.py) every build writes src/data again, so a change to them shows in
# git and ship.sh commits it with the build; elsewhere (a clone, CI) the committed copies are compiled as they are.
rm -f build/gen/BodyArm.inc build/gen/PlayerBody.inc build/gen/SeatedTrooper.inc
mkdir -p src/data
if [ -f re/body/seated/seated.json ]; then
  python tools/seated_trooper.py re/body/SeatedTrooper.inc >/dev/null || exit 1
  python tools/strip_meshes.py re/body/SeatedTrooper.inc src/data/SeatedTrooper.inc >/dev/null || exit 1
fi
for NAME in BodyArm PlayerBody; do
  if [ -f re/body/$NAME.inc ]; then
    python tools/strip_texels.py re/body/$NAME.inc build/gen/$NAME.texels.inc >/dev/null &&
      python tools/strip_meshes.py build/gen/$NAME.texels.inc src/data/$NAME.inc >/dev/null &&
      rm build/gen/$NAME.texels.inc || exit 1
  fi
done
python tools/check_stripped.py src/data || exit 1
python tools/texpatch_inc.py >/dev/null || exit 1 # build/gen/TexturePatches.inc (our differences, tools/data/texpatch)
python tools/meshpatch_inc.py >/dev/null || exit 1 # build/gen/MeshPatches.inc (our offsets, tools/data/meshpatch)
# src/dev (the developer's scripted test runs, tools/scripts; not in the public source): compiled in when present.
# Its files keep their place in the list sorted by file name (src/dev/Script.cpp sorts as Script.cpp): runs 813 and
# 815 showed the arms drawn see-through when Script.cpp was linked last, a layout-dependent fault not yet found.
DEV=""
LIST="src/*.cpp"
if [ -d src/dev ] && [ "$PUBLIC" != 1 ]; then DEV="-DSWBF_VR_DEV=1 -iquote src -iquote src/dev"; LIST="src/*.cpp src/dev/*.cpp"; fi
SOURCES=$(for f in $LIST; do echo "$(basename "$f") $f"; done | LC_ALL=C sort -f | cut -d' ' -f2)
"${T}i686-w64-mingw32-clang++" -std=c++20 -Ibuild/gen -Isrc/data $DEV -O2 -g -fno-omit-frame-pointer -Wall -Wextra -shared -static -I$MH/include -Iexternal/openxr/include \
  -DBUILD_ID="\"$ID\"" $SOURCES build/obj/*.o src/dinput8.def -lwinmm -lbcrypt -lgdi32 -lole32 -luuid -lsapi -Wl,--no-insert-timestamp -Wl,--build-id=none -o build/dinput8.debug.dll
"${T}llvm-strip" -o build/dinput8.dll build/dinput8.debug.dll
echo "built build/dinput8.dll ($ID)"
