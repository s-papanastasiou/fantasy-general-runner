# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile the runner for 64-bit Windows with MinGW-w64 (Fedora:
# mingw64-gcc-c++, mingw64-SDL3). tools/build_windows.sh uses this.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_TRIPLE x86_64-w64-mingw32)

set(CMAKE_C_COMPILER ${MINGW_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_TRIPLE}-g++)
set(CMAKE_RC_COMPILER ${MINGW_TRIPLE}-windres)
set(PKG_CONFIG_EXECUTABLE ${MINGW_TRIPLE}-pkg-config)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLE}/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
