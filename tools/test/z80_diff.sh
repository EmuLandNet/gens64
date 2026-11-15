#!/bin/sh
# gens64: z80.asm differential test. Usage: tools/test/z80_diff.sh <original z80.obj> [runs]
# Like asm_diff.sh, but DAA_Table of the original is made global (orig_DAA_Table) so that the port,
# built with GENS64_Z80_TEST, reads past the table end at the same place as the original.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
out=${Z80_DIFF_OUT:-$(mktemp -d)}; mkdir -p "$out"
i686-w64-mingw32-objcopy --globalize-symbol=DAA_Table "$1" "$out/g.obj"
i686-w64-mingw32-objcopy --prefix-symbols=orig_ "$out/g.obj" "$out/orig.obj"
cc="clang --target=i686-w64-mingw32 -march=i686 -O2 -fno-strict-aliasing -fwrapv -std=gnu89 -fcommon"
$cc -I"$src" -DGENS64_Z80_TEST -c "$src/z80_asm.c" -o "$out/port.o"
$cc -I"$src" -c "$src/gens64_addr.c" -o "$out/addr.o"
$cc -I"$src" -fno-common -c "$here/z80_diff.c" -o "$out/test.o"
# constant data right after the original's .data: DAA with H set reads 0x800 bytes past its table
printf 'unsigned char z80_daa_guard[0x800] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };\n' > "$out/guard.c"
$cc -c "$out/guard.c" -o "$out/guard.o"
$cc -fuse-ld=lld -static "$out/test.o" "$out/port.o" "$out/addr.o" "$out/orig.obj" "$out/guard.o" -o "$out/test.exe"
WINEDEBUG=-all wine "$out/test.exe" ${2:-3000}
