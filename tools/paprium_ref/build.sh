#!/bin/sh
# Builds the Genesis Plus GX fork with Paprium ("Full Source/core" of roms/paprium) as a plain program
# for comparing with Gens64. Usage: build.sh <path to "Full Source"> <out dir>
# Run: gref <rom> <frames> [screenshot prefix]; env PRESS_AT=720,820 (Start, 12 frames each) or
# PRESS_START=N (every 120 frames from N). Writes ref_trace.txt in the current folder.
# The chip commands appear in ref_trace.txt only with the fprintf added at "paprium_cmd(data);" in
# paprium_w16.
set -e
SRC_ROOT="$1"; OUT="$2"; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/stub/streams"
cp -r "$SRC_ROOT/core" "$OUT/"
cp "$SRC_ROOT/libretro/osd.h" "$SRC_ROOT/libretro/scrc32.h" "$OUT/"
cp "$HERE/libretro_stub.h" "$OUT/stub/libretro.h"
touch "$OUT/stub/streams/file_stream_transforms.h"
printf '#include <stdio.h>\ntypedef FILE RFILE;\n#define rfopen fopen\n#define rfclose fclose\n#define rfread fread\n#define rfseek fseek\n#define rftell ftell\n#define rfgets fgets\n' > "$OUT/stub/streams/file_stream.h"
cp "$HERE/main.c" "$OUT/"
cd "$OUT"
gcc -O2 -w -include stub/libretro.h -I. -Istub -Icore -Icore/z80 -Icore/m68k -Icore/ntsc -Icore/sound -Icore/input_hw \
  -Icore/cd_hw -Icore/cart_hw -Icore/cart_hw/svp -DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -DUSE_32BPP_RENDERING \
  -DFRONTEND_SUPPORTS_RGB888 -D__LIBRETRO__ -DM68K_OVERCLOCK_SHIFT=20 -DZ80_OVERCLOCK_SHIFT=20 -DHAVE_YM3438_CORE \
  -DHAVE_OPLL_CORE -DMAXROMSIZE=33554432 -DUSE_PER_SOUND_CHANNELS_CONFIG '-DINLINE=static inline' \
  main.c core/*.c core/z80/*.c core/m68k/*.c core/ntsc/*.c core/sound/*.c core/input_hw/*.c core/cd_hw/*.c \
  core/cart_hw/*.c core/cart_hw/svp/*.c -o gref -lm -lz
