#!/usr/bin/env bash
#
# sound/ shares midisynth's DSP by compiling its .cpp files directly. That is
# what makes "the same patch sounds the same in both projects" a fact rather
# than a hope -- run_goldens.sh proves it by rendering one patch through both
# engines and comparing hashes.
#
# The failure this prevents is not a broken build. It is a QUIET one, in two
# directions:
#
#   1. midisynth grows a DSP source we do not compile. Sometimes that is a link
#      error, and sometimes -- when the new code is only reached through a path
#      we happen not to take -- it is a silent behavioural fork, where our
#      render and theirs diverge for a reason no test names.
#
#   2. CMakeLists.txt and build_wasm.sh drift apart. Both lists are written out
#      by hand, exactly as midisynth writes its own two. If they differ, the
#      native binary and the wasm module are two different programs, and the
#      native-vs-wasm determinism leg is comparing them and calling it proof.
#
# dsp_manifest.txt is the source of truth for what is compiled, what is
# deliberately excluded (with a reason), and which headers are used without a
# matching source. A file in none of those three lists stops the build.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MANIFEST=dsp_manifest.txt
CMAKE=CMakeLists.txt
WASM=scripts/build_wasm.sh

# Standalone builds vendor the sources in and have no midisynth beside them;
# there is nothing to drift from, so the check is a no-op rather than a
# failure.
if [ -d src/vendor/synth ]; then
  echo "check_dsp_sources: standalone tree (src/vendor), nothing to compare"
  exit 0
fi

SYNTH=../midisynth/src
if [ ! -d "$SYNTH" ]; then
  echo "check_dsp_sources: no midisynth at $SYNTH -- skipping" >&2
  exit 0
fi

fail=0
err() { echo "check_dsp_sources: $*" >&2; fail=1; }

section() {
  # Prints the entries of one section of the manifest, stripped of comments.
  awk -v want="$1" '
    /^[A-Z_]+$/ { sec = $0; next }
    sec == want {
      sub(/#.*/, "")
      gsub(/^[ \t]+|[ \t]+$/, "")
      if (length($0)) print
    }
  ' "$MANIFEST"
}

COMPILED=$(section COMPILED)
EXCLUDED=$(section EXCLUDED)
HEADERS=$(section HEADERS_ONLY)

# ---- 1. every midisynth source is accounted for ---------------------------
# src/output and src/bindings are midisynth's own CLIs and its own embedding;
# neither is DSP and neither is ours to track.
while read -r f; do
  rel="${f#$SYNTH/}"
  rel="src/$rel"
  if ! grep -qxF "$rel" <<<"$COMPILED" && ! grep -qxF "$rel" <<<"$EXCLUDED"; then
    err "midisynth has a source this project does not classify: $rel
    Add it to dsp_manifest.txt under COMPILED, or under EXCLUDED with a reason.
    There is no safe default: compiling it may change our render, and not
    compiling it may leave us building against DSP that has moved."
  fi
done < <(find "$SYNTH" -name '*.cpp' ! -path "$SYNTH/output/*" ! -path "$SYNTH/bindings/*" | sort)

# ---- 2. nothing we claim to compile has vanished --------------------------
while read -r rel; do
  [ -z "$rel" ] && continue
  if [ ! -f "$SYNTH/${rel#src/}" ]; then
    err "manifest lists a source that no longer exists: $rel"
  fi
done <<<"$COMPILED"

while read -r rel; do
  [ -z "$rel" ] && continue
  if [ ! -f "$SYNTH/${rel#src/}" ]; then
    err "manifest lists a header that no longer exists: $rel"
  fi
done <<<"$HEADERS"

# ---- 3. a header that grew a source ---------------------------------------
# A header listed as HEADERS_ONLY acquiring a .cpp is exactly case 1 in
# disguise: the code moved out of line and we would keep using a declaration
# whose definition we never link.
while read -r rel; do
  [ -z "$rel" ] && continue
  src="${rel%.h}.cpp"
  if [ -f "$SYNTH/${src#src/}" ]; then
    if ! grep -qxF "$src" <<<"$COMPILED" && ! grep -qxF "$src" <<<"$EXCLUDED"; then
      err "$rel is listed HEADERS_ONLY but $src now exists and is unclassified"
    fi
  fi
done <<<"$HEADERS"

# ---- 4. the two build lists agree with the manifest -----------------------
list_from() {
  # Pull "…/synth/foo.cpp" style paths out of a build file, normalised to the
  # manifest's "src/synth/foo.cpp" spelling. Both spellings and both quoting
  # styles: ${DSND_SYNTH_DIR}/x.cpp in CMake, "$SYNTH_DIR"/x.cpp in the shell.
  grep -oE '(\$\{DSND_SYNTH_DIR\}|"?\$SYNTH_DIR"?)/[A-Za-z0-9_/]+\.cpp' "$1" \
    | sed -E 's#^[^/]*/#src/#' | sort -u
}

CMAKE_LIST=$(list_from "$CMAKE")
want=$(sort -u <<<"$COMPILED")

if [ "$CMAKE_LIST" != "$want" ]; then
  err "CMakeLists.txt does not compile the manifest's COMPILED set:
$(diff <(echo "$want") <(echo "$CMAKE_LIST") | sed 's/^/    /' || true)"
fi

if [ -f "$WASM" ]; then
  WASM_LIST=$(list_from "$WASM")
  if [ "$WASM_LIST" != "$want" ]; then
    err "scripts/build_wasm.sh does not compile the manifest's COMPILED set:
$(diff <(echo "$want") <(echo "$WASM_LIST") | sed 's/^/    /' || true)
    Native and wasm must compile the same DSP, or the determinism leg is
    comparing two different programs and calling the result proof."
  fi
fi

# ---- 5. this project's own sources, per target ----------------------------
# Everything above is about midisynth's DSP. These are OURS, and until the
# OWN_* sections existed nothing checked them at all: check 4 matches only
# $SYNTH_DIR paths, so a source added to one build and forgotten in another
# passed silently.
OWN_NATIVE=$(section OWN_NATIVE)
OWN_WASM_ENGINE=$(section OWN_WASM_ENGINE)
OWN_WASM_TOOL=$(section OWN_WASM_TOOL)

# CMake's DSND_SOURCES block.
cmake_own() {
  awk '/^set\(DSND_SOURCES/,/^\)/' "$CMAKE" \
    | grep -oE 'src/[A-Za-z0-9_/]+\.cpp' | sort -u
}

# One named array body out of build_wasm.sh. Parsing the array rather than
# grepping the whole file is what makes this robust now that the same paths
# appear in two link steps.
wasm_array() {
  awk -v name="$1" '$0 ~ "^" name "=\\(" {on=1; next} on && /^\)/ {on=0} on' "$WASM" \
    | sed -E 's/#.*//' | grep -oE 'src/[A-Za-z0-9_/]+\.cpp' | sort -u
}

cmp_set() {
  # $1 label, $2 expected, $3 actual
  if [ "$2" != "$3" ]; then
    err "$1 does not match dsp_manifest.txt:
$(diff <(echo "$2") <(echo "$3") | sed 's/^/    /' || true)"
  fi
}

cmp_set "CMakeLists.txt DSND_SOURCES" "$(sort -u <<<"$OWN_NATIVE")" "$(cmake_own)"

SFX_A=$(wasm_array SFX_SRCS)
FIT_A=$(wasm_array FIT_SRCS)
cmp_set "build_wasm.sh SFX_SRCS" "$(sort -u <<<"$OWN_WASM_ENGINE")" "$SFX_A"
cmp_set "build_wasm.sh SFX_SRCS + FIT_SRCS" \
        "$(sort -u <<<"$OWN_WASM_TOOL")" "$(printf '%s\n%s\n' "$SFX_A" "$FIT_A" | sort -u)"

# ---- 6. the two wasm modules differ by EXACTLY the fitter ------------------
# This is the one that keeps run_goldens.sh's three-way hash comparison
# meaningful. That leg proves "linking the fitter beside the engine moved no
# sample" only if the fitter is the ONLY difference between the two modules.
# Anything else drifting in and the leg still passes, while quietly proving
# something far weaker: not one engine plus a tool, but two programs that
# happen to agree today.
WANT_DIFF=$(printf '%s\n' src/bindings/embind_fit.cpp src/fit/fit.cpp | sort)
GOT_DIFF=$(comm -13 <(sort -u <<<"$OWN_WASM_ENGINE") <(sort -u <<<"$OWN_WASM_TOOL"))
if [ "$GOT_DIFF" != "$WANT_DIFF" ]; then
  err "the tool module must differ from the engine module by exactly the fitter.
    want: $(echo "$WANT_DIFF" | tr '\n' ' ')
    got:  $(echo "$GOT_DIFF" | tr '\n' ' ')
    run_goldens.sh's native == dsound == dsound-tool leg is evidence about the
    fitter only while this holds."
fi
# ...and nothing may be in the engine module that is not also in the tool one.
EXTRA=$(comm -23 <(sort -u <<<"$OWN_WASM_ENGINE") <(sort -u <<<"$OWN_WASM_TOOL"))
if [ -n "$EXTRA" ]; then
  err "the engine module compiles sources the tool module does not: $(echo "$EXTRA" | tr '\n' ' ')"
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi

n=$(grep -c . <<<"$COMPILED")
m=$(grep -c . <<<"$OWN_WASM_TOOL")
echo "check_dsp_sources: ok ($n shared sources, $m own sources, 3 targets agree)"
