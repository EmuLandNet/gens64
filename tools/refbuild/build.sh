#!/bin/bash
# Reference 32-bit Windows build of Gens RR (r341 + gens64 fixes) on Linux — thin wrapper around CMake.
#
# Ubuntu 24.04 prerequisites:
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install cmake ninja-build clang lld g++-mingw-w64-i686 mingw-w64-i686-dev wine wine32:i386 python3
#
# Usage: tools/refbuild/build.sh [builddir]   -> <builddir>/Gens.exe  (default: build/ref)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="${1:-$ROOT/build/ref}"
cmake -S "$ROOT" -B "$B" -G Ninja --toolchain "$ROOT/cmake/toolchain-i686-mingw-clang.cmake"
cmake --build "$B" 2>&1 | grep -v -i 'drectve' || true
ls -la "$B/Gens.exe"
