cmake_minimum_required(VERSION 3.20)
project(DigidreamSound CXX)

# DigidreamSound -- a sound-effects engine and editor.
#
# Built on midisynth's synthesis primitives, which are COMPILED FROM SOURCE
# rather than forked (see dsp_manifest.txt). DSND_SYNTH_DIR is the one place
# that path lives, so the same tree builds in-repo and standalone:
#
#   in-repo      ../midisynth/src          (and ../core/include for fixed.hpp)
#   standalone    src/vendor               (assembled by scripts/make_standalone.sh)
#
# The compile flags below are copied verbatim from midisynth's, and that is
# load-bearing rather than tidy: the same source compiled with different flags
# is a different program, and the cross-project golden would start failing for
# a reason nobody would look for.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/synth/config.h")
  set(DSND_SYNTH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/vendor" CACHE PATH "midisynth src root")
  set(DSND_FIXED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/include" CACHE PATH "dnp/math/fixed.hpp root")
else()
  set(DSND_SYNTH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../midisynth/src" CACHE PATH "midisynth src root")
  set(DSND_FIXED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" CACHE PATH "dnp/math/fixed.hpp root")
endif()

if(NOT EXISTS "${DSND_SYNTH_DIR}/synth/config.h")
  message(FATAL_ERROR
    "DSND_SYNTH_DIR=${DSND_SYNTH_DIR} does not contain synth/config.h.\n"
    "In-repo this should be ../midisynth/src; standalone, run scripts/make_standalone.sh.")
endif()

# Keep in step with dsp_manifest.txt and scripts/build_wasm.sh.
# scripts/check_dsp_sources.sh fails the build if they disagree.
set(DSND_SYNTH_SOURCES
  ${DSND_SYNTH_DIR}/synth/tables.cpp
  ${DSND_SYNTH_DIR}/synth/wavetables.cpp
  ${DSND_SYNTH_DIR}/synth/wavetable_gen_additive.cpp
  ${DSND_SYNTH_DIR}/synth/wavetable_gen_extra.cpp
  ${DSND_SYNTH_DIR}/synth/wavetable_cache.cpp
  ${DSND_SYNTH_DIR}/synth/json_min.cpp
  ${DSND_SYNTH_DIR}/synth/patch_loader.cpp
  ${DSND_SYNTH_DIR}/synth/song_config.cpp
  ${DSND_SYNTH_DIR}/synth/voice.cpp
  ${DSND_SYNTH_DIR}/synth/master_chain.cpp
  ${DSND_SYNTH_DIR}/synth/effects/reverb.cpp
)

set(DSND_SOURCES
  src/sfx/sfx_json.cpp
  src/sfx/bank.cpp
  src/sfx/voice_alloc.cpp
  src/sfx/instance.cpp
  src/sfx/pcm_cache.cpp
  src/sfx/spatial.cpp
  src/sfx/sfx_engine.cpp
  src/sfx/script.cpp
  src/bindings/c_api.cpp
  src/fit/fit.cpp
)

add_library(dsound_core STATIC ${DSND_SYNTH_SOURCES} ${DSND_SOURCES})

target_include_directories(dsound_core PUBLIC
  src
  src/sfx
  ${DSND_SYNTH_DIR}
  ${DSND_FIXED_DIR}
)

target_compile_options(dsound_core PUBLIC
  -Wall -Wextra -Werror
  -fno-fast-math
  -ffp-contract=off
  -fno-strict-aliasing
)

if(NOT EMSCRIPTEN)
  add_executable(dsound_render src/tools/render_cli.cpp)
  target_link_libraries(dsound_render dsound_core)

  add_executable(dsound_bake src/tools/bake_cli.cpp)
  target_link_libraries(dsound_bake dsound_core)

  # The fitter is float code and lives outside the deterministic audio path,
  # exactly as midisynth's does. It is never linked into a render.
  add_executable(dsound_fit src/tools/fit_cli.cpp)
  target_link_libraries(dsound_fit dsound_core)

  # Scores two banks against the same reference with the fitter's own loss --
  # the control that says whether fitting actually helped.
  add_executable(dsound_score src/tools/score_cli.cpp)
  target_link_libraries(dsound_score dsound_core)

  file(GLOB DSND_TEST_SOURCES test/unit/*.cpp)
  add_executable(dsound_tests ${DSND_TEST_SOURCES})
  target_link_libraries(dsound_tests dsound_core)

  enable_testing()
  add_test(NAME unit COMMAND dsound_tests)
endif()
