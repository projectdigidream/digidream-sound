#!/usr/bin/env bash
#
# The desktop app.
#
#   ./scripts/package_electron.sh                installers for this platform
#   ./scripts/package_electron.sh --dir          an unpacked app, faster
#   ./scripts/package_electron.sh --stage-only   just the payload
#   ./scripts/package_electron.sh --linux        cross-build for Linux
#
# This stages the EXACT payload scripts/package_web.sh produces, so the browser
# and the desktop run byte-identical tool code. That is worth the indirection:
# a desktop-only copy of the UI is a second thing to fix every time.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="${1:-dist}"

./scripts/package_web.sh electron/payload > /dev/null
# The shell serves over dsnd://, so neither the tarball nor the fallback server
# belongs in the app bundle.
rm -f electron/payload.tar.gz electron/payload/serve.js
echo "staged electron/payload ($(du -sh electron/payload | cut -f1))"

if [ "$MODE" = "--stage-only" ]; then exit 0; fi

cd electron
if [ ! -d node_modules ]; then
  echo "installing electron and electron-builder (first run only)…"
  npm install --silent --no-audit --no-fund
fi

case "$MODE" in
  --dir)   npx electron-builder --dir ;;
  --linux) npx electron-builder --linux ;;
  *)       npx electron-builder ;;
esac
echo "package_electron: see electron/dist/"
