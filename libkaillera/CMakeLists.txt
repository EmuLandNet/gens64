# libkaillera - Kaillera netplay client. LGPL-2.1-or-later, see LICENSE.
cmake_minimum_required(VERSION 3.13)
project(libkaillera C)

add_library(kaillera STATIC src/kc.c src/kc_net.c)
target_include_directories(kaillera PUBLIC include)
set_target_properties(kaillera PROPERTIES C_STANDARD 99)
if(WIN32)
	target_link_libraries(kaillera PUBLIC ws2_32)
else()
	target_compile_definitions(kaillera PRIVATE _POSIX_C_SOURCE=200809L)
endif()

option(KAILLERA_TEST_CLIENT "Build the console test client" OFF)
if(KAILLERA_TEST_CLIENT)
	add_executable(kc_test tools/kc_test.c)
	target_link_libraries(kc_test kaillera)
endif()
