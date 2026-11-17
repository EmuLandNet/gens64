#!/bin/sh
# gens64: vdp_32X.asm differential test. Usage: tools/test/vdp_32X_diff.sh <original vdp_32X.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../gens64/src/vdp_32X_asm.c" "$here/vdp_32X_diff.c" ${2:-2000}
