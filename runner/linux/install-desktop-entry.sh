#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Add Fantasy General (this unpacked runner) to the applications menu of the
# current user, with its icon; --remove takes the entry out again.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
entry="$apps/fantasy-general-runner.desktop"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$entry"
    echo "removed $entry"
    exit 0
fi

mkdir -p "$apps"
# Desktop entries need quoting of the program path if it contains spaces.
exec_path=$(printf '%s' "$here/fg-runner" | sed 's/["`$\\]/\\&/g')
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=Fantasy General
Comment=SSI's Fantasy General (1996) from your GOG installation
Exec="$exec_path"
Path=$here
Icon=$here/fg-runner.png
Terminal=false
Categories=Game;StrategyGame;
StartupWMClass=fantasy-general-runner
EOF
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" || true
echo "added $entry"
