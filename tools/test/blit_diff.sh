#!/bin/sh
# gens64: blit.asm differential test. Usage: tools/test/blit_diff.sh <original blit.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../Gens-rr/src/blit_asm.c" "$here/blit_diff.c" ${2:-3000}
