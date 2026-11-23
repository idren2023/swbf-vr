#!/bin/sh
# Once per PC (Git Bash): fetches the llvm-mingw compiler for Windows into re/toolchain (ignored by git), which is
# where tools/ship.sh looks for it. Written 2026-10-09 and NOT yet run on the PC: check the release file name at
# https://github.com/mstorsjo/llvm-mingw/releases if the download fails.
set -e
cd "$(dirname "$0")/.."
V=20250114
N="llvm-mingw-$V-ucrt-x86_64"
[ -x re/toolchain/bin/i686-w64-mingw32-clang.exe ] && { echo "toolchain already in re/toolchain"; exit 0; }
curl -fL -o "re/$N.zip" "https://github.com/mstorsjo/llvm-mingw/releases/download/$V/$N.zip"
(cd re && { unzip -q "$N.zip" || powershell -NoProfile -Command "Expand-Archive -Force '$N.zip' ."; } && rm -rf toolchain && mv "$N" toolchain && rm -f "$N.zip")
re/toolchain/bin/i686-w64-mingw32-clang.exe --version | head -1
