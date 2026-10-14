# The sound bank format — full reference

Two files, one schema.

| File | Written by | Purpose |
|---|---|---|
| `.sfxproj` | the editor, or by hand | the authoring document: JSON, readable, diffable |
| `.sfxb` | `dsound_bake` | the shipping deliverable: sealed binary, loads without parsing patches twice |

Anything that accepts a `.sfxb` also accepts a `.sfxproj`, which is slower —
it builds every wavetable from its recipe — but means a tool never has to bake
to hear something.

**General rules, everywhere below.** Durations are milliseconds. Gains are
decibels. Distances are world units, where one unit is 48 Doom units (about
1.46 m). Numbers are plain decimals — the parser has no exponent notation,
deliberately, because `strtod` never touches content. Out-of-range values are
clamped with a warning rather than refused; a bank that half-loads and plays is
worth more than one that refuses, because the person who has to fix it is
usually mid-session with headphones on.

There are exactly two hard errors: a bank with no loadable sound, and a variant
whose voice can never end (§3.1).

---

## 1. The document

```json
{
  "version": 1,
  "name": "starter",
  "buses":  [ ... ],
  "fx":     { "reverb": { ... } },
  "master": { ... },
  "sounds": [ ... ]
}
```

`version` above 1 is refused outright rather than read optimistically.

`fx` and `master` are **midisynth's own schemas**, handed to midisynth's own
loaders — see `docs/PATCH_FORMAT.md` §5 in that project. They are not
re-parsed here, so `reverb` in a `.sfxproj` means exactly what `reverb` means
in an `fx.json`, and cannot drift from it.

All three sends are wired: reverb, chorus and delay. An effect needs two things
to be heard -- a character here, and a per-bus send (§2) -- and a wet gain of
zero means it never runs at all. That is not only cheaper: an effect being fed
silence still advances its delay lines, so a render would start depending on how
long the engine had been alive.

**Chorus is the "many from one" effect.** Detuned, delayed copies of a source:
what turns a single synthesized voice into a choir, and a steady machine into a
beating one. It is the cheapest way to make a bank sound like it has more in it
than it does.

## 2. Buses

A bus is a voice budget with a gain on it.

```json
{ "name": "world", "gainDb": 0, "maxInstances": 20,
  "spatial": true, "sendReverb": 0.18, "sendChorus": 0, "sendDelay": 0 }
```

| Field | Range | Default | Meaning |
|---|---|---|---|
| `name` | string | `busN` | what a sound's `bus` refers to |
| `gainDb` | −96..24 | `0` | applied to the whole bus, rampable at runtime |
| `maxInstances` | 1..48 | `16` | **the budget**. Stealing never crosses a bus |
| `spatial` | bool | `true` | `false` means the position argument is ignored and `pan` is taken directly — UI, the player's own weapon, anything not in the world |
| `sendReverb` | 0..1 | `0` | how much of this bus reaches the reverb |
| `sendChorus` | 0..1 | `0` | how much of this bus reaches the chorus |
| `sendDelay` | 0..1 | `0` | how much of this bus reaches the delay |

**Why stealing is confined.** A designer who puts the player's weapon on its own
bus is buying a guarantee: however many things are happening in the world, and
whatever priorities they carry, they cannot reach across and cut it. A single
global budget with priorities alone gets this wrong the first time someone
raises a number.

A `spatial` bus caches its sounds **mono** — a point source's own stereo image
would fight the panner, and the baker warns when it discards a non-zero one.

## 3. Sounds

```json
{
  "name": "weapon.shotgun.fire",
  "bus": "player",
  "gainDb": -2,
  "priority": 210,
  "maxInstances": 2,
  "onLimit": "reject",
  "cooldownMs": 90,
  "pitchJitterCents": 25,
  "gainJitterDb": 1.0,
  "live": false,
  "loop": false,
  "attenuation": { "minDist": 3.33, "maxDist": 25, "curve": "exp",
                   "spread": 1.0, "rearDb": 0, "airLpfHz": 0 },
  "duckMusic": { "db": -6, "ms": 400 },
  "tags": ["weapon"],
  "variants": [ ... ]
}
```

| Field | Range | Default | Meaning |
|---|---|---|---|
| `name` | string | *required* | how the game asks for it. Convention is `group.subject.event` |
| `bus` | string | first bus | an unknown name warns and falls back |
| `gainDb` | −96..24 | `0` | |
| `priority` | 0..255 | `128` | higher survives. **A higher-priority instance is never cut for a lower-priority one** — the new play is refused instead |
| `maxInstances` | 1..64 | `4` | how many of *this* sound may overlap |
| `onLimit` | `reject` \| `stealOldest` | `reject` | what happens at the cap. `stealOldest` cuts this sound's own oldest, never anyone else's |
| `cooldownMs` | ms | `0` | minimum gap between accepted triggers. Eight monsters firing the same pistol in one tick is one trigger and seven rejections, none of which touches the pool |
| `pitchJitterCents` | 0..2400 | `0` | ± per trigger. Above 600 forces `live` (§4) |
| `gainJitterDb` | 0..24 | `0` | ± per trigger |
| `live` | bool | `false` | synthesize per instance instead of playing cached PCM |
| `loop` | bool | `false` | implies `live` — a loop has no finite render |
| `tags` | strings | `[]` | editor only; **stripped from `.sfxb`** |

### 3.1 The error the format refuses

A layer whose amp envelope sustains above zero, on a sound that does not loop,
whose gesture note has no `durMs`, and whose patch is not `oneShot`, produces a
voice that can never finish and an instance that is never reclaimed. The sound
plays a few times and then that slot is gone for the session.

This is caught at **load**, from the patch itself, and refused by name. It is
not caught by watching a render, because a render can only see that something
is still quietly sounding and a long decay looks exactly like that.

The fix is any one of: `"oneShot": true` on the patch, `"sustain": 0` on the
amp envelope, `"durMs"` on the gesture note, or `"loop": true` on the sound.

### 3.2 The modal body

```json
"resonatorDry": 0.4,
"resonators": [ { "hz": 220, "decayMs": 700, "gain": 1.2 },
                { "hz": 523, "decayMs": 450, "gain": 0.8 } ]
```

A bank of up to eight damped resonators, rung by the patch.

This is **modal synthesis**, and it is the one technique here that is not
subtractive. Everything else builds a sound by filtering noise or shaping a
wavetable; a modal body is a short broadband excitation followed by a handful
of exponentially decaying sinusoids at an object's own resonant frequencies.
That is what anything *struck* sounds like — metal, wood, glass, a casing, a
mechanism — and it is what the procedural-audio literature reaches for first
for impacts.

The division of labour is the classical one: **the patch is the exciter, and
this is the body it excites.** A four-millisecond noise click into a 700 ms
mode is a one-and-a-half-second sound, and none of it came from the patch.

| Field | Range | Default | Meaning |
|---|---|---|---|
| `hz` | 20..12000 | — | where the mode rings. Capped at fs/4, where the two-pole form stops behaving |
| `decayMs` | 1..4000 | — | time to fall 60 dB, as everywhere else in this format |
| `gain` | 0..4 | `1` | the amplitude the mode rings at when **struck** by a unit impulse |
| `resonatorDry` | 0..2 | `1` | how much of the exciter survives underneath. `0` is the body alone, with no strike |

**What the numbers mean, exactly.** Each mode is normalised so that its
impulse response is `gain · rⁿ · sin((n+1)ω)`: struck, it rings at `gain` times
the strike, and it is 60 dB down after `decayMs`. That is the standard modal
parameterisation, and it is tested at both ends of the range — a 3 kHz, 80 ms
click and a 200 Hz, 4 s gong both land on −60.0 dB at their stated decay.

It is *not* normalised to a steady sine. A long, narrow mode driven by a
sustained tone accumulates energy exactly as a real body does, and gets very
loud; a body is for things that are struck. Pair a resonator with a short
exciter.

**How it ends.** An instance stays alive while any mode still carries energy,
so the tail is never cut at the end of the exciter. And a mode *does* end: a
fixed-point two-pole left to itself settles into a limit cycle rather than
reaching zero, so a mode whose ring energy falls below half a 16-bit LSB is set
to exact zero, and one that has had no input for twice its `decayMs` — 120 dB
down by its own definition — always is. Both are integer tests, so a body ends
on the same sample on every platform.

A mode longer than about two seconds may still be sounding when the PCM cache's
four-second render cap arrives; such a sound has to be `live`.

The modes are rung on the **synthesis** path only, so they are baked into the
PCM cache and cached playback does not apply the body a second time. A cached
rung sound is bit-identical to its live render, exactly as an unrung one is.

### 3.3 Attenuation

| Field | Default | Meaning |
|---|---|---|
| `minDist` | `3.33` | full gain within this |
| `maxDist` | `25.0` | silent beyond |
| `curve` | `linear` | `linear`, `exp`, `expAttack`, `sCurve` — the same curve tables envelopes use, so `exp` means one thing across the project |
| `spread` | `1.0` | scales pan width. `0` is always centre |
| `rearDb` | `0` | attenuation for a source directly behind |
| `airLpfHz` | `0` (off) | the low-pass corner reached at `maxDist` |

The defaults are **Doom's own**: `S_CLOSE_DIST` is 160 map units and
`S_CLIPPING_DIST` is 1200, which at the engine's 1/48 scale are 3.33 and 25.0.
A converted map therefore sounds the way its author placed it without anyone
tuning a number.

Pan collapses toward centre as the listener enters a source. Without that,
walking through a sound swings it hard from one ear to the other over a few
centimetres, which is the most noticeable artefact a naive panner has.

**`rearDb` is disambiguation, not a cue.** Stereo cannot tell front from back —
a source directly behind pans exactly like one directly ahead — and a few dB is
enough to tell them apart without pretending to be HRTF. There is no height
cue: elevation folds into distance only.

**There is no doppler**, and it is not an omission. It would make the playback
rate a function of relative velocity, so `rate != 1.0` for every moving source
and the cached-equals-live property would cover nothing that ships; velocity is
not part of instance state; doing it without clicks needs a variable delay line;
and in a game with hitscan and fast rockets it is wrong more often than right. A
passing vehicle is a `live` sound with an authored pitch envelope.

### 3.4 `duckMusic`

`{ "db": -6, "ms": 400 }` — how far to pull the music down and for how long.
A pure function of how long ago the sound fired, so it is reproducible rather
than dependent on a level detector. At most four requests are honoured at once
and the engine takes the minimum; a bank where everything ducks is a bank where
nothing stands out.

The engine does not touch the music itself — it is a separate instance. The
host polls `dsnd_music_duck_milli()` and passes it on.

## 4. Variants

```json
{ "weight": 2,
  "patch": { ...a midisynth patch... },
  "gesture": [ { "atMs": 0,   "note": 36, "vel": 120 },
               { "atMs": 140, "note": 55, "vel": 70, "durMs": 300 } ] }
```

`weight` is the relative odds when the caller does not name a variant.

`patch` is a midisynth patch, unchanged — all nine wavetable generators, noise
colours, the metal oscillator, four layers, two filters, four LFOs, the mod
matrix. Its reference is `midisynth/docs/PATCH_FORMAT.md`.

**Prefer layers over gestures.** A patch already carries four layers with their
own `startDelayMs`, round-robin groups and choke groups, so a shotgun's click,
boom and tail are three *layers* on one note: one voice group, one steal
decision, the engine's own choke working. A gesture is for events at *different
pitches* — a door's thunk and its creak, a pickup's two-note rise. Capped at
eight steps.

`durMs` of 0 (the default) means no note-off at all: the envelope ends the
voice, which is what `oneShot` is for and is the common case.

## 5. Cached and live

The shipped asset is always a recipe. At load, `dsnd_build_cache` renders every
non-`live` variant once, in descending priority order, until a byte budget is
reached; anything past it falls back to live, and so does a `loop`, a
`pitchJitterCents` above 600, and a render that never falls silent.

**A cached sound played at unity rate is bit-identical to its live render.**
That is a tested property, not a claim — `scripts/run_goldens.sh` renders every
eligible case twice, once with the cache forced down to one kilobyte so
everything falls back to live, and the hashes must match. It is what makes
budget exhaustion and a half-built cache inaudible.

Two divergences follow from caching and are worth knowing before they are filed
as bugs:

- **Pitch jitter does different things to noise.** The noise oscillator ignores
  pitch, so jittering a live noise burst changes nothing, while resampling its
  cached PCM shifts the whole spectrum. Use `gainJitterDb` for noise-based
  sounds and `pitchJitterCents` for tonal ones.
- **Round robin does not survive the cache.** `rrGroup` is selected from a
  channel counter that a cached render sees once. Express the variation as
  weighted variants instead, which the format already has.

A sound that merely *fell back* to live uses the cache's canonical seed rather
than the caller's, so changing a budget never alters a sound nobody edited.
Only a sound the author marked `live` varies its noise per trigger.

## 6. `.sfxb` — the baked bank

Framed exactly like midisynth's `.mstk`, and for the same reasons: magic, a
`u16` version, a SHA-256 of the payload, then a section table of
`{tag, offset, size}`. Unknown sections are ignored so a newer baker can add
one; a newer **major** version is refused outright; a payload whose hash does
not match is refused whole rather than half-read.

| Tag | Contents |
|---|---|
| `BUSS` | buses |
| `SNDS` | the resolved playback policy, in fixed point |
| `VARS` | variants: patch index, weight, gesture |
| `PTCH` | patch JSON, deduped by content |
| `FX  ` | reverb, chorus and delay parameters, with an append-only tail |
| `BSND` | per-bus chorus and delay sends. Optional: written only when one is non-zero, absent means all zero |
| `RESO` | per-sound modal resonator banks. Optional on the same terms |
| `MSTR` | mastering, written only when enabled — absent means bypass |
| `META` | name, counts, the content hash, and an optional render seal |

`BSND` is how chorus and delay were added without a version bump, and it is
worth knowing why it is a section rather than three more fields on `BUSS`.
`BUSS` records are packed with no length prefix, so appending to them would make
every bank written before today unreadable -- and this build is allowed to read
those, since the version gate only refuses banks *newer* than it. A new section
is the path the format was designed for: an old reader skips it, a new reader
treats its absence as "all sends zero", which is exactly what an old bank meant.

**A `.sfxb` carries no PCM.** Baking the rendered audio would multiply the file
by about a hundred, pin the shipped sound to one build of the synthesiser — so a
DSP fix would silently ship stale audio — and duplicate work the runtime has to
be able to do anyway for `live` sounds.

What `META` carries instead is a **render seal**: the SHA-256 of a reference
`.sfxscript` render. `dsound_bake --seal` writes it and `--check` re-verifies it
on any machine, so a bank that would sound different elsewhere says so before a
player hears it.

`content_hash` is the SHA-256 of the patch JSON in order — it identifies the
*sound* of a bank, so changing a cooldown or a priority does not move it.

## 7. `.sfxscript` — the render script

What makes a sound engine testable. A game's audio is driven by events nobody
can replay, so the gate is driven by a written-down sequence instead.

```json
{ "version": 1, "seconds": 3,
  "listener": [ { "atSample": 0, "pos": [0, 0, 1.2], "yawDeg": 0 } ],
  "events":   [ { "atSample": 0, "sound": "world.door.open", "variant": 0,
                  "seed": 12345, "gainDb": 0, "pos": [3, 4, 0] } ] }
```

`atSample` is absolute, and the engine's own "apply at or before now" rule
quantises it to a control block exactly as a gameplay trigger would — so a
script exercises the real path rather than a special one.

```sh
dsound_render bank.sfxb script.sfxscript out.wav --hash [--no-cache]
```
