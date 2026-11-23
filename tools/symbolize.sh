#!/bin/sh
# Turns "DINPUT8.dll+0x..." offsets from swbf-vr-crash.log into function names and source lines.
# Usage: TOOLCHAIN=/path/to/llvm-mingw/bin tools/symbolize.sh <dinput8.debug.dll of that build> <offset hex> ...
# The debug DLL of every deployed build is kept in re/builds/<id>/ (see docs/WORKFLOW.md).
set -e
T="${TOOLCHAIN:+$TOOLCHAIN/}"
DLL="$1"; shift
BASE=$("${T}llvm-objdump" -p "$DLL" | awk '/ImageBase/ {print $2}')
for off in "$@"; do
  off=${off#0x}
  echo "+0x$off:"
  "${T}llvm-symbolizer" -e "$DLL" "0x$(printf '%x' $((0x$BASE + 0x$off)))" | sed '/^$/d; s/^/  /'
done
