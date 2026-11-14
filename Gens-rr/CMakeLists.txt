# Legacy Gens RR (r341) built as the 32-bit Windows reference exe.
# Must be compiled exactly like the official MSVC build behaves: see GENS64.md, "Ловушки".
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT WIN32)
  message(FATAL_ERROR "Gens-rr is the 32-bit Windows reference build: use cmake/toolchain-i686-mingw-clang.cmake")
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
                        /usr/i686-w64-mingw32/include
                COMMAND_ERROR_IS_FATAL ANY)

# --- gens64 version tag (window title, About box): git revision + asm modules ported to C ---------
set(GENS_ORIGINAL_ASM_MODULES blit gfx_cd io Mem_M68k Mem_S68k Mem_SH2 Mem_Z80 Misc pwm SH2a vdp_32X vdp_io vdp_rend z80)
set(GENS_PORTED_MODULES ${GENS_ORIGINAL_ASM_MODULES})
if(GENS_ASM_MODULES)
  list(REMOVE_ITEM GENS_PORTED_MODULES ${GENS_ASM_MODULES})
endif()
set(GENS_VERSION_H ${GEN}/ci/gens64_version.h)
add_custom_target(gens64_version
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DOUT=${GENS_VERSION_H}
          "-DPORTED=${GENS_PORTED_MODULES}" -P ${TOOLS}/version.cmake
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

# Starscream 68000 cores: build the generator for the host, generate asm, assemble.
foreach(cpu Main Sub)
  string(TOLOWER ${cpu} lc)
  set(gen_exe ${GEN}/${cpu}Star)
  set(asm ${GEN}/${lc}68k.asm)
  set(obj ${GEN}/asm/${lc}68k.obj)
  add_custom_command(OUTPUT ${gen_exe}
    COMMAND ${HOST_CC} -O1 -w -o ${gen_exe} ${R}/Starscream/${cpu}68k/Star.c
    DEPENDS ${R}/Starscream/${cpu}68k/Star.c COMMENT "Starscream generator (${cpu})" VERBATIM)
  add_custom_command(OUTPUT ${asm}
    COMMAND ${gen_exe} ${lc}68k.asm -hog -name ${lc}68k_
    WORKING_DIRECTORY ${GEN} DEPENDS ${gen_exe} COMMENT "Starscream ${lc}68k.asm" VERBATIM)
  add_custom_command(OUTPUT ${obj}
    COMMAND ${WINE_ENV} ${TOOLS}/nasmw.sh ${NASMW} ${asm} ${obj}
    DEPENDS ${WINE_STAMP} ${asm} ${TOOLS}/nasmw.sh ${TOOLS}/fix_coff_vsize.py COMMENT "NASMW ${lc}68k.asm" VERBATIM)
  list(APPEND GENS_ASM_OBJS ${obj})
endforeach()
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

# --- the exe -----------------------------------------------------------------------------------------
add_executable(Gens WIN32 ${GENS_C_CXX_SOURCES} ${GENS_7Z_SOURCES} ${GENS_ASM_OBJS} ${res_obj})
add_dependencies(Gens gens64_version)
set_target_properties(Gens PROPERTIES OUTPUT_NAME Gens RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_include_directories(Gens PRIVATE ${GEN}/ci ${R}/src ${R}/dependencies ${R}/dependencies/DirectX ${R}/7z/CPP ${R})
target_compile_definitions(Gens PRIVATE _WIN32_WINNT=0x0501 WIN32 NDEBUG _WINDOWS _CRT_SECURE_NO_DEPRECATE EXTRACT_ONLY _NO_CRYPTO)
target_compile_options(Gens PRIVATE
  -O2 -march=i686             # i686 => x87 floating point, like MSVC 2008/2010 by default
  -fms-extensions -fasm-blocks -fno-strict-aliasing -fwrapv -w
  $<$<COMPILE_LANGUAGE:C>:-std=gnu89 -fcommon        # -fcommon: MSVC "communal" C globals (pwm.c/pwm.asm)
     -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion
     -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types>
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11 -include ${GEN}/ci/msvc_compat.h -fdelayed-template-parsing
     -Wno-error=incompatible-function-pointer-types -Wno-error=invalid-token-paste>)
target_link_options(Gens PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--stack,8388608)
target_link_libraries(Gens PRIVATE
  ${R}/dependencies/libs/libpng.lib ${R}/dependencies/libs/zlib.lib
  htmlhelp vfw32 dsound ddraw dinput dxguid winmm wsock32 comdlg32 comctl32 shell32 ole32 oleaut32 uuid gdi32 user32 advapi32)
