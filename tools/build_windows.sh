#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compile the runner for 64-bit Windows (Unicorn CPU backend) and
# package it as dist/fantasy-general-runner-VERSION-windows-x64.zip.
#
#   tools/build_windows.sh [VERSION]      (default: dev)
#
# Host packages (Fedora): mingw64-gcc-c++ mingw64-SDL3 cmake ninja-build zip.
# Unicorn is not packaged for MinGW: its source (pinned, SHA-256 verified) is
# built here as a static library into tools/vendor/unicorn-mingw64/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/tools/vendor"
DL="$VENDOR/downloads"
VERSION="${1:-dev}"
BUILD="$ROOT/build-windows"
DIST="$ROOT/dist"
NAME="fantasy-general-runner-$VERSION-windows-x64"
PKG="$DIST/$NAME"
MINGW_BIN=/usr/x86_64-w64-mingw32/sys-root/mingw/bin

UNICORN_VER="2.1.4"
UNICORN_URL="https://github.com/unicorn-engine/unicorn/archive/refs/tags/$UNICORN_VER.tar.gz"
UNICORN_SHA="ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4"
UNICORN_PREFIX="$VENDOR/unicorn-mingw64"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

for tool in x86_64-w64-mingw32-g++ cmake ninja zip; do
    command -v "$tool" >/dev/null || { echo "missing: $tool (see the header of this script)" >&2; exit 1; }
done
[[ -f "$MINGW_BIN/SDL3.dll" ]] || { echo "missing: mingw64-SDL3" >&2; exit 1; }

# --- Unicorn (static, x86 only) -------------------------------------------
if [[ ! -f "$UNICORN_PREFIX/lib/libunicorn.a" ]]; then
    mkdir -p "$DL/unicorn"
    tarball="$DL/unicorn/unicorn-$UNICORN_VER.tar.gz"
    if ! { [[ -f "$tarball" ]] && echo "$UNICORN_SHA  $tarball" | sha256sum -c --status; }; then
        log "downloading Unicorn $UNICORN_VER source"
        curl -fsSL --retry 3 -o "$tarball.part" "$UNICORN_URL"
        echo "$UNICORN_SHA  $tarball.part" | sha256sum -c --status || { echo "checksum mismatch" >&2; rm -f "$tarball.part"; exit 1; }
        mv "$tarball.part" "$tarball"
    fi
    src="$VENDOR/unicorn-src"
    rm -rf "$src" "$VENDOR/unicorn-build-mingw64"
    mkdir -p "$src"
    tar xzf "$tarball" -C "$src" --strip-components=1
    log "building Unicorn for Windows (static, x86 only)"
    cmake -S "$src" -B "$VENDOR/unicorn-build-mingw64" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/runner/cmake/toolchain-mingw64.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DUNICORN_LEGACY_STATIC_ARCHIVE=ON \
        -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=ON -DCMAKE_INSTALL_PREFIX="$UNICORN_PREFIX" >/dev/null
    cmake --build "$VENDOR/unicorn-build-mingw64" >/dev/null
    cmake --install "$VENDOR/unicorn-build-mingw64" >/dev/null
fi

# --- runner ----------------------------------------------------------------
log "building fg-runner.exe $VERSION"
# A build folder configured for another checkout location (host vs. the
# release container's /src) cannot be reused.
if [[ -f "$BUILD/CMakeCache.txt" ]] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$ROOT/runner" "$BUILD/CMakeCache.txt"; then
    rm -rf "$BUILD"
fi
cmake -S "$ROOT/runner" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/runner/cmake/toolchain-mingw64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DFG_CPU=unicorn -DFG_UNICORN_ROOT="$UNICORN_PREFIX" -DFG_VERSION="$VERSION" >/dev/null
cmake --build "$BUILD" >/dev/null

# --- package ---------------------------------------------------------------
log "packaging $DIST/$NAME.zip"
rm -rf "$PKG"
mkdir -p "$PKG"
x86_64-w64-mingw32-strip -o "$PKG/fg-runner.exe" "$BUILD/fg-runner.exe"
cp "$MINGW_BIN/SDL3.dll" "$MINGW_BIN/libwinpthread-1.dll" "$PKG/"
cp "$ROOT/LICENSE" "$PKG/LICENSE.txt"
cp "$ROOT/runner/third_party/fonts/OFL-Inter.txt" "$ROOT/runner/third_party/fonts/OFL-Metamorphous.txt" "$PKG/"
cp "$VENDOR/unicorn-src/COPYING" "$PKG/LICENSE-Unicorn.txt"
cp /usr/share/licenses/mingw64-SDL3/LICENSE.txt "$PKG/LICENSE-SDL3.txt"
cp "$ROOT/runner/win/README-windows.txt" "$PKG/README.txt"
sed -i 's/$/\r/' "$PKG"/*.txt
rm -f "$DIST/$NAME.zip"
(cd "$DIST" && zip -qr "$NAME.zip" "$NAME")
log "done: $DIST/$NAME.zip"
