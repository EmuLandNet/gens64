#!/bin/sh
# gens64: Mem_SH2.asm differential test. Usage: tools/test/sh2mem_diff.sh <original Mem_SH2.obj> [ops [seed]]
# SH2MEM_FULL=1 compares all memory after every access (slow; to locate a difference).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
out=${SH2MEM_DIFF_OUT:-$(mktemp -d)}; mkdir -p "$out"
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$1" "$out/orig.obj"
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
$cc -I"$src" -c "$src/Mem_SH2_asm.c" -o "$out/port.o"
$cc -I"$src" -fno-common -c "$here/sh2mem_diff.c" -o "$out/test.o"
$cc -fuse-ld=lld -static -Wl,--stack,8388608 "$out/test.o" "$out/port.o" "$out/orig.obj" -o "$out/test.exe"
shift
WINEDEBUG=-all wine "$out/test.exe" "$@"
