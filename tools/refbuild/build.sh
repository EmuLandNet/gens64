#!/bin/bash
# Reference 32-bit Windows build of Gens RR (r341 + gens64 fixes) on Linux, from this repository's working tree.
# Toolchain: clang (MS asm blocks) + mingw-w64 i686 + ORIGINAL NASMW.EXE under Wine.
#
# Ubuntu 24.04 prerequisites:
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install clang lld g++-mingw-w64-i686 mingw-w64-i686-dev wine wine32:i386 python3
#
# Usage: tools/refbuild/build.sh [builddir]   -> <builddir>/Gens.exe  (default: build/ref)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
R="$ROOT/Gens-rr"
B="$(realpath -m "${1:-$ROOT/build/ref}")"
export WINEDEBUG=-all WINEARCH=win32 WINEPREFIX="${WINEPREFIX:-$ROOT/build/wineprefix}"
rm -rf "$B"; mkdir -p "$B"/{ci,cobj,aobj}

# 2. Starscream 68000 cores: build the generators natively, generate asm (same options as run_*_generator.bat)
for m in Main Sub; do
  gcc -O1 -w -o "$B/${m}Star" "$R/Starscream/${m}68k/Star.c"
done
( cd "$B" && ./MainStar main68k.asm -hog -name main68k_ >/dev/null && ./SubStar sub68k.asm -hog -name sub68k_ >/dev/null )

# 3. Assemble with the ORIGINAL NASMW.EXE (modern NASM picks different instruction encodings)
wineboot -i >/dev/null 2>&1 || true
NASMW="$R/dependencies/NASMW.EXE"
( cd "$R/src"
  for f in blit gfx_cd io Mem_M68k Mem_S68k Mem_SH2 Mem_Z80 Misc pwm SH2a vdp_32X vdp_io vdp_rend z80; do
    wine "$NASMW" -i ./ $f.asm -f win32 -o "$(winepath -w "$B/aobj/$f.obj")"
  done )
( cd "$B" && for f in main68k sub68k; do wine "$NASMW" $f.asm -f win32 -o "aobj/$f.obj"; done )
python3 "$HERE/fix_coff_vsize.py" "$B"/aobj/*.obj   # GNU ld misreads NASM 0.98 VirtualSize -> broken .bss

# 4. Case-insensitive include shim (sources were written on Windows)
cp "$HERE/msvc_compat.h" "$B/ci/"
printf '#define SVN_REV "341"\n' > "$B/ci/svnrev.h"
ln -sf /usr/i686-w64-mingw32/include/basetsd.h "$B/ci/BaseTsd.h"
printf '#ifndef __STDAFX_H\n#define __STDAFX_H\n#include "%s/7z/CPP/Common/MyWindows.h"\n#include "%s/7z/CPP/Common/NewHandler.h"\n#endif\n' "$R" "$R" > "$B/ci/StdAfx.h"
printf '#include "%s/7z/CPP/7zip/Crypto/MyAes.h"\n' "$R" > "$B/ci/MyAES.h"
python3 - "$R" "$B/ci" <<'EOF'
import os, re, glob, sys
root, ci = sys.argv[1], sys.argv[2]
dirs = [root+'/src', root+'/dependencies', root+'/7z/CPP', root+'/7z', root]
index = {}
for d in [root+'/src', root+'/dependencies', root+'/dependencies/DirectX', root+'/dependencies/lua/src', root+'/dependencies/mp3_dec']:
    for f in os.listdir(d): index.setdefault(f.lower(), os.path.join(d, f))
for s in glob.glob(root+'/src/*.[ch]') + glob.glob(root+'/src/*.cpp') + glob.glob(root+'/dependencies/mp3_dec/*'):
    for inc in re.findall(r'#\s*include\s*"([^"]+)"', open(s, errors='replace').read()):
        if os.path.exists(os.path.join(os.path.dirname(s), inc)) or any(os.path.exists(os.path.join(d, inc)) for d in dirs): continue
        key = inc.replace('\\', '/').split('/')[-1].lower()
        if key in index:
            p = os.path.join(ci, inc)
            if not os.path.lexists(p): os.symlink(index[key], p)
EOF

# 5. Source list from the VS2010 project (gens.vcxproj) + 7z/7zip.vcproj
python3 - "$R" > "$B/sources.txt" <<'EOF'
import re, sys
R = sys.argv[1]
x = open(R+'/gens.vcxproj', encoding='utf-8-sig', errors='replace').read()
y = open(R+'/7z/7zip.vcproj', encoding='utf-8', errors='replace').read()
print('\n'.join([p.replace('\\','/') for p in re.findall(r'<ClCompile Include="([^"]+)"', x)] +
                ['7z/'+p.replace('\\','/')[2:] for p in re.findall(r'RelativePath="([^"]+\.(?:c|cpp))"', y)]))
EOF

# 6. Compile C/C++. -march=i686 => x87 FP like MSVC 2008/2010 default; -fcommon => MSVC "communal" C globals.
export R B
export CC="clang --target=i686-w64-mingw32 -march=i686"
export COMMON="-O2 -fms-extensions -fasm-blocks -fno-strict-aliasing -fwrapv -w -D_WIN32_WINNT=0x0501 -DWIN32 -DNDEBUG -D_WINDOWS -D_CRT_SECURE_NO_DEPRECATE -DEXTRACT_ONLY -D_NO_CRYPTO -I$B/ci -I$R/src -I$R/dependencies -I$R/dependencies/DirectX -I$R/7z/CPP -I$R"
compile() {
  s="$R/$1"; o="$B/cobj/$(echo "$1" | tr '/\\' '__').o"
  case "$s" in
    *.cpp) c="$CC -x c++ -std=gnu++11 -include $B/ci/msvc_compat.h -fdelayed-template-parsing -Wno-error=incompatible-function-pointer-types -Wno-error=invalid-token-paste";;
    *)     c="$CC -x c -std=gnu89 -fcommon -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types";;
  esac
  $c $COMMON -c "$s" -o "$o" 2> "$o.log" || { echo "FAIL $1"; cat "$o.log"; return 1; }
}
export -f compile
xargs -P"$(nproc)" -I{} bash -c 'compile "$@"' _ {} < "$B/sources.txt"

# 7. Resources (windres has no DLGINIT; the single DLGINIT block only pre-fills an unused combo box).
#    Icons: fill in bit depth like MS rc.exe does, otherwise Windows shows the 16-colour variant.
rm -rf "$B/res"; cp -r "$R/resource" "$B/res"
for i in "$B"/res/*.ico; do python3 "$HERE/fix_ico.py" "$i" "$i"; done
python3 - "$R/src/Gens.rc" "$B/Gens_build.rc" "$B/res" <<'EOF'
import re, sys
s = open(sys.argv[1], 'rb').read().decode('latin1').replace('\r\n', '\n').replace('\r', '\n')
s = re.sub(r'\nIDD_CONTROLLER DLGINIT\nBEGIN\n.*?\nEND\n', '\n', s, flags=re.S).replace('"resource\\\\', '"' + sys.argv[3] + '/')
open(sys.argv[2], 'wb').write(s.encode('latin1'))
EOF
( cd "$R" && i686-w64-mingw32-windres -I src -I resource -I . -I "$B/ci" -DWIN32 -D_WIN32_WINNT=0x0501 "$B/Gens_build.rc" -O coff -o "$B/cobj/Gens.res.o" )

# 8. Link
i686-w64-mingw32-g++ -static -static-libgcc -static-libstdc++ -mwindows -Wl,--stack,8388608 -o "$B/Gens.exe" \
  "$B"/cobj/*.o "$B"/aobj/*.obj "$R/dependencies/libs/libpng.lib" "$R/dependencies/libs/zlib.lib" \
  -lhtmlhelp -lvfw32 -ldsound -lddraw -ldinput -ldxguid -lwinmm -lwsock32 -lcomdlg32 -lcomctl32 -lshell32 \
  -lole32 -loleaut32 -luuid -lgdi32 -luser32 -ladvapi32 2>&1 | grep -v -i 'drectve' || true
ls -la "$B/Gens.exe"
