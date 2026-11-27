#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Launch the repo-local Ghidra (installed by tools/setup_tooling.sh).
#   tools/ghidra.sh                 GUI
#   tools/ghidra.sh --headless ...  analyzeHeadless with the given arguments
#
# The GhidraMCP plugin is started with script execution enabled, so every
# request to it must carry the token in tools/vendor/ghidra-mcp.token (the
# bridge wrapper tools/ghidra-mcp-bridge.sh sends it), and file paths it
# accepts are confined to this repository.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GHIDRA="$ROOT/tools/vendor/ghidra"
TOKEN_FILE="$ROOT/tools/vendor/ghidra-mcp.token"
[[ -x "$GHIDRA/ghidraRun" && -s "$TOKEN_FILE" ]] || { echo "run tools/setup_tooling.sh first" >&2; exit 1; }
export JAVA_HOME="$ROOT/tools/vendor/jdk"
export GHIDRA_MCP_AUTH_TOKEN="$(cat "$TOKEN_FILE")"
export GHIDRA_MCP_ALLOW_SCRIPTS=1
export GHIDRA_MCP_FILE_ROOT="$ROOT"
if [[ "${1:-}" == "--headless" ]]; then
    shift
    exec "$GHIDRA/support/analyzeHeadless" "$@"
fi
exec "$GHIDRA/ghidraRun" "$@"
