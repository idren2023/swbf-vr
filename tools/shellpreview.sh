#!/bin/sh
# Builds tools/shellpreview.exe from the shell sources and writes re/shells/<odf>.obj for each stock tank.
cd "$(dirname "$0")/.."
T="${TOOLCHAIN:-$PWD/re/toolchain/bin}/"
python tools/lvllabels_inc.py >/dev/null || exit 1 # build/gen/LvlLabels.inc (the stock pieces come from the game files at run time)
python tools/seated_trooper.py >/dev/null || exit 1 # build/gen/SeatedTrooper.inc (not in git; empty without re/body/seated)
mkdir -p re/shells
"${T}i686-w64-mingw32-clang++" -std=c++20 -Ibuild/gen -O1 -static tools/shellpreview.cpp src/ShellDesigns.cpp src/ShellKit.cpp src/StockTables.cpp src/LvlFile.cpp \
    -o re/shells/shellpreview.exe || exit 1
re/shells/shellpreview.exe re/shells "${@:-cis_hover_aat}"
