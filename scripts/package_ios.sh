#!/usr/bin/env bash
# iOS, via scripts/package_mobile.sh. Present so that `ls scripts/` tells the
# truth about what this project can build; the two platforms differ only by an
# argument, so there is no second implementation to drift.
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/package_mobile.sh" ios "$@"
