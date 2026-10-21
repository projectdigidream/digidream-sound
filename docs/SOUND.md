# The editor

A sound in this project is a **recipe**, not a recording. What ships is a few
hundred bytes of synthesis parameters, and the engine renders them identically
on every platform it runs on. The editor is where those parameters are written.

Everything you hear here is produced by the same fixed-point engine a game
runs, and everything you export is produced by that engine — so the deliverable
is what you auditioned. That is checked rather than claimed:
`scripts/run_goldens.sh` renders every golden case through the native binary
and through both wasm modules and requires all three to hash identically.

```sh
node tool/serve.js            # then http://localhost:8940/tool/sfx.html
```

Three ways to ship it, all from the same staged payload so they run
byte-identical code:

| | |
|---|---|
| browser | `./scripts/package_web.sh` → a static directory |
| desktop | `./scripts/package_electron.sh` → macOS, Windows, Linux |
| Android | `./scripts/package_android.sh` → `build/android/digidreamsound.apk`, from the SDK alone |
| iOS | `./scripts/package_mobile.sh ios` — written, not scaffolded |

## Model

**The document is the `.sfxproj`.** Not the engine's idea of it: `bankInfoJson`
reports post-clamp, post-resolution state, so round-tripping through it would
quietly rewrite what you wrote. The editor parses the file, edits that, and
uses the engine to render.

**Defaults are never written down.** A field the file does not mention reads as
its default and stays unmentioned, and setting a value back to its default
removes the key. A diff therefore shows what a person changed and nothing else,
and a 173 KB bank does not inflate on its first save.

**What it does not understand, it keeps.** `_generated_by`, `_originality` and
`_fitted` round-trip untouched, because the loader ignores unknown keys by
design and an editor that dropped them would be destroying provenance.

**Two engines, for two questions.** A Worker renders sounds offline — waveforms,
peaks, auditions, baking, fitting. An AudioWorklet runs the real engine for
*Test in the engine*. That split is not tidiness: `loadBankJson` costs 2–25 ms per edit and
unloads the bank when a patch fails to validate, so doing it on the audio thread
would drop quanta and silence the engine mid-edit.

## The layout

One screen, in the shape every asset browser already has — Wwise, FMOD, a DAW's
browser, editor and inspector:

| | |
|---|---|
| **Library** (left) | every sound, grouped by the first part of its name — `weapon.pistol.fire` sits under WEAPON — with a picture, its length, and a dot if something is wrong |
| **The sound** (middle) | the one you selected, top to bottom in the order you work on it |
| **In the game** (right) | how the game plays it, asked as a designer's questions |

Bank-wide things — groups, effects, the live engine — are the second view,
**Mix & test**, so the main one only ever talks about the sound you selected.
Under 1250 px *In the game* becomes a drawer; under 900 px the page is one
column, the library and the sound are two screens with a back button, and
*In the game* is a bottom sheet.

The old four tabs map onto this directly: Bank is the library, Design is the
sound, Fit is **Match a recording…**, Play is **Mix & test → Test in the
engine**, and the policy rail is *In the game*.

## The sound, top to bottom

**The player.** The play button sits in the top bar (Space), next to the name
of what it will play, so it is in reach wherever you have scrolled. Below, the
waveform is drawn at the level
the sound actually plays at — turn the volume up and it visibly nears the
ceiling, and past it the waveform turns red and a *too loud* badge offers
**Fix level**. *Repeat* (L) replays it for judging a transient; it is a
listening aid, not the sound's `loop` flag.

**The timeline.** Under the waveform, one lane per layer, each in its own
colour on the same time axis, rendered *alone* through the real engine. It
answers the question a stack of layer panels never did: which part is the
crack, which is the body, and when does the late one come in. **S** plays one
layer alone and **M** plays the sound without it; neither touches the document.

**Problems**, in words, with a button for each documented fix.

**Takes.** A sound can have several; the game picks one at random each time it
plays (weighted), so footsteps and gunshots stop sounding repeated. A new take
of a recipe sound starts as a *variation*, not a copy.

**Start from.** Thirteen kinds of sound — gunshot, explosion, laser, hit, metal,
pickup, jingle, door, machine, engine, monster, voice, whoosh — in the manner of
sfxr and Bfxr: one press makes one, **Randomize** (R) makes another of the same
kind, **Vary** (V) nudges the one you have. Every press is one undo step.

The kinds are the fitter's own architectures, so a generated sound and a
matched one are the same kind of object: a **recipe**. A take stores it under
`_recipe` (`arch` and the normalised knob vector — the loader ignores `_` keys,
as it already did for `_fitted`), and its patch is rebuilt from that by the
worker. Every generated sound is levelled to −6 dB before you hear it: the
architectures span 40 dB between their corners, and a generator that sometimes
blasts you is one nobody presses twice.

**Shape.** Four knobs that work on any sound — volume, pitch, length,
brightness — and, for a recipe take, the recipe's own knobs, labelled by part
and quality (*Crack · length*, *Body · pitch*) with real units. Every slider
renders while you drag and plays when you let go; double-click a name to reset
it; click a value to type one (a comma decimal is fine).

Pitch, length and brightness are transforms, recorded under `_shape` and
applied to the take's *source* — its recipe, or for a hand-made take a copy of
its original patch kept under `_base`. So they never compound: set pitch back
to 0 and the patch is byte-for-byte what it was.

**Layers**, folded away: **every** setting a patch can hold has a control
here — nothing is reachable only through JSON. It opens on a diagram of how the
parts connect: each layer is its own chain (source, tone, level), they all
start together and are **added**, and the sum goes through the rings, the
volume and out. Then the patch-wide settings (ends by itself, choke group,
name), and each layer in sections:

| Section | What is in it |
|---|---|
| Source | noise (and its colour), metal, or a tone and its generator with **every** parameter: harmonic amplitudes, drawbars and Chebyshev weights are bars you drag across; a custom wave is a cycle you draw (or start from sine, triangle, saw, square); each can morph to a second shape; morph frames |
| Level and pitch | level, pan, start delay, fixed pitch, detune |
| Loudness over time | the amp envelope: wait, rise, hold, fall, stay, fade — and, folded, the three stage curves, loop, key scaling and the DX7-style rates |
| Tone | filter (low, band, high, notch, comb, vowel with its start and end vowel), frequency, resonance, steepness, drive, key tracking; a second filter with its routing and "stays put"; the filter sweep (a full envelope); distortion after |
| Pitch sweep, Timbre sweep | the pitch and wavetable envelopes, full |
| Stack and ring | unison (voices, detune, width, phase spread); ring or FM (ratio, depth) |
| Strength, turns and sends | velocity window and curve, release velocity, round-robin group, per-layer room/chorus/echo sends |
| LFOs | up to four: shape, speed, delay, retrigger, tempo sync |
| Modulation | up to twelve routings: source → destination × amount |

The main sections are open and the specialist ones folded, each with a
one-line summary, so nothing is hidden and nothing is in the way. **Rings**
(the modal body) follow the layers, and *Everything, as JSON* is still there
for pasting. Editing a recipe take's layers by hand detaches it from its
recipe — the card says so before you touch anything. A change that adds or
removes controls rebuilds the page **without moving it**.

**Timing & notes**, folded away: the take's gesture. Prefer layers for a
multi-part hit — a shotgun's click, boom and tail are three layers on one note;
a gesture is for parts at *different pitches*, like a jingle, or for holding a
sound.

## In the game

Where it plays (its group, its importance), what happens when it is triggered a
lot (at most N at once, then skip the new one or cut the oldest, a minimum
gap), how much it varies each time (pitch, volume), how far it carries (only
for a group placed in the world), whether it lowers the music, and whether it
repeats until stopped.

Every control comes from one field table transcribed from the loader's actual
clamps, which also stops the engine's "clamped" warning from ever firing.
**Defaults are never written**: set a value back to its default and the key is
removed.

## Match a recording

Three steps instead of a control panel: **the recording** (drop a WAV; drag
across it to match only one part), **what it is** (the same kinds as *Start
from*, or *Not sure*, which tries every recipe briefly and refines the best),
and **the match** (hear the recording and the match side by side, then *Use
it*). The result is written as a recipe, so a matched sound keeps its knobs,
with `_fitted` provenance naming the recording. The weights of the three things
the match compares are under *Advanced*, with their documented defaults.

Structure is fixed by hand because it is exactly what a gradient cannot cross;
the optimiser only moves numbers. Thirty-two architectures, including the
categorical expansions — `impact/brown+highpass` is reachable, not just
`impact`.

Two of them are structurally different from the rest rather than being another
set of knobs. **`weapon`** is the only one that places *delayed* layers, which
is what a firearm with a mechanical action needs: a shotgun is a sequence — the
crack, the body, the break-open, the shells — and every other architecture fires
everything at once. **`modal`** is not subtractive at all: a short exciter
ringing a bank of resonators, for anything struck — though see below for how
it has actually fared.

Both carry more parameters than the older families, and finite differences cost
one render per parameter per step — so they need a real search to show their
advantage. At one restart and twenty-five iterations they lose to the simpler
architectures; at three and sixty they win. If a fit looks disappointing, raise
the restarts before changing the family.

**`roar` and `growl` are the ones that SWELL and HOLD.** Every older family
starts at full level and fades — right for a hit, wrong for a large share of any
game's set. `roar` is a swept noise body whose attack, filter sweep and LFO are
all free, with a tonal buzz under it: the teleport that peaks two-thirds of the
way through, the BFG that holds for a second and a half. `growl` is `voice` with
a real sustain and an LFO on level and pitch, which at tens of hertz is what
makes a throat a growl rather than a tone. On DOOM they moved the energy-type
roles from a mean loss of 10.85 to 3.88, and the monster voices from 6.52 to
3.74 — the two families that had held 60% of the remaining error.

A hold needs something to release it, and in this format that is the variant's
gesture — a note with a `durMs`. A family produces only a patch, so a
sustaining one writes `holdMs` inside it; applying a fit turns that into the
gesture, exactly as it lifts a modal body onto the sound. Left in the patch it
would be ignored, and the engine would refuse the sound as a voice that never
ends.

**A fit is scored only where the reference has information.** A recording's
bandwidth is half its sample rate — 5.5 kHz for DOOM's 11 kHz lumps — and
nothing above it is scored: not the spectrogram, not the envelope, not the
band balance. What a fitted sound does up there is left free, deliberately.
Differences more than 60 dB below the reference's loudest point are ignored
too, because in an 8-bit recording that is hiss rather than sound.

That replaced a measurement in which about 80% of the distance being minimised
came from above 5.5 kHz, where the old reader's sample-repeating resampler had
manufactured mirror images of the whole spectrum. **Loss numbers from before
that change are not comparable with numbers after it**, so every bank was
re-scored with the new one: hand-authored 8.32, the first fit 5.02, the fit
before the change 4.12.

**Three families are several events or a motor, not one hit.** `burst` is a
short splat re-triggered by an eight-step gesture — a gib splat is ten onsets in
a second. `sequence` is a pitched blip over four gesture steps with their own
notes — a pickup is two tones. `motor` is a driven, held, pitched source pulsed
by an LFO up to 90 Hz — a chainsaw fires at around 54 Hz. Event times start on
the reference's detected onsets, the way frequencies start on its spectral
peaks: descent cannot move an event through time any more than it can move a
mode through frequency.

**Pick the family by what the thing IS, not by the score.** And be sceptical of
any family that suddenly wins everything.

`modal` is the cautionary example. An early version of the resonator did not
normalise its modes and let their tails run forever, and in that state `modal`
won seven DOOM roles — both doors, the crusher, the lift — by margins of up to
half. Every one of those wins was the bug: bodies ringing at +42 dB and tails
that never ended were filling the reference's decay in a way the loss
rewarded. With the resonator correct, `modal` wins **none** of DOOM's roles at
the default search. It is still the right tool for authoring a struck body by
hand, under Layers → Rings; it has not, so far, earned its place as a fitting
family for this set.

And treat a low score as evidence rather than a verdict. On a super shotgun the
best-scoring architecture is `voice` — the formant one built for monster growls
— which can match a spectrum closely while sounding nothing like a gun.
`content/game/compare_arch.sh` fits one reference with every family and writes
the reference and each candidate into a folder named by score, so the choice can
be made by ear:

```sh
content/game/compare_arch.sh build/refs/dsdshtgn.wav
```

**Every search also gets one extra restart begun on the reference's own
spectral peaks.** Descent cannot traverse a frequency axis — move a mode five
per cent and it lands somewhere else in the spectrum — so frequencies get a
starting point from analysis rather than from the middle of their range. It is
an extra restart rather than a replacement for the neutral one, which is what
makes it unable to regress: the first version replaced the centre start and
measured worse on four of six references.

**Match one part at a time.** In step 1, drag across the recording's waveform
to select a region. A real recording is usually several events, and one architecture
matching all of them at once is a harder problem than it needs to be — the same
`gunshot` architecture scores 9.22 against a whole super-shotgun sample, 7.21
against its crack alone and 7.18 against its tail alone. Match the crack, use
it, then match the tail into a second take.

The reference is analysed and discarded. Nothing but numbers reaches the bank,
and `scripts/check_no_samples.sh` enforces that half of the claim.

Two honest limits. The browser fitter is **two to four times slower** than
`dsound_fit`. And its answer will **not** be identical to the CLI's: the loss
calls `log`, `cos`, `pow` and `sqrt`, whose last bits differ between platforms,
so the descent takes a different path. The *engine* is bit-identical everywhere;
the fitter is an authoring aid. For a long search over a folder of references,
use the CLI.

## Mix & test

**Groups** are the buses: a shared volume, a limit on how many play at once,
whether they are placed in the world, and how much each sends into the
effects. **Effects** are the room (reverb), chorus and echo, including echo
time as a beat division (the SFX engine has no song, so it counts at
120 BPM). **Master** is the mastering chain on everything — EQ (three bands),
compressor, saturation, width, output gain and a look-ahead limiter; off, the
output is bit-exact with an unmastered engine.

**Test in the engine** is the real engine in an AudioWorklet, with the whole
bank, and the only place voice limiting is observable — cooldowns, instance
caps, group budgets, stealing and priority do not exist until sounds compete
for a finite pool in real time. It is built around **refusals**: "rejected 40:
38 cooldown, 2 bus budget" tells you which number to change. Hold *flood* and
watch it refuse politely rather than stutter — the number that would be a fault
is a dropped command.

## The modal body

Up to eight damped resonators per sound, under **Layers → Rings**. The patch is the
**exciter**; these are the body it rings. That is the difference between a sound
that is merely loud and one that seems *struck*, and it is the right tool for
anything with a casing, a shell or a mechanism — which in a game set is most
things.

Each mode is a frequency, a decay to −60 dB, and a level. Add one and the editor
offers an octave above the last at half the decay, which is roughly the series a
real object has, so a second mode is useful before anyone touches it. `dry
exciter` at zero is the resonance alone, with no strike under it.

A four-millisecond click into a 700 ms mode is a one-and-a-half-second sound,
and none of that length came from the patch. The instance stays alive while any
mode still has energy, so a tail is never cut at the end of the exciter.

## Effects

An effect needs two things: a character (Mix & test → Effects) and a send
from a group (Mix & test → Groups). A wet gain of zero means it never runs at all — not merely
cheaper, since an effect fed silence still advances its delay lines and the
render would start depending on how long the engine had been alive.

**Chorus is the "many from one" effect** — detuned, delayed copies of a source.
It is what turns a single synthesized voice into a choir, and a steady machine
into a beating one. The cheapest way to make a bank sound like it has more in it
than it does.

## Files

Save and Open are in the top bar; the rest are under **⋯**.

| Action | Result |
|---|---|
| Open | `.sfxproj`, or drag one onto the page |
| Save / Save as | `.sfxproj` — the editable bank |
| Export for the game | `.sfxb`, byte-identical to `dsound_bake`'s |
| Export this sound as WAV | a 48 kHz stereo `.wav` of the selected take |
| Even out the levels | (library ⋯) sets every sound's volume so its loudest moment lands on one target |

**Autosave**, 1 s after edits stop and at most 10 s apart. The working copy
goes into the app's own storage on every platform, so a reload or a crash
reopens the bank as it was, unsaved changes included; nothing asks "leave the
page?". A bank that came from a file — the desktop and Android apps know its
path — is also written back to that file (⋯ → *Save to the file
automatically*, on by default). A browser has no path, only downloads, so
there Save is still how a file is made, and the dot beside the bank's name
means "not in a file yet", not "at risk". If a file write fails (moved,
permission lapsed), autosave stops writing files until the next Save and says
so. Inside the studio none of this runs: the studio keeps drafts and saves into
the module on Back. Beside the working copy, what is remembered is where you
were — view, selection, search, which cards are folded.

## Mobile

One layout, driven by width: under 900 px the library and the sound are two
screens, *In the game* is a bottom sheet above its dimmer, Undo and Redo move
into ⋯, targets are finger-sized, inputs are 16 px so Safari does not zoom on
focus, and the height is `dvh` so the shrinking URL bar does not leave the
bottom of the page unreachable.

### The Android app

`./scripts/package_android.sh` stages exactly what `package_web.sh` stages and
wraps it in a WebView, built with aapt2, javac, d8 and apksigner — no Gradle,
no Capacitor, no NDK (the engine is wasm, so the APK has no native code and
runs on any ABI). The result is `build/android/digidreamsound.apk`, debug-signed
with a key kept in `build/android/` so each build installs over the last.

- **The page is not modified.** `MainActivity` serves the assets on
  `https://appassets.androidplatform.net` — AudioWorklet, module Workers and
  wasm need a secure context — and injects one script, `android/web/android-shell.js`,
  which defines `window.digidream`: the same bridge `electron/preload.js`
  defines. The vendored `lib/desktop.js` then takes its native path.
- **Files are the system's own pickers.** Open is `ACTION_OPEN_DOCUMENT`, Save
  As / Bake / Export WAV are `ACTION_CREATE_DOCUMENT`, and File → Save writes
  back into the document you opened, in place. The match sheet's recording picker
  is the WebView file chooser. Access is kept across restarts.
- **No permissions**, not even INTERNET: nothing opens a socket, and every
  request that is not the asset origin is answered 404.
- **Failures are on the screen.** An uncaught error becomes a red banner with
  the build stamp (`assets/build.txt`); a missing page becomes a report of what
  was asked and what the APK holds. A screenshot is the bug report.
- Back asks before discarding an unsaved bank. Rotation keeps the page.

Measured on the emulator (Android 17, WebView 152) with the previous layout:
boot, thumbnails, audition, editing, the live engine, a fit restart (≈16 s for
8 steps: slower than a desktop), bake, open, save, save in place, the file
chooser, the banner and a rotation. The page changed since; the shell did not.

Only the starter bank ships inside the APK. To work on another bank, put its
`.sfxproj` on the phone and use File → Open.

## Keys

| | |
|---|---|
| Space | play / stop the selected sound |
| R / V | randomize / vary (a recipe take) |
| L | repeat |
| ↑ ↓ | previous / next sound |
| ⌘Z / ⌘⇧Z | undo / redo — one history for everything |
| ⌘S / ⌘⇧S | save / save as |
| ⌘O | open |
| ? | how this works |
| Esc | close a sheet or menu |

## Tests

Browser tests stay out of `scripts/ci.sh`, which is offline and needs no Chrome.
They skip cleanly if run without one.

```sh
node tool/serve.js &
PUPPETEER_DIR=/path/with/puppeteer-core node test/sfx_tool_smoke.mjs
```

It drives `window.__sfx` — the live subsystems, not a bespoke test API — so what
it exercises is what a finger would: the library and its pictures, the
transport, *In the game* writing and undoing a field, generating from a kind
and levelling it, Vary, a recipe knob rebuilding the patch, the four knobs
never compounding, clipping and *Fix level*, takes, the timeline and solo,
the layer flow, a tone change that must not scroll the page, drawing
harmonics, every layer section being present, timing, new/delete/undo, both
exports, a full match of a WAV it writes itself, the master chain, the live
engine refusing a flood, a comma decimal, and the phone layout — 61 checks.
