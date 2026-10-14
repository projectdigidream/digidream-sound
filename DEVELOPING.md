# Developing DigidreamSound

How the engine works, how it is tested, and the tools beside the editor. To
build it at all, see "Build from source" in [README.md](README.md): the
sibling `midisynth` and `engine` checkouts are needed.

## Tests

```sh
cmake -S . -B build -G Ninja && ninja -C build
./build/dsound_tests          # the unit suite
./scripts/ci.sh               # the whole gate
```

`scripts/ci.sh` runs unattended and offline. It checks the source lists, builds,
runs the unit suite and the goldens, checks the vendored UI modules, round-trips
the editor's document model, exercises the WebAssembly engine under Node (when
`tool/dist/` is built), bakes every shipped bank, and finally builds the
standalone extract and runs that tree's own gate. `DSND_SKIP_STANDALONE=1` skips
the last step, which roughly doubles the run time.

The goldens check four things, and they fail for four different reasons:

- **The pinned hashes.** A change means the DSP or the playback policy moved.
- **Cached playback is bit-identical to live synthesis.** Every eligible case
  is rendered twice, once with `--no-cache`. This is what lets the engine cache
  some sounds and not others, finish its cache while already playing, and fall
  back to live synthesis when a budget runs out, with nobody able to hear the
  seam. The `budget` case forces the cache down to one kilobyte and must hash
  identically to the cached run.
- **A baked `.sfxb` renders like the `.sfxproj` it came from.**
- **Native and WebAssembly agree**, byte for byte. This catches a shift or a
  rounding rule behaving differently on another target.

## Recipes, cached or live

A bank is a set of named sounds, each a synthesis patch and a short gesture. At
load, the runtime renders them once into a PCM cache and plays them back
cheaply; sounds that want per-instance variation synthesize live instead. The
cached-equals-live golden above is the proof that a listener cannot tell which
is which.

Everything is fixed-point integer arithmetic, so a bank produces the **same
bytes** on arm64, on x86-64 and under WebAssembly. That is what makes a sound
set testable at all.

## What the engine does that a sample player does not

**Voice limiting a designer can reason about.** Three tiers: a per-sound
cooldown and instance cap that refuse a trigger before anything is allocated; a
per-bus budget where stealing is confined to the bus, so a hundred world sounds
cannot silence the UI click or the player's own weapon however the priorities
were set; and the synth pool underneath. A higher-priority sound is never cut
for a lower-priority one: the new play is refused instead, which makes the
number a promise rather than a hint.

**Every refusal is counted, by reason.** `cooldown`, `maxInstances`,
`priority`, `busBudget`, `noVoices`, `unknown`. "The sound didn't play" is
otherwise the least debuggable complaint in a game.

**Placement in fixed point.** Distance, constant-power panning, a front/back
cue and optional air absorption, all from baked integer tables: no libm, no
`atan2`, no doppler ([docs/SFX_FORMAT.md](docs/SFX_FORMAT.md) says why not).

**Mastering.** EQ, compressor, saturation, width, gain and a lookahead limiter,
bypassed by default and byte-identical when off.

**Seeded, not random.** Every trigger takes a seed, which drives variant
choice, pitch jitter, gain jitter and the synthesis noise in a fixed order. Two
machines that agree on the seed hear the same sound.

## Layout

```
src/sfx/        the runtime: bank, cache, sampler, spatialiser, engine
src/bindings/   the C API and the WebAssembly class
src/fit/        the fitter: a reference sample in, a recipe out
src/tools/      dsound_render, dsound_bake, dsound_fit, dsound_score
tool/           the editor: sfx.html, its worker, its worklet
electron/       the desktop shell
android/        the Android shell (WebView, SDK-only build)
mobile/         the Capacitor shell (iOS)
content/banks/  the starter bank
test/golden/    scripts and pinned hashes
```

The synthesis itself is DigidreamSynth's, compiled from source rather than
forked: eleven files listed in `dsp_manifest.txt`. `scripts/check_dsp_sources.sh`
fails the build if that list drifts from what midisynth has, or if the native
and WebAssembly builds stop compiling the same thing. The compile flags in
`CMakeLists.txt` are copied from midisynth's on purpose: the same source with
different flags is a different program.

`tool/lib/` holds two UI modules copied from midisynth (`scripts/sync_lib.sh`);
`scripts/check_lib_sync.sh` checks them against `tool/lib/MANIFEST`, so the
check works offline and without midisynth beside it.

## Formats

| | |
|---|---|
| `.sfxproj` | the authoring document: JSON, human-editable |
| `.sfxb` | the baked bank: sealed with a SHA-256, framed like midisynth's `.mstk`, with an optional render seal that `dsound_bake --check` re-verifies anywhere |

Field by field: [docs/SFX_FORMAT.md](docs/SFX_FORMAT.md).
Embedding it: [docs/EMBEDDING.md](docs/EMBEDDING.md).

## Fitting

`dsound_fit` takes a reference recording and returns a recipe: finite-difference
descent over hand-fixed architectures. The structural choices are fixed by hand
because they are exactly the ones a gradient cannot cross; the optimiser only
moves the numbers. The result is a patch you can open in the editor and
recognise.

There are sixteen families. `weapon` places delayed events, `roar` and `growl`
swell and hold, `burst` and `sequence` are several events rather than one,
`motor` is a held pitched source that pulses, and `modal` rings a resonant
body. Frequencies start on the reference's spectral peaks and event times on
its onsets. A fit is scored only within the reference's own bandwidth; see
[docs/SOUND.md](docs/SOUND.md).

```sh
./build/dsound_fit --list                 # every architecture, expansions included
./build/dsound_fit reference.wav --arch gunshot --out patch.json --wav compare.wav
./build/dsound_score reference.wav before.sfxproj after.sfxproj role.name
```

`dsound_score` is the control: it measures two banks against the same reference
with the same loss, so "we fitted it" can be checked rather than asserted.

## Originality

Nothing here is a recording. Every sound in `content/banks/` is synthesized from
a recipe this project authored, and no bank contains PCM, only recipes.
`scripts/check_no_samples.sh` enforces that on every build.

The editor's **Match a recording** reads a reference recording to derive parameters. The
reference is analysed and discarded; what lands in the document is a few
hundred bytes of synthesis parameters, and the sound records where it came from
in a `_fitted` key.

## The editor

`tool/sfx.html`, with its user guide in [tool/README.md](tool/README.md) (the
last section, "For developers", covers its modules and the keys it writes).
It ships three ways from one staged payload, so all three run byte-identical
code: `package_web.sh` (a static directory), `package_electron.sh` (macOS,
Windows, Linux) and `package_android.sh` (an APK, from the Android SDK alone).

## Standalone

```sh
./scripts/make_standalone.sh && (cd build/standalone && ./scripts/ci.sh)
```

This copies midisynth's DSP and the engine's fixed-point header into
`src/vendor`, and runs the extracted tree's own gate with no sibling repository
on the include path. `scripts/ci.sh` does it on every run, because a header
quietly read from a sibling is invisible until someone tries to ship.
