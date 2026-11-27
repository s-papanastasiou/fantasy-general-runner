#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# (Re)create the Ghidra project from the original game executable.
#
#   tools/ghidra/import.sh [path/to/BARENA.EXE]
#
# Steps: extract the LE image to extracted/, then import extracted/image.elf
# into ghidra-project/FantasyGeneral.gpr using the Watcom compiler spec and run
# Ghidra's auto-analysis. Refuses to overwrite an existing project (it holds
# hand-made annotations); delete ghidra-project/ yourself to start over.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GAME_DIR="${FG_GAME_DIR:-$HOME/GOG Games/Fantasy General/game/data}"
EXE="${1:-$GAME_DIR/EXE/BARENA.EXE}"
PROJ_DIR="$ROOT/ghidra-project"
PROJ_NAME="FantasyGeneral"

if [[ -e "$PROJ_DIR/$PROJ_NAME.gpr" ]]; then
    echo "Ghidra project already exists: $PROJ_DIR/$PROJ_NAME.gpr" >&2
    echo "Delete $PROJ_DIR to re-import from scratch." >&2
    exit 1
fi

python3 -I "$ROOT/tools/le_extract.py" "$EXE" --out "$ROOT/extracted"
cp "$ROOT/extracted/image.elf" "$ROOT/extracted/BARENA.elf"

mkdir -p "$PROJ_DIR"
"$ROOT/tools/ghidra.sh" --headless "$PROJ_DIR" "$PROJ_NAME" \
    -import "$ROOT/extracted/BARENA.elf" \
    -processor x86:LE:32:default -cspec watcom \
    -scriptPath "$ROOT/tools/ghidra/scripts" \
    -postScript FGPostImport.java "$ROOT/extracted" \
    -postScript FGRecoverCode.java "$ROOT/extracted" \
    -max-cpu "$(nproc)" \
    -log "$PROJ_DIR/import.log" -scriptlog "$PROJ_DIR/script.log"
echo "Project ready: $PROJ_DIR/$PROJ_NAME.gpr  (open with tools/ghidra.sh)"
