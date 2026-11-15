#!/bin/sh
# gens64: SH2 core (SH2a.asm + SH2_IO.inc) differential test.
# Usage: tools/test/sh2_diff.sh <original SH2a.obj> <original SH2.c.obj> [execs [seed]]
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
out=${SH2_DIFF_OUT:-$(mktemp -d)}; mkdir -p "$out"
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$1" "$out/orig_sh2a.obj"
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$2" "$out/orig_sh2c.obj"
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
$cc -I"$src" -c "${SH2_PORT_SRC:-$src/SH2a_asm.c}" -o "$out/port.o"
$cc -I"$src" -c "$src/gens64_addr.c" -o "$out/addr.o"
ci=${GENS_CI_DIR:-$here/../../build/ref/Gens-rr/gen/ci}
$cc -w -I"$ci" -I"$src" -c "$src/SH2.c" -o "$out/sh2c.o"
$cc -I"$src" -fno-common -c "$here/sh2_diff.c" -o "$out/test.o"
$cc -fuse-ld=lld -static -Wl,--stack,16777216 "$out/test.o" "$out/port.o" "$out/addr.o" "$out/sh2c.o" "$out/orig_sh2a.obj" "$out/orig_sh2c.obj" -o "$out/test.exe"
shift 2
WINEDEBUG=-all wine "$out/test.exe" "$@"
