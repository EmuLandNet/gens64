#!/bin/sh
# gens64: runs every differential test against the original objects (step 3 ports).
# Usage: tools/test/run_all.sh <dir with the original objects>
#   expects there: blit gfx_cd io Misc vdp_32X vdp_io vdp_rend z80 Mem_M68k Mem_S68k Mem_SH2 SH2a .obj,
#   SH2c.obj (SH2.c of the same build), main68k/sub68k .asm + .obj (Starscream output and its NASM object)
here=$(cd "$(dirname "$0")" && pwd)
d=${1:?directory with the original objects}
fail=0
t() {
	name=$1; shift
	r=$("$@" 2>&1 | grep -a "^OK\|^FAIL\|rror" | tail -1)
	case "$r" in OK*) ;; *) fail=1;; esac
	printf '%-10s %s\n' "$name" "${r:-NO OUTPUT}"
}
t blit "$here/blit_diff.sh" "$d/blit.obj"
t gfx_cd "$here/gfx_cd_diff.sh" "$d/gfx_cd.obj"
t io "$here/io_diff.sh" "$d/io.obj"
t misc "$here/misc_diff.sh" "$d/Misc.obj"
t vdp_32X "$here/vdp_32X_diff.sh" "$d/vdp_32X.obj"
t vdp_io "$here/vdp_io_diff.sh" "$d/vdp_io.obj"
t vdp_rend "$here/vdp_rend_diff.sh" "$d/vdp_rend.obj"
t z80 "$here/z80_diff.sh" "$d/z80.obj"
t m68k "$here/m68k_diff.sh" "$d/Mem_M68k.obj"
t s68k "$here/s68k_diff.sh" "$d/Mem_S68k.obj"
t sh2mem "$here/sh2mem_diff.sh" "$d/Mem_SH2.obj"
t sh2 "$here/sh2_diff.sh" "$d/SH2a.obj" "$d/SH2c.obj" 20000
t main68k "$here/star_diff.sh" "$d/main68k.asm" "$d/main68k.obj" 20000
t sub68k "$here/star_diff.sh" "$d/sub68k.asm" "$d/sub68k.obj" 20000
exit $fail
