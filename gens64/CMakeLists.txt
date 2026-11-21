# Legacy Gens RR (r341) built as the 32-bit Windows reference exe, or as the same classic Win32 program
# for 64-bit Windows (cmake/toolchain-x86_64-mingw-clang.cmake).
# Must be compiled exactly like the official MSVC build behaves (flags below).
if(NOT WIN32 OR NOT (CMAKE_SIZEOF_VOID_P EQUAL 4 OR CMAKE_SIZEOF_VOID_P EQUAL 8))
  message(FATAL_ERROR "gens64 is the Windows build: use cmake/toolchain-i686-mingw-clang.cmake (32-bit) or cmake/toolchain-x86_64-mingw-clang.cmake (64-bit)")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(GENS_WIN64 ON)
  set(GENS_ARCH_TAG x64)
  set(GENS_TRIPLE_DIR /usr/x86_64-w64-mingw32)
else()
  set(GENS_WIN64 OFF)
  set(GENS_TRIPLE_DIR /usr/i686-w64-mingw32)
endif()

include(sources.cmake)

set(R ${CMAKE_CURRENT_SOURCE_DIR})
set(TOOLS ${PROJECT_SOURCE_DIR}/tools/refbuild)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(GENS_SVN_REV "341" CACHE STRING "Revision shown in the window title")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(WINE wine REQUIRED)
find_program(HOST_CC NAMES gcc cc REQUIRED)
if(NOT DEFINED ENV{WINEPREFIX})
  set(GENS_WINEPREFIX ${PROJECT_BINARY_DIR}/wineprefix)
else()
  set(GENS_WINEPREFIX $ENV{WINEPREFIX})
endif()
set(WINE_ENV ${CMAKE_COMMAND} -E env WINEPREFIX=${GENS_WINEPREFIX} WINEARCH=win32 WINEDEBUG=-all)

# --- include shim (case-insensitive includes, svnrev.h, 7z StdAfx.h) -------------------------------
execute_process(COMMAND ${Python3_EXECUTABLE} ${TOOLS}/mkshim.py ${R} ${GEN}/ci ${GENS_SVN_REV}
                        ${GENS_TRIPLE_DIR}/include
                COMMAND_ERROR_IS_FATAL ANY)

# --- gens64 version tag (window title, About box): git revision + asm modules ported to C ---------
set(GENS_ORIGINAL_ASM_MODULES blit gfx_cd io Mem_M68k Mem_S68k Mem_SH2 Mem_Z80 Misc pwm SH2a vdp_32X vdp_io vdp_rend z80 Starscream)
set(GENS_PORTED_MODULES ${GENS_ORIGINAL_ASM_MODULES})
if(GENS_ASM_MODULES)
  list(REMOVE_ITEM GENS_PORTED_MODULES ${GENS_ASM_MODULES})
endif()
set(GENS_VERSION_H ${GEN}/ci/gens64_version.h)
add_custom_target(gens64_version
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DOUT=${GENS_VERSION_H}
          "-DPORTED=${GENS_PORTED_MODULES}" "-DARCH=${GENS_ARCH_TAG}" -P ${TOOLS}/version.cmake
  BYPRODUCTS ${GENS_VERSION_H} COMMENT "gens64 version tag" VERBATIM)

# --- assembly: original NASMW.EXE under Wine -------------------------------------------------------
set(WINE_STAMP ${GEN}/wineprefix.stamp)   # initialise the prefix once, before parallel NASMW runs
add_custom_command(OUTPUT ${WINE_STAMP}
  COMMAND ${WINE_ENV} wineboot -i
  COMMAND ${CMAKE_COMMAND} -E touch ${WINE_STAMP}
  COMMENT "Initialising Wine prefix" VERBATIM)
set(NASMW ${R}/dependencies/NASMW.EXE)
set(ASM_INCS ${R}/src/nasmhead.inc ${R}/src/Mem_M68K_32X.inc ${R}/src/Mem_M68K_CD.inc ${R}/src/SH2_IO.inc)
set(GENS_ASM_OBJS)
foreach(m IN LISTS GENS_ASM_MODULES)
  set(obj ${GEN}/asm/${m}.obj)
  add_custom_command(OUTPUT ${obj}
    COMMAND ${WINE_ENV} ${TOOLS}/nasmw.sh ${NASMW} ${R}/src/${m}.asm ${obj}
    DEPENDS ${WINE_STAMP} ${R}/src/${m}.asm ${ASM_INCS} ${TOOLS}/nasmw.sh ${TOOLS}/fix_coff_vsize.py
    COMMENT "NASMW ${m}.asm" VERBATIM)
  list(APPEND GENS_ASM_OBJS ${obj})
endforeach()

# Starscream 68000 cores: build the generator for the host, generate the asm as before, translate it
# to C instruction by instruction (tools/star2c: same behaviour, x86 flag semantics included).
set(STAR2C ${PROJECT_SOURCE_DIR}/tools/star2c)
set(GENS_STAR_SOURCES)
foreach(cpu Main Sub)
  string(TOLOWER ${cpu} lc)
  set(gen_exe ${GEN}/${cpu}Star)
  set(asm ${GEN}/${lc}68k.asm)
  set(csrc ${GEN}/${lc}68k.c)
  set(s2c_opt)
  if(cpu STREQUAL "Sub")
    set(s2c_opt --sub)
  endif()
  add_custom_command(OUTPUT ${gen_exe}
    COMMAND ${HOST_CC} -O1 -w -o ${gen_exe} ${R}/Starscream/${cpu}68k/Star.c
    DEPENDS ${R}/Starscream/${cpu}68k/Star.c COMMENT "Starscream generator (${cpu})" VERBATIM)
  add_custom_command(OUTPUT ${asm}
    COMMAND ${gen_exe} ${lc}68k.asm -hog -name ${lc}68k_
    WORKING_DIRECTORY ${GEN} DEPENDS ${gen_exe} COMMENT "Starscream ${lc}68k.asm" VERBATIM)
  add_custom_command(OUTPUT ${csrc}
    COMMAND ${Python3_EXECUTABLE} ${STAR2C}/star2c.py ${asm} ${csrc} ${s2c_opt}
    DEPENDS ${asm} ${STAR2C}/star2c.py COMMENT "star2c ${lc}68k.c" VERBATIM)
  list(APPEND GENS_STAR_SOURCES ${csrc})
endforeach()
set_source_files_properties(${GENS_STAR_SOURCES} PROPERTIES GENERATED TRUE
  OBJECT_DEPENDS ${STAR2C}/x86ops.h COMPILE_OPTIONS "-I${STAR2C};-Wno-unused-label")
set_source_files_properties(${GENS_ASM_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# --- resources ---------------------------------------------------------------------------------------
file(GLOB GENS_RES_FILES ${R}/resource/*)
set(res_obj ${GEN}/Gens.res.o)
add_custom_command(OUTPUT ${res_obj}
  COMMAND ${Python3_EXECUTABLE} ${TOOLS}/mkrc.py ${R}/src/Gens.rc ${R}/resource ${GEN} ${GENS_VERSION_H}
  COMMAND ${CMAKE_RC_COMPILER} -I ${R}/src -I ${R} -I ${GEN}/ci -DWIN32 -D_WIN32_WINNT=0x0501
          ${GEN}/Gens_build.rc -O coff -o ${res_obj}
  WORKING_DIRECTORY ${R}
  DEPENDS ${R}/src/Gens.rc ${R}/src/resource.h ${GENS_RES_FILES} ${TOOLS}/mkrc.py ${TOOLS}/fix_ico.py ${GENS_VERSION_H}
  COMMENT "Resources" VERBATIM)
set_source_files_properties(${res_obj} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# gens64: MessageBox -> Gens64_MessageBox (non-blocking in -batch mode)
file(GLOB GENS_SRC_CXX RELATIVE ${R} ${R}/src/*.cpp)
set_source_files_properties(${GENS_SRC_CXX} src/CCnet.c src/cd_file.c src/cd_sys.c
  PROPERTIES COMPILE_OPTIONS "-include;${R}/src/gens64_msgbox.h")

# yx5200.c includes minimp3 (C99)
set_source_files_properties(src/yx5200.c PROPERTIES COMPILE_OPTIONS "-std=gnu99")

# --- the exe -----------------------------------------------------------------------------------------
add_executable(Gens WIN32 ${GENS_C_CXX_SOURCES} ${GENS_7Z_SOURCES} ${GENS_STAR_SOURCES} ${GENS_ASM_OBJS} ${res_obj})
add_dependencies(Gens gens64_version)
set_target_properties(Gens PROPERTIES OUTPUT_NAME Gens RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_include_directories(Gens PRIVATE ${GEN}/ci ${R}/src ${R}/dependencies ${R}/dependencies/DirectX ${R}/7z/CPP ${R})
target_compile_definitions(Gens PRIVATE _WIN32_WINNT=0x0501 WIN32 NDEBUG _WINDOWS _CRT_SECURE_NO_DEPRECATE EXTRACT_ONLY _NO_CRYPTO)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86|x86)$")
  set(GENS_MARCH)                # not x86 (e.g. aarch64): no -march
elseif(GENS_WIN64)
  set(GENS_MARCH -march=x86-64)  # SSE floating point (it mattered for the Sega CD PCM step: pcm.c)
else()
  set(GENS_MARCH -march=i686)    # i686 => x87 floating point, like MSVC 2008/2010 by default
endif()
# -fsigned-char: plain char is signed on x86 (and MSVC), unsigned on Linux/Android ARM.
# -ffp-contract=off: no fused multiply-add (always available on ARM), so float results round like x86.
target_compile_options(Gens PRIVATE
  -O2 ${GENS_MARCH}
  -fms-extensions -fno-strict-aliasing -fwrapv -fsigned-char -ffp-contract=off -w
  $<$<COMPILE_LANGUAGE:C>:-std=gnu89 -fcommon        # -fcommon: MSVC "communal" C globals (pwm.c/pwm.asm)
     -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion
     -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types>
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11 -include ${GEN}/ci/msvc_compat.h -fdelayed-template-parsing
     -Wno-error=incompatible-function-pointer-types -Wno-error=invalid-token-paste>)
target_link_options(Gens PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--stack,8388608)
# Windows XP for the 32-bit build: no thread-safe static locals (they need Vista's condition variables
# from the C++ runtime) and our GetThreadId import (xp_compat.c); the imports are checked against XP's
# after linking (tools/check_imports.py, tools/xp_imports.txt).
if(NOT GENS_WIN64 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
  target_sources(Gens PRIVATE src/xp_compat.c)
  target_compile_options(Gens PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
  add_custom_command(TARGET Gens POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_imports.py ${CMAKE_OBJDUMP}
            $<TARGET_FILE:Gens> ${PROJECT_SOURCE_DIR}/tools/xp_imports.txt
    COMMENT "Imports available on Windows XP" VERBATIM)
endif()
# Release exes are stripped (no symbol table: ~3.4 MB less). -DGENS64_STRIP=OFF keeps the symbols for debugging.
option(GENS64_STRIP "strip the symbol table from Gens.exe" ON)
if(GENS64_STRIP)
  target_link_options(Gens PRIVATE -s)
endif()
if(GENS_WIN64)
  # 64-bit: zlib 1.1.3 and libpng 1.2.0 (the versions of the headers in src/) built from source
  set(TP ${PROJECT_SOURCE_DIR}/third_party)
  file(GLOB GENS_ZLIB_SRC ${TP}/zlib-1.1.3/*.c)
  file(GLOB GENS_PNG_SRC ${TP}/libpng-1.2.0/*.c)
  add_library(gens_zlib STATIC ${GENS_ZLIB_SRC})
  target_compile_options(gens_zlib PRIVATE -O2 -w)
  add_library(gens_png STATIC ${GENS_PNG_SRC})
  target_include_directories(gens_png PRIVATE ${TP}/libpng-1.2.0 ${TP}/zlib-1.1.3)
  target_compile_definitions(gens_png PRIVATE PNG_NO_ASSEMBLER_CODE PNG_NO_MMX_CODE)
  target_compile_options(gens_png PRIVATE -O2 -w)
  set(GENS_IMAGE_LIBS gens_png gens_zlib)
else()
  set(GENS_IMAGE_LIBS ${R}/dependencies/libs/libpng.lib ${R}/dependencies/libs/zlib.lib)
endif()
target_link_libraries(Gens PRIVATE
  ${GENS_IMAGE_LIBS}
  htmlhelp vfw32 dsound ddraw dinput dxguid winmm wsock32 comdlg32 comctl32 shell32 ole32 oleaut32 uuid gdi32 user32 advapi32)

# Paprium (T-574120-00): high-level emulation of the cartridge's processor, all in src/paprium.c.
# OFF leaves it out: the hooks become empty and the ROM does not boot.
option(GENS64_PAPRIUM "Paprium cartridge processor (src/paprium.c)" ON)
if(GENS64_PAPRIUM)
  target_sources(Gens PRIVATE src/paprium.c)
  target_compile_definitions(Gens PRIVATE GENS64_PAPRIUM)
  set_source_files_properties(src/paprium.c PROPERTIES COMPILE_OPTIONS "-std=gnu99")
endif()


# gens64: CHD images for Sega CD: libchdr with zlib 1.2.11 (Z_PREFIX: z_*),
# LZMA SDK 19.00 (renamed chdlzma_*, third_party/libchdr/compat/lzma_rename.h: Gens has its own 7-Zip
# LZMA), zstd and dr_flac, as in Genesis Plus GX (libretro/deps). Only chd_*/cdrom_* are used by Gens.
set(CHD ${PROJECT_SOURCE_DIR}/third_party/libchdr)
set(CHD_LZMA Alloc Bra86 BraIA64 CpuArch Delta LzFind Lzma86Dec LzmaDec LzmaEnc Sort)
list(TRANSFORM CHD_LZMA PREPEND ${CHD}/deps/lzma-19.00/src/)
list(TRANSFORM CHD_LZMA APPEND .c)
add_library(gens_chd STATIC
  ${CHD}/src/libchdr_bitstream.c ${CHD}/src/libchdr_cdrom.c ${CHD}/src/libchdr_chd.c
  ${CHD}/src/libchdr_flac.c ${CHD}/src/libchdr_huffman.c
  ${CHD_LZMA}
  ${CHD}/deps/zstd/lib/common/entropy_common.c ${CHD}/deps/zstd/lib/common/error_private.c
  ${CHD}/deps/zstd/lib/common/fse_decompress.c ${CHD}/deps/zstd/lib/common/zstd_common.c
  ${CHD}/deps/zstd/lib/common/xxhash.c ${CHD}/deps/zstd/lib/decompress/huf_decompress.c
  ${CHD}/deps/zstd/lib/decompress/zstd_ddict.c ${CHD}/deps/zstd/lib/decompress/zstd_decompress.c
  ${CHD}/deps/zstd/lib/decompress/zstd_decompress_block.c
  ${CHD}/deps/zlib-1.2.11/adler32.c ${CHD}/deps/zlib-1.2.11/crc32.c ${CHD}/deps/zlib-1.2.11/inffast.c
  ${CHD}/deps/zlib-1.2.11/inflate.c ${CHD}/deps/zlib-1.2.11/inftrees.c ${CHD}/deps/zlib-1.2.11/zutil.c)
target_include_directories(gens_chd PRIVATE ${CHD}/compat ${CHD}/include ${CHD}/deps/lzma-19.00/include
  ${CHD}/deps/zlib-1.2.11 ${CHD}/deps/zstd/lib)
target_compile_definitions(gens_chd PRIVATE _7ZIP_ST ZSTD_DISABLE_ASM Z_PREFIX
  z_errmsg=chdz_errmsg inflate_fast=chdz_inflate_fast inflate_table=chdz_inflate_table zcalloc=chdz_zcalloc zcfree=chdz_zcfree)
target_compile_options(gens_chd PRIVATE -O2 -w -std=gnu99 -include ${CHD}/compat/lzma_rename.h)
target_include_directories(Gens PRIVATE ${CHD}/include ${CHD}/compat)
set_source_files_properties(src/cd_chd.c PROPERTIES COMPILE_OPTIONS "-std=gnu99")
target_link_libraries(Gens PRIVATE gens_chd)
