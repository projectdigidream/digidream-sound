#!/usr/bin/env bash
#
# Assemble a tree you can lift out of this repository and build.
#
#   ./scripts/make_standalone.sh [outdir]      default: build/standalone
#
# WHY THIS EXISTS AS A SCRIPT AND NOT AS A CLAIM. "It is standalone" is not
# checkable by reading; a build of the extracted copy is. scripts/ci.sh runs
# this and then runs the extracted tree's OWN ci.sh, with this repository
# nowhere on the include path. That is the only version of the statement worth
# making.
#
# Two things get vendored, and DSND_SYNTH_DIR is the single place either path
# lives (CMakeLists.txt and scripts/build_wasm.sh both read it):
#
#   ../midisynth/src            -> src/vendor            the borrowed DSP
#   ../engine/core/include/dnp/math -> src/vendor/include  fixed.hpp
#
# And one thing is deliberately left out: content/game/. The audio there is as
# original as anything else in this project, but its ROLE NAMES reference
# another game's bestiary, and it is engine-side content rather than part of
# the product. scripts/check_no_samples.sh guards the other half of that.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="${1:-build/standalone}"
SYNTH="${DSND_SYNTH_DIR:-../midisynth/src}"
FIXED="${DSND_FIXED_DIR:-../engine/core/include}"

if [ -d src/vendor/synth ]; then
  echo "make_standalone: this tree is already standalone" >&2
  exit 1
fi
for d in "$SYNTH" "$FIXED"; do
  [ -d "$d" ] || { echo "make_standalone: $d is missing" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT"

# ---- our own tree ---------------------------------------------------------
cp -R src test scripts docs tool android "$OUT/"
cp CMakeLists.txt dsp_manifest.txt README.md LICENSE .gitignore "$OUT/" 2>/dev/null || true
mkdir -p "$OUT/content/banks"
cp content/banks/*.sfxproj "$OUT/content/banks/"

# Build output and the fitter's references are not part of a source tree.
rm -rf "$OUT/build" "$OUT/tool/dist"

# ---- the vendored DSP -----------------------------------------------------
# Headers wholesale (the manifest's HEADERS_ONLY set is large and interlinked),
# sources only as the manifest's COMPILED set names them -- so an extracted
# tree cannot quietly compile something the in-repo build does not.
mkdir -p "$OUT/src/vendor/include/dnp/math"

# Absolute, resolved once. `cp --parents` is a GNU extension that macOS does
# not have, and computing the destination inside a subshell from $OLDPWD picks
# up whatever directory the script was INVOKED from -- both of which I got
# wrong here, and both fail by copying nothing rather than by erroring.
SYNTH_ABS="$(cd "$SYNTH" && pwd)"
OUT_ABS="$(cd "$OUT" && pwd)"
HDRS=0
while IFS= read -r f; do
  rel="${f#"$SYNTH_ABS"/}"
  mkdir -p "$OUT_ABS/src/vendor/$(dirname "$rel")"
  cp "$f" "$OUT_ABS/src/vendor/$rel"
  HDRS=$((HDRS + 1))
# .inc as well as .h: synth/tables.cpp includes tables_baked.inc, which is the
# baked integer tables and is not a header by extension. Globbing only *.h
# produced a tree that configured cleanly and then failed one file into the
# build -- so take everything that is not a .cpp.
done < <(find "$SYNTH_ABS" \( -name '*.h' -o -name '*.inc' -o -name '*.hpp' \))
[ "$HDRS" -gt 0 ] || { echo "make_standalone: copied no headers from $SYNTH" >&2; exit 1; }

awk '/^COMPILED/{on=1;next} /^[A-Z_]+$/{on=0} on' dsp_manifest.txt \
  | sed -E 's/#.*//' | tr -d ' \t' | grep -v '^$' \
  | while read -r rel; do
      src="$SYNTH/${rel#src/}"
      dst="$OUT/src/vendor/${rel#src/}"
      mkdir -p "$(dirname "$dst")"
      cp "$src" "$dst"
    done

cp "$FIXED/dnp/math/fixed.hpp" "$OUT/src/vendor/include/dnp/math/"
# trig.hpp if the spatialiser needs it; harmless when it does not.
[ -f "$FIXED/dnp/math/trig.hpp" ] && cp "$FIXED/dnp/math/trig.hpp" "$OUT/src/vendor/include/dnp/math/"

cat > "$OUT/src/vendor/README.md" <<'MD'
# Vendored

Assembled by `scripts/make_standalone.sh`. Do not edit by hand.

`synth/` and `output/` are midisynth's synthesis primitives, compiled from
source rather than forked — which is what makes "the same patch sounds the same
in both projects" a fact rather than a hope. `include/dnp/math/fixed.hpp` is
the fixed-point type aliases and shift helpers the whole engine is built on.

In the development tree these are read from `../midisynth/src` and
`../engine/core/include` through `DSND_SYNTH_DIR` / `DSND_FIXED_DIR`. Here they are
copies, and `scripts/check_dsp_sources.sh` becomes a no-op: there is no longer
a sibling to drift from.
MD

# ---- the bits that only make sense in-repo --------------------------------
rm -f "$OUT/scripts/make_standalone.sh" "$OUT/scripts/sync_lib.sh"

# The check that would have caught both of the copy bugs above, and any future
# one: the tree CMake is about to look for has to actually be there.
for must in src/vendor/synth/config.h src/vendor/include/dnp/math/fixed.hpp \
            src/vendor/synth/voice.cpp src/vendor/output/fit.h \
            src/vendor/synth/tables_baked.inc; do
  [ -f "$OUT/$must" ] || { echo "make_standalone: $must is missing from the extract" >&2; exit 1; }
done

SIZE=$(du -sh "$OUT" | cut -f1)
N=$(find "$OUT" -type f | wc -l | tr -d ' ')
echo "make_standalone: $N files, $SIZE, $HDRS headers vendored"
echo "  build it:  cd $OUT && ./scripts/ci.sh"
