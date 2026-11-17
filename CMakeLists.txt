cmake_minimum_required(VERSION 3.20)
project(gens64 C CXX)

# Stage 1: the original Gens RR r341 code base, built as the 32-bit Windows reference exe.
# Configure with:  cmake -S . -B build/ref -G Ninja --toolchain cmake/toolchain-i686-mingw-clang.cmake
option(GENS_LEGACY_REF "Build the legacy 32-bit reference Gens.exe (gens64/)" ON)
if(GENS_LEGACY_REF)
  add_subdirectory(gens64)
endif()
