#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build (if needed) and run the runner with this repo's defaults.
#   tools/run.sh [runner options] [-- game args]
#   Default CPU backend: Unicorn (64-bit build in build-unicorn/).
#   FG_CPU=native tools/run.sh ...    # the native backend (32-bit build in build/)
# Uses extracted/cd as drive D: (created by tools/extract_cd.py on first run).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${FG_GAME_DIR:-$HOME/GOG Games/Fantasy General/game/data}"
CPU="${FG_CPU:-unicorn}"
case "$CPU" in
native)  BUILD="$ROOT/build"
         CONFIG=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/runner/cmake/toolchain-i686.cmake") ;;
unicorn) BUILD="$ROOT/build-unicorn"
         CONFIG=(-DFG_CPU=unicorn) ;;
*) echo "FG_CPU must be native or unicorn" >&2; exit 2 ;;
esac
if [[ ! -d "$BUILD" ]]; then
    cmake -S "$ROOT/runner" -B "$BUILD" -G Ninja "${CONFIG[@]}" >/dev/null
fi
cmake --build "$BUILD" >/dev/null
if [[ ! -d "$ROOT/extracted/cd/FG" ]]; then
    python3 -I "$ROOT/tools/extract_cd.py" "$GAME_DIR" --out "$ROOT/extracted/cd"
fi
exec "$BUILD/fg-runner" --game-dir "$GAME_DIR" --cd-dir "$ROOT/extracted/cd" "$@"
