# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)
project(fg_runner LANGUAGES C CXX ASM)

# The game runs in the Unicorn x86 emulator (GPL-2.0): a 64-bit build.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The runner is a 64-bit program")
endif()

# Shown by --version and in the log; release builds pass the git tag.
set(FG_VERSION "dev" CACHE STRING "Version string compiled into the runner")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

add_executable(fg-runner
  src/main.cpp
  src/address_space.cpp
  src/ldt.cpp
  src/le_image.cpp
  src/machine.cpp
  src/traps.cpp
  src/dos.cpp
  src/dos_fs.cpp
  src/dpmi.cpp
  src/cdrom.cpp
  src/ports.cpp
  src/video.cpp
  src/png.cpp
  src/ui.cpp
  src/input.cpp
  src/cd_audio.cpp
  src/cd_extract.cpp
  src/dig_driver.cpp
  src/game_locate.cpp
  src/settings.cpp
  src/launcher.cpp
  src/launcher_text.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/launcher_fonts.S
  src/crt.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/crt_shaders.S
  src/log.cpp
)
# The CRT filter's shaders (shaders/, MIT): compiled to SPIR-V with glslang
# (Fedora: glslang; Ubuntu: glslang-tools) and embedded with .incbin.
find_program(FG_GLSLANG NAMES glslangValidator glslang REQUIRED)
set(FG_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(FG_SPV_FILES)
foreach(shader vga-1080p crt-hyllian)
  set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.frag")
  set(spv "${FG_SPV_DIR}/${shader}.spv")
  add_custom_command(OUTPUT "${spv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FG_SPV_DIR}"
    COMMAND ${FG_GLSLANG} -V "${src}" -o "${spv}"
    DEPENDS "${src}" VERBATIM)
  list(APPEND FG_SPV_FILES "${spv}")
endforeach()
add_custom_target(fg_shaders DEPENDS ${FG_SPV_FILES})
add_dependencies(fg-runner fg_shaders)
configure_file(src/crt_shaders.S.in crt_shaders.S @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/crt_shaders.S PROPERTIES OBJECT_DEPENDS "${FG_SPV_FILES}")

# The launcher's fonts, embedded with .incbin (third_party/fonts/README.md).
set(FG_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts")
configure_file(src/launcher_fonts.S.in launcher_fonts.S @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/launcher_fonts.S PROPERTIES OBJECT_DEPENDS
  "${FG_FONTS}/Inter-Regular.ttf;${FG_FONTS}/Inter-SemiBold.ttf;${FG_FONTS}/Metamorphous-Regular.ttf")
target_include_directories(fg-runner PRIVATE src third_party/stb)

# Ogg Vorbis decoder for the CD-audio tracks (third_party/stb/README.md).
add_library(stb_vorbis STATIC third_party/stb/stb_vorbis.c)
target_compile_options(stb_vorbis PRIVATE -w)
target_link_libraries(fg-runner PRIVATE stb_vorbis)
# TrueType rasteriser for the launcher (third_party/stb/README.md).
add_library(stb_truetype STATIC third_party/stb/stb_truetype.c)
target_compile_options(stb_truetype PRIVATE -w)
target_link_libraries(fg-runner PRIVATE stb_truetype)
if(NOT WIN32)
  target_link_libraries(fg-runner PRIVATE m)
endif()
target_compile_options(fg-runner PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(fg-runner PRIVATE FG_VERSION="${FG_VERSION}")
target_link_libraries(fg-runner PRIVATE PkgConfig::SDL3)

target_sources(fg-runner PRIVATE src/cpu_unicorn.cpp)
# FG_UNICORN_ROOT: a Unicorn install outside the system (e.g. the static
# Windows build from tools/build_windows.sh); otherwise pkg-config.
set(FG_UNICORN_ROOT "" CACHE PATH "Unicorn install prefix (default: pkg-config)")
if(FG_UNICORN_ROOT)
  find_path(UNICORN_INCLUDE unicorn/unicorn.h PATHS "${FG_UNICORN_ROOT}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  find_library(UNICORN_LIB NAMES unicorn PATHS "${FG_UNICORN_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT UNICORN_INCLUDE OR NOT UNICORN_LIB)
    message(FATAL_ERROR "Unicorn not found in FG_UNICORN_ROOT=${FG_UNICORN_ROOT}")
  endif()
  target_include_directories(fg-runner PRIVATE "${UNICORN_INCLUDE}")
  target_link_libraries(fg-runner PRIVATE "${UNICORN_LIB}")
else()
  pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn>=2.0)
  target_link_libraries(fg-runner PRIVATE PkgConfig::UNICORN)
endif()

# FG_PORTABLE (Linux release builds, tools/release/linux-build.sh): the C++
# runtime linked in, a static Unicorn, and SDL3 looked up next to the
# program in lib/ ($ORIGIN), so the binary runs without system packages.
option(FG_PORTABLE "Linux: self-contained build for a release package" OFF)
if(FG_PORTABLE AND NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(fg-runner PRIVATE Threads::Threads ${CMAKE_DL_LIBS} rt)
  target_link_options(fg-runner PRIVATE -static-libgcc -static-libstdc++)
  set_target_properties(fg-runner PROPERTIES BUILD_RPATH "\$ORIGIN/lib" INSTALL_RPATH "\$ORIGIN/lib")
endif()

if(WIN32)
  enable_language(RC)
  target_sources(fg-runner PRIVATE win/fg-runner.rc)
  target_compile_definitions(fg-runner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(fg-runner PRIVATE winmm advapi32)  # timeBeginPeriod; registry
  # No console window; started from a command prompt it attaches to that
  # prompt's console (main.cpp). The entry point stays main().
  target_link_options(fg-runner PRIVATE -mwindows)
  # C/C++ runtime linked in; SDL3.dll and libwinpthread-1.dll ship next to
  # the program (tools/build_windows.sh).
  target_link_options(fg-runner PRIVATE -static-libgcc -static-libstdc++)
endif()
