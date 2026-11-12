#!/bin/bash
# Play the same ROM + GMV movie in two Gens builds under Wine and compare per-frame state hashes.
# Usage: ./compare.sh <GensA.exe> <GensB.exe> <rom> <movie.gmv> <frames>
# Needs: wine (32-bit). On a headless box run under xvfb-run.
# Note: Gens shows a window; each run lasts until <frames> frames are emulated, then is killed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A="$(realpath "$1")"; B="$(realpath "$2")"; ROM="$(realpath "$3")"; GMV="$(realpath "$4")"; N="${5:?frames}"
export WINEDEBUG=-all FRAMEHASH_MAX="$N"
T="$(mktemp -d)"
run() { # $1 exe, $2 dir
  mkdir -p "$2"; cp "$1" "$2/Gens.exe"
  ( cd "$2" && wine Gens.exe -rom "$(winepath -w "$ROM")" -play "$(winepath -w "$GMV")" \
      -lua "$(winepath -w "$HERE/test/framehash.lua")" >/dev/null 2>&1 & echo $! > "$2/pid" )
  for _ in $(seq 1 600); do grep -q '^END' "$2/framehash.txt" 2>/dev/null && break; sleep 1; done
  kill "$(cat "$2/pid")" 2>/dev/null || true; wineserver -k 2>/dev/null || true
}
run "$A" "$T/a"; run "$B" "$T/b"
if cmp -s "$T/a/framehash.txt" "$T/b/framehash.txt"; then
  echo "SYNC OK: $(grep -c . "$T/a/framehash.txt") lines identical"
else
  echo "DESYNC. First differing frame:"
  diff "$T/a/framehash.txt" "$T/b/framehash.txt" | head -4
  echo "(columns: frame, adler32(68k RAM), adler32(Z80 RAM), d0-d7, a0-a7, pc, sr)"
fi
echo "Logs: $T"
