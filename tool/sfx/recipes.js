//
// Recipes: how a sound is MADE in this editor, in words a person uses.
//
// Three ideas, borrowed from the tools people already find easy:
//
//   * START FROM A KIND OF SOUND (sfxr, Bfxr, ChipTone). One click on
//     "Explosion" gives an explosion, and "Randomize" gives another one. The
//     kinds are the fitter's own architectures (src/fit/fit.cpp), which are
//     hand-built signal chains for exactly these families -- so a generated
//     sound and a fitted one are the same kind of object.
//
//   * A RECIPE IS KNOBS, NOT A PATCH. A take remembers the architecture and
//     its normalised parameter vector under `_recipe` (the loader ignores `_`
//     keys, as it already does for `_fitted`), and the patch is rebuilt from
//     it by the worker. So the sliders are the architecture's own parameters,
//     labelled by what they do ("Crack · length"), and moving one can never
//     produce a patch the architecture could not.
//
//   * FOUR KNOBS THAT WORK ON EVERYTHING: volume, pitch, length, brightness.
//     They are transforms over whatever the take's source is -- a recipe or a
//     hand-made patch -- recorded under `_shape`, so they stay adjustable and
//     never compound: the patch is always source + shape, recomputed.
//
// Nothing in here touches the document except through the functions at the
// bottom, which the caller runs inside a ProjStore.mutate().
//

// ---------------------------------------------------------------------------
// The kinds of sound. `archs` are expanded architecture names; a family name
// alone (e.g. 'impact') means "any of its expansions".
// ---------------------------------------------------------------------------
export const KINDS = [
  { id: 'gunshot', label: 'Gunshot', icon: 'gunshot', archs: ['gunshot', 'weapon'],
    hint: 'a crack, a body and a tail' },
  { id: 'explosion', label: 'Explosion', icon: 'explosion', archs: ['explosion', 'burst/pink', 'burst/white'],
    hint: 'a blast that rolls off' },
  { id: 'laser', label: 'Laser / zap', icon: 'laser', archs: ['energy'],
    hint: 'a bright tone that sweeps' },
  { id: 'hit', label: 'Hit / punch', icon: 'hit', archs: ['impact'],
    hint: 'a short filtered thud' },
  { id: 'metal', label: 'Metal / bell', icon: 'bell', archs: ['modal/white', 'modal/pink', 'modal/brown'],
    hint: 'a struck object that rings' },
  { id: 'pickup', label: 'Pickup / blip', icon: 'pickup', archs: ['blip'],
    hint: 'a short tone for items and UI' },
  { id: 'jingle', label: 'Jingle', icon: 'jingle', archs: ['sequence/saw', 'sequence/square'],
    hint: 'a few notes in a row' },
  { id: 'door', label: 'Door / mechanism', icon: 'door', archs: ['mechanism'],
    hint: 'a clunk, a slide, a stop' },
  { id: 'machine', label: 'Machine', icon: 'machine', archs: ['machine'],
    hint: 'a buzzing, working thing' },
  { id: 'engine', label: 'Engine hum', icon: 'engine', archs: ['motor'],
    hint: 'a held, throbbing drone' },
  { id: 'monster', label: 'Monster', icon: 'monster', archs: ['roar/pink', 'roar/white', 'growl'],
    hint: 'a roar or a growl' },
  { id: 'voice', label: 'Voice / grunt', icon: 'voice', archs: ['voice'],
    hint: 'a vowel-like shout' },
  { id: 'whoosh', label: 'Whoosh', icon: 'whoosh', archs: ['whoosh'],
    hint: 'air moving past' },
]

/** The concrete architecture names a kind may use, given the full list. */
export function archsOfKind (kind, all) {
  const out = []
  for (const want of kind.archs) {
    for (const a of all) if (a.name === want || a.family === want) out.push(a)
  }
  return out
}

/** Which kind an architecture belongs to, for labelling a fitted sound. */
export function kindOfArch (archName) {
  const fam = archName.split('/')[0]
  return KINDS.find((k) => k.archs.some((a) => a === archName || a === fam)) ?? null
}

// ---------------------------------------------------------------------------
// Parameter names -> words.
//
// The architectures name their parameters `<part><Quality>` -- crackGain,
// bodyDecay, tailHz -- and the few that do not are whole-sound controls. So a
// label is a part and a quality, and the sliders group by part.
// ---------------------------------------------------------------------------
const PARTS = {
  crack: 'Crack', body: 'Body', tail: 'Tail', breath: 'Breath', bed: 'Rumble',
  thump: 'Thump', thunk: 'Thunk', click: 'Click', splat: 'Splat', slide: 'Slide',
  mech: 'Mechanism', motor: 'Motor', buzz: 'Buzz', grit: 'Grit', air: 'Air',
  spin: 'Spin', tone: 'Tone', noise: 'Noise', excite: 'Strike', formant: 'Voice',
  growl: 'Growl', sweep: 'Sweep', lfo: 'Wobble', m1: 'Ring 1', m2: 'Ring 2',
  m3: 'Ring 3', m4: 'Ring 4', filt: 'Filter', wt: 'Timbre', gritWt: 'Grit',
}

const QUALITIES = {
  Gain: ['level', ''], Decay: ['length', 'ms'], Hz: ['pitch', 'Hz'], Q: ['resonance', ''],
  Note: ['pitch', 'note'], Drop: ['pitch drop', 'st'], Attack: ['fade in', 'ms'],
  At: ['starts at', 'ms'], Depth: ['amount', ''], Drive: ['grit', ''],
  Sustain: ['hold level', ''], Release: ['fade out', 'ms'], Hold: ['hold', 'ms'],
  Rate: ['speed', 'Hz'], Index: ['bite', ''], Swell: ['swell', ''], Delay: ['delay', 'ms'],
  PitchDecay: ['pitch sweep', 'ms'], WtDecay: ['timbre sweep', 'ms'], FiltDecay: ['sweep length', 'ms'],
  FiltDepth: ['sweep amount', ''], CutHz: ['brightness', 'Hz'], PitchSemis: ['pitch bend', 'st'],
  PitchAttack: ['bend time', 'ms'], WtAttack: ['timbre fade', 'ms'], ToLevel: ['on level', ''],
  ToFilter: ['on tone', ''],
}

/** Whole-sound parameters with no part prefix. */
const BARE = {
  attack: ['Main', 'fade in', 'ms'], decay: ['Main', 'length', 'ms'], sustain: ['Main', 'hold level', ''],
  release: ['Main', 'fade out', 'ms'], hold: ['Main', 'hold', 'ms'], holdMs: ['Main', 'held for', 'ms'],
  cutoff: ['Tone', 'brightness', 'Hz'], reso: ['Tone', 'resonance', ''], drive: ['Tone', 'grit', ''],
  note: ['Main', 'pitch', 'note'], pitchDrop: ['Main', 'pitch drop', 'st'],
  pitchDecay: ['Main', 'pitch sweep', 'ms'], startHarm: ['Tone', 'bright start', ''],
  endHarm: ['Tone', 'bright end', ''], wtDecay: ['Tone', 'timbre sweep', 'ms'],
  filtDepth: ['Tone', 'sweep amount', ''], filtDecay: ['Tone', 'sweep length', 'ms'],
  filtAttack: ['Tone', 'sweep fade in', 'ms'], wobble: ['Main', 'wobble', ''], swell: ['Main', 'swell', ''],
  ratio: ['Tone', 'metal ratio', ''], pulse: ['Main', 'pulse', ''], gain: ['Main', 'level', ''],
  dry: ['Body', 'dry strike', ''], fmIndex: ['Tone', 'bite', ''], startIndex: ['Tone', 'bite start', ''],
  endIndex: ['Tone', 'bite end', ''], startDrive: ['Tone', 'grit start', ''],
  endDrive: ['Tone', 'grit end', ''], motorRate: ['Motor', 'throb speed', 'Hz'],
  growlRate: ['Growl', 'speed', 'Hz'], growlDepth: ['Growl', 'amount', ''],
  sweepDepth: ['Sweep', 'amount', ''], sweepDecay: ['Sweep', 'length', 'ms'],
  sweepAttack: ['Sweep', 'fade in', 'ms'], lfoRate: ['Wobble', 'speed', 'Hz'],
  lfoToFilter: ['Wobble', 'on tone', ''], lfoToLevel: ['Wobble', 'on level', ''],
}

/** { group, label, unit } for one architecture parameter. */
export function describeParam (name) {
  if (BARE[name]) { const [group, label, unit] = BARE[name]; return { group, label, unit } }
  // Steps of a sequence or a burst: t2At, v3, n1.
  let m = /^t(\d)At$/.exec(name)
  if (m) return { group: 'Steps', label: `step ${m[1]} time`, unit: 'ms' }
  m = /^v(\d)$/.exec(name)
  if (m) return { group: 'Steps', label: `step ${m[1]} level`, unit: '' }
  m = /^n(\d)$/.exec(name)
  if (m) return { group: 'Steps', label: `step ${m[1]} note`, unit: 'note' }
  // <part><Quality>, longest quality first so "FiltDecay" beats "Decay".
  const quals = Object.keys(QUALITIES).sort((a, b) => b.length - a.length)
  for (const q of quals) {
    if (name.endsWith(q) && name.length > q.length) {
      const part = name.slice(0, -q.length)
      const [label, unit] = QUALITIES[q]
      return { group: PARTS[part] ?? cap(part), label, unit }
    }
  }
  return { group: 'Main', label: name, unit: '' }
}

const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1).replace(/([A-Z])/g, ' $1').toLowerCase()

/** The parameter's real value at normalised position u. */
export function denorm (p, u) {
  return p.log ? p.lo * Math.pow(p.hi / p.lo, u) : p.lo + (p.hi - p.lo) * u
}

const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B']
export const noteName = (n) => `${NOTE_NAMES[((Math.round(n) % 12) + 12) % 12]}${Math.floor(Math.round(n) / 12) - 1}`

/** A value, formatted for a slider readout. */
export function formatValue (v, unit) {
  if (unit === 'note') return noteName(v)
  if (unit === 'ms') return v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${Math.round(v)} ms`
  if (unit === 'Hz') return v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`
  if (unit === 'st') return `${v > 0 ? '+' : ''}${v.toFixed(1)} st`
  return v >= 10 ? v.toFixed(0) : v.toFixed(2)
}

// ---------------------------------------------------------------------------
// Randomness. Seeded, so "Randomize" is repeatable within a session and a
// test can pin what it produced.
// ---------------------------------------------------------------------------
let seed = (Date.now() >>> 0) || 1
export const reseed = (s) => { seed = (s >>> 0) || 1 }
function rand () {
  seed ^= seed << 13; seed >>>= 0
  seed ^= seed >>> 17
  seed ^= seed << 5; seed >>>= 0
  return seed / 4294967296
}
const gauss = () => { const u = rand() || 1e-9; return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * rand()) }
const clamp01 = (x) => Math.min(1, Math.max(0, x))

/**
 * A fresh parameter vector. Not uniform: the corners of a fitter's range are
 * where it goes to find odd references, and a generator that lives there
 * produces mostly noise. Centre-weighted is what makes "Randomize" worth
 * pressing twice.
 */
export function randomVector (arch) {
  return arch.params.map(() => clamp01(0.5 + gauss() * 0.22))
}

/** A nearby vector: "the same sound, a bit different". */
export function varyVector (u, amount = 0.07) {
  return u.map((x) => clamp01(x + gauss() * amount))
}

export const pick = (list) => list[Math.floor(rand() * list.length)]

// ---------------------------------------------------------------------------
// The four knobs that work on everything.
// ---------------------------------------------------------------------------
export const SHAPE = [
  { key: 'pitch', label: 'Pitch', lo: -24, hi: 24, step: 0.5, def: 0,
    fmt: (v) => `${v > 0 ? '+' : ''}${v} st`, hint: 'higher or lower, in semitones' },
  { key: 'length', label: 'Length', lo: -2, hi: 2, step: 0.05, def: 0,
    fmt: (v) => `×${Math.pow(2, v).toFixed(2)}`, hint: 'shorter or longer, by stretching every envelope' },
  { key: 'bright', label: 'Brightness', lo: -1, hi: 1, step: 0.05, def: 0,
    fmt: (v) => `${v > 0 ? '+' : ''}${Math.round(v * 100)}%`, hint: 'darker or brighter, by moving every filter' },
]

const ENVS = ['ampEnv', 'filterEnv', 'pitchEnv', 'wtPosEnv']
const TIMES = ['delay', 'attack', 'hold', 'decay', 'release']
const r2 = (x) => Math.round(x * 100) / 100
const hz = (x) => Math.round(Math.min(12000, Math.max(20, x)))

/**
 * Apply the shape to a (patch, gesture, resonators) triple, returning new
 * copies. Pure: the caller decides where the result goes.
 */
export function shapeOf ({ patch, gesture, resonators }, shape = {}) {
  const pitch = shape.pitch ?? 0
  const stretch = Math.pow(2, shape.length ?? 0)
  const bright = Math.pow(4, shape.bright ?? 0)
  const p = structuredClone(patch)
  let g = gesture ? structuredClone(gesture) : undefined
  const res = resonators ? structuredClone(resonators) : undefined
  const pr = Math.pow(2, pitch / 12)

  let follows = false
  for (const l of p.layers ?? []) {
    for (const e of ENVS) {
      const env = l[e]
      if (!env) continue
      for (const t of TIMES) if (typeof env[t] === 'number') env[t] = r2(env[t] * stretch)
    }
    if (typeof l.startDelayMs === 'number') l.startDelayMs = r2(Math.min(2000, l.startDelayMs * stretch))
    for (const f of [l.filter, l.filter2]) {
      if (!f || f.mode === 'comb' || f.mode === 'bypass') continue
      if (typeof f.cutoffHz === 'number' || f.mode) f.cutoffHz = hz((f.cutoffHz ?? 1000) * pr * bright)
    }
    if (typeof l.pitchNote === 'number' && l.pitchNote >= 0) {
      l.pitchNote = Math.min(127, Math.max(0, r2(l.pitchNote + pitch)))
    } else if ((l.osc ?? 'wavetable') !== 'noise') {
      follows = true
    }
  }
  if (g) {
    for (const s of g) {
      if (typeof s.atMs === 'number') s.atMs = Math.round(s.atMs * stretch)
      if (typeof s.durMs === 'number') s.durMs = Math.round(s.durMs * stretch)
      if (follows) s.note = Math.min(127, Math.max(0, r2((s.note ?? 60) + pitch)))
    }
  } else if (follows && pitch !== 0) {
    // A layer that follows the played note has nothing to move without a
    // gesture: give it the one step the engine would have played anyway.
    g = [{ atMs: 0, note: 60 + pitch }]
  }
  for (const m of res ?? []) {
    m.hz = hz(m.hz * pr)
    if (typeof m.decayMs === 'number') m.decayMs = Math.round(Math.min(4000, m.decayMs * stretch))
  }
  return { patch: p, gesture: g, resonators: res }
}

// ---------------------------------------------------------------------------
// Where a take comes from, and writing it back.
//
// A take's SOURCE is either `_recipe` {arch, u} or `_base` (a hand-made patch
// with its gesture and body) -- whichever the shape is applied on top of. A
// take with neither has its current patch as its source, implicitly; the
// first shape edit copies it into `_base` so the knobs have something fixed
// to work from.
// ---------------------------------------------------------------------------

/** The unshaped source of a take, as {patch, gesture, resonators}. */
export function sourceOf (sound, variant) {
  const v = sound.variants[variant]
  if (v._base) return structuredClone(v._base)
  return {
    patch: structuredClone(v.patch),
    gesture: v.gesture ? structuredClone(v.gesture) : undefined,
    resonators: sound.resonators ? structuredClone(sound.resonators) : undefined,
    resonatorDry: sound.resonatorDry,
  }
}

/**
 * Split a patch the fitter built into what belongs to the patch, the take and
 * the sound. Mirrors src/fit/fit.h wrap_as_bank(): an architecture holds one
 * template string, so a body, a gesture or a hold travels inside the patch
 * and has to be hoisted out, or the loader ignores it.
 */
export function hoist (built) {
  const patch = structuredClone(built)
  const out = { patch }
  if ('resonators' in patch) { out.resonators = patch.resonators; delete patch.resonators }
  if ('resonatorDry' in patch) { out.resonatorDry = patch.resonatorDry; delete patch.resonatorDry }
  if ('gesture' in patch) {
    out.gesture = patch.gesture
    delete patch.gesture
    delete patch.holdMs
  } else if ('holdMs' in patch) {
    out.gesture = [{ atMs: 0, durMs: patch.holdMs }]
    delete patch.holdMs
  }
  return out
}

/**
 * Write a source + shape into take `vi` of `sound` (a document object, inside
 * a mutate). `source` is {patch, gesture, resonators, resonatorDry}.
 */
export function writeTake (sound, vi, source, shape) {
  const v = sound.variants[vi]
  const s = shape && Object.values(shape).some((x) => x) ? shape : null
  const shaped = s ? shapeOf(source, s) : source
  v.patch = shaped.patch
  if (shaped.gesture && shaped.gesture.length) v.gesture = shaped.gesture
  else delete v.gesture
  if (sound.loop) for (const g of v.gesture ?? []) delete g.durMs
  // The body is the SOUND's, not the take's (docs/SFX_FORMAT.md 3.2): the
  // take being edited decides it.
  if (shaped.resonators && shaped.resonators.length) sound.resonators = shaped.resonators
  else delete sound.resonators
  if (source.resonatorDry !== undefined) sound.resonatorDry = source.resonatorDry
  else delete sound.resonatorDry
  if (s) v._shape = s
  else delete v._shape
}

/** Freeze the current patch as the take's own: detach it from recipe and shape. */
export function detach (sound, vi) {
  const v = sound.variants[vi]
  delete v._recipe
  delete v._base
  delete v._shape
}

/** Make sure a hand-made take has a fixed `_base` before the shape moves. */
export function ensureBase (sound, vi) {
  const v = sound.variants[vi]
  if (v._recipe || v._base) return
  v._base = {
    patch: structuredClone(v.patch),
    ...(v.gesture ? { gesture: structuredClone(v.gesture) } : {}),
    ...(sound.resonators ? { resonators: structuredClone(sound.resonators) } : {}),
    ...(sound.resonatorDry !== undefined ? { resonatorDry: sound.resonatorDry } : {}),
  }
}
