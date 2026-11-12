#!/bin/bash
# Assemble with the original NASMW.EXE (NASM 0.98) under Wine, then fix the COFF headers for GNU ld.
# Usage: nasmw.sh <NASMW.EXE> <source.asm> <output.obj>
set -euo pipefail
NASMW="$1"; SRC="$(realpath "$2")"; OUT="$(realpath -m "$3")"
export WINEDEBUG=-all WINEARCH=win32
mkdir -p "$(dirname "$OUT")"
cd "$(dirname "$SRC")"
wine "$NASMW" -i ./ "$(basename "$SRC")" -f win32 -o "$(winepath -w "$OUT")"
python3 "$(dirname "$0")/fix_coff_vsize.py" "$OUT"
