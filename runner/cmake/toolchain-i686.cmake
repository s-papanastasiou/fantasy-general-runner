# SPDX-License-Identifier: GPL-2.0-or-later
# Build the runner as a 32-bit (i686) Linux program on an x86-64 host.
#   cmake -S runner -B build -DCMAKE_TOOLCHAIN_FILE=runner/cmake/toolchain-i686.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")

# Find 32-bit libraries (Fedora multilib puts them in /usr/lib).
set(CMAKE_LIBRARY_ARCHITECTURE "")
set(CMAKE_FIND_ROOT_PATH /usr)
list(APPEND CMAKE_PREFIX_PATH /usr/lib/cmake)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/pkgconfig:/usr/share/pkgconfig")
set(FIND_LIBRARY_USE_LIB64_PATHS OFF)
set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB32_PATHS OFF)
