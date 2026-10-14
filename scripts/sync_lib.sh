#!/usr/bin/env bash
#
# Copy the shared UI modules in from midisynth, and record what was copied.
#
# WHY A COPY AND NOT AN IMPORT. The editor shares two small modules with
# midisynth's tools -- ui.js (the boot line and the log) and desktop.js (the
# bridge a desktop or phone shell provides as window.digidream) -- and a
# product whose import paths climb out of its own directory is not
# standalone. (It used to vendor midisynth's 1878-line patch editor too; the
# editor now has its own layer editor, written for sound effects.)
#
# The objection to copying is silent drift, and check_lib_sync.sh answers it
# exactly: it fails the moment a copy differs from what was recorded here.
#
# WHAT IT VERIFIES AGAINST, which is the part worth reading. Not
# ../midisynth/tool/lib as it stands today -- that tree is largely uncommitted
# and changes under us, so a check comparing one moving target to another would
# prove nothing and would break for reasons unrelated to this project. Instead
# this writes tool/lib/MANIFEST with a sha256 per file, and the check compares
# the local copies against THAT. The manifest is committed, so "did our copy
# change" is answerable offline, in a standalone tree, with midisynth nowhere.
#
# Pulling a newer midisynth in is then a deliberate act: run this, read the
# diff, run the gate.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SRC="${DSND_LIB_SRC:-../midisynth/tool/lib}"
DST=tool/lib

if [ ! -d "$SRC" ]; then
  echo "sync_lib: no midisynth at $SRC" >&2
  echo "  This is normal in a standalone tree -- the copies are already here and" >&2
  echo "  check_lib_sync.sh verifies them against tool/lib/MANIFEST." >&2
  exit 1
fi

mkdir -p "$DST"
for f in "${FILES[@]}"; do
  if [ ! -f "$SRC/$f" ]; then
    echo "sync_lib: $SRC/$f is missing" >&2
    exit 1
  fi
  if [ -f "$DST/$f" ] && cmp -s "$SRC/$f" "$DST/$f"; then
    echo "  same     $f"
  else
    cp "$SRC/$f" "$DST/$f"
    echo "  updated  $f"
  fi
done

{
  echo "# Written by scripts/sync_lib.sh. sha256 of each module copied from"
  echo "# midisynth/tool/lib. scripts/check_lib_sync.sh verifies the local copies"
  echo "# against THIS, not against midisynth -- so the check works offline and in"
  echo "# an extracted tree, and a deliberate update is a visible diff here."
  echo "#"
  echo "# Synced from: $SRC"
  for f in "${FILES[@]}"; do
    printf '%s  %s\n' "$(shasum -a 256 "$DST/$f" | cut -d' ' -f1)" "$f"
  done
} > "$DST/MANIFEST"

echo "sync_lib: ${#FILES[@]} modules, manifest written to $DST/MANIFEST"
