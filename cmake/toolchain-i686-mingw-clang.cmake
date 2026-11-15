# Cross toolchain for the 32-bit reference build: clang targeting i686-w64-mingw32.
# clang (not mingw gcc): GNU asm aliases (.set) and MSVC extensions in the sources (-fms-extensions).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(GENS_TRIPLE i686-w64-mingw32)
set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET   ${GENS_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${GENS_TRIPLE})
set(CMAKE_RC_COMPILER ${GENS_TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${GENS_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
