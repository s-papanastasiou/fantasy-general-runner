#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build both release packages in containers (Podman or Docker), the same way
# as the GitHub workflow (.github/workflows/release.yml):
#
#   tools/build_release.sh [VERSION]     (default: from `git describe`)
#
#   Linux:   Ubuntu 22.04 -> dist/fantasy-general-runner-VERSION-linux-x86_64.tar.gz
#   Windows: Fedora 45    -> dist/fantasy-general-runner-VERSION-windows-x64.zip
#   plus dist/SHA256SUMS-VERSION.txt
#
# Downloads and dependency builds are kept in tools/vendor/ between runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always --dirty)}"
VERSION="${VERSION#v}"   # tag v0.1.0 -> 0.1.0
LINUX_IMAGE="docker.io/library/ubuntu:22.04"
WINDOWS_IMAGE="registry.fedoraproject.org/fedora:45"

ENGINE="${CONTAINER_ENGINE:-}"   # podman or docker; default: whichever exists
if [[ -z "$ENGINE" ]]; then command -v podman >/dev/null && ENGINE=podman || ENGINE=docker; fi
if [[ "$ENGINE" == podman ]] && command -v podman >/dev/null; then
    ENGINE=podman
    MOUNT_OPTS=":z"   # SELinux label; rootless: files stay the user's
elif [[ "$ENGINE" == docker ]] && command -v docker >/dev/null; then
    ENGINE=docker
    MOUNT_OPTS=""
else
    echo "needs podman or docker (CONTAINER_ENGINE=$ENGINE)" >&2
    exit 1
fi

# run IMAGE SCRIPT: the script runs as root in the container on this checkout.
run() {
    local fix_owner=""
    # Docker runs as real root: give the build output back to the user.
    [[ "$ENGINE" == docker ]] && fix_owner="; chown -R $(id -u):$(id -g) /src/dist /src/tools/vendor /src/build-windows 2>/dev/null || true"
    "$ENGINE" run --rm -v "$ROOT":/src$MOUNT_OPTS -w /src "$1" \
        bash -c "bash $2 '$VERSION'; status=\$?$fix_owner; exit \$status"
}

printf '\033[1;34m==>\033[0m %s\n' "release $VERSION: Linux ($LINUX_IMAGE)"
run "$LINUX_IMAGE" tools/release/linux-build.sh
printf '\033[1;34m==>\033[0m %s\n' "release $VERSION: Windows ($WINDOWS_IMAGE)"
run "$WINDOWS_IMAGE" tools/release/windows-build.sh

cd "$ROOT/dist"
files=("fantasy-general-runner-$VERSION-linux-x86_64.tar.gz" "fantasy-general-runner-$VERSION-windows-x64.zip")
sha256sum "${files[@]}" > "SHA256SUMS-$VERSION.txt"
printf '\033[1;34m==>\033[0m %s\n' "done:"
ls -l "${files[@]}" "SHA256SUMS-$VERSION.txt"
