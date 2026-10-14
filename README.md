# DigidreamSound

A sound-effects engine that ships **recipes, not recordings**.

A bank is a set of named sounds, each one a synthesis patch and a short
gesture. A hundred sounds is about eighty kilobytes. At load, the runtime
renders them once into a PCM cache and plays them back cheaply; sounds that
want per-instance variation synthesize live instead. A listener cannot tell
which is which, and there is a golden render that proves it.

Everything is fixed-point integer arithmetic, so a bank produces the **same
bytes** on arm64, on x86-64 and under WebAssembly. That is what makes a sound
set testable at all.

```sh
cmake -S . -B build -G Ninja && ninja -C build
./build/dsound_tests          # unit suite
./scripts/ci.sh               # the whole gate
```

## What it does that a sample player does not

**Voice limiting that a designer can reason about.** Three tiers: a per-sound
cooldown and instance cap that refuse a trigger before anything is allocated; a
per-bus budget where stealing is confined to the bus, so a hundred world sounds
cannot silence the UI click or the player's own weapon however the priorities
were set; and the synth pool underneath. A higher-priority sound is never cut
for a lower-priority one — the new play is refused instead, which is what makes
the number a promise rather than a hint.

**Every refusal is counted, by reason.** `cooldown`, `maxInstances`,
`priority`, `busBudget`, `noVoices`, `unknown`. "The sound didn't play" is
otherwise the least debuggable complaint in a game.

**Placement in fixed point.** Distance, constant-power panning, a front/back
cue and optional air absorption, all out of baked integer tables — no libm, no
`atan2`, no doppler (and [docs/SFX_FORMAT.md](docs/SFX_FORMAT.md) says why
not).

**Mastering.** EQ → compressor → saturation → width → gain → lookahead
limiter, bypassed by default and byte-identical when off.

**Seeded, not random.** Every trigger takes a seed, and it drives variant
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
content/banks/  the starter bank that ships with this
content/game/   engine-side content, excluded from the standalone build
test/golden/    scripts and pinned hashes
```

The synthesis itself is **midisynth's**, compiled from source rather than
forked — eleven files listed in `dsp_manifest.txt`, with
`scripts/check_dsp_sources.sh` failing the build if that list ever drifts from
what midisynth actually has, or if the native and WebAssembly builds stop
compiling the same thing.

## The gate

`scripts/ci.sh` runs all of it. Four things are checked, and they fail for four
different reasons:

- **The pinned hashes.** A change means the DSP or the policy moved.
- **Cached playback is bit-identical to live synthesis.** Every eligible case
  is rendered twice, once with `--no-cache`. This is the property the hybrid
  model rests on: it is what lets the engine cache some sounds and not others,
  finish its cache while already playing, and fall back to live when a budget
  runs out, with nobody able to hear the seam. The `budget` case forces the
  cache down to one kilobyte and must hash identically to the cached run.
- **A baked `.sfxb` renders like the `.sfxproj` it came from.**
- **Native and WebAssembly agree**, byte for byte. This is the leg that catches
  a shift or a rounding rule behaving differently on another target.

## Formats

| | |
|---|---|
| `.sfxproj` | the authoring document — JSON, human-editable |
| `.sfxb` | the baked bank — sealed with a SHA-256, framed like midisynth's `.mstk`, with an optional render seal that `dsound_bake --check` re-verifies anywhere |

Field by field: [docs/SFX_FORMAT.md](docs/SFX_FORMAT.md).
Embedding it: [docs/EMBEDDING.md](docs/EMBEDDING.md).

## Fitting

`dsound_fit` takes a reference recording and returns a recipe: finite-difference
descent over hand-fixed architectures. The structural choices are fixed by hand
because they are exactly the ones a gradient cannot cross; the optimiser only
moves the numbers. The result is a patch you can open in the editor and
recognise.

There are sixteen families, and the later ones exist because the earlier ones
could not make a shape the references actually have: `weapon` places delayed
events, `roar` and `growl` swell and hold, `burst` and `sequence` are several
events rather than one, `motor` is a held pitched source that pulses, and
`modal` rings a resonant body. Frequencies start on the reference's spectral
peaks and event times on its onsets. A fit is scored only within the
reference's own bandwidth — see [docs/SOUND.md](docs/SOUND.md).

```sh
./build/dsound_fit --list                 # 32 architectures, expansions included
./build/dsound_fit reference.wav --arch gunshot --out patch.json --wav compare.wav
./build/dsound_score reference.wav before.sfxproj after.sfxproj role.name
```

`dsound_score` is the control: it measures two banks against the same reference
with the same loss, so "we fitted it" can be checked rather than asserted.

## Originality

Nothing here is a recording. Every sound in `content/banks/` is synthesized from
a recipe this project authored, and **no bank contains PCM — only recipes**.
`scripts/check_no_samples.sh` enforces that half of the claim on every build.

The editor's Fit tab reads a reference recording to derive parameters from it.
The reference is analysed and discarded; what lands in the document is a few
hundred bytes of synthesis parameters, and the sound it produces records where
it came from in a `_fitted` key.

`content/game/` is different and is kept separate for that reason: it is
engine-side content, excluded from the standalone build, and some of it is
fitted against reference audio extracted from a locally owned game WAD. What
that produces is synthesis parameters — a few hundred bytes of JSON — and the
references are a local input to an authoring step, never committed. It is the
same relationship the engine's `tools/wad/` already has with a WAD for music.

## The editor

`tool/sfx.html` — a library of sounds, the selected sound, and how the game
plays it; generate from a kind of sound, shape it, match a recording, and test
the bank in the live engine. The user guide is [tool/README.md](tool/README.md);
the design background is [docs/SOUND.md](docs/SOUND.md).

```sh
node tool/serve.js            # then http://localhost:8940/tool/sfx.html
```

It ships three ways from one staged payload, so all three run byte-identical
code: `package_web.sh` (a static directory), `package_electron.sh` (macOS,
Windows, Linux), `package_android.sh` (an APK, from the Android SDK alone).

**iOS is written, not scaffolded.** `npx cap add ios` needs Xcode and
CocoaPods; this machine has Command Line Tools and system Ruby 2.6. The config
and the steps are there and are believed correct, and nobody has run them.

## Standalone

`sound/` is a product you can lift out of this repository:

```sh
./scripts/make_standalone.sh && (cd build/standalone && ./scripts/ci.sh)
```

That vendors midisynth's DSP in through `DSND_SYNTH_DIR`, drops
`content/game/`, and runs the extracted tree's own gate with this repository
nowhere on the include path. `scripts/ci.sh` does it on every run, because an
assertion of standalone-ness is worth nothing and a build of the extract is
proof.
