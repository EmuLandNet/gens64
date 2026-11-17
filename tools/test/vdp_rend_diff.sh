#!/bin/sh
# gens64: vdp_rend.asm differential test. Usage: tools/test/vdp_rend_diff.sh <original vdp_rend.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../gens64/src/vdp_rend_asm.c" "$here/vdp_rend_diff.c" ${2:-200}
