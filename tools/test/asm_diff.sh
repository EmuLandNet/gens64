#!/bin/sh
# gens64: build and run a differential test of a C port against the original asm object, under Wine.
# Usage: tools/test/asm_diff.sh <original .obj> <C port source> <test .c> [args...]
# The original object comes from a build that still assembles the module (gen/asm/<module>.obj);
# every symbol in it, externs included, gets the prefix "orig_" so both versions link side by side.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
obj="$1"; port="$2"; test="$3"; shift 3
out=$(mktemp -d)
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$obj" "$out/orig.obj"
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
$cc -I"$src" -c "$port" -o "$out/port.o"
$cc -I"$src" -c "$src/gens64_addr.c" -o "$out/addr.o"
$cc -fno-common -c "$test" -o "$out/test.o"
$cc -fuse-ld=lld -static "$out/test.o" "$out/port.o" "$out/addr.o" "$out/orig.obj" -o "$out/test.exe"
WINEDEBUG=-all wine "$out/test.exe" "$@"
