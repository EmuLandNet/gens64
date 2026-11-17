#!/bin/sh
# gens64: Misc.asm differential test. Usage: tools/test/misc_diff.sh <original Misc.obj> [iterations]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../gens64/src/Misc_asm.c" "$here/misc_diff.c" ${2:-3000}
