#!/usr/bin/env bash
#
# The determinism gate.
#
# Three things are checked, and they fail for three different reasons:
#
#   1. Each case still renders to its pinned hash. A change here means the DSP
#      or the policy moved -- sometimes deliberately, which is what --regen is
#      for, and never silently.
#
#   2. Cached playback is bit-identical to live synthesis. This is the property
#      the whole hybrid design rests on: it is what lets the engine cache some
#      sounds and not others, finish its cache while already playing, and fall
#      back to live when a budget runs out, with nobody able to hear the seam.
#      Checked by rendering the same script twice, once with --no-cache.
#
#   3. A baked .sfxb renders identically to the .sfxproj it came from.
#
# Usage: scripts/run_goldens.sh [--regen]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RENDER=build/dsound_render
BAKE=build/dsound_bake
GOLD=test/golden/goldens.txt
REVIEW=test/golden/review
REGEN=0
[ "${1:-}" = "--regen" ] && REGEN=1

if [ ! -x "$RENDER" ] || [ ! -x "$BAKE" ]; then
  echo "build first: cmake -S . -B build -G Ninja && ninja -C build" >&2
  exit 1
fi

# name : bank : script : extra args
CASES=(
  "identity:test/golden/identity.sfxproj:test/golden/identity.sfxscript:"
  "starter:content/banks/starter.sfxproj:test/golden/oneshot.sfxscript:"
  "flood:content/banks/starter.sfxproj:test/golden/flood.sfxscript:"
  "contest:test/golden/contest.sfxproj:test/golden/contest.sfxscript:"
  "budget:test/golden/identity.sfxproj:test/golden/identity.sfxscript:--budget-kb 1"
)

# Cases whose sounds carry no pitch jitter, so cached and live must agree
# exactly. With jitter the two paths diverge BY DESIGN -- one resamples, the
# other re-synthesizes -- and that divergence is documented rather than hidden.
IDENTITY_CASES="identity contest budget"

mkdir -p "$REVIEW"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail=0
declare -a NEW

hash_of() { awk '/^sha256/ {print $2}'; }

for c in "${CASES[@]}"; do
  IFS=: read -r name bank script extra <<<"$c"
  # shellcheck disable=SC2086
  got=$("$RENDER" "$bank" "$script" "$tmp/$name.wav" --hash --quiet $extra | hash_of)
  if [ -z "$got" ]; then
    echo "FAIL $name: render produced no hash" >&2
    fail=1
    continue
  fi
  NEW+=("$name $got")

  if [ "$REGEN" -eq 0 ]; then
    want=$(awk -v n="$name" '$1 == n {print $2}' "$GOLD")
    if [ -z "$want" ]; then
      echo "FAIL $name: no pinned hash (run --regen to add it)" >&2
      fail=1
    elif [ "$want" != "$got" ]; then
      echo "FAIL $name" >&2
      echo "  want $want" >&2
      echo "  got  $got" >&2
      cp "$tmp/$name.wav" "$REVIEW/$name.wav"
      fail=1
    else
      echo "ok   $name $got"
    fi
  else
    cp "$tmp/$name.wav" "$REVIEW/$name.wav"
    echo "regen $name $got"
  fi

  # --- leg 2: cached must equal live ---
  case " $IDENTITY_CASES " in
    *" $name "*)
      # shellcheck disable=SC2086
      live=$("$RENDER" "$bank" "$script" "$tmp/$name.live.wav" --hash --quiet --no-cache $extra \
             | hash_of)
      if [ "$live" != "$got" ]; then
        echo "FAIL $name: cached and live renders differ" >&2
        echo "  cached $got" >&2
        echo "  live   $live" >&2
        echo "  This is the property the hybrid model rests on. See sample_voice.h." >&2
        fail=1
      else
        echo "ok   $name cached == live"
      fi
      ;;
  esac
done

# --- leg 3: a baked bank renders like its source ---
for c in "${CASES[@]}"; do
  IFS=: read -r name bank script extra <<<"$c"
  case "$bank" in *.sfxproj) ;; *) continue ;; esac
  "$BAKE" "$bank" "$tmp/$name.sfxb" --quiet
  # shellcheck disable=SC2086
  a=$("$RENDER" "$bank" "$script" "$tmp/src.wav" --hash --quiet $extra | hash_of)
  # shellcheck disable=SC2086
  b=$("$RENDER" "$tmp/$name.sfxb" "$script" "$tmp/bak.wav" --hash --quiet $extra | hash_of)
  if [ "$a" != "$b" ]; then
    echo "FAIL $name: baked .sfxb does not render like its .sfxproj" >&2
    fail=1
  else
    echo "ok   $name baked == source"
  fi
done

# --- leg 4: native vs wasm ---
#
# The leg that catches per-target divergence. Everything above runs one
# compiler on one architecture; this runs a different compiler targeting a
# different machine, and the bytes must still match.
# A stale module is worse than a missing one: the leg still runs, still
# passes or fails, and is comparing the current engine against whatever was
# built last time. That cost an hour once, chasing a determinism bug that was
# really two different programs.
# BOTH modules, because a stale tool module would make the A/B below pass
# against last week's fitter -- which is the same trap in a new place.
newest=$(find src -name '*.cpp' -o -name '*.h' | xargs ls -t 2>/dev/null | head -1)
for m in dsound dsound-tool; do
  if [ -f "tool/dist/$m.mjs" ] && [ -n "$newest" ] && [ "$newest" -nt "tool/dist/$m.mjs" ]; then
    echo "FAIL tool/dist/$m.mjs is older than $newest -- run scripts/build_wasm.sh" >&2
    fail=1
  fi
done

if [ -f tool/dist/dsound.mjs ] && command -v node >/dev/null 2>&1; then
  for c in "${CASES[@]}"; do
    IFS=: read -r name bank script extra <<<"$c"
    case "$bank" in *.sfxproj) ;; *) continue ;; esac
    case "$extra" in "") ;; *) continue ;; esac   # the wasm leg takes no flags
    nat=$("$RENDER" "$bank" "$script" "$tmp/nat.wav" --hash --quiet | hash_of)
    # Every module we ship must agree with native AND with each other. The
    # second one links the fitter beside the engine; if that ever perturbs a
    # sample, this is the line that says so, on the build that did it.
    agreed=1
    for m in dsound dsound-tool; do
      [ -f "tool/dist/$m.mjs" ] || continue
      wsm=$(node test/determinism_wasm.mjs "$bank" "$script" --module "$m" 2>/dev/null | hash_of)
      if [ -z "$wsm" ]; then
        echo "skip $name native==$m (module would not load)"
        agreed=0
      elif [ "$nat" != "$wsm" ]; then
        echo "FAIL $name: native and $m renders differ" >&2
        echo "  native     $nat" >&2
        echo "  $m  $wsm" >&2
        fail=1
        agreed=0
      fi
    done
    [ "$agreed" -eq 1 ] && echo "ok   $name native == dsound == dsound-tool"
  done
else
  echo "skip native==wasm (no tool/dist/dsound.mjs -- run scripts/build_wasm.sh)"
fi

if [ "$REGEN" -eq 1 ]; then
  {
    echo "# name  sha256"
    echo "# Written by scripts/run_goldens.sh --regen. LISTEN to the review WAVs before"
    echo "# committing a change to these: a hash moving is not a decision until someone"
    echo "# has heard what moved."
    printf '%s\n' "${NEW[@]}"
  } > "$GOLD"
  echo
  echo "Wrote $GOLD. Review WAVs are in $REVIEW -- LISTEN to them before committing."
  exit 0
fi

exit "$fail"
