#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Windows release package. Runs INSIDE Fedora (a container from
# tools/build_release.sh, or a CI job): installs the MinGW cross-compiler
# and SDL3 for Windows when run as root, then tools/build_windows.sh.
#
#   tools/release/windows-build.sh VERSION
#
# Result: dist/fantasy-general-runner-VERSION-windows-x64.zip
set -euo pipefail

VERSION="${1:?usage: windows-build.sh VERSION}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$(id -u)" == 0 ]] && command -v dnf >/dev/null; then
    printf '\033[1;34m==>\033[0m %s\n' "installing build packages" >&2
    dnf install -y -q mingw64-gcc-c++ mingw64-SDL3 cmake ninja-build zip curl binutils pkgconf-pkg-config >/dev/null
fi
exec "$ROOT/tools/build_windows.sh" "$VERSION"
