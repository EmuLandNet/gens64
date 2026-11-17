#!/bin/sh
# gens64: gfx_cd.asm differential test. Usage: tools/test/gfx_cd_diff.sh <original gfx_cd.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../gens64/src/gfx_cd_asm.c" "$here/gfx_cd_diff.c" ${2:-300}
