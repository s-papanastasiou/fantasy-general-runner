#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install the reverse-engineering toolchain into tools/vendor/ (git-ignored).
#
#   tools/setup_tooling.sh          install anything missing
#   tools/setup_tooling.sh --force  wipe tools/vendor and reinstall everything
#
# Everything is pinned to an exact version and verified by SHA-256.
# Nothing is installed outside this repository.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/tools/vendor"
DL="$VENDOR/downloads"

# --- pinned versions -------------------------------------------------------
JDK_URL="https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.5.1%2B2/OpenJDK25U-jdk_x64_linux_hotspot_25.0.5.1_2.tar.gz"
JDK_SHA="760f91b22c3efc9bd1b64ff950598231be82928cc807e30691a0e7a2995a62c0"

GHIDRA_VER="12.2"
GHIDRA_URL="https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.2_build/ghidra_12.2_PUBLIC_20261016.zip"
GHIDRA_SHA="2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67"

MCP_VER="7.1.0"
MCP_EXT_URL="https://github.com/bethington/ghidra-mcp/releases/download/v7.1.0/GhidraMCP-7.1.0.zip"
MCP_EXT_SHA="015176c1842dec635ca44907f5aafd1be8a3b7f307cf401a13ad64336a4c18b4"
MCP_WHL_URL="https://github.com/bethington/ghidra-mcp/releases/download/v7.1.0/ghidra_mcp_bridge-7.1.0-py3-none-any.whl"
MCP_WHL_SHA="cbeee8e039838f6d85f2e9193923765d6b7bfeb71db8a35d176377119842486b"

OW_TAG="2026-11-01-Build"
OW_URL="https://github.com/open-watcom/open-watcom-v2/releases/download/$OW_TAG/ow-snapshot.tar.xz"
OW_SHA="89e90b7544cf3ea4a38124ec4c104a5027f31101405d7115c3f833daa465bb48"

# --- helpers ---------------------------------------------------------------
log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# fetch NAME URL SHA -> path of verified file (each download in its own dir)
fetch() {
    local name="$1" url="$2" sha="$3"
    local dir="$DL/$name" file="$DL/$name/$(basename "${url//%2B/+}")"
    mkdir -p "$dir"
    if [[ -f "$file" ]] && echo "$sha  $file" | sha256sum -c --status; then
        echo "$file"; return
    fi
    log "downloading $name" >&2
    curl -fL --retry 3 -o "$file.part" "$url" >&2
    if ! echo "$sha  $file.part" | sha256sum -c --status; then
        echo "checksum mismatch for $name ($url)" >&2
        rm -f "$file.part"; exit 1
    fi
    mv "$file.part" "$file"
    echo "$file"
}

# extract ARCHIVE into DEST, stripping the single top-level directory
extract() {
    local archive="$1" dest="$2" tmp
    tmp="$(mktemp -d "$VENDOR/.extract.XXXXXX")"
    case "$archive" in
        *.zip) unzip -q "$archive" -d "$tmp" ;;
        *.tar.gz|*.tgz) tar -xzf "$archive" -C "$tmp" ;;
        *.tar.xz) tar -xJf "$archive" -C "$tmp" ;;
    esac
    shopt -s nullglob dotglob
    local entries=("$tmp"/*)
    rm -rf "$dest"
    if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then
        mv "${entries[0]}" "$dest"
    else
        mkdir -p "$dest"; mv "$tmp"/* "$dest"/
    fi
    rm -rf "$tmp"
}

if [[ "${1:-}" == "--force" ]]; then
    log "removing $VENDOR"
    rm -rf "$VENDOR"
fi
mkdir -p "$VENDOR" "$DL"

# --- Java 25 (Temurin) -----------------------------------------------------
if [[ ! -x "$VENDOR/jdk/bin/java" ]]; then
    f="$(fetch jdk "$JDK_URL" "$JDK_SHA")"
    log "installing JDK 25"
    extract "$f" "$VENDOR/jdk"
fi

# --- Ghidra ----------------------------------------------------------------
GHIDRA="$VENDOR/ghidra"
if [[ ! -x "$GHIDRA/ghidraRun" || "$(cat "$GHIDRA/.fg-version" 2>/dev/null)" != "$GHIDRA_VER" ]]; then
    f="$(fetch ghidra "$GHIDRA_URL" "$GHIDRA_SHA")"
    log "installing Ghidra $GHIDRA_VER"
    extract "$f" "$GHIDRA"
    echo "$GHIDRA_VER" > "$GHIDRA/.fg-version"
fi
# Point Ghidra at our JDK regardless of the system Java.
props="$GHIDRA/support/launch.properties"
if ! grep -q "^JAVA_HOME_OVERRIDE=$VENDOR/jdk\$" "$props"; then
    sed -i '/^JAVA_HOME_OVERRIDE=/d' "$props"
    echo "JAVA_HOME_OVERRIDE=$VENDOR/jdk" >> "$props"
fi

# Watcom compiler spec: install the cspec and register it for x86:LE:32:default.
langs="$GHIDRA/Ghidra/Processors/x86/data/languages"
cp "$ROOT/tools/ghidra/x86watcom.cspec" "$langs/x86watcom.cspec"
if ! grep -q 'id="watcom"' "$langs/x86.ldefs"; then
    log "registering Watcom compiler spec with Ghidra"
    # Insert after the gcc entry of the 32-bit language only (the first match).
    sed -i '0,/<compiler name="gcc" spec="x86gcc.cspec" id="gcc"\/>/s##&\n    <compiler name="Watcom" spec="x86watcom.cspec" id="watcom"/>#' "$langs/x86.ldefs"
fi

# --- GhidraMCP extension (installed into Ghidra's own Extensions folder) ----
if [[ ! -d "$GHIDRA/Ghidra/Extensions/GhidraMCP" || "$(cat "$GHIDRA/Ghidra/Extensions/GhidraMCP/.fg-version" 2>/dev/null)" != "$MCP_VER" ]]; then
    f="$(fetch ghidra-mcp-ext "$MCP_EXT_URL" "$MCP_EXT_SHA")"
    log "installing GhidraMCP $MCP_VER extension"
    extract "$f" "$GHIDRA/Ghidra/Extensions/GhidraMCP"
    echo "$MCP_VER" > "$GHIDRA/Ghidra/Extensions/GhidraMCP/.fg-version"
fi

# --- GhidraMCP bridge (MCP server, in its own venv) ------------------------
BRIDGE="$VENDOR/ghidra-mcp-bridge"
if [[ ! -x "$BRIDGE/bin/bridge-mcp-ghidra" || "$(cat "$BRIDGE/.fg-version" 2>/dev/null)" != "$MCP_VER" ]]; then
    f="$(fetch ghidra-mcp-bridge "$MCP_WHL_URL" "$MCP_WHL_SHA")"
    log "installing GhidraMCP $MCP_VER bridge"
    rm -rf "$BRIDGE"
    uv venv -q "$BRIDGE"
    VIRTUAL_ENV="$BRIDGE" uv pip install -q "$f"
    echo "$MCP_VER" > "$BRIDGE/.fg-version"
fi

# Shared secret between Ghidra's MCP plugin and the bridge. Required because
# tools/ghidra.sh enables script execution over the plugin's HTTP port.
if [[ ! -s "$VENDOR/ghidra-mcp.token" ]]; then
    log "generating GhidraMCP auth token"
    (umask 077; head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$VENDOR/ghidra-mcp.token")
fi

# --- Open Watcom v2 (runtime source/headers, wdump, manuals) ---------------
OW="$VENDOR/openwatcom"
if [[ ! -x "$OW/binl64/wdump" ]]; then
    f="$(fetch openwatcom "$OW_URL" "$OW_SHA")"
    log "installing Open Watcom v2"
    extract "$f" "$OW"
fi

# --- Open Watcom runtime library sources (reference only) ------------------
# The binary snapshot has no C runtime source; fetch just those directories
# from the same build tag so we can compare against the game's runtime code.
OWSRC="$VENDOR/openwatcom-src"
if [[ ! -d "$OWSRC/bld/clib" ]]; then
    log "fetching Open Watcom runtime sources ($OW_TAG)"
    rm -rf "$OWSRC"
    git clone -q --depth 1 --branch "$OW_TAG" --filter=blob:none --sparse \
        https://github.com/open-watcom/open-watcom-v2.git "$OWSRC"
    git -C "$OWSRC" sparse-checkout set bld/clib bld/lib_misc bld/mathlib bld/cpplib bld/hdr bld/watcom/h
fi

log "done"
echo "  Ghidra GUI:       tools/ghidra.sh"
echo "  Ghidra headless:  tools/ghidra.sh --headless <args>"
echo "  Open Watcom env:  source tools/openwatcom-env.sh"
