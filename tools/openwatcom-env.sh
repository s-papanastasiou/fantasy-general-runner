# SPDX-License-Identifier: GPL-2.0-or-later
# Source this to use the repo-local Open Watcom v2:  source tools/openwatcom-env.sh
_fg_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export WATCOM="$_fg_root/tools/vendor/openwatcom"
export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
export INCLUDE="$WATCOM/h"
export EDPATH="$WATCOM/eddat"
unset _fg_root
