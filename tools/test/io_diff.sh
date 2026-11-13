#!/bin/sh
# gens64: build and run the io.asm differential test (tools/test/io_diff.c) under Wine.
# Usage: tools/test/io_diff.sh <build dir with gen/asm/io.obj from an io.asm build> [runs]
# The original object comes from a build that still assembles io.asm (e.g. git checkout b330314).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
obj="$1"; runs="${2:-20000}"
out=$(mktemp -d)
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$obj" "$out/io_orig.obj"
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
$cc -I"$src" -c "$src/io_asm.c" -o "$out/io_asm.o"
$cc -c "$here/io_diff.c" -o "$out/io_diff.o"
$cc -fuse-ld=lld -static "$out/io_diff.o" "$out/io_asm.o" "$out/io_orig.obj" -o "$out/io_diff.exe" 2>/dev/null \
  || $cc -static "$out/io_diff.o" "$out/io_asm.o" "$out/io_orig.obj" -o "$out/io_diff.exe"
WINEDEBUG=-all wine "$out/io_diff.exe" "$runs"
