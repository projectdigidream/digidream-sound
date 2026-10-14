#!/usr/bin/env bash
#
# No committed bank contains PCM.
#
# The originality claim has two halves. One is a statement about intent -- that
# these sounds were authored rather than sampled -- which no script can check.
# The other is mechanical and this checks it: what ships is a RECIPE, a few
# hundred bytes of synthesis parameters, and never audio.
#
# That is what lets the Fit tab read a reference recording without the claim
# becoming untrue. A reference is an input to an authoring step; it is analysed
# and discarded, and if any of it ever survived into a bank, this fails.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

fail=0
err() { echo "check_no_samples: $*" >&2; fail=1; }

# A .sfxproj is JSON. PCM would have to arrive as a long numeric array or a
# base64 blob, so look for both rather than for a key name someone could rename.
for f in $(git ls-files 'content/**/*.sfxproj' 'test/**/*.sfxproj' 2>/dev/null || \
           find content test -name '*.sfxproj' 2>/dev/null); do
  [ -f "$f" ] || continue

  # A base64 run long enough to be audio. Patch JSON has no long opaque strings.
  if grep -qE '"[A-Za-z0-9+/]{512,}={0,2}"' "$f"; then
    err "$f contains a long base64 string -- that looks like embedded audio"
  fi

  # A numeric array with more entries than any recipe field has. The largest
  # legitimate one is a harmonic list, which is tens of entries, not thousands.
  if python3 - "$f" <<'PY'
import json, sys
def walk(v, path=""):
    if isinstance(v, list):
        nums = sum(1 for x in v if isinstance(x, (int, float)))
        if nums > 512:
            print(f"  {path or '<root>'}: {nums} numbers")
            return True
        return any(walk(x, f"{path}[{i}]") for i, x in enumerate(v))
    if isinstance(v, dict):
        return any(walk(x, f"{path}.{k}" if path else k) for k, x in v.items())
    return False
sys.exit(1 if walk(json.load(open(sys.argv[1]))) else 0)
PY
  then :; else
    err "$f contains a numeric array long enough to be PCM (above)"
  fi

  # Audio file magic, in case someone embedded a whole file.
  if grep -qE 'RIFF|WAVEfmt|OggS|ID3|fLaC' "$f"; then
    err "$f contains audio-file magic bytes"
  fi
done

# A .sfxb carries no PCM by design (docs/SFX_FORMAT.md 6): it is sections of
# parameters and deduped patch JSON. The cheap invariant is SIZE -- a bank with
# a hundred rendered sounds in it would be megabytes, not kilobytes.
for f in $(find content -name '*.sfxb' 2>/dev/null); do
  bytes=$(wc -c < "$f" | tr -d ' ')
  sounds=$(grep -o 'SNDS' "$f" | wc -l | tr -d ' ')
  if [ "$bytes" -gt 2000000 ]; then
    err "$f is $bytes bytes -- a recipe bank should be kilobytes; is there PCM in it?"
  fi
done

[ "$fail" -eq 0 ] || exit 1
echo "check_no_samples: ok (no committed bank contains PCM)"
