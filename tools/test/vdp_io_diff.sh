#!/bin/sh
# gens64: vdp_io.asm differential test. Usage: tools/test/vdp_io_diff.sh <original vdp_io.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../Gens-rr/src/vdp_io_asm.c" "$here/vdp_io_diff.c" ${2:-400}
