# Embedding DigidreamSound in the engine

Written for whoever wires this into Digidream. Everything here was found by
reading the engine; none of it needs re-deriving, and nothing in `sound/`
depends on any of it being done a particular way.

The library is finished and gated. What does not exist yet is any engine-side
code at all: no wire message, no vocabulary entry, no client module, no asset
mount. That is deliberate — `sound/` was built as a standalone product first,
and the integration is a separate pass with its own decisions.

## The surface

`src/bindings/c_api.h` is the whole contract. The header is the reference; this
is the shape of it:

```c
dsnd_ctx* ctx = dsnd_create();
dsnd_load_bank(ctx, sfxb_bytes, len, report, sizeof report);   /* a .sfxb */
dsnd_build_cache(ctx, 0, report, sizeof report);               /* costly, optional */

const int shotgun = dsnd_sound_id(ctx, "weapon.shotgun.fire"); /* once, at map load */

dsnd_set_listener(ctx, x_q16, y_q16, z_q16, yaw_brad);         /* once a frame */
uint32_t h = dsnd_play(ctx, shotgun, -1, seed, sx_q16, sy_q16, sz_q16);

dsnd_render(ctx, out, frames);                                 /* audio thread only */
```

`src/bindings/embind_api.cpp` is the same thing for the browser, as a `WebSfx`
class designed to live inside an AudioWorklet — the arrangement
`client/src/audio/music.js` already uses for the synthesiser.

## Six things that will save you a day each

**1. Resolve ids at map load, never per trigger.** `dsnd_sound_id` is a linear
scan over names. The hot path takes an int.

**2. A handle is not a promise of audibility.** `dsnd_play` returns non-zero as
soon as the trigger is queued; whether it is ever heard is decided on the audio
thread by the cooldown, the sound's instance cap, the bus budget and its
neighbours' priorities. `dsnd_alive` answers, and `dsnd_diag` counts every
refusal separately — which matters, because "the sound didn't play" is
otherwise the least debuggable complaint a player can make.

**3. The seed is load-bearing.** Derive it from something both machines agree
on — an event id, an entity id, a tick. Two clients that agree on the
simulation then hear the same sound. A counter or a clock will drift.

**4. Simulation space, and the SIM yaw.** X/Y on the ground, Z up, Q16.16
units, yaw as a `brad` about +Z. `client/src/mapgeom.js` owns the one mapping
to Three.js and this is deliberately on the other side of it. **Do not take the
yaw off the camera** — it is Y-up with Z = −y, and everything will pan
backwards. One world unit is 48 Doom units.

**5. Two engines, summed in float.** The SFX engine is a separate instance from
the music synthesiser, which is what stops a burst of explosions stealing the
music's voices. Consequences: two `AudioWorkletNode`s into one `GainNode` in the
browser, and on native, sum the two int16 streams **in float** — there is
deliberately no `dsnd_render_add`, because summing two already soft-clipped
int16 streams quantises twice and can still clip.

**6. Ducking crosses by hand.** Poll `dsnd_music_duck_milli()` once per audio
callback and pass it to `msynth_duck_milli()`. The hook on the music side is
already written and waiting: `client/src/audio/music.js:360`, whose own comment
says *"what deserves ducking is a gameplay decision"*.

## Threading

`dsnd_render` is the only audio-thread call. Everything else is control-thread
and goes through a lock-free queue — **except** `dsnd_load_bank*`,
`dsnd_build_cache` and `dsnd_unload_bank`, which rebuild what the audio thread
is reading and must not run concurrently with it. Same contract as midisynth's
C API, for the same reason.

`frames` must be a multiple of 64. A partial trailing block is filled with
silence rather than rendered, so the engine never falls half a block out of step
with its own clock.

---

## What I found in the engine

### The seam that already exists

`VfxEvent` — `core/include/dnp/proto/messages.hpp:486` —
`{u16 kind; fx3 pos; brad dir; u32 param;}`.

It is already a fire-and-forget, unreliable, catalogue-driven transient. Its
spec says *"A client that missed one has missed nothing it needs"*
(`docs/protocol.md:474`) and rule **R4** (`:478`) forbids it from being the sole
carrier of a simulation effect. A sound event wants exactly those semantics,
and R4 applies to it verbatim: a knockback must still happen through
authoritative state if the sound is lost.

Whether to extend that `kind` namespace or add a parallel `SoundEvent` array is
your call. The argument for reuse is the snapshot budget: `docs/protocol.md:319`
leaves about 1082 bytes for entities *and* transients together, and a
`VfxEvent` is already ~22 bytes of it.

The catalogue pattern to copy is `assets/props.txt:99-118`, parsed by
`client/src/render/vfx.js:21-45`, whose comment is the right philosophy here
too: *"The engine attaches no meaning to the number at all… the file says what
a 1 is."*

### The split that matters

Do not replicate everything.

- **Client-predicted, never sent**: the local player's own weapon fire,
  footsteps, jump, landing, swing whoosh. The client already predicts all of
  these, and `client/src/render/swing.js:42` reconstructs the entire swing from
  replicated parameters every frame without anything crossing the wire.
- **Server-originated, droppable**: a monster dying across the room, a door, an
  explosion. These must be events and must be safe to lose.

Replicating the first group as well double-plays it, and the duplicate arrives
a round trip late, which is worse than either alone.

### Where the frame hook goes

`client/src/main.js`, immediately after `camera.update(...)` and before
`renderer.render(...)` (around :1758-1763). That is the first point where both
listener position and orientation are final for the frame. The subject's world
position is read at :1509-1518; facing is `a.facing`, a `brad`.

### Assets

`docs/MODULES.md:154` is explicit that `module.txt` lists no assets, because *"a
manifest that has to be kept in step with the filesystem is a second source of
truth, and it goes stale the first time somebody adds a file."* So a new
`assets/sounds/` directory needs no manifest entry anywhere — but four route
lists need the new kind added:

| | |
|---|---|
| `tools/devserver.js` | :120 mount, :232 per-module override |
| `app/static.js` | :62 mount, :166 override |
| `ios/Shell.swift` | :313 override list |
| `scripts/build_apk.sh:191`, `scripts/build_ios.sh:139` | `for kind in textures sprites music` |

A second wasm module follows the **synth's** precedent rather than the core's:
staged as a flat root with its `dist/` sibling intact (the worklet imports it
relatively), optional at every stage, and warned about loudly when absent —
`scripts/build_app.sh:170-191` is the model.

### The vocabulary

`sound` would be a sibling of `vfx`, which already exists in all three of
`maps/scripts/trigger_vocab.json`, `mob_vocab.json` and `weapon_vocab.json`.
`scripts/check_vocab_docs.sh` gates the docs against the vocabulary in both
directions, so the doc has to land with the entry.

`maps/scripts/mobs.lua` already fires `M.on_death`, `M.on_shot` and
`M.on_player_hurt`; `docs/WEAPONS.md`'s phase machine means a per-phase `sound`
field would give "sound on entering this phase" with no new event plumbing at
all.

### Nothing here touches the simulation

`docs/physics-contract.md` governs `core/` and says nothing about audio, and
nothing about audio needs to enter it — as long as a sound is never the sole
carrier of an effect. Music already establishes the precedent
(`client/src/audio/music.js:17-21`): *"Music is not simulation: it is never
hashed, never predicted, never replicated, and two clients hearing slightly
different things is nobody's bug."*

Sound is the same, with one difference worth keeping: because triggers are
seeded from simulation state, two clients that agree on the simulation *do*
hear the same thing. That is a property worth not giving up by accident.

## Content

`content/game/doom2.sfxproj` is the engine-side set: ~100 roles named after the
engine's own identifiers (`imp`, `supershotgun`, `world.door.open`), so a
mapping from mob and weapon names is mechanical. It is deliberately **not** part
of the standalone product — `scripts/make_standalone.sh` excludes
`content/game/` — and `content/banks/starter.sfxproj` is the generic set that
ships with the editor instead.

Bake before shipping: `dsound_bake content/game/doom2.sfxproj doom2.sfxb`. The
result is ~85 KB for a hundred sounds, because a bank carries recipes rather
than recordings.
