#!/bin/sh
# Builds re/gpu-transfer-probe.exe (32-bit, like the mod) from tools/gpu-transfer-probe.cpp.
cd "$(dirname "$0")/.."
re/toolchain/bin/i686-w64-mingw32-clang++ -std=c++20 -O2 -Wall -static -Iexternal/openxr/include \
  tools/gpu-transfer-probe.cpp -ld3d9 -ld3d11 -ld3d12 -ldxgi -luuid -o re/gpu-transfer-probe.exe && echo built
