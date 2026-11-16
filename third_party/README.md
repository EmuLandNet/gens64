# third_party

Sources of libraries that the legacy 32-bit build links as prebuilt MSVC `.lib` files
(`Gens-rr/dependencies/libs`), for builds that cannot use those (x86-64). Same versions as the
headers in `Gens-rr/src` (identical files), taken from the upstream git tags:

- `zlib-1.1.3/` — https://github.com/madler/zlib tag `v1.1.3` (library sources only).
- `libpng-1.2.0/` — https://github.com/pnggroup/libpng tag `v1.2.0` (library sources only; the
  MMX assembler readers `pnggccrd.c`/`pngvcrd.c` are not used, `PNG_NO_ASSEMBLER_CODE`).
- `minimp3/` — https://github.com/lieff/minimp3 (`minimp3.h`, `minimp3_ex.h`; public domain, CC0),
  the copy shipped with Genesis Plus GX (`core/sound/minimp3`). MP3 music of the YX5200 player
  (`docs/yx5200.md`).
