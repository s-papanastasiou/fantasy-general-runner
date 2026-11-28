#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Linux release package. Runs INSIDE Ubuntu 22.04 (glibc 2.35, so
# the result runs on Ubuntu 22.04+, Debian 12+, Fedora, SteamOS, ...):
# from tools/build_release.sh in a Podman/Docker container, or as a CI job.
#
#   tools/release/linux-build.sh VERSION
#
# Result: dist/fantasy-general-runner-VERSION-linux-x86_64.tar.gz holding
# fg-runner (C++ runtime and Unicorn linked in) and lib/libSDL3.so.0 (found
# through RPATH $ORIGIN/lib). SDL3 loads X11/Wayland/PipeWire/PulseAudio/
# ALSA from the player's system at run time, whichever exist.
set -euo pipefail

VERSION="${1:?usage: linux-build.sh VERSION}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VENDOR="$ROOT/tools/vendor"
DL="$VENDOR/downloads"
WORK="$VENDOR/linux-release"          # persistent between local runs
DIST="$ROOT/dist"
NAME="fantasy-general-runner-$VERSION-linux-x86_64"

SDL_VER="3.4.20"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz"
SDL_SHA="f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"
UNICORN_VER="2.1.4"
UNICORN_URL="https://github.com/unicorn-engine/unicorn/archive/refs/tags/$UNICORN_VER.tar.gz"
UNICORN_SHA="ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4"
CMAKE_PIP="cmake==3.31.6"
NINJA_PIP="ninja==1.11.1.4"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }

# fetch NAME URL SHA256 -> path of the verified download
fetch() {
    local file="$DL/$1"
    mkdir -p "$DL"
    if ! { [[ -f "$file" ]] && echo "$3  $file" | sha256sum -c --status; }; then
        log "downloading $1"
        curl -fsSL --retry 3 -o "$file.part" "$2"
        echo "$3  $file.part" | sha256sum -c --status || { echo "checksum mismatch: $1" >&2; rm -f "$file.part"; exit 1; }
        mv "$file.part" "$file"
    fi
    echo "$file"
}

# --- build packages (Ubuntu 22.04) -------------------------------------------
if [[ "$(id -u)" == 0 ]] && command -v apt-get >/dev/null; then
    log "installing build packages"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential g++-12 gcc-12 curl ca-certificates file pkg-config python3-pip \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
        libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
        libpulse-dev libasound2-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev \
        >/dev/null
    pip3 install -q "$CMAKE_PIP" "$NINJA_PIP"
fi
export CC=gcc-12 CXX=g++-12   # C++20 library support beyond GCC 11
for tool in "$CC" "$CXX" cmake ninja curl strip; do
    command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

# --- SDL3 (shared, bundled with the program) ----------------------------------
SDL_PREFIX="$WORK/sdl3-$SDL_VER"
if [[ ! -f "$SDL_PREFIX/lib/libSDL3.so.0" ]]; then
    tarball="$(fetch "SDL3-$SDL_VER.tar.gz" "$SDL_URL" "$SDL_SHA")"
    rm -rf "$WORK/sdl3-src" "$WORK/sdl3-build"
    mkdir -p "$WORK/sdl3-src"
    tar xzf "$tarball" -C "$WORK/sdl3-src" --strip-components=1
    log "building SDL3 $SDL_VER"
    cmake -S "$WORK/sdl3-src" -B "$WORK/sdl3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON >/dev/null
    cmake --build "$WORK/sdl3-build" >/dev/null
    cmake --install "$WORK/sdl3-build" >/dev/null
fi

# --- Unicorn (static, x86 only) ----------------------------------------------
UNICORN_PREFIX="$WORK/unicorn-$UNICORN_VER"
if [[ ! -f "$UNICORN_PREFIX/lib/libunicorn.a" ]]; then
    tarball="$(fetch "unicorn-$UNICORN_VER.tar.gz" "$UNICORN_URL" "$UNICORN_SHA")"
    rm -rf "$WORK/unicorn-src" "$WORK/unicorn-build"
    mkdir -p "$WORK/unicorn-src"
    tar xzf "$tarball" -C "$WORK/unicorn-src" --strip-components=1
    log "building Unicorn $UNICORN_VER (static, x86 only)"
    cmake -S "$WORK/unicorn-src" -B "$WORK/unicorn-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$UNICORN_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DUNICORN_LEGACY_STATIC_ARCHIVE=ON \
        -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=ON >/dev/null
    cmake --build "$WORK/unicorn-build" >/dev/null
    cmake --install "$WORK/unicorn-build" >/dev/null
fi

# --- the runner --------------------------------------------------------------
BUILD="$WORK/runner-build"
log "building fg-runner $VERSION"
rm -rf "$BUILD"
PKG_CONFIG_PATH="$SDL_PREFIX/lib/pkgconfig" cmake -S "$ROOT/runner" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DFG_UNICORN_ROOT="$UNICORN_PREFIX" \
    -DFG_PORTABLE=ON -DFG_VERSION="$VERSION" >/dev/null
cmake --build "$BUILD" >/dev/null

# --- package -----------------------------------------------------------------
log "packaging $NAME.tar.gz"
PKG="$WORK/$NAME"
rm -rf "$PKG"
mkdir -p "$PKG/lib"
strip -o "$PKG/fg-runner" "$BUILD/fg-runner"
cp -L "$SDL_PREFIX/lib/libSDL3.so.0" "$PKG/lib/"
strip "$PKG/lib/libSDL3.so.0"
cp "$ROOT/runner/linux/fg-runner.png" "$PKG/"
install -m 755 "$ROOT/runner/linux/install-desktop-entry.sh" "$PKG/"
cp "$ROOT/runner/linux/README-linux.txt" "$PKG/README.txt"
cp "$ROOT/LICENSE" "$PKG/LICENSE.txt"
cp "$ROOT/runner/third_party/fonts/OFL-Inter.txt" "$ROOT/runner/third_party/fonts/OFL-Metamorphous.txt" "$PKG/"
cp "$WORK/sdl3-src/LICENSE.txt" "$PKG/LICENSE-SDL3.txt"
cp "$WORK/unicorn-src/COPYING" "$PKG/LICENSE-Unicorn.txt"

# Nothing but the C library and SDL3 may be needed from outside the package.
needed="$(readelf -d "$PKG/fg-runner" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')"
for lib in $needed; do
    case "$lib" in
        libSDL3.so.0|libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|ld-linux-x86-64.so.2) ;;
        *) echo "unexpected library dependency: $lib" >&2; exit 1 ;;
    esac
done
glibc="$(objdump -T "$PKG/fg-runner" "$PKG/lib/libSDL3.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
log "needs: $needed(newest symbol $glibc)"

mkdir -p "$DIST"
rm -f "$DIST/$NAME.tar.gz"
tar czf "$DIST/$NAME.tar.gz" -C "$WORK" --owner=0 --group=0 "$NAME"
log "done: $DIST/$NAME.tar.gz"
