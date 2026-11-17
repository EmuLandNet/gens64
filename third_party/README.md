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
- `libchdr/` — https://github.com/rtissera/libchdr (BSD 3-clause, Romain Tisserand), the copy shipped
  with Genesis Plus GX (`libretro/deps/libchdr`), with its dependencies from the same place:
  `deps/zstd` (Zstandard, BSD, Meta), `deps/lzma-19.00` (LZMA SDK, public domain, Igor Pavlov),
  `deps/zlib-1.2.11` (zlib license, Jean-loup Gailly and Mark Adler), `include/dr_libs/dr_flac.h`
  (public domain / MIT-0, David Reid). Built as `gens_chd` (Gens-rr/CMakeLists.txt): zlib with
  `Z_PREFIX` and the LZMA symbols renamed (`compat/lzma_rename.h`) so they do not clash with the zlib
  and 7-Zip sources of Gens; `compat/retro_inline.h` replaces the libretro-common header. CHD images
  for Sega CD (`docs/segacd-formats.md`).
