#!/usr/bin/env bash
#
# The whole gate. Everything here runs unattended and offline.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== source lists =="
./scripts/check_dsp_sources.sh

echo
echo "== build =="
cmake -S . -B build -G Ninja >/dev/null
ninja -C build

echo
echo "== unit tests =="
./build/dsound_tests

echo
echo "== goldens =="
./scripts/run_goldens.sh

echo
echo "== vendored ui =="
./scripts/check_lib_sync.sh

echo
echo "== document model =="
# The editor's model of a .sfxproj must not change a document it did not mean
# to. Both shipped banks are pinned goldens, so a lossy round trip is a silent
# content change the gate would then re-pin.
if command -v node >/dev/null 2>&1; then
  node test/proj_roundtrip.mjs | tail -1
  node test/validate_agrees.mjs | tail -1
else
  echo "skip (no node)"
fi

echo
echo "== wasm bank path =="
if [ -f tool/dist/dsound.mjs ] && command -v node >/dev/null 2>&1; then
  ./build/dsound_bake test/golden/identity.sfxproj build/identity.sfxb --quiet
  node test/wasm_bank.mjs
else
  echo "skip (no tool/dist/dsound.mjs)"
fi

echo
echo "== wasm editor surface =="
# Everything the editor depends on that the goldens do not reach:
#   variant  -- renderVariant is the cache's own renderer, so a waveform drawn
#               from it is the waveform the game plays
#   bake     -- the browser produces the shipping artifact byte-identically
#   worklet  -- the module loads where it will actually run
#   parity   -- the JS face and the C face describe the same engine
if [ -f tool/dist/dsound.mjs ] && command -v node >/dev/null 2>&1; then
  for t in wasm_variant wasm_bake worklet_env embind_parity wasm_memory content_levels; do
    printf '%-16s ' "$t"
    node "test/$t.mjs" > "build/$t.log" 2>&1 \
      && tail -1 "build/$t.log" \
      || { echo "FAILED -- see build/$t.log"; tail -20 "build/$t.log"; exit 1; }
  done
else
  echo "skip (no tool/dist/dsound.mjs)"
fi

echo
echo "== content =="
# Every shipped bank must load, bake, and contain no silent sound. A recipe
# that renders silence is the commonest authoring mistake and the one a UI
# with a hundred entries hides best.
for b in content/banks/*.sfxproj content/game/*.sfxproj; do
  [ -e "$b" ] || continue
  ./build/dsound_bake "$b" "$(mktemp -t sfxb).sfxb" --quiet
  echo "ok   $b bakes"
done

echo
echo
echo "== standalone =="
# An assertion of standalone-ness is worth nothing; a build of the extracted
# copy is. This vendors midisynth in, drops content/game/, and runs the
# extracted tree's OWN gate with this repository nowhere on the include path.
#
# Skippable because it roughly doubles the gate's wall time and the thing it
# proves changes rarely -- but it is ON by default, because the failure it
# catches (a header nobody noticed was being read from a sibling) is invisible
# until someone tries to ship.
if [ "${DSND_SKIP_STANDALONE:-0}" = "1" ]; then
  echo "skip (DSND_SKIP_STANDALONE=1)"
elif [ -d src/vendor/synth ]; then
  echo "skip (already a standalone tree)"
else
  ./scripts/make_standalone.sh build/standalone | tail -1
  ( cd build/standalone && ./scripts/ci.sh > ../standalone-ci.log 2>&1 ) \
    && echo "ok   the extracted tree builds and passes its own gate" \
    || { echo "FAIL the extracted tree does not pass its own gate:" >&2
         tail -30 build/standalone-ci.log >&2; exit 1; }
fi

echo
echo "CI gate: all green"
