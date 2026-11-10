cmake_minimum_required(VERSION 3.20)
# Deadstation Portable: PPSSPP's emulation (core/) for the TrimUI Brick (NextUI, tg5040). Brick only: build with
# shell/brick/build.sh, which passes ui/toolchain/tg5040.cmake as the toolchain file.
project(deadstation-portable VERSION 1.1.0 LANGUAGES C CXX ASM)

# The Brick's four Cortex-A53 are r0p4 (/proc/cpuinfo): Arm's errata 835769 (multiply-accumulate, fixed by the compiler)
# and 843419 (ADRP at a page's end, fixed by the linker) apply to them.
add_compile_options(-mfix-cortex-a53-835769)
add_link_options(-Wl,--fix-cortex-a53-843419)
# Line tables, for profiles by source line (local/brick/lines.py): the same code is generated; the pak's copy is stripped.
add_compile_options(-gline-tables-only)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# no path of this machine in the binary
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

execute_process(COMMAND git rev-parse --short=8 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE DEADSTATION_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(DEADSTATION_VERSION "${PROJECT_VERSION}-${DEADSTATION_GIT_HASH}")

if(NOT DEADSTATION_FFMPEG OR NOT EXISTS "${DEADSTATION_FFMPEG}/lib/libavcodec.a")
	message(FATAL_ERROR "DEADSTATION_FFMPEG: FFmpeg for the Brick (shell/brick/ffmpeg.sh)")
endif()

# --- the core: everything left in core/ builds ------------------------------------------------------------------
set(C core)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${C}/Common/*.cpp ${C}/Core/*.cpp ${C}/GPU/*.cpp)
set(E ${C}/ext)
set(ZS ${E}/zstd/lib)
file(GLOB ZSTD_SOURCES ${ZS}/common/*.c ${ZS}/compress/*.c ${ZS}/decompress/*.c)
file(GLOB PNG_SOURCES ${E}/libpng17/*.c)
list(FILTER PNG_SOURCES EXCLUDE REGEX "pngtest")
file(GLOB_RECURSE GLSLANG_SOURCES ${E}/glslang/glslang/MachineIndependent/*.cpp ${E}/glslang/glslang/GenericCodeGen/*.cpp)
set(CH ${E}/libchdr)
add_library(deadstation_core STATIC ${CORE_SOURCES}
		${ZSTD_SOURCES} ${PNG_SOURCES} ${GLSLANG_SOURCES}
		${E}/glslang/glslang/OSDependent/Unix/ossource.cpp
		${E}/glslang/OGLCompilersDLL/InitializeDll.cpp
		${E}/glslang/SPIRV/GlslangToSpv.cpp ${E}/glslang/SPIRV/InReadableOrder.cpp ${E}/glslang/SPIRV/Logger.cpp
		${E}/glslang/SPIRV/SpvBuilder.cpp ${E}/glslang/SPIRV/SpvPostProcess.cpp ${E}/glslang/SPIRV/SPVRemapper.cpp
		${E}/glslang/SPIRV/disassemble.cpp ${E}/glslang/SPIRV/doc.cpp ${E}/glslang/SPIRV/SpvTools.cpp
		${E}/at3_standalone/atrac.cpp ${E}/at3_standalone/atrac3.cpp ${E}/at3_standalone/atrac3plus.cpp ${E}/at3_standalone/atrac3plusdec.cpp
		${E}/at3_standalone/atrac3plusdsp.cpp ${E}/at3_standalone/get_bits.cpp ${E}/at3_standalone/compat.cpp ${E}/at3_standalone/fft.cpp ${E}/at3_standalone/mem.cpp
		${E}/minimp3/minimp3.cpp
		${E}/libkirk/AES.c ${E}/libkirk/amctrl.c ${E}/libkirk/bn.c ${E}/libkirk/ec.c ${E}/libkirk/kirk_engine.c ${E}/libkirk/SHA1.c
		${E}/sfmt19937/SFMT.c
		${E}/cityhash/city.cpp
		${E}/gason/gason.cpp
		${E}/jpge/jpgd.cpp ${E}/jpge/jpge.cpp
		${E}/snappy/snappy-c.cpp ${E}/snappy/snappy.cpp ${E}/snappy/snappy-sinksource.cpp ${E}/snappy/snappy-stubs-internal.cpp
		${E}/vma/vk_mem_alloc.cpp
		${E}/xxhash.c
		${E}/aemu_postoffice/client/postoffice.c ${E}/aemu_postoffice/client/postoffice_mem_stdc.c ${E}/aemu_postoffice/client/mutex_impl_cpp.cpp
		${E}/aemu_postoffice/client/delay_impl_cpp.cpp ${E}/aemu_postoffice/client/log_impl_ppsspp.cpp ${E}/aemu_postoffice/client/sock_impl_linux.c
		${CH}/deps/lzma-24.05/src/Alloc.c ${CH}/deps/lzma-24.05/src/Bra.c ${CH}/deps/lzma-24.05/src/Bra86.c ${CH}/deps/lzma-24.05/src/BraIA64.c
		${CH}/deps/lzma-24.05/src/CpuArch.c ${CH}/deps/lzma-24.05/src/Delta.c ${CH}/deps/lzma-24.05/src/LzFind.c ${CH}/deps/lzma-24.05/src/LzmaDec.c
		${CH}/deps/lzma-24.05/src/LzmaEnc.c ${CH}/deps/lzma-24.05/src/Lzma86Dec.c ${CH}/deps/lzma-24.05/src/Sort.c
		${CH}/src/libchdr_bitstream.c ${CH}/src/libchdr_cdrom.c ${CH}/src/libchdr_chd.c ${CH}/src/libchdr_flac.c ${CH}/src/libchdr_huffman.c)
target_include_directories(deadstation_core PUBLIC ${C} ${C}/Common ${E} ${E}/vulkan ${E}/glslang ${E}/zstd/lib ${E}/libpng17 ${E}/vulkan/.. ${CH}/include ${CH}/deps/lzma-24.05/include ${E}/at3_standalone ${E}/snappy ${DEADSTATION_FFMPEG}/include)
target_compile_definitions(deadstation_core PUBLIC PPSSPP_GIT_VERSION="${DEADSTATION_VERSION}" __STDC_CONSTANT_MACROS USE_FFMPEG PNG_ARM_NEON_OPT=0 _XOPEN_SOURCE=700 _XOPEN_SOURCE_EXTENDED _BSD_SOURCE _DEFAULT_SOURCE
		_LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64 HAVE_GETAUXVAL NDEBUG SHARED_ZLIB Z7_CRC_NUM_TABLES=1 Z7_ST)
set_source_files_properties(${CH}/deps/lzma-24.05/src/CpuArch.c PROPERTIES COMPILE_DEFINITIONS __SWITCH__)
target_compile_options(deadstation_core PUBLIC -O2 -fno-strict-aliasing -fno-math-errno -Wno-everything)
find_package(ZLIB REQUIRED)
target_link_libraries(deadstation_core PUBLIC ${DEADSTATION_FFMPEG}/lib/libavformat.a ${DEADSTATION_FFMPEG}/lib/libavcodec.a ${DEADSTATION_FFMPEG}/lib/libswresample.a
		${DEADSTATION_FFMPEG}/lib/libswscale.a ${DEADSTATION_FFMPEG}/lib/libavutil.a ZLIB::ZLIB m dl rt pthread)

# --- Deadstation Portable: the menu and pages, the store and hosting recomp modules (ui/), and what runs the core
find_library(DEADSTATION_FREETYPE NAMES libfreetype.so REQUIRED)
add_library(freetype SHARED IMPORTED)
set_target_properties(freetype PROPERTIES IMPORTED_LOCATION ${DEADSTATION_FREETYPE}
		INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SYSROOT}/usr/include/freetype2)
if(NOT DEADSTATION_CURL_INCLUDE OR NOT DEADSTATION_CURL_LIBRARY)
	message(FATAL_ERROR "DEADSTATION_CURL_INCLUDE / DEADSTATION_CURL_LIBRARY: libcurl's headers and the device's libcurl.so.4")
endif()
add_library(CURL::libcurl SHARED IMPORTED)
set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION ${DEADSTATION_CURL_LIBRARY}
		INTERFACE_INCLUDE_DIRECTORIES ${DEADSTATION_CURL_INCLUDE})
set(DEADUI_APP_NAME "Deadstation Portable")
set(DEADUI_HOME_ENV DEADSTATION_PORTABLE_HOME)
set(DEADUI_ROM_ENV DEADSTATION_PORTABLE_ROM)
set(DEADUI_LINK_PREFIX dplink)
set(DEADUI_STORE_URL https://raw.githubusercontent.com/tyrannotorus/emulator-deadstation-portable/store/catalog.cfg)
set(DEADUI_CONSOLE psp)
add_subdirectory(ui)
add_subdirectory(core/deadstation)
