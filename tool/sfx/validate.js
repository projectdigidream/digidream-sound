//
// Engine diagnostics, arranged for a UI.
//
// The binding already splits each message into {path, text, sound, variant,
// field} -- see src/bindings/embind_api.cpp. This groups them so a view can
// ask the two questions it actually has: "is there anything wrong with THIS
// sound" and "what is wrong anywhere".
//
// The design rule throughout: a diagnostic is never swallowed. One that cannot
// be attached to a control still appears, in the problems list, with its text
// intact. Silently dropping the ones that do not fit a pattern is how a tool
// ends up lying about a bank being clean.
//
import { LIMITS } from './proj.js'

export const SEV = { error: 2, warning: 1 }

/** Flatten a worker report into one list, tagged by severity. */
export function toList (report) {
  if (!report) return []
  const out = []
  for (const w of report.warnings ?? []) out.push({ ...normalise(w), severity: 'warning' })
  for (const e of report.errors ?? []) out.push({ ...normalise(e), severity: 'error' })
  return out
}

// The binding emits objects; older paths and the CLI emit plain strings. Accept
// both so a caller never has to care which it got.
function normalise (m) {
  if (typeof m === 'string') return { path: '', text: m, sound: -1, variant: -1, field: '' }
  return {
    path: m.path ?? '',
    text: m.text ?? '',
    sound: m.sound ?? -1,
    variant: m.variant ?? -1,
    field: m.field ?? '',
  }
}

/** Group by sound index; -1 collects everything bank-wide or unattributable. */
export function bySound (list) {
  const map = new Map()
  for (const d of list) {
    const k = d.sound
    if (!map.has(k)) map.set(k, [])
    map.get(k).push(d)
  }
  return map
}

/** The worst severity in a list, for a status dot. */
export function worst (list) {
  let w = null
  for (const d of list) if (!w || SEV[d.severity] > SEV[w]) w = d.severity
  return w
}

/** Diagnostics that name a particular field, for marking a control. */
export function byField (list) {
  const map = new Map()
  for (const d of list) if (d.field) {
    if (!map.has(d.field)) map.set(d.field, [])
    map.get(d.field).push(d)
  }
  return map
}

// ---------------------------------------------------------------------------
// Checks the editor makes for itself.
//
// These are not duplicates of the engine's. They exist because the engine can
// only speak when a document is LOADED, and two of the conditions below make a
// document refuse to load at all -- at which point its message arrives with no
// bank behind it, so the binding cannot resolve a sound index and the UI has
// nothing to point at. Catching them here means the author sees the problem
// against the control that causes it, before the load fails.
// ---------------------------------------------------------------------------

/**
 * The one structural error the format refuses (docs/SFX_FORMAT.md 3.1):
 * a layer that sustains above zero, on a sound that does not loop, whose
 * gesture never releases it and whose patch is not oneShot. Its voice can
 * never finish, so the instance is never reclaimed -- the sound plays a few
 * times and then that slot is gone for the session.
 *
 * Returned as a fix list rather than a sentence, because every item is an
 * action the editor can offer as a button.
 */
export function neverEnds (sound, variantIndex = 0) {
  if (sound.loop) return null
  const v = sound.variants?.[variantIndex]
  if (!v || !v.patch) return null
  if (v.patch.oneShot === true) return null

  const held = (v.gesture ?? []).some((g) => (g.durMs ?? 0) > 0)
  if (held) return null

  const layers = v.patch.layers ?? []
  const bad = []
  layers.forEach((l, i) => {
    const sustain = l.ampEnv?.sustain
    if (sustain === undefined || sustain > 0) bad.push(i)
  })
  if (bad.length === 0) return null

  return {
    layers: bad,
    text: bad.length === 1
      ? `layer ${bad[0]} sustains and is never released, so this sound would leak an instance every time it played`
      : `layers ${bad.join(', ')} sustain and are never released, so this sound would leak an instance every time it played`,
    fixes: [
      { label: 'set oneShot on the patch', path: 'patch.oneShot', value: true },
      { label: 'set the amp sustain to 0', path: 'ampEnv.sustain', value: 0 },
      { label: 'give the gesture a hold time', path: 'gesture.durMs', value: 200 },
      { label: 'mark the sound as looping', path: 'loop', value: true },
    ],
  }
}

/** Editor-side checks over one sound, before the engine ever sees it. */
export function inspect (sound, variantIndex = 0) {
  const out = []
  const ends = neverEnds(sound, variantIndex)
  if (ends) out.push({ severity: 'error', field: '', text: ends.text, fix: ends })

  if (!sound.variants || sound.variants.length === 0) {
    out.push({ severity: 'error', field: '', text: 'a sound needs at least one variant' })
  }
  for (const [i, v] of (sound.variants ?? []).entries()) {
    if ((v.gesture?.length ?? 0) > LIMITS.maxGestureSteps) {
      out.push({
        severity: 'warning',
        field: 'gesture',
        text: `variant ${i}: ${v.gesture.length} gesture steps; the engine keeps the first ${LIMITS.maxGestureSteps}`,
      })
    }
    for (const g of v.gesture ?? []) {
      if ((g.note ?? 60) > LIMITS.gestureNoteWarn) {
        out.push({
          severity: 'warning',
          field: 'gesture',
          text: `variant ${i}: note ${g.note} is above the pitch table's ceiling of ${LIMITS.gestureNoteWarn}`,
        })
      }
    }
  }

  // A duck with no duration is discarded entirely by the loader, so "-6 dB"
  // sits in the document looking effective and does nothing.
  if (sound.duckMusic && (sound.duckMusic.db ?? 0) < 0 && !(sound.duckMusic.ms > 0)) {
    out.push({
      severity: 'warning',
      field: 'duckMusic.ms',
      text: 'a duck with no duration is ignored; give it a time or remove the dB',
    })
  }
  return out
}
