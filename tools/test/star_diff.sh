#!/bin/sh
# gens64: Starscream 68000 core translated to C (tools/star2c) vs the original NASM object.
# Usage: tools/test/star_diff.sh <main68k.asm|sub68k.asm> <original .obj> [execs [seed]]
#   STAR2C_DIR: another translator (star2c.py + x86ops.h) for mutation runs; STAR_CORE_OPT: e.g. -O1
#   STAR_TRACE=1: replay the failing exec one instruction at a time; STAR_LOGS=1: print both logs
#   (the .asm is the Starscream generator output the .obj was assembled from; a "sub" in the name
#    selects the Sega CD sub 68000)
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${STAR_DIFF_OUT:-$(mktemp -d)}; mkdir -p "$out"
sub=; case "$1" in *sub*) sub=--sub;; esac
def=; [ -n "$sub" ] && def=-DSUB
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
s2c=${STAR2C_DIR:-$here/../star2c}
python3 "$s2c/star2c.py" "$1" "$out/core.c" $sub
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$2" "$out/orig.obj"
if [ ! -f "$out/core.o" ] || [ "$out/core.c" -nt "$out/core.o" ]; then
	$cc ${STAR_CORE_OPT:-} -Wno-unused-label -DSTAR_INT_ACK=test_int_ack -I"$s2c" -I"$here/../../Gens-rr/src" -c "$out/core.c" -o "$out/core.o"
fi
$cc $def -c "$here/star_diff.c" -o "$out/test.o"
$cc -fuse-ld=lld -static "$out/test.o" "$out/core.o" "$out/orig.obj" -o "$out/test.exe"
shift 2
WINEDEBUG=-all wine "$out/test.exe" "$@"
