#!/bin/sh
# gens64: sound tables vs libm (see ym_tables.c). Usage: tools/test/ym_tables.sh [cc]
#   cc: compiler command (default: cc, the host). The hashes it prints can be compared between
#   platforms; e.g. "clang --target=x86_64-w64-mingw32 -fuse-ld=lld" plus WINE=wine runs it on msvcrt.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Gens-rr/src"
out=${YM_TABLES_OUT:-$(mktemp -d)}; mkdir -p "$out"
cc=${1:-cc}
flags="-O2 -w -std=gnu89 -fcommon -fwrapv -fsigned-char -ffp-contract=off"
case "$cc" in *clang*) flags="$flags -Wno-error=incompatible-function-pointer-types";; esac
ci=${GENS_CI_DIR:-$here/../../build/ref/Gens-rr/gen/ci}	# BaseTsd.h (case) on Windows targets
$cc $flags -include "$here/ym_tables_perturb.h" -I"$ci" -I"$src" -c "$src/ym2612.c" -o "$out/ym2612.o"
$cc $flags -include "$here/ym_tables_perturb.h" -I"$src" -c "$src/psg.c" -o "$out/psg.o"
$cc $flags -c "$here/ym_tables.c" -o "$out/test.o"
$cc "$out/test.o" "$out/ym2612.o" "$out/psg.o" -lm -o "$out/test.exe"
${WINE:+env WINEDEBUG=-all $WINE} "$out/test.exe"
