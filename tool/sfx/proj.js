//
// The .sfxproj document: the editor's model of truth.
//
// The engine is NOT the model. `WebSfx::bankInfoJson()` reports post-clamp,
// post-resolution state -- `bus` as a resolved index, `live` as the EFFECTIVE
// flag rather than the authored one (sound_def.h:107-117 documents those as
// different questions, and only the resolved one is exposed). Round-tripping a
// document through it would quietly rewrite the author's intent. So the parsed
// JSON is the document, and the engine is something we render with.
//
// Two rules follow, and both matter more than they look:
//
//   1. WE DO NOT NORMALISE ON LOAD. Defaults are supplied at READ time by the
//      field table below, never written into the document. A file that did not
//      say `priority` still does not say it after a round trip, so a diff shows
//      what a person changed and nothing else.
//
//   2. WE PRESERVE WHAT WE DO NOT UNDERSTAND. `content/game/doom2.sfxproj`
//      already carries `_generated_by`, `_originality` and `_fitted`, which the
//      loader ignores by design (sfx_json.cpp ignores unknown keys). Editing a
//      cooldown must not drop them.
//
import { FIELD_HELP } from './field-help.js';

// ---------------------------------------------------------------------------
// The field table.
//
// Transcribed from src/sfx/sfx_json.cpp's actual clamps -- not from the prose
// in docs/SFX_FORMAT.md, which rounds some of them. THREE things read this
// table: the policy inspector's controls, the UI-side clamp that keeps
// clamp_warn from firing in the first place, and validate.js deciding which
// control owns an engine diagnostic. When they disagree the UI lies, so there
// is one table.
//
// `lo`/`hi` are the engine's clamp. `def` is what the engine uses when the key
// is absent -- writing it into the document is exactly what rule 1 forbids.
// ---------------------------------------------------------------------------

const num = (path, label, lo, hi, def, unit) =>
  ({ path, label, kind: 'number', lo, hi, def, unit, help: FIELD_HELP[path] });
const bool = (path, label, def) =>
  ({ path, label, kind: 'bool', def, help: FIELD_HELP[path] });
const pick = (path, label, options, def) =>
  ({ path, label, kind: 'enum', options, def, help: FIELD_HELP[path] });

export const BUS_FIELDS = [
  num('gainDb', 'gain', -96, 24, 0, 'dB'),
  num('maxInstances', 'budget', 1, 48, 16, ''),
  bool('spatial', 'spatial', true),
  num('sendReverb', 'reverb send', 0, 1, 0, ''),
  num('sendChorus', 'chorus send', 0, 1, 0, ''),
  num('sendDelay', 'delay send', 0, 1, 0, ''),
];

// ---------------------------------------------------------------------------
// Bank effects.
//
// These are midisynth's own schemas, handed to midisynth's own loaders, so
// `reverb` here means exactly what `reverb` means in an fx.json and cannot
// drift from it (docs/SFX_FORMAT.md 1). The ranges below are song_config.cpp's.
//
// Reaching them is a two-step: an effect has a character (these) and a per-bus
// send (above). Wet gain at zero means the effect never runs at all, which is
// not only cheaper -- an effect fed nothing would still advance its delay
// lines, and the render would start depending on how long the engine had been
// alive.
// ---------------------------------------------------------------------------
export const REVERB_FIELDS = [
  num('fx.reverb.roomSize', 'room size', 0, 1, 0.5, ''),
  num('fx.reverb.damping', 'damping', 0, 1, 0.5, ''),
  num('fx.reverb.width', 'width', 0, 1, 1, ''),
  num('fx.reverb.wetGain', 'wet', 0, 2, 0, ''),
  num('fx.reverb.preDelayMs', 'pre-delay', 0, 200, 0, 'ms'),
  num('fx.reverb.highPassHz', 'high-pass', 0, 2000, 0, 'Hz'),
]

export const CHORUS_FIELDS = [
  num('fx.chorus.rateHz', 'rate', 0, 10, 0.8, 'Hz'),
  num('fx.chorus.depthMs', 'depth', 0, 20, 3.5, 'ms'),
  num('fx.chorus.wetGain', 'wet', 0, 2, 0, ''),
]

export const DELAY_FIELDS = [
  num('fx.delay.timeMs', 'time', 1, 640, 320, 'ms'),
  num('fx.delay.feedback', 'feedback', 0, 0.9, 0.35, ''),
  num('fx.delay.wetGain', 'wet', 0, 2, 0, ''),
  bool('fx.delay.pingPong', 'ping-pong', false),
]

export const SOUND_FIELDS = [
  num('gainDb', 'gain', -96, 24, 0, 'dB'),
  num('priority', 'priority', 0, 255, 128, ''),
  num('maxInstances', 'max instances', 1, 64, 4, ''),
  pick('onLimit', 'on limit', ['reject', 'stealOldest'], 'reject'),
  num('cooldownMs', 'cooldown', 0, 32000, 0, 'ms'),
  num('pitchJitterCents', 'pitch jitter', 0, 2400, 0, '¢'),
  num('gainJitterDb', 'gain jitter', 0, 24, 0, 'dB'),
  bool('live', 'live', false),
  bool('loop', 'loop', false),
];

export const ATTEN_FIELDS = [
  num('attenuation.minDist', 'min distance', 0, 4096, 3.333, 'u'),
  num('attenuation.maxDist', 'max distance', 0, 4096, 25.0, 'u'),
  pick('attenuation.curve', 'curve', ['linear', 'exp', 'expAttack', 'sCurve'], 'linear'),
  num('attenuation.spread', 'spread', 0, 1, 1.0, ''),
  num('attenuation.rearDb', 'rear', -96, 0, 0, 'dB'),
  num('attenuation.airLpfHz', 'air low-pass', 0, 20000, 0, 'Hz'),
];

// ---------------------------------------------------------------------------
// The modal body.
//
// An exciter feeding damped resonators: the standard way to synthesize
// anything STRUCK, and the one technique the engine gained rather than
// borrowed. Each mode is a frequency, a decay to -60 dB, and a level.
// ---------------------------------------------------------------------------
export const RESONATOR_FIELDS = [
  num('hz', 'frequency', 20, 12000, 0, 'Hz'),
  num('decayMs', 'decay', 1, 4000, 0, 'ms'),
  num('gain', 'level', 0, 4, 1, ''),
];

export const RESONATOR_DRY = num('resonatorDry', 'dry exciter', 0, 2, 1, '');

/** Modes the engine keeps; extras are dropped with a warning. */
export const MAX_RESONATORS = 8;

export const DUCK_FIELDS = [
  num('duckMusic.db', 'duck music', -48, 0, 0, 'dB'),
  num('duckMusic.ms', 'duck for', 0, 32000, 0, 'ms'),
];

export const GESTURE_FIELDS = [
  num('atMs', 'at', 0, 32000, 0, 'ms'),
  num('note', 'note', 0, 127, 60, ''),
  num('vel', 'vel', 1, 127, 100, ''),
  num('durMs', 'hold', 0, 32000, 0, 'ms'),
];

export const VARIANT_FIELDS = [
  num('weight', 'weight', 1, 1000, 1, ''),
];

/** Caps the engine enforces that are not per-field ranges. */
export const LIMITS = {
  maxBuses: 8,          // config.h MAX_BUSES
  maxGestureSteps: 8,   // sound_def.h MAX_GESTURE_NOTES
  /** Above this, pitchJitterCents forces `live` (sfx_json.cpp:178). */
  jitterForcesLive: 600,
  /** sfx_json.cpp:104 warns above this -- the pitch table's ceiling. */
  gestureNoteWarn: 108,
};

// ---------------------------------------------------------------------------
// Dotted-path access, with the table's defaults applied on read.
// ---------------------------------------------------------------------------

/** Read `path` out of `obj`, falling back to the field's documented default. */
export function getField(obj, field) {
  const parts = field.path.split('.');
  let v = obj;
  for (const p of parts) {
    if (v === null || typeof v !== 'object') return field.def;
    v = v[p];
  }
  return v === undefined ? field.def : v;
}

/** True when the document actually says this, rather than inheriting it. */
export function hasField(obj, field) {
  const parts = field.path.split('.');
  let v = obj;
  for (const p of parts) {
    if (v === null || typeof v !== 'object' || !(p in v)) return false;
    v = v[p];
  }
  return v !== undefined;
}

/**
 * Write `path` into `obj`, creating intermediate objects.
 *
 * Setting a value back to its default REMOVES the key (and any container it
 * emptied), so a document never accumulates keys that say nothing. That is
 * rule 1 in the header, and it is why a round trip through the editor does not
 * inflate a 173 KB bank.
 */
export function setField(obj, field, value) {
  const parts = field.path.split('.');
  const last = parts.pop();
  const chain = [obj];
  let cur = obj;
  for (const p of parts) {
    if (cur[p] === null || typeof cur[p] !== 'object') cur[p] = {};
    cur = cur[p];
    chain.push(cur);
  }
  if (value === field.def || value === undefined) delete cur[last];
  else cur[last] = value;

  // Drop containers this emptied, innermost first.
  for (let i = chain.length - 1; i >= 1; i--) {
    if (Object.keys(chain[i]).length === 0) delete chain[i - 1][parts[i - 1]];
    else break;
  }
}

/**
 * Read a number out of a control, whatever the reader's locale.
 *
 * `<input type="number">` renders and returns a LOCALE-FORMATTED string: in a
 * French or German browser 0.05 comes back as "0,05", and `Number("0,05")` is
 * NaN. Every numeric control in this editor went through `Number(el.value)`,
 * so for half the world every fractional field silently reverted -- and in the
 * Fit tab, where the fallback was `|| 0` rather than the documented default,
 * it silently switched the envelope and band terms of the loss OFF. The fitter
 * then settled in the wrong octaves, which is precisely the failure the band
 * term exists to prevent.
 *
 * `valueAsNumber` is locale-independent by spec and is the right answer when
 * the element provides it; the string path is the fallback for a plain value.
 */
export function parseNum(value) {
  if (typeof value === 'number') return value;
  if (value === null || value === undefined) return NaN;
  if (typeof value === 'object' && 'valueAsNumber' in value) {
    const n = value.valueAsNumber;
    if (Number.isFinite(n)) return n;
    value = value.value;
  }
  // Strip grouping spaces first: French formats 1234.5 as "1 234,5", and the
  // separator is usually U+202F or U+00A0 rather than a plain space.
  const t = String(value).replace(/[\s\u00a0\u202f]/g, '');
  if (t === '') return NaN;
  // One comma and no dot: a decimal separator. Both present: the comma is a
  // thousands separator, so drop it.
  const norm = t.includes('.') ? t.replace(/,/g, '') : t.replace(',', '.');
  return Number(norm);
}

/** Clamp to the field's range, so the engine's clamp_warn never has to fire. */
export function clampField(field, value) {
  if (field.kind === 'number') {
    let v = parseNum(value);
    if (!Number.isFinite(v)) return field.def;
    if (v < field.lo) v = field.lo;
    if (v > field.hi) v = field.hi;
    return v;
  }
  if (field.kind === 'bool') return !!value;
  if (field.kind === 'enum') return field.options.includes(value) ? value : field.def;
  return value;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

/** A minimal document that loads, plays, and breaks none of §3.1. */
export function newProj(name = 'untitled') {
  return {
    version: 1,
    name,
    buses: [
      { name: 'world', gainDb: 0, maxInstances: 20, spatial: true, sendReverb: 0.15 },
      { name: 'player', gainDb: 0, maxInstances: 6, spatial: false },
      { name: 'ui', gainDb: -3, maxInstances: 4, spatial: false },
    ],
    sounds: [],
  };
}

/**
 * Parse, and repair only what would stop the document being editable.
 *
 * This is deliberately NOT a normaliser. It fills in the containers the UI
 * indexes into (`buses`, `sounds`, each sound's `variants`) and leaves every
 * scalar alone, because the engine's own clamping is the authority on scalars
 * and duplicating it here is how the two drift.
 *
 * Returns `{doc, repairs}`; throws only on JSON that will not parse.
 */
export function readProj(text) {
  const doc = JSON.parse(text);
  const repairs = [];
  if (doc === null || typeof doc !== 'object' || Array.isArray(doc)) {
    throw new Error('not a .sfxproj: the document is not a JSON object');
  }
  if (doc.version === undefined) { doc.version = 1; repairs.push('added version 1'); }
  if (doc.version > 1) {
    throw new Error(`this file is version ${doc.version}; this tool reads version 1`);
  }
  if (typeof doc.name !== 'string') { doc.name = 'untitled'; repairs.push('added a name'); }
  if (!Array.isArray(doc.buses) || doc.buses.length === 0) {
    doc.buses = newProj().buses;
    repairs.push('no buses: added world/player/ui');
  }
  if (!Array.isArray(doc.sounds)) { doc.sounds = []; repairs.push('added an empty sound list'); }
  for (const s of doc.sounds) {
    if (!Array.isArray(s.variants) || s.variants.length === 0) {
      s.variants = [{ patch: { name: s.name || 'new', oneShot: true, layers: [] } }];
      repairs.push(`${s.name}: added an empty variant`);
    }
  }
  return { doc, repairs };
}

/** Sounds, in the order the engine will index them. */
export const soundIndex = (doc, name) => doc.sounds.findIndex((s) => s.name === name);
export const busIndex = (doc, name) => doc.buses.findIndex((b) => b.name === name);

/** The bus a sound actually plays on: an unknown name falls back to bus 0. */
export function resolveBus(doc, sound) {
  const i = busIndex(doc, sound.bus);
  return i < 0 ? 0 : i;
}

/**
 * Is this sound cacheable, or will the engine force it live?
 *
 * Mirrors sfx_json.cpp:171-183 and pcm_cache.cpp's admission. The editor shows
 * this because "why is my sound live" is otherwise only answerable by reading
 * the load report.
 */
export function liveReason(sound) {
  if (sound.live) return 'authored live';
  if (sound.loop) return 'loop implies live';
  if ((sound.pitchJitterCents ?? 0) > LIMITS.jitterForcesLive) {
    return `pitch jitter above ${LIMITS.jitterForcesLive}¢ implies live`;
  }
  return null;
}

// ---------------------------------------------------------------------------
// Serialisation
//
// A hand-readable shape close to the authored files: a leaf object or array --
// one whose values are all primitive -- is written inline when it fits, so an
// envelope stays one line instead of six. JSON.stringify(doc, null, 2) on
// doom2.sfxproj produces roughly 19k lines; this produces about 4k, and diffs
// stay reviewable, which is the whole argument for shipping recipes.
// ---------------------------------------------------------------------------

const INLINE_WIDTH = 96;

const isPrimitive = (v) => v === null || typeof v !== 'object';
const isLeaf = (v) => Array.isArray(v)
  ? v.every(isPrimitive)
  : Object.values(v).every(isPrimitive);

function fmt(v) {
  // Round-trip floats exactly; the engine reads them with its own parser, and
  // JS's shortest round-trip repr is what JSON.stringify already produces.
  return JSON.stringify(v);
}

function write(v, indent, depth) {
  if (isPrimitive(v)) return fmt(v);
  const pad = indent.repeat(depth);
  const inner = indent.repeat(depth + 1);

  if (isLeaf(v)) {
    const body = Array.isArray(v)
      ? v.map(fmt).join(', ')
      : Object.entries(v).map(([k, x]) => `${fmt(k)}: ${fmt(x)}`).join(', ');
    const open = Array.isArray(v) ? '[' : '{';
    const close = Array.isArray(v) ? ']' : '}';
    if (body === '') return open + close;
    const line = `${open} ${body} ${close}`;
    if (pad.length + line.length <= INLINE_WIDTH) return line;
  }

  if (Array.isArray(v)) {
    if (v.length === 0) return '[]';
    const items = v.map((x) => inner + write(x, indent, depth + 1));
    return `[\n${items.join(',\n')}\n${pad}]`;
  }
  const entries = Object.entries(v);
  if (entries.length === 0) return '{}';
  const items = entries.map(([k, x]) => `${inner}${fmt(k)}: ${write(x, indent, depth + 1)}`);
  return `{\n${items.join(',\n')}\n${pad}}`;
}

/** Serialise a document. Key order is insertion order, so authored order survives. */
export function writeProj(doc) {
  return write(doc, '  ', 0) + '\n';
}

// ---------------------------------------------------------------------------
// ProjStore -- the document plus its undo history.
//
// Modelled on midisynth/tool/daw/project.js:245-298, with one difference that
// the Design tab forces: patch bodies are edited through a Proxy by
// patch-editor.js, which keeps its own 50-deep undo. So this store must be able
// to take a snapshot WITHOUT pushing it (`hold`), and push it later only if the
// document actually changed -- that is what makes a whole Design session one
// document-level undo step instead of hundreds.
// ---------------------------------------------------------------------------

const HISTORY_MAX = 100;
const COALESCE_MS = 700;

export class ProjStore extends EventTarget {
  constructor(doc = newProj(), path = null) {
    super();
    this._doc = doc;
    this._undo = [];
    this._redo = [];
    this._dirty = false;
    this._last = { label: null, at: 0 };
    this.path = path;
  }

  get doc() { return this._doc; }
  get dirty() { return this._dirty; }
  get canUndo() { return this._undo.length > 0; }
  get canRedo() { return this._redo.length > 0; }
  get undoLabel() { return this._undo.length ? this._undo[this._undo.length - 1].label : null; }
  get redoLabel() { return this._redo.length ? this._redo[this._redo.length - 1].label : null; }

  /** A serialised copy of the current document, for `pushHeld`. */
  hold() { return JSON.stringify(this._doc); }

  /**
   * Push a snapshot taken earlier, but only if the document moved since.
   * Returns true when something was pushed.
   */
  pushHeld(snap, label) {
    if (snap === JSON.stringify(this._doc)) return false;
    this._undo.push({ label, json: snap });
    if (this._undo.length > HISTORY_MAX) this._undo.shift();
    this._redo.length = 0;
    this._dirty = true;
    this._emit(label, false);
    return true;
  }

  /**
   * Apply `fn` to the document as one undoable step.
   *
   * `coalesce` folds consecutive edits carrying the same label into one entry,
   * so dragging a slider is one undo rather than forty.
   */
  mutate(label, fn, { coalesce = false, structural = false } = {}) {
    const before = JSON.stringify(this._doc);
    fn(this._doc);
    const after = JSON.stringify(this._doc);
    if (before === after) return false;

    const now = Date.now();
    const fold = coalesce
      && this._last.label === label
      && now - this._last.at < COALESCE_MS
      && this._undo.length > 0;
    if (!fold) {
      this._undo.push({ label, json: before });
      if (this._undo.length > HISTORY_MAX) this._undo.shift();
    }
    this._last = { label, at: now };
    this._redo.length = 0;
    this._dirty = true;
    this._emit(label, structural);
    return true;
  }

  undo() {
    const e = this._undo.pop();
    if (!e) return false;
    this._redo.push({ label: e.label, json: JSON.stringify(this._doc) });
    this._doc = JSON.parse(e.json);
    this._dirty = true;
    this._last = { label: null, at: 0 };
    this._emit(e.label, true);
    return true;
  }

  redo() {
    const e = this._redo.pop();
    if (!e) return false;
    this._undo.push({ label: e.label, json: JSON.stringify(this._doc) });
    this._doc = JSON.parse(e.json);
    this._dirty = true;
    this._last = { label: null, at: 0 };
    this._emit(e.label, true);
    return true;
  }

  /** A different document entirely: opening a file. Clears the history. */
  replace(doc, path = null) {
    this._doc = doc;
    this._undo.length = 0;
    this._redo.length = 0;
    this._last = { label: null, at: 0 };
    this._dirty = false;
    this.path = path;
    this._emit('open', true);
  }

  /** Saved: the document on disk now matches. */
  markSaved(path = this.path) {
    this._dirty = false;
    this.path = path;
    this._emit('save', false);
  }

  _emit(label, structural) {
    this.dispatchEvent(new CustomEvent('change', { detail: { label, structural } }));
  }
}
