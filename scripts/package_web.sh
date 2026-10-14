#!/usr/bin/env bash
#
# Stage the editor as a static site.
#
#   ./scripts/package_web.sh [outdir]      default: dist-web
#
# The result is a directory you can drop on any static host. Two requirements,
# and only two:
#
#   * HTTPS, or localhost. AudioWorklet needs a secure context; plain HTTP on a
#     LAN address will load the page and then fail to make a sound, which is a
#     miserable thing to debug.
#   * .mjs served as text/javascript. Everything here is ES modules.
#
# No COOP/COEP headers are needed. The wasm ships as a single-file ES module
# imported straight into the worklet, which is the entire reason this project
# avoided -sAUDIO_WORKLET and SharedArrayBuffer.
#
# This script is also the payload stager for the desktop and mobile shells, so
# all three ship byte-identical tool code -- that is why it takes an outdir
# rather than hardcoding one.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="${1:-dist-web}"

if [ ! -f tool/dist/dsound.mjs ]; then
  echo "tool/dist/dsound.mjs is missing -- run ./scripts/build_wasm.sh first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/tool" "$OUT/content/banks"

cp tool/sfx.html "$OUT/tool/"
cp tool/sfx-worklet.js tool/tool-worker.js "$OUT/tool/"
cp -R tool/sfx tool/lib tool/dist "$OUT/tool/"

# The starter bank ships; content/game/ does NOT. It is engine-side content
# whose role names reference another game's bestiary, and scripts/
# make_standalone.sh excludes it for the same reason.
cp content/banks/starter.sfxproj "$OUT/content/banks/"

# A production fallback, for someone who just wants to run it locally.
cp tool/serve.js "$OUT/"

cat > "$OUT/index.html" <<'HTML'
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DigidreamSound</title>
<style>
  :root { color-scheme: dark }
  body { margin: 0; min-height: 100vh; display: flex; align-items: center;
         justify-content: center; background: #14161a; color: #d6d9e0;
         font: 15px/1.6 -apple-system, "Segoe UI", sans-serif; padding: 24px }
  main { max-width: 40em }
  h1 { color: #64b5f6; margin: 0 0 4px }
  p { color: #8a8f9c }
  a.card { display: block; margin-top: 20px; padding: 16px 18px; border-radius: 10px;
           background: #1d2026; border: 1px solid #32363f; color: #d6d9e0;
           text-decoration: none }
  a.card:hover { border-color: #64b5f6 }
  a.card b { color: #64b5f6 }
</style>
</head>
<body>
<main>
  <h1>DigidreamSound</h1>
  <p>A sound-effect engine and its editor. Sounds are recipes rather than
     recordings: what ships is a few hundred bytes of synthesis parameters, and
     the engine renders them identically on every platform.</p>
  <a class="card" href="tool/sfx.html">
    <b>Open the editor →</b>
    <div>Bank, Design, Fit and Play, with the starter bank loaded.</div>
  </a>
</main>
</body>
</html>
HTML

tar -czf "$OUT.tar.gz" "$OUT"
SIZE=$(du -sh "$OUT" | cut -f1)
TGZ=$(du -h "$OUT.tar.gz" | cut -f1)
echo "package_web: $OUT ($SIZE, $TGZ packed)"
echo "  try it:  node $OUT/serve.js --root $OUT"
