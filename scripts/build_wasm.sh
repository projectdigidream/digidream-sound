#!/usr/bin/env bash
#
# The web build: TWO ES6 modules, from one source list.
#
#   tool/dist/dsound.mjs        the engine. What a game ships, and what the
#                               editor loads into its AudioWorklet -- so what
#                               you hear while authoring is the shipping binary.
#
#   tool/dist/dsound-tool.mjs   the same sources PLUS the fitter. Loaded by the
#                               editor's worker only; never by a game.
#
# WHY TWO. The fitter is float code -- FFTs, libm, a descent -- and this
# project's whole claim is that its engine is deterministic to the sample. The
# weak way to show that linking them together changed nothing is a commit whose
# golden hashes did not move. The strong way is to build both and require
#
#     native == dsound.mjs == dsound-tool.mjs
#
# on every golden case, on every build, forever. scripts/run_goldens.sh does
# exactly that. It costs a second em++ invocation and proves a property that
# would otherwise decay silently.
#
# It also lets the two be sized differently: the engine needs a fraction of the
# heap the fitter does, and a game should not pay for a tool it never loads.
#
# The source lists below must match dsp_manifest.txt and CMakeLists.txt.
# scripts/check_dsp_sources.sh enforces that, including the rule that matters
# most here: the two modules must differ by EXACTLY the fitter. If they drift
# by anything else, the A/B above stops meaning "the same engine plus a fitter"
# and starts meaning "two different programs that happen to agree".
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SYNTH_DIR="${DSND_SYNTH_DIR:-../midisynth/src}"
FIXED_DIR="${DSND_FIXED_DIR:-../engine/core/include}"
[ -d src/vendor/synth ] && { SYNTH_DIR=src/vendor; FIXED_DIR=src/vendor/include; }

EMXX="${EMXX:-}"
if [ -z "$EMXX" ]; then
  for c in ../emsdk/upstream/emscripten/em++ em++; do
    if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then EMXX="$c"; break; fi
  done
fi
if [ -z "$EMXX" ]; then
  echo "em++ not found. Run ../scripts/setup_emsdk.sh, or set EMXX." >&2
  exit 1
fi

mkdir -p tool/dist

# One literal path per line. check_dsp_sources.sh parses these array bodies,
# so do not build them with a loop or a glob -- a list it cannot read is a list
# nothing checks.
SYNTH_SRCS=(
  "$SYNTH_DIR"/synth/tables.cpp
  "$SYNTH_DIR"/synth/wavetables.cpp
  "$SYNTH_DIR"/synth/wavetable_gen_additive.cpp
  "$SYNTH_DIR"/synth/wavetable_gen_extra.cpp
  "$SYNTH_DIR"/synth/wavetable_cache.cpp
  "$SYNTH_DIR"/synth/json_min.cpp
  "$SYNTH_DIR"/synth/patch_loader.cpp
  "$SYNTH_DIR"/synth/song_config.cpp
  "$SYNTH_DIR"/synth/voice.cpp
  "$SYNTH_DIR"/synth/master_chain.cpp
  "$SYNTH_DIR"/synth/effects/reverb.cpp
)

SFX_SRCS=(
  src/sfx/sfx_json.cpp
  src/sfx/bank.cpp
  src/sfx/voice_alloc.cpp
  src/sfx/instance.cpp
  src/sfx/pcm_cache.cpp
  src/sfx/spatial.cpp
  src/sfx/sfx_engine.cpp
  src/sfx/script.cpp
  src/bindings/embind_api.cpp
)

FIT_SRCS=(
  src/fit/fit.cpp
  src/bindings/embind_fit.cpp
)

# Identical in both modules. Every determinism-bearing flag lives here exactly
# once, so the two cannot be compiled differently by accident: the same source
# under different flags is a different program, and the A/B would be comparing
# those instead of what it means to.
COMMON_FLAGS=(
  -std=c++17 -O3
  -Wall -Wextra -Werror
  -fno-fast-math -ffp-contract=off -fno-strict-aliasing
  -fno-exceptions -fno-rtti
  -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0
  -I src -I src/sfx -I "$SYNTH_DIR" -I "$FIXED_DIR"
  -lembind
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSfx
  -sSINGLE_FILE=1
  # ENVIRONMENT=shell is DELIBERATE, and browsers work anyway. Read this before
  # "fixing" it to web,worker:
  #
  #   - It selects JS glue, not codegen. With SINGLE_FILE=1 the wasm is decoded
  #     from an inline string, so readAsync/readBinary/fetch are never reached
  #     and the whole web/worker preamble would be dead code.
  #   - AudioWorkletGlobalScope is neither a Window nor a WorkerGlobalScope, so
  #     ENVIRONMENT_IS_WEB and ENVIRONMENT_IS_WORKER are both FALSE inside the
  #     worklet -- the one host we care most about. Adding them changes nothing
  #     there and adds branches whose contents move with the emsdk.
  #   - The evidence is next door: midisynth ships this identical flag block
  #     and imports its module from an AudioWorklet, two Workers and a page.
  #
  # test/worklet_env.mjs pins it by loading the glue with worklet globals and
  # throwing stubs for fetch/require, which is a check rather than a belief.
  -sENVIRONMENT=shell
  -sMALLOC=emmalloc -sSTACK_SIZE=1048576 -sASSERTIONS=0
  # HEAP32 is here for renderVariant, which hands back unclipped Q8.24 i32 --
  # the only way the editor can report a peak above 0 dBFS, because render()
  # returns i16 and has already clamped it.
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16,HEAP32,HEAPF32
  -sEXPORTED_FUNCTIONS=_malloc,_free
)

# The heap cannot grow (ALLOW_MEMORY_GROWTH=0), because growth would change the
# emitted wasm and, worse, detach the cached HEAP16 view the worklet's render
# loop holds. So each module's size is a decision, and test/wasm_memory.mjs
# pins the high-water marks these are derived from.
#
#   engine: a 101-sound bank is ~25-40 MB steady state (bank, wavetables, an
#           8 MB PCM cache, two voice pools). 64 MiB is comfortable headroom.
#   tool:   the fitter's wavetable working set is bounded at ~12 MB
#           (FIT_WT_CACHE_ENTRIES) on top of the engine's own use, plus FFT
#           churn through emmalloc. 192 MiB, as before.
ENGINE_MEMORY=67108864
TOOL_MEMORY=201326592

echo "== engine module =="
"$EMXX" "${COMMON_FLAGS[@]}" \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY="$ENGINE_MEMORY" \
  "${SYNTH_SRCS[@]}" "${SFX_SRCS[@]}" \
  -o tool/dist/dsound.mjs

echo "== tool module (engine + fitter) =="
"$EMXX" "${COMMON_FLAGS[@]}" \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY="$TOOL_MEMORY" \
  "${SYNTH_SRCS[@]}" "${SFX_SRCS[@]}" "${FIT_SRCS[@]}" \
  -o tool/dist/dsound-tool.mjs

ls -l tool/dist/dsound.mjs tool/dist/dsound-tool.mjs
