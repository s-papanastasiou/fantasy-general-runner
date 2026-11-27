#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# MCP server for Claude Code (see .mcp.json): runs the GhidraMCP bridge with
# the auth token that tools/ghidra.sh gives the Ghidra plugin.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GHIDRA_MCP_URL="${GHIDRA_MCP_URL:-http://127.0.0.1:8089}"
export GHIDRA_MCP_AUTH_TOKEN="$(cat "$ROOT/tools/vendor/ghidra-mcp.token")"
exec "$ROOT/tools/vendor/ghidra-mcp-bridge/bin/bridge-mcp-ghidra" --transport stdio "$@"
