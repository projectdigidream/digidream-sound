#!/usr/bin/env bash
#
# The vendored UI modules must be what tool/lib/MANIFEST says they are.
#
# This is the other half of sync_lib.sh. The reason to check against the
# manifest rather than against ../midisynth is that midisynth's tool/lib is
# largely uncommitted and moves independently: a check that compared one
# working tree to another would fail for reasons that have nothing to do with
# this project, and would pass in a standalone tree only by being skipped.
#
# So the property is narrow and true: nobody has edited our copies. Taking a
# newer midisynth is a deliberate act -- run sync_lib.sh, read the diff, run
# the gate -- and it shows up here as a manifest change in the same commit.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DST=tool/lib
MANIFEST="$DST/MANIFEST"

if [ ! -f "$MANIFEST" ]; then
  echo "check_lib_sync: no $MANIFEST -- run scripts/sync_lib.sh" >&2
  exit 1
fi

fail=0
n=0
while read -r want file; do
  case "$want" in \#*|"") continue ;; esac
  [ -n "$file" ] || continue
  n=$((n + 1))
  if [ ! -f "$DST/$file" ]; then
    echo "check_lib_sync: $DST/$file is missing -- run scripts/sync_lib.sh" >&2
    fail=1
    continue
  fi
  got=$(shasum -a 256 "$DST/$file" | cut -d' ' -f1)
  if [ "$got" != "$want" ]; then
    echo "check_lib_sync: $DST/$file has been edited locally." >&2
    echo "    manifest $want" >&2
    echo "    on disk  $got" >&2
    echo "  These modules are vendored from midisynth and must stay byte-identical," >&2
    echo "  so a fix belongs in midisynth and comes back through sync_lib.sh." >&2
    echo "  Adapt from OUTSIDE instead: the page's CSS and the shim in" >&2
    echo "  tool/sfx/patch-shim.js exist for exactly that." >&2
    fail=1
  fi
done < "$MANIFEST"

# A module that appeared in tool/lib without going through the manifest is the
# same drift in the other direction.
for f in "$DST"/*.js; do
  [ -e "$f" ] || continue
  base=$(basename "$f")
  if ! grep -q "  $base\$" "$MANIFEST"; then
    echo "check_lib_sync: $base is in $DST but not in the manifest." >&2
    echo "  Either it is vendored (add it to sync_lib.sh) or it is ours (say so)." >&2
    fail=1
  fi
done

[ "$fail" -eq 0 ] || exit 1
echo "check_lib_sync: ok ($n vendored modules match the manifest)"
