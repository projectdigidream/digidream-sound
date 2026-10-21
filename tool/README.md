# DigidreamSound — the sound-effect editor

Make the sound effects for a game. Start from a kind of sound, shape it with a
few sliders, give it variations, decide how the game plays it, and export the
bank the game loads. Everything you hear in the editor is rendered by the same
fixed-point engine the game runs, so what you hear is what ships.

A bank is saved as **recipes, not recordings**: a hundred sounds are a couple
of hundred kilobytes of JSON, every one of them can still be changed, and no
audio sample is ever stored in it.

- [Getting started](#getting-started)
- [Five ideas](#five-ideas)
- [The screen](#the-screen)
- [Making a sound, step by step](#making-a-sound-step-by-step)
- [The sound, section by section](#the-sound-section-by-section)
- [In the game](#in-the-game)
- [Match a recording](#match-a-recording)
- [Mix & test](#mix--test)
- [Files and exporting](#files-and-exporting)
- [Keys](#keys)
- [On a phone, a tablet, or the desktop app](#on-a-phone-a-tablet-or-the-desktop-app)
- [Troubleshooting](#troubleshooting)
- [For developers](#for-developers)

---

## Getting started

From the `sound/` directory:

```sh
./scripts/build_wasm.sh        # once, and after changing the engine (needs emsdk)
node tool/serve.js             # then open http://localhost:8940/tool/sfx.html
```

Press **Start**. (Browsers only let a page make sound after a click; the
desktop and Android apps skip this.) The editor opens the small example bank,
so there is always something to play.

`serve.js` takes a port, `--host 0.0.0.0` to reach it from another device on
your network, and `--root DIR` to serve a packaged build instead of the source
tree. The editor needs a **secure context**: `localhost` is one; a plain
`http://` LAN address is not, and will load the page but make no sound. Use
the Android app, or serve over HTTPS, for another device.

---

## Five ideas

Everything in the editor is one of these.

| | |
|---|---|
| **Sound** | One thing the game asks for by name — `weapon.pistol.fire`, `monster.imp.death`. The list on the left. |
| **Recipe** | How a sound is made: a kind of sound (gunshot, explosion…) and the positions of its knobs. Pressing a kind makes one; the knobs change it. |
| **Take** | A version of a sound. A sound can have several; the game picks one at random each time it plays, so repeats do not sound identical. |
| **Layer** | A part of a take — the crack, the body, the tail. Layers start together and are added together. |
| **Group** | A bus: sounds that share a volume, a limit on how many play at once, and effect sends. Every sound plays on one group. |

---

## The screen

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ DigidreamSound  bank ●  [Sounds|Mix & test]  ▶ weapon.pistol.fire   ↶ ↷ Open Save ⋯ │
├───────────────┬──────────────────────────────────────────────┬───────────────────┤
│ LIBRARY       │ THE SOUND                                    │ IN THE GAME       │
│ search  ＋New │  name, tags                                  │  where it plays   │
│ ▾ WEAPON 2    │  waveform + one lane per layer (S / M)       │  when triggered   │
│   pistol.fire │  problems, with fixes                        │   a lot           │
│   shotgun…    │  Takes  A B ＋                               │  different every  │
│ ▾ IMPACT 1    │  Start from · Shape · Layers · Timing        │   time · distance │
│   …           │                                              │  music · playback │
├───────────────┴──────────────────────────────────────────────┴───────────────────┤
│ status line (click for the full log)                                             │
└──────────────────────────────────────────────────────────────────────────────────┘
```

- **Top bar.** The bank's name (click to rename; the dot means unsaved), the
  two views, the **play button** with the name of what it will play, undo and
  redo, Open, Save, and **⋯** for everything else.
- **Library (left).** Every sound, grouped by the first part of its name.
  Each row shows a picture of the sound, its length, a dot if something is
  wrong, and *too loud* if it would clip. Search matches names and tags.
- **The sound (middle).** The selected sound, top to bottom in the order you
  work on it.
- **In the game (right).** How the game plays the selected sound.
- **Mix & test** (top bar) swaps all three for the bank-wide settings: groups,
  effects, the master chain, and the live engine.

---

## Making a sound, step by step

1. **＋ New** in the library. A sound appears, made from a random kind, and
   plays.
2. **Name it** — click the name at the top of the sound. Use the name the game
   will ask for, with dots: `weapon.laser.fire`.
3. **Pick its kind** in *Start from*: Laser / zap. Every press makes a new one.
   Press **Randomize** (or **R**) until one is close.
4. **Vary** (**V**) nudges it: the same sound, a little different.
5. **Shape** it. Pitch, Length and Brightness first; then the recipe's own
   knobs, which are named by part (*Sweep · length*, *Tone · bite*). It plays
   each time you let go of a slider.
6. **Add a take** (**＋** next to *Takes*): it starts as a variation of the
   current one. Two or three takes stop a sound the player hears often from
   sounding mechanical.
7. **In the game**: choose its group, how many may play at once, and a little
   pitch variation.
8. **Save** (⌘S) keeps the editable bank. **⋯ → Export for the game** writes
   the `.sfxb` the game loads.

Every step is one undo (⌘Z). There is one history for everything.

---

## The sound, section by section

### The player

The **play button** is in the top bar, always in reach (**Space**). Under the
name:

- **The waveform**, drawn at the level the sound really plays at. Turn the
  volume up and it rises toward the orange ceiling lines; past them it turns
  red, the badge says **too loud**, and **Fix level** sets the volume so the
  loudest moment is at −6 dB. A very quiet sound offers **Make louder**.
- **The timeline**: one coloured lane per layer, on the same time axis, each
  rendered on its own by the engine. It shows which part is which and when
  each comes in — the crack at 0, the body carrying on, a late layer entering
  at 200 ms. **S** plays one layer alone, **M** plays the sound without it,
  *Hear everything* goes back. Listening this way never changes the sound.
  Click a lane's name to jump to that layer.
- **Length** and **peak** badges. **Repeat** (**L**) replays it continuously,
  for judging a short sound by repetition; it is a listening aid, not the
  sound's own *Repeats until stopped*. **Play on change** plays after every
  edit.

### Problems

Anything the engine or the editor objects to, in words, with a button for each
fix. The one to know: a sound that **never ends** (a layer that holds and is
never released). The fixes offered are *ends by itself*, *hold level 0*, *give
it a hold time*, or *mark it as repeating*.

### Takes

**A B C …** are the takes. The game picks one at random each time; with more
than one, each shows how often it is picked and has a **weight** (a take with
weight 2 is picked twice as often as one with weight 1). **＋** adds a take —
a variation if the take is a recipe, a copy otherwise. **Remove take** deletes
the selected one.

### Start from

Thirteen kinds of sound:

| Kind | Good for | Recipes it draws from |
|---|---|---|
| 🔫 Gunshot | firearms: a crack, a body, a tail, a mechanical action | `gunshot`, `weapon` |
| 💥 Explosion | blasts, rockets, barrels | `explosion`, `burst/*` |
| ⚡ Laser / zap | energy weapons, magic, electricity | `energy` |
| 👊 Hit / punch | impacts, footsteps, thuds | `impact/*` (9 noise × filter combinations) |
| 🔔 Metal / bell | anything struck that rings | `modal/*` |
| ✨ Pickup / blip | items, UI | `blip/*` |
| 🎵 Jingle | a few notes in a row: power-ups, alerts | `sequence/*` |
| 🚪 Door / mechanism | doors, switches, lifts | `mechanism` |
| ⚙️ Machine | working, buzzing things | `machine` |
| 🚗 Engine hum | held drones, motors, chainsaws | `motor` |
| 👹 Monster | roars and growls | `roar/*`, `growl` |
| 🗣️ Voice / grunt | vowel-like shouts, pain | `voice` |
| 💨 Whoosh | swings, air, passing things | `whoosh` |

- **Pressing a kind** replaces the selected take with a new sound of that kind.
- **Randomize** makes another of the same kind (staying with the recipe you
  have, if you have one).
- **Vary** moves every knob a little.
- **Match a recording…** — see [below](#match-a-recording).

Every generated sound is levelled to −6 dB before you hear it. The subtitle of
the card says where the take came from: *made from the Explosion recipe*,
*matched to shotgun.wav*, or *hand-made*.

### Shape

**Four knobs that work on any sound:**

| Knob | What it does |
|---|---|
| Volume | the sound's level, −40 to +12 dB |
| Pitch | up or down, ±24 semitones — tones and noise filters move together |
| Length | stretches or squeezes every time in the sound, ¼× to 4× |
| Brightness | moves every filter darker or brighter |

Pitch, Length and Brightness are applied to the take's original, so they never
pile up: set Pitch back to 0 and the sound is exactly what it was.

**The recipe's own knobs**, for a take made from a recipe, grouped by part
(*Crack*, *Body*, *Tail*, *Tone*, *Steps*…) and labelled with real units
(ms, Hz, notes, semitones).

Every slider:
- plays when you let go (with *Play on change* on);
- **double-click its name** to reset it;
- **click its value** to type one — `2.5k` for Hz and `0,5` both work.

### Layers

For when the knobs are not enough: **every** setting a take can hold has a
control here.

At the top, **how the parts connect**: each layer is its own chain (source,
tone, level), they all start together and are **added**, and the sum goes
through the rings, the volume, and out to the game. Then the whole-take
settings:

- **Ends by itself** — on for almost every effect. Off only for a sound the
  game holds and releases, like an engine.
- **Choke group** — a new sound in the same group cuts the others short.
- **Patch name** — a label for the recipe itself.

Then each layer (up to four), in sections. The first four are open; the rest
are folded one click away, with a summary in their header.

| Section | Controls |
|---|---|
| **Source** | Noise (white hiss, pink wash, brown rumble), Metal (a cymbal-like cluster), or Tone, with its generator and **every** parameter of it — see below |
| **Level and pitch** | level, pan, starts after (a delay), fixed pitch or follow the notes, detune |
| **Loudness over time** | a picture of the envelope, then wait, rise, hold at top, fall, stay at, fade out. Folded inside: the curve of each stage, loop, *faster when higher*, and the 0–99 rate alternatives |
| **Tone** | filter: low-pass, band, high-pass, notch, **comb** (a tuned echo that rings), **vowel** (formants, with a start and an end vowel); frequency, resonance, steepness, grit before, follows pitch. A **second filter** with its own settings, *stays put*, and whether it runs after or beside the first. A **filter sweep** (a full envelope). *Distortion after* |
| **Pitch sweep** | how far off the pitch starts, and the envelope that brings it back |
| **Timbre sweep** | how far the tone travels from its start shape to its end shape, and when |
| **Stack and ring** | a stack of detuned copies (voices, spread in pitch, spread in space, start apart); **ring** or **FM** against a hidden second sine |
| **Strength, turns and sends** | the strength window the layer answers to, how strength becomes level, release following the lift, **take turns** (layers in the same group alternate), and the layer's own room / chorus / echo sends |
| **LFOs** | up to four: shape (sine, triangle, saw, square, random), speed, delay, restart each play, tempo sync |
| **Modulation** | up to twelve routings: a source (an LFO, an envelope, strength…) → a destination (pitch, filter, level, pan, drive, timbre…) × an amount |

**Tone generators**, and how you edit them:

| Generator | Controls |
|---|---|
| Plain sine | — |
| Harmonics (morph) | brightness at the start and the end, and the harmonic shape (saw, square, triangle, flat) |
| **Drawn harmonics** | up to 64 bars — **drag across them to draw** the level of each harmonic; bar 1 is the fundamental |
| **Drawn wave** | **draw one cycle** of the wave with the pointer, or start from sine, triangle, saw or square; choose how many points |
| **Drawbars** | nine organ drawbars, 0–8, drawn the same way |
| **Chebyshev** | eight harmonic weights (−1 to 1, drawn), and the drive at the start and the end |
| Pulse | width at the start and the end |
| FM (metallic) | metal ratio, bite at the start and the end |
| Driven | curve (soft, hard, fold, crush, stairs), input wave, grit at the start and the end, bias, and the crush levels or stair steps |
| Sync | sync ratio at the start and the end, and the wave |

Every drawn shape can **morph to a second shape** (the Timbre sweep moves
between them), and **morph frames** sets how many steps lie between the two.

Below the layers:

- **Rings (a struck body)** — up to eight resonances, each a pitch, a ring
  time and a level, and how much of the dry strike is heard. The layers become
  the strike; the rings are the object that rings. This is what makes metal,
  glass and bells sound struck rather than merely loud.
- **Everything, as JSON** — the take exactly as the file stores it, for
  pasting between sounds or banks.

A take made from a **recipe** is rebuilt from its knobs, so the card first
says so: editing layers by hand turns the recipe off for that take (Shape
keeps its four general knobs). **Edit the layers anyway** opens them; undo
brings the recipe back.

### Timing & notes

The take's notes. With none, it plays once at its own pitch. Each step is a
time, a note, a strength and, for a sound that does not repeat, how long it is
held. Use it for parts at *different pitches* — a jingle, a door's thunk then
its creak — or for holding a sound. For a many-part hit (click, boom, tail),
use layers instead: they are one note, one decision, and cheaper.

---

## In the game

How the selected sound behaves when the game plays it.

| | |
|---|---|
| **Group** | which bus it plays on. A group placed in the world makes the sound quieter with distance; one that is not plays at the same level wherever you are |
| **Importance** | when a group is full, a more important sound wins |
| **At most, at once** | how many copies may play together |
| **Then** | when there are too many: *skip the new one*, or *cut the oldest* |
| **Minimum gap** | triggers closer together than this are skipped |
| **Different every time** | a random pitch and volume change on each play |
| **Distance** | full volume up to, silent from, the fade between, stereo width when near, muffled far away, quieter behind — for a group placed in the world |
| **Music** | lower the music while it plays, and for how long |
| **Playback** | *Repeats until stopped* (engines, alarms, ambience — the game must stop it) and *Render live* (rendered every play instead of once) |

Nothing you leave at its default is written to the file.

---

## Match a recording

Give it a recording and it finds a recipe that sounds like it.
**Match a recording…** in *Start from*, then three steps:

1. **The recording.** Drop a WAV (or click to choose one). **▶ Play it**.
   Drag across the waveform to match **one part** — a real sound is often a
   crack, then a tail, and each matches better on its own.
2. **What it is.** Pick the kind, or **Not sure** (it tries every recipe
   briefly, then refines the best). **Effort**: Quick, Good or Best, with an
   estimate of the time.
3. **The match.** Progress, then **▶ Recording** and **▶ Match** side by side,
   and how close it got (*very close*, *close*, *rough*, *loose*). **Use it for
   take A** writes it into the selected take.

The result is a recipe, so its knobs are available in *Shape* straight away.
The sound records which file it was matched to. **The recording itself is
never saved** — only numbers reach the bank. Use your own recordings, or ones
you are allowed to use.

*Advanced* in step 2 has the weights of the three things a match compares
(the spectrum, the loudness over time, the balance between octaves). Leave
them unless you know why; a zero switches one off.

A match in the browser takes about a minute. For many references, the
command-line fitter `build/dsound_fit` is several times faster.

---

## Mix & test

Settings for the whole bank.

- **Groups** — one card per bus: volume, at most at once, placed in the world,
  and how much it sends to the room, chorus and echo. Rename a group and its
  sounds follow; a group with sounds in it cannot be removed.
- **Effects** — **Room** (size, darkness, width, level, gap before, cut lows),
  **Chorus** (speed, depth, level), **Echo** (time, repeats, level, ping-pong,
  or time as a beat). An effect is heard only when its level is above zero
  **and** a group sends into it.
- **Master** — a mastering chain on everything: three-band EQ, compressor,
  saturation, stereo width, output gain and a look-ahead limiter. Off by
  default; off means the output is exactly the unmastered engine.
- **Test in the engine** — the real engine with the whole bank, as the game
  runs it. **Start the engine**, then play a sound once, or **hold to flood**
  it at a rate you choose, from random positions if you like; drag in the
  world view to move the listener. The counters show what is playing, and
  **Refusals** says why sounds were not played and what to change —
  refusing politely is the design; a *dropped* command would be a fault.

---

## Files and exporting

| | |
|---|---|
| **Open** (⌘O) | a `.sfxproj`, or drag one onto the page |
| **Save** (⌘S) / **Save as** (⌘⇧S) | the `.sfxproj` — the editable bank. Keep this one |
| **⋯ → Export for the game** | the `.sfxb` the game loads |
| **⋯ → Export this sound as WAV** | the selected take, 48 kHz stereo |
| **⋯ → Open the example bank** | the small starter bank |
| **⋯ → New bank** | an empty bank with three groups |
| **Library ⋯ → Even out the levels** | sets every sound's volume so its loudest moment lands on one target |
| **Library ⋯ → Sort A → Z** | reorders the sounds by name |

**Autosave.** A second after you stop editing, your work is kept in the app,
whatever you are running it in: close it, reload, or lose power, and it opens
again where you were, unsaved changes included.

- **Desktop and Android apps:** a bank you opened or saved is a file, and it is
  written back to that file automatically too. To experiment without touching
  the file, turn off **⋯ → Save to the file automatically**; Save writes it when
  you choose.
- **In a browser:** a browser can only download files, so it keeps your work in
  its own storage instead, and **Save** downloads a `.sfxproj`. The dot beside
  the bank's name means "not in a file yet". Clearing the browser's site data
  clears that copy, so save the ones you want to keep.
- **Inside the studio:** the studio saves the pack when you go back to it, and
  keeps a draft meanwhile.

Only one bank is kept this way: opening another replaces it (the editor asks
first if the current one is not in a file). The editor also remembers where you
were — the view, the selected sound, the search, which sections were folded.

---

## Keys

| | |
|---|---|
| Space | play / stop |
| R | randomize |
| V | vary |
| L | repeat |
| ↑ ↓ | previous / next sound |
| ⌘Z / ⌘⇧Z | undo / redo |
| ⌘S / ⌘⇧S | save / save as |
| ⌘O | open |
| ? | how this works |
| Esc | close a sheet or menu |

(Ctrl instead of ⌘ on Windows and Linux.) Double-click a slider's name to reset
it; click its value to type one.

---

## On a phone, a tablet, or the desktop app

- **Tablet / narrow window** (under 1250 px): *In the game* becomes a drawer —
  **In the game ›** at the top of the sound.
- **Phone** (under 900 px): the library and the sound are two screens
  (**‹ Sounds** goes back), *In the game* slides up from the bottom, undo and
  redo move into **⋯**, and every control is finger-sized.
- **Android app**: `./scripts/package_android.sh` builds
  `build/android/digidreamsound.apk` from the Android SDK alone. Install it by
  copying it to the phone. Open, Save and Export use Android's own file
  picker; *Save* writes back into the file you opened. It needs no
  permissions. Any error shows as a red banner with the build date — a
  screenshot is enough to report it.
- **Desktop app**: `./scripts/package_electron.sh` (macOS, Windows, Linux).
  Native Open and Save dialogs, and no *Start* click.
- **Static site**: `./scripts/package_web.sh` produces a folder to put on any
  HTTPS host.

All of them run the same page.

---

## Troubleshooting

| | |
|---|---|
| **No sound at all** | Press Start (the browser needs a click). Check the page is on `localhost` or HTTPS, not a plain `http://` address. |
| **A sound shows *too loud*** | Press **Fix level**, or lower Volume. For the whole bank: Library ⋯ → Even out the levels. |
| **"This sound does not load"** | Read the problem above the takes; most have a fix button. The JSON box in Layers shows exactly what is stored. |
| **A sound never stops in the game** | It holds and is never released: turn on *Ends by itself* (Layers), or use the fix button. |
| **The recipe knobs disappeared** | The take's layers were edited by hand, which detaches it from its recipe. Undo, or pick a kind again. |
| **I hear it in the editor but not in *Test in the engine*** | Look at *Refusals*: usually the minimum gap, *at most at once*, or the group's limit. |
| **The page says it cannot reach the example bank** | The page was opened from a file or a packaged build without it; use Open. |
| **Anything else** | Click the status line at the bottom for the full log. |

---

## For developers

### How it works

```
sfx.html ── sfx/app.js ─┬─ library.js   the list and its pictures
                        ├─ stage.js     the selected sound: player, timeline, takes,
                        │               Start from, Shape  (+ layers.js: Layers, Timing)
                        ├─ ingame.js    In the game
                        ├─ match.js     Match a recording
                        └─ mix.js       Mix & test  (+ play-tab.js: the live engine)

   renders, previews, bakes, fits ─▶ tool-worker.js ─▶ dist/dsound-tool.mjs  (engine + fitter)
   Test in the engine             ─▶ sfx-worklet.js ─▶ dist/dsound.mjs       (engine only)
```

- **The `.sfxproj` is the model.** `proj.js` parses it, `ProjStore` edits it
  as undoable steps, and `writeProj` writes it back in a diff-friendly shape.
  Defaults are never written, and keys the editor does not understand —
  anything starting with `_` — are kept.
- **All rendering happens in a Worker**, never on the audio thread. Loading a
  bank costs milliseconds per edit and unloads the engine if a patch fails to
  validate, so the AudioWorklet is used only by *Test in the engine*.
  Auditions are rendered buffers played through the Web Audio API.
- **What you hear is dry + volume.** The worker's render is the sound's
  samples; a sound's `gainDb` is applied at playback, exactly as the engine
  applies it, so the editor applies it to the audition, the waveform and the
  peak (`heardPeak` in `stage.js`).
- **Everything is one engine.** `scripts/run_goldens.sh` requires the native
  binary and both wasm modules to render every golden case to the same hash.

### What the editor writes into a take

All of these are ignored by the engine's loader.

| Key | On | Meaning |
|---|---|---|
| `_recipe` | a take | `{arch, u}`: the architecture and its normalised knob vector (each 0–1). The worker's `recipe` call (`SfxFitter.buildPatch`) rebuilds the patch from it |
| `_shape` | a take | `{pitch, length, bright}` for the four general knobs |
| `_base` | a take | for a hand-made take with a shape: the untouched original (`patch`, `gesture`, `resonators`), so the shape never compounds |
| `_fitted` | a sound | `{arch, ref, loss, at}`: provenance of a match |

The patch itself is always the finished result — recipe (or `_base`), then
`_shape` — so the game never needs any of this.

### Files

| File | |
|---|---|
| `sfx.html` | the page, all of its CSS, and the layout |
| `sfx/app.js` | boot, views, the document, file actions, keys, help, `window.__sfx` |
| `sfx/library.js` | the library: groups, search, content-addressed thumbnails, level-all |
| `sfx/stage.js` | the selected sound: player, per-layer timeline, solo/mute, problems, takes, Start from, Shape |
| `sfx/recipes.js` | the kinds, parameter labels, randomize/vary, the four general knobs, writing a take |
| `sfx/layers.js` | Layers (every patch setting, bar and wave drawing) and Timing |
| `sfx/ingame.js` | In the game |
| `sfx/match.js` | Match a recording |
| `sfx/mix.js` | Mix & test: groups, effects, master |
| `sfx/play-tab.js` | Test in the engine |
| `sfx/controls.js` | the shared slider, choice, switch, number box and card |
| `sfx/proj.js` | the document model, field tables (the engine's real clamps), undo |
| `sfx/audition.js` | playing a rendered take |
| `sfx/tool-client.js` / `sfx/sfx-client.js` | promise wrappers for the worker and the worklet |
| `sfx/validate.js`, `sfx/field-help.js`, `sfx/waveform.js` | diagnostics, help text, drawing |
| `tool-worker.js` | the worker: whole-bank and scratch engines, the fitter, bake, WAV |
| `sfx-worklet.js` | the AudioWorklet for the live engine |
| `lib/ui.js`, `lib/desktop.js` | shared with midisynth, byte-checked against `lib/MANIFEST` |
| `serve.js` | the dev server |

### Tests

```sh
./scripts/ci.sh                        # the whole gate: engine, goldens, wasm, content, standalone
```

The browser test drives the page through `window.__sfx` — the same objects
the page uses — in headless Chrome, 61 checks. `puppeteer-core` is not a
dependency of this repository; install it anywhere:

```sh
node tool/serve.js &
mkdir -p /tmp/pptr && (cd /tmp/pptr && npm i puppeteer-core@23)
PUPPETEER_DIR=/tmp/pptr node test/sfx_tool_smoke.mjs
```

It skips cleanly without Chrome, puppeteer or the server. Set `CHROME=` for a
different browser path and `DSND_TOOL_URL=` for a different address.

### Extending it

- **A new kind in *Start from***: add an entry to `KINDS` in
  `sfx/recipes.js`, pointing at existing architectures.
- **A new architecture**: add it to `src/fit/fit.cpp`, rebuild the wasm
  (`scripts/build_wasm.sh`); it appears in the match list and can be used by a
  kind. Name its parameters `<part><Quality>` (`crackGain`, `bodyDecay`) and
  `describeParam` labels them; add the part to `PARTS` if it is new.
- **A new engine setting**: add it to the loader, then to the field table in
  `sfx/proj.js` (for sound, bus or effect settings) or to `sfx/layers.js`
  (for patch settings). Every setting the loader reads must have a control.

More background — why the fitter works the way it does, what was measured,
and the design decisions — is in [docs/SOUND.md](../docs/SOUND.md).
