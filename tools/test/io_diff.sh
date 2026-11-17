#!/bin/sh
# gens64: io.asm differential test. Usage: tools/test/io_diff.sh <original io.obj> [runs]
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/asm_diff.sh" "$1" "$here/../../gens64/src/io_asm.c" "$here/io_diff.c" ${2:-20000}
