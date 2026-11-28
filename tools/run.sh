#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build (if needed) and run the runner with this repo's defaults.
#   tools/run.sh [runner options] [-- game args]
#   Builds into build/.
# Uses extracted/cd as drive D: (created by tools/extract_cd.py on first run).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${FG_GAME_DIR:-$HOME/GOG Games/Fantasy General/game/data}"
BUILD="$ROOT/build"
if [[ ! -f "$BUILD/build.ninja" ]]; then
    cmake -S "$ROOT/runner" -B "$BUILD" -G Ninja >/dev/null
fi
cmake --build "$BUILD" >/dev/null
if [[ ! -d "$ROOT/extracted/cd/FG" ]]; then
    python3 -I "$ROOT/tools/extract_cd.py" "$GAME_DIR" --out "$ROOT/extracted/cd"
fi
exec "$BUILD/fg-runner" --game-dir "$GAME_DIR" --cd-dir "$ROOT/extracted/cd" "$@"
