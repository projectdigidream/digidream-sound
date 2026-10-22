//
// Layers, rings and timing: EVERY setting a take's patch can hold, with a
// control for each.
//
// Written for sound effects rather than instruments, which is why it replaces
// the vendored midisynth patch editor instead of hosting it -- but it is
// complete: each key in midisynth/docs/PATCH_FORMAT.md sections 3-4 that the
// loader reads has a control here, grouped by what it does to the sound:
//
//   Source             oscillator, noise colour, tone generator and every one
//                      of its parameters -- harmonic amplitudes, drawbars and
//                      Chebyshev weights are bars you drag across, and a
//                      custom wave is a cycle you draw
//   Level and pitch    level, pan, detune, fixed pitch, start delay
//   Loudness           the amp envelope, every stage, curve and rate
//   Tone               both filters, comb and formant, key tracking, drive,
//                      and the filter envelope
//   Pitch / Timbre     the pitch and wavetable-position envelopes
//   Stack and ring     unison and ring / FM
//   Strength, sends    velocity window and curve, release velocity, round
//                      robin, per-layer effect sends
//   LFOs, Modulation   up to four LFOs and twelve routings
//
// The main groups are open; the specialist ones are folded, with a one-line
// summary in their header, so nothing is hidden and nothing is in the way.
// "Everything, as JSON" remains at the bottom for pasting.
//
// A take made from a recipe is rebuilt from its knobs, so a hand edit here
// DETACHES it (the stage's commit() drops `_recipe`), and the card says so
// before you touch anything.
//
import { el, slider, choice, toggle, numberBox, row } from './controls.js'
import { noteName } from './recipes.js'
import { fit } from './waveform.js'

const MAX_LAYERS = 4
const MAX_RINGS = 8
const MAX_LFOS = 4
const MAX_ROUTINGS = 12

/** One colour per layer, used everywhere a layer appears. */
export const LAYER_COLORS = ['#7fd1b9', '#5aa9ff', '#e8b35a', '#d68cf0']

function r (v, d = 2) { return Math.round(v * 10 ** d) / 10 ** d }

const GENERATORS = {
  additiveHarmonicMorph: { label: 'Harmonics (morph)', make: () => ({ startHarmonics: 8, endHarmonics: 2, falloff: 'saw' }) },
  harmonics: { label: 'Drawn harmonics', make: () => ({ amps: [1, 0.5, 0.33, 0.25, 0.2, 0.16, 0.14, 0.12] }) },
  custom: { label: 'Drawn wave', make: () => ({ samples: Array.from({ length: 64 }, (_, i) => r(Math.sin((i / 64) * Math.PI * 2), 3)) }) },
  drawbar: { label: 'Drawbars (organ)', make: () => ({ drawbars: [8, 0, 6, 0, 0, 4, 0, 0, 0] }) },
  pulse: { label: 'Pulse', make: () => ({ startDuty: 0.5, endDuty: 0.15 }) },
  fm2op: { label: 'FM (metallic)', make: () => ({ ratio: 2, startIndex: 4, endIndex: 0.5 }) },
  waveshaper: { label: 'Driven', make: () => ({ shape: 'soft', input: 'sine', startDrive: 1, endDrive: 8 }) },
  chebyshev: { label: 'Chebyshev', make: () => ({ weights: [1, 0, 0.5, 0, 0, 0, 0, 0] }) },
  hardsync: { label: 'Sync', make: () => ({ startRatio: 1, endRatio: 3, slave: 'saw' }) },
}

const FILTER_WORDS = { lowpass: 'low-pass', bandpass: 'band', highpass: 'high-pass', notch: 'notch', comb: 'comb', formant: 'vowel' }
const CURVES = [['linear', 'linear'], ['exp', 'exp'], ['expAttack', 'swell'], ['sCurve', 'S']]
const MOD_SRC = ['LFO1', 'LFO2', 'LFO3', 'LFO4', 'AmpEnv', 'FilterEnv', 'PitchEnv', 'WtPosEnv', 'Velocity', 'KeyTrack', 'ModWheel', 'ChannelAftertouch', 'PolyAftertouch']
const MOD_DST = ['Pitch', 'FilterCutoff', 'FilterRes', 'Amplitude', 'WtPosition', 'Pan', 'FilterDrive', 'PostDrive', 'RingAmount']
const SYNCS = ['', '1/1', '1/2', '1/4', '1/8', '1/16', '1/32', '1/64', '1/4T', '1/8T', '1/16T', '1/4.', '1/8.', '1/16.']

// ---------------------------------------------------------------------------
// Helpers over dotted paths (numeric parts index arrays: `lfos.0.rateHz`).
// ---------------------------------------------------------------------------
function get (o, path, def) {
  let v = o
  for (const p of path.split('.')) { if (v == null || typeof v !== 'object') return def; v = v[p] }
  return v === undefined ? def : v
}
function set (o, path, value) {
  const parts = path.split('.')
  const last = parts.pop()
  let cur = o
  for (const p of parts) { if (cur[p] == null || typeof cur[p] !== 'object') cur[p] = {}; cur = cur[p] }
  if (value === undefined) delete cur[last]; else cur[last] = value
}

const msFmt = (v) => v <= 0 ? '0 ms' : v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${Math.round(v)} ms`
const hzFmt = (v) => v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`
const pct = (v) => `${Math.round(v * 100)}%`
const signed = (v, d = 2) => `${v > 0 ? '+' : ''}${v.toFixed(d)}`

/** A layer in a few words: "Noise (pink) · band 2.2 kHz". */
export function layerSummary (l) {
  const osc = l.osc ?? 'wavetable'
  let src = osc === 'noise' ? `Noise${l.noiseColor && l.noiseColor !== 'white' ? ` (${l.noiseColor})` : ''}`
    : osc === 'metal' ? 'Metal'
      : `Tone${l.wavetable?.generator ? ` · ${GENERATORS[l.wavetable.generator]?.label ?? l.wavetable.generator}` : ''}`
  if (osc !== 'noise' && typeof l.pitchNote === 'number' && l.pitchNote >= 0) src += ` at ${noteName(l.pitchNote)}`
  const f = l.filter
  const filt = f && f.mode !== 'bypass' ? ` · ${FILTER_WORDS[f.mode ?? 'lowpass'] ?? f.mode} ${hzFmt(f.cutoffHz ?? 1000)}` : ''
  const late = l.startDelayMs > 0 ? ` · after ${Math.round(l.startDelayMs)} ms` : ''
  return src + filt + late
}

/**
 * How the parts connect, drawn as what it is: every layer is its own chain
 * (source, then its filter, then its level), they all start together and are
 * ADDED, and the sum goes through the rings, the sound's volume, and out.
 */
export function renderFlow (s, v, onPick) {
  const layers = v.patch.layers ?? []
  const lanes = el('div', { class: 'flow-lanes' }, layers.map((l, i) => el('button', {
    class: 'flow-lane', type: 'button', style: `--c:${LAYER_COLORS[i % 4]}`, onclick: () => onPick?.(i),
    title: 'jump to this layer',
  }, el('span', { class: 'flow-dot' }), el('b', { text: `${i + 1}` }), el('span', { text: layerSummary(l) }),
  el('span', { class: 'flow-lvl', text: `${Math.round((l.mixGain ?? 1) * 100)}%` }))))
  const rings = s.resonators?.length
  return el('div', { class: 'flow' },
    lanes,
    el('div', { class: 'flow-sum', title: 'the layers are added together', text: '+' }),
    el('span', { class: 'flow-arrow', text: '→' }),
    rings ? el('div', { class: 'flow-node', title: 'the mix strikes a body that rings', text: `${rings} ring${rings === 1 ? '' : 's'}` }) : null,
    rings ? el('span', { class: 'flow-arrow', text: '→' }) : null,
    el('div', { class: 'flow-node', text: `volume ${(s.gainDb ?? 0) > 0 ? '+' : ''}${(s.gainDb ?? 0)} dB` }),
    el('span', { class: 'flow-arrow', text: '→' }),
    el('div', { class: 'flow-node out', text: 'game' }))
}

// ---------------------------------------------------------------------------
// Bound controls: a control that reads and writes one path of layer `li`.
// ---------------------------------------------------------------------------
function makeBinder (api, li, layer) {
  const L = (vv) => vv.patch.layers[li]
  return {
    /** A slider. `zeroBelow` lets a log track say 0 (times: 0 = instant). */
    slider (path, label, o) {
      const { def = 0, zeroBelow = 0, round = 2, hint = '' } = o
      const clean = (x) => (zeroBelow && x <= zeroBelow ? 0 : r(x, round))
      return slider({
        label, hint, ...o,
        def: Math.max(o.min, def),
        value: Math.max(o.min, Math.min(o.max, get(layer, path, def))),
        fmt: (x) => o.fmt(clean(x)),
        onInput: (x) => { api.preview((s, vv) => set(L(vv), path, clean(x))); o.onLive?.(clean(x)) },
        onChange: (x) => api.commit(label, (s, vv) => set(L(vv), path, clean(x))),
      })
    },
    /** A segmented choice; `structural` when it changes which controls exist. */
    choice (path, options, def, label, structural = false) {
      return choice(options, get(layer, path, def),
        (x) => api.commit(label, (s, vv) => set(L(vv), path, x), { structural }))
    },
    select (path, options, def, label, structural = false) {
      const cur = get(layer, path, def)
      const sel = el('select', {}, options.map(([v, t]) => el('option', { value: v, text: t, selected: String(cur) === String(v) })))
      sel.onchange = () => api.commit(label, (s, vv) => set(L(vv), path, sel.value === '' ? undefined : sel.value), { structural })
      return sel
    },
    toggle (path, label, def = false, structural = false) {
      return toggle(get(layer, path, def), (on) => api.commit(label, (s, vv) => set(L(vv), path, on === def ? undefined : on), { structural }))
    },
    number (path, o) {
      return numberBox(get(layer, path, o.def ?? ''), { ...o, onChange: (x) => api.commit(o.label ?? path, (s, vv) => set(L(vv), path, x === o.def ? undefined : x)) })
    },
    /** Replace a whole value (turning a feature on or off). */
    put (path, value, label) { api.commit(label, (s, vv) => set(L(vv), path, value), { structural: true }) },
  }
}

/** A folded section inside a layer; remembers open/closed by section kind. */
function sec (ctx, kind, title, summary, body, open) {
  const key = `lsec-${kind}`
  const saved = ctx.ui.load().cards?.[key]
  const d = el('details', { class: 'lsec' }, el('summary', {}, el('span', { class: 'lsec-t', text: title }), summary ? el('span', { class: 'lsec-s', text: summary }) : null), body)
  d.open = saved ?? open
  d.ontoggle = () => ctx.ui.save({ cards: { ...(ctx.ui.load().cards ?? {}), [key]: d.open } })
  return d
}

// ---------------------------------------------------------------------------
// Drawing editors: bars you drag across, and a wave you draw.
// ---------------------------------------------------------------------------

/**
 * Bars: one per value, drag across them to draw. A symmetric range
 * (min < 0) draws from the centre line.
 */
function bars (values, { min = 0, max = 1, labels = null, step = 0, color = '#7fd1b9', onInput, onChange, height = 110 }) {
  const c = el('canvas', { class: 'bars', style: `height:${height}px` })
  let vals = values.slice()
  const draw = () => {
    const { ctx, w, h } = fit(c)
    ctx.clearRect(0, 0, w, h)
    const n = vals.length
    const bw = w / n
    const zero = min < 0 ? h / 2 : h - 14
    const scale = min < 0 ? (h / 2 - 16) : (h - 18)
    ctx.strokeStyle = '#2e343f'
    ctx.beginPath(); ctx.moveTo(0, zero); ctx.lineTo(w, zero); ctx.stroke()
    for (let i = 0; i < n; i++) {
      const f = min < 0 ? vals[i] / Math.max(-min, max) : (vals[i] - min) / (max - min)
      ctx.fillStyle = color
      const y = zero - f * scale
      ctx.fillRect(i * bw + 1, Math.min(y, zero), Math.max(1, bw - 2), Math.abs(zero - y) || 1)
      if (labels && bw > 12) {
        ctx.fillStyle = '#666d7b'
        ctx.font = '9px ui-monospace, monospace'
        ctx.fillText(String(labels[i]), i * bw + 2, h - 3)
      }
    }
  }
  let drag = false
  let last = -1
  const at = (e) => {
    const b = c.getBoundingClientRect()
    const i = Math.max(0, Math.min(vals.length - 1, Math.floor(((e.clientX - b.left) / b.width) * vals.length)))
    const yy = e.clientY - b.top
    const h = b.height
    let v = min < 0
      ? ((h / 2 - yy) / (h / 2 - 16)) * Math.max(-min, max)
      : min + ((h - 14 - yy) / (h - 18)) * (max - min)
    v = Math.max(min, Math.min(max, v))
    if (step) v = Math.round(v / step) * step
    return [i, r(v, 3)]
  }
  const paint = (e) => {
    const [i, v] = at(e)
    // Fill every bar between the last one and this one, so a fast stroke
    // leaves no gaps.
    const from = last < 0 ? i : last
    for (let k = Math.min(from, i); k <= Math.max(from, i); k++) vals[k] = v
    last = i
    draw()
    onInput?.(vals.slice())
  }
  c.onpointerdown = (e) => { drag = true; last = -1; c.setPointerCapture(e.pointerId); paint(e) }
  c.onpointermove = (e) => { if (drag) paint(e) }
  c.onpointerup = () => { if (!drag) return; drag = false; onChange?.(vals.slice()) }
  c.onpointercancel = () => { drag = false }
  new ResizeObserver(() => { if (c.clientWidth) draw() }).observe(c)
  return c
}

/** One cycle of a wave, drawn with the pointer. */
function waveDraw (samples, { onInput, onChange, color = '#7fd1b9' }) {
  const c = el('canvas', { class: 'bars', style: 'height:120px; cursor:crosshair' })
  const vals = samples.slice()
  const draw = () => {
    const { ctx, w, h } = fit(c)
    ctx.clearRect(0, 0, w, h)
    ctx.strokeStyle = '#2e343f'
    ctx.beginPath(); ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2); ctx.stroke()
    ctx.strokeStyle = color
    ctx.lineWidth = 2
    ctx.beginPath()
    vals.forEach((v, i) => {
      const x = (i / (vals.length - 1)) * w
      const y = h / 2 - v * (h / 2 - 6)
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }
  let drag = false
  let last = -1
  let lastV = 0
  const paint = (e) => {
    const b = c.getBoundingClientRect()
    const i = Math.max(0, Math.min(vals.length - 1, Math.round(((e.clientX - b.left) / b.width) * (vals.length - 1))))
    const v = r(Math.max(-1, Math.min(1, (b.height / 2 - (e.clientY - b.top)) / (b.height / 2 - 6))), 3)
    if (last < 0 || last === i) vals[i] = v
    else {
      // Interpolate between the previous point and this one.
      for (let k = Math.min(last, i); k <= Math.max(last, i); k++) {
        const t = (k - last) / (i - last)
        vals[k] = r(lastV + (v - lastV) * t, 3)
      }
    }
    last = i
    lastV = v
    draw()
    onInput?.(vals.slice())
  }
  c.onpointerdown = (e) => { drag = true; last = -1; c.setPointerCapture(e.pointerId); paint(e) }
  c.onpointermove = (e) => { if (drag) paint(e) }
  c.onpointerup = () => { if (!drag) return; drag = false; onChange?.(vals.slice()) }
  c.onpointercancel = () => { drag = false }
  new ResizeObserver(() => { if (c.clientWidth) draw() }).observe(c)
  return c
}

const SHAPES = {
  sine: (n) => Array.from({ length: n }, (_, i) => r(Math.sin((i / n) * Math.PI * 2), 3)),
  triangle: (n) => Array.from({ length: n }, (_, i) => { const t = i / n; return r(t < 0.25 ? 4 * t : t < 0.75 ? 2 - 4 * t : 4 * t - 4, 3) }),
  saw: (n) => Array.from({ length: n }, (_, i) => r(1 - 2 * (i / n), 3)),
  square: (n) => Array.from({ length: n }, (_, i) => (i < n / 2 ? 1 : -1)),
}

// ---------------------------------------------------------------------------
// The tone generators, every parameter.
// ---------------------------------------------------------------------------
function generatorEditor (api, li, layer, color) {
  const B = makeBinder(api, li, layer)
  const g = layer.wavetable?.generator ?? 'sine'
  const P = (k) => `wavetable.params.${k}`
  const p = (k, def) => get(layer, P(k), def)
  const box = el('div', {})
  const grid = el('div', { class: 'sliders' })
  const L = (vv) => vv.patch.layers[li]

  // A pair of arrays: the start, and an optional end the tone morphs to.
  const arrayPair = (key, endKey, title, o, make) => {
    const start = p(key, make())
    const hasEnd = Array.isArray(p(endKey))
    const live = (k) => (vals) => api.preview((s, vv) => set(L(vv), P(k), vals))
    const done = (k, label) => (vals) => api.commit(label, (s, vv) => set(L(vv), P(k), vals))
    box.append(el('div', { class: 'sub', text: `${title}${hasEnd ? ' — start' : ''}` }),
      bars(start, { ...o, color, onInput: live(key), onChange: done(key, title) }))
    if (hasEnd) {
      box.append(el('div', { class: 'sub', text: `${title} — end (the tone morphs to this)` }),
        bars(p(endKey), { ...o, color, onInput: live(endKey), onChange: done(endKey, `${title} end`) }))
    }
    box.append(el('div', { class: 'gen-row' },
      el('button', { text: hasEnd ? 'Remove the end shape' : '＋ Morph to a second shape', onclick: () =>
        B.put(P(endKey), hasEnd ? undefined : start.slice(), 'morph target') }),
      el('span', { class: 'hint', text: 'drag across the bars to draw' })))
  }

  if (g === 'additiveHarmonicMorph') {
    grid.append(B.slider(P('startHarmonics'), 'bright start', { min: 1, max: 256, log: true, step: 1, def: 1, round: 0, fmt: (x) => `${x} harmonics` }))
    grid.append(B.slider(P('endHarmonics'), 'bright end', { min: 1, max: 256, log: true, step: 1, def: 8, round: 0, fmt: (x) => `${x} harmonics` }))
    grid.append(row('shape', B.choice(P('falloff'), [['saw', 'saw'], ['square', 'square'], ['triangle', 'triangle'], ['flat', 'flat']], 'saw', 'falloff')))
  } else if (g === 'harmonics') {
    const amps = p('amps', [1])
    box.append(row('how many harmonics', numberBox(amps.length, { min: 1, max: 64, onChange: (n) => n && api.commit('harmonic count', (s, vv) => {
      const resize = (a) => Array.from({ length: n }, (_, i) => a?.[i] ?? 0)
      set(L(vv), P('amps'), resize(get(L(vv), P('amps'), [1])))
      if (Array.isArray(get(L(vv), P('endAmps')))) set(L(vv), P('endAmps'), resize(get(L(vv), P('endAmps'))))
    }, { structural: true }) }), 'bar 1 is the fundamental, bar 2 an octave up, bar 3 an octave and a fifth…'))
    arrayPair('amps', 'endAmps', 'Harmonic levels', { min: 0, max: 1, labels: amps.map((_, i) => i + 1) }, () => [1])
  } else if (g === 'drawbar') {
    arrayPair('drawbars', 'endDrawbars', 'Drawbars', { min: 0, max: 8, step: 1, labels: ['1', '2', '3', '4', '5', '6', '8', '10', '12'] }, () => [8, 0, 6, 0, 0, 4, 0, 0, 0])
    box.append(el('div', { class: 'hint', text: 'Levels 0–8 for harmonics 1, 2, 3, 4, 5, 6, 8, 10 and 12, like an organ’s drawbars.' }))
  } else if (g === 'chebyshev') {
    arrayPair('weights', 'endWeights', 'Harmonic weights', { min: -1, max: 1, labels: ['T1', 'T2', 'T3', 'T4', 'T5', 'T6', 'T7', 'T8'] }, () => [1, 0, 0, 0, 0, 0, 0, 0])
    grid.append(B.slider(P('startDrive'), 'drive start', { min: 0, max: 1, def: 1, fmt: pct }))
    grid.append(B.slider(P('endDrive'), 'drive end', { min: 0, max: 1, def: p('startDrive', 1), fmt: pct }))
  } else if (g === 'custom') {
    const n = p('samples', SHAPES.sine(64)).length
    const hasEnd = Array.isArray(p('endSamples'))
    box.append(el('div', { class: 'sub', text: hasEnd ? 'The wave — start' : 'The wave: draw one cycle' }),
      waveDraw(p('samples', SHAPES.sine(64)), { color,
        onInput: (vals) => api.preview((s, vv) => set(L(vv), P('samples'), vals)),
        onChange: (vals) => api.commit('draw wave', (s, vv) => set(L(vv), P('samples'), vals)) }))
    if (hasEnd) {
      box.append(el('div', { class: 'sub', text: 'The wave — end (the tone morphs to this)' }),
        waveDraw(p('endSamples'), { color,
          onInput: (vals) => api.preview((s, vv) => set(L(vv), P('endSamples'), vals)),
          onChange: (vals) => api.commit('draw end wave', (s, vv) => set(L(vv), P('endSamples'), vals)) }))
    }
    box.append(el('div', { class: 'gen-row' }, el('span', { class: 'hint', text: 'start from' }),
      ...Object.keys(SHAPES).map((k) => el('button', { text: k, onclick: () => B.put(P('samples'), SHAPES[k](n), `draw ${k}`) })),
      el('span', { class: 'hint', text: 'points' }),
      numberBox(n, { min: 8, max: 512, onChange: (m) => m && api.commit('wave points', (s, vv) => {
        const rs = (a) => Array.from({ length: m }, (_, i) => a[Math.floor((i / m) * a.length)])
        set(L(vv), P('samples'), rs(get(L(vv), P('samples'), SHAPES.sine(64))))
        if (Array.isArray(get(L(vv), P('endSamples')))) set(L(vv), P('endSamples'), rs(get(L(vv), P('endSamples'))))
      }, { structural: true }) })))
    box.append(el('div', { class: 'gen-row' }, el('button', { text: hasEnd ? 'Remove the end wave' : '＋ Morph to a second wave',
      onclick: () => B.put(P('endSamples'), hasEnd ? undefined : p('samples', SHAPES.sine(64)).slice(), 'morph target') })))
  } else if (g === 'pulse') {
    grid.append(B.slider(P('startDuty'), 'width start', { min: 0.02, max: 0.98, def: 0.5, fmt: pct }))
    grid.append(B.slider(P('endDuty'), 'width end', { min: 0.02, max: 0.98, def: 0.15, fmt: pct }))
  } else if (g === 'fm2op') {
    grid.append(B.slider(P('ratio'), 'metal ratio', { min: 1, max: 16, step: 1, def: 2, round: 0, fmt: (x) => `×${x}` }))
    grid.append(B.slider(P('startIndex'), 'bite start', { min: 0, max: 12, def: 0.5, fmt: (x) => x.toFixed(1) }))
    grid.append(B.slider(P('endIndex'), 'bite end', { min: 0, max: 12, def: 4, fmt: (x) => x.toFixed(1) }))
  } else if (g === 'waveshaper') {
    grid.append(row('curve', B.choice(P('shape'), [['soft', 'soft'], ['hard', 'hard'], ['fold', 'fold'], ['crush', 'crush'], ['stairs', 'stairs']], 'soft', 'shaper curve', true)))
    grid.append(row('input', B.choice(P('input'), [['sine', 'sine'], ['triangle', 'triangle'], ['saw', 'saw']], 'sine', 'shaper input')))
    grid.append(B.slider(P('startDrive'), 'grit start', { min: 0.25, max: 64, log: true, def: 1, fmt: (x) => x.toFixed(1) }))
    grid.append(B.slider(P('endDrive'), 'grit end', { min: 0.25, max: 64, log: true, def: 8, fmt: (x) => x.toFixed(1) }))
    grid.append(B.slider(P('bias'), 'bias', { min: -1, max: 1, def: 0, fmt: (x) => signed(x), hint: 'off-centre drive: adds even harmonics' }))
    if (p('shape', 'soft') === 'crush') grid.append(B.slider(P('levels'), 'levels', { min: 2, max: 32, step: 1, def: 6, round: 0, fmt: (x) => String(x) }))
    if (p('shape', 'soft') === 'stairs') grid.append(B.slider(P('steps'), 'steps', { min: 2, max: 64, step: 1, def: 16, round: 0, fmt: (x) => String(x) }))
  } else if (g === 'hardsync') {
    grid.append(B.slider(P('startRatio'), 'sync start', { min: 1, max: 16, def: 1, fmt: (x) => `×${x.toFixed(1)}` }))
    grid.append(B.slider(P('endRatio'), 'sync end', { min: 1, max: 16, def: 3, fmt: (x) => `×${x.toFixed(1)}` }))
    grid.append(row('wave', B.choice(P('slave'), [['saw', 'saw'], ['pulse', 'pulse']], 'saw', 'sync wave')))
  }
  if (g !== 'sine') {
    grid.append(B.slider('wavetable.frameCount', 'morph frames', { min: 1, max: 32, step: 1, def: 1, round: 0,
      fmt: (x) => x === 1 ? 'static' : String(x), hint: 'how many steps between the start and end tone; 1 = a fixed wave' }))
  }
  box.append(grid)
  return box
}

// ---------------------------------------------------------------------------
// An envelope: every stage, curve, rate, loop and key scaling.
// ---------------------------------------------------------------------------
function drawEnv (canvas, env = {}, oneShot = false, isAmp = true) {
  const { ctx, w, h } = fit(canvas)
  ctx.clearRect(0, 0, w, h)
  const dl = env.delay ?? 0
  const a = env.attack ?? (isAmp ? 1.3 : 0)
  const hd = env.hold ?? 0
  const d = env.decay ?? 0
  const s = oneShot && isAmp ? 0 : (env.sustain ?? 1)
  const rl = env.release ?? (isAmp ? 50 : 0)
  const held = s > 0 ? Math.max(60, (a + d + rl) * 0.4) : 0
  const total = Math.max(1, dl + a + hd + d + held + rl)
  const X = (t) => 4 + (t / total) * (w - 8)
  const Y = (v) => h - 4 - v * (h - 10)
  ctx.strokeStyle = '#7fd1b9'
  ctx.fillStyle = '#7fd1b91c'
  ctx.lineWidth = 2
  ctx.beginPath()
  ctx.moveTo(X(0), Y(0))
  ctx.lineTo(X(dl), Y(0))
  ctx.lineTo(X(dl + a), Y(1))
  ctx.lineTo(X(dl + a + hd), Y(1))
  const n = 24
  for (let i = 1; i <= n; i++) { const t = i / n; ctx.lineTo(X(dl + a + hd + d * t), Y(s + (1 - s) * Math.exp(-5 * t) * (1 - t))) }
  if (s > 0) ctx.lineTo(X(dl + a + hd + d + held), Y(s))
  for (let i = 1; i <= n; i++) { const t = i / n; ctx.lineTo(X(dl + a + hd + d + held + rl * t), Y(s * Math.exp(-5 * t) * (1 - t))) }
  ctx.stroke()
  ctx.lineTo(X(total), Y(0))
  ctx.closePath()
  ctx.fill()
  ctx.fillStyle = '#666d7b'
  ctx.font = '10px ui-monospace, monospace'
  const label = `${Math.round(dl + a + hd + d + rl)} ms${s > 0 ? ' + held' : ''}${env.loop ? ' · loops' : ''}`
  ctx.fillText(label, w - ctx.measureText(label).width - 6, 12)
}

function envEditor (ctx, api, li, layer, key, { isAmp = false, oneShot = false, depth = null }) {
  const B = makeBinder(api, li, layer)
  const env = layer[key] ?? {}
  const box = el('div', {})
  const canvas = el('canvas', { class: 'env' })
  const live = { ...env }
  const redraw = () => drawEnv(canvas, live, oneShot, isAmp)
  new ResizeObserver(() => { if (canvas.clientWidth) redraw() }).observe(canvas)
  const t = (k, label, def) => B.slider(`${key}.${k}`, label, { min: 1, max: 30000, log: true, zeroBelow: 1.05, def, fmt: msFmt, onLive: (x) => { live[k] = x; redraw() } })
  const grid = el('div', { class: 'sliders' })
  if (depth) grid.append(B.slider(`${key}.${depth.key}`, depth.label, depth))
  grid.append(t('delay', 'wait', 0), t('attack', 'rise', isAmp ? 1.3 : 0), t('hold', 'hold at top', 0), t('decay', 'fall', 0))
  if (!(isAmp && oneShot)) grid.append(B.slider(`${key}.sustain`, 'stay at', { min: 0, max: 1, def: 1, fmt: pct, onLive: (x) => { live.sustain = x; redraw() } }))
  grid.append(t('release', 'fade out', isAmp ? 50 : 0))
  box.append(canvas, grid)

  const more = el('div', { class: 'sliders' })
  more.append(row('rise shape', B.choice(`${key}.attackCurve`, CURVES, 'linear', 'rise curve')))
  more.append(row('fall shape', B.choice(`${key}.decayCurve`, CURVES, 'exp', 'fall curve')))
  more.append(row('fade shape', B.choice(`${key}.releaseCurve`, CURVES, 'exp', 'fade curve')))
  more.append(row('loop rise and fall', B.toggle(`${key}.loop`, 'envelope loop'), 'repeats while the note is held'))
  more.append(B.slider(`${key}.keyScaleRate`, 'faster when higher', { min: -1, max: 1, def: 0, fmt: (x) => signed(x), hint: 'stages shorten (or lengthen) as the pitch goes up' }))
  for (const [k, label] of [['attackRate', 'rise rate'], ['decayRate', 'fall rate'], ['releaseRate', 'fade rate']]) {
    more.append(row(`${label} (0–99)`, B.number(`${key}.${k}`, { min: 0, max: 99, label }), 'an alternative to the time above; a time, if set, wins'))
  }
  box.append(sec(ctx, `env-more-${key}`, 'Shapes, loop and rates', '', more, false))
  return box
}

// ---------------------------------------------------------------------------
// One layer.
// ---------------------------------------------------------------------------
function layerCard (ctx, s, v, li, api) {
  const layer = v.patch.layers[li]
  const osc = layer.osc ?? 'wavetable'
  const n = v.patch.layers.length
  const color = LAYER_COLORS[li % 4]
  const B = makeBinder(api, li, layer)
  const L = (vv) => vv.patch.layers[li]

  const card = el('div', { class: 'layer', id: `layer-${li}`, style: `--c:${color}` },
    el('div', { class: 'layer-head' },
      el('span', { class: 'flow-dot', style: `--c:${color}` }),
      el('span', { class: 'ln', text: `Layer ${li + 1}` }),
      el('span', { class: 'hint', text: layerSummary(layer) }),
      el('span', { class: 'sp' }),
      el('button', { class: 'ghost', text: 'Duplicate', disabled: n >= MAX_LAYERS, title: n >= MAX_LAYERS ? 'four layers is the most a patch can hold' : '',
        onclick: () => api.commit('duplicate layer', (ss, vv) => { vv.patch.layers.splice(li + 1, 0, structuredClone(vv.patch.layers[li])) }, { structural: true }) }),
      el('button', { class: 'ghost danger', text: 'Remove', disabled: n <= 1,
        onclick: () => api.commit('remove layer', (ss, vv) => { vv.patch.layers.splice(li, 1) }, { structural: true }) })))

  // ---- source ----
  const src = el('div', {})
  src.append(row('source', choice([['noise', 'Noise'], ['wavetable', 'Tone'], ['metal', 'Metal']], osc, (x) => api.commit('layer source', (ss, vv) => {
    const l = L(vv)
    if (x === 'wavetable') { delete l.osc; if (!l.wavetable) l.wavetable = { generator: 'additiveHarmonicMorph', frameCount: 8, params: GENERATORS.additiveHarmonicMorph.make() } }
    else { l.osc = x; delete l.wavetable }
    if (x !== 'noise') delete l.noiseColor
  }, { structural: true })), osc === 'metal' ? 'six detuned squares at cymbal ratios — band-pass it around 4–8 kHz for hats' : ''))
  if (osc === 'noise') {
    src.append(row('colour', B.choice('noiseColor', [['white', 'White (hiss)'], ['pink', 'Pink (wash)'], ['brown', 'Brown (rumble)']], 'white', 'noise colour')))
  } else if (osc === 'wavetable') {
    const g = layer.wavetable?.generator ?? 'sine'
    const sel = el('select', {}, ...['sine', ...Object.keys(GENERATORS)].map((k) =>
      el('option', { value: k, text: k === 'sine' ? 'Plain sine' : GENERATORS[k].label, selected: k === g })))
    sel.onchange = () => api.commit('tone generator', (ss, vv) => {
      const l = L(vv)
      if (sel.value === 'sine') delete l.wavetable
      else l.wavetable = { generator: sel.value, frameCount: l.wavetable?.frameCount ?? 8, params: GENERATORS[sel.value].make() }
    }, { structural: true })
    src.append(row('tone', sel))
    src.append(generatorEditor(api, li, layer, color))
  }
  card.append(sec(ctx, 'source', 'Source', '', src, true))

  // ---- level and pitch ----
  const lp = el('div', { class: 'sliders' })
  lp.append(B.slider('mixGain', 'level', { min: 0, max: 1, def: 1, fmt: pct }))
  lp.append(B.slider('pan', 'pan', { min: -1, max: 1, def: 0, fmt: (x) => x === 0 ? 'centre' : x < 0 ? `L ${Math.round(-x * 100)}` : `R ${Math.round(x * 100)}` }))
  lp.append(B.slider('startDelayMs', 'starts after', { min: 1, max: 2000, log: true, zeroBelow: 1.05, def: 0, fmt: msFmt }))
  if (osc !== 'noise') {
    const fixed = typeof layer.pitchNote === 'number' && layer.pitchNote >= 0
    lp.append(row('fixed pitch', toggle(fixed, (on) => api.commit('fixed pitch', (ss, vv) => {
      if (on) L(vv).pitchNote = 60; else delete L(vv).pitchNote
    }, { structural: true })), fixed ? '' : 'follows the notes in Timing'))
    if (fixed) lp.append(B.slider('pitchNote', 'pitch', { min: 0, max: 127, step: 0.5, def: 60, round: 1, fmt: (x) => noteName(x) }))
    lp.append(B.slider('detuneCents', 'detune', { min: -1200, max: 1200, step: 1, def: 0, round: 0, fmt: (x) => `${x > 0 ? '+' : ''}${x}¢` }))
  }
  card.append(sec(ctx, 'level', 'Level and pitch', '', lp, true))

  // ---- loudness ----
  card.append(sec(ctx, 'amp', 'Loudness over time', '', envEditor(ctx, api, li, layer, 'ampEnv', { isAmp: true, oneShot: !!v.patch.oneShot }), true))

  // ---- tone ----
  const tone = el('div', {})
  const mode = layer.filter ? (layer.filter.mode ?? 'lowpass') : 'off'
  const MODES = [['off', 'Off'], ['lowpass', 'Low-pass'], ['bandpass', 'Band'], ['highpass', 'High-pass'], ['notch', 'Notch'], ['comb', 'Comb'], ['formant', 'Vowel']]
  if (!MODES.some(([m]) => m === mode)) MODES.push([mode, mode])
  tone.append(row('filter', choice(MODES, mode, (x) => api.commit('filter mode', (ss, vv) => {
    const l = L(vv)
    if (x === 'off') { delete l.filter; delete l.filterEnv; delete l.filter2; delete l.filterRouting } else l.filter = { cutoffHz: 1000, resonance: 0.2, ...(l.filter ?? {}), mode: x }
  }, { structural: true })), mode === 'comb' ? 'a tuned echo: rings at the frequency and its harmonics' : mode === 'formant' ? 'three peaks at a vowel’s formants' : ''))
  if (layer.filter) {
    const f = el('div', { class: 'sliders' })
    f.append(B.slider('filter.cutoffHz', mode === 'comb' ? 'ring pitch' : mode === 'formant' ? 'voice size' : 'frequency', { min: 20, max: 12000, log: true, def: 1000, round: 0, fmt: hzFmt }))
    f.append(B.slider('filter.resonance', mode === 'comb' ? 'feedback' : 'resonance', { min: 0, max: 0.95, def: mode === 'formant' ? 0.85 : 0, fmt: pct }))
    if (mode === 'formant') {
      const vowels = [['a', 'a'], ['e', 'e'], ['i', 'i'], ['o', 'o'], ['u', 'u']]
      f.append(row('vowel', B.choice('filter.vowel', vowels, 'a', 'vowel')))
      f.append(row('moves to vowel', B.select('filter.endVowel', [['', 'none'], ...vowels], '', 'end vowel'), 'the filter sweep morphs between them'))
    } else if (mode !== 'comb') {
      f.append(row('steepness', B.choice('filter.slope', [[12, 'gentle'], [24, 'steep']], 12, 'filter slope')))
    }
    f.append(B.slider('filter.drive', 'grit before', { min: 1, max: 8, def: 1, fmt: (x) => x <= 1 ? 'clean' : `×${x.toFixed(1)}` }))
    f.append(B.slider('keyTrackFilter', 'follows pitch', { min: 0, max: 1, def: 0, fmt: pct, hint: 'the filter moves with the note, keeping brightness constant' }))
    tone.append(f)

    // The second filter (not with a formant, which uses every stage).
    if (mode !== 'formant') {
      const f2 = el('div', { class: 'sliders' })
      const m2 = layer.filter2 ? (layer.filter2.mode ?? 'lowpass') : 'off'
      f2.append(row('second filter', choice([['off', 'Off'], ['lowpass', 'Low'], ['bandpass', 'Band'], ['highpass', 'High'], ['notch', 'Notch']], m2, (x) => api.commit('second filter', (ss, vv) => {
        const l = L(vv)
        if (x === 'off') { delete l.filter2; delete l.filterRouting } else l.filter2 = { cutoffHz: 1000, resonance: 0, ...(l.filter2 ?? {}), mode: x }
      }, { structural: true }))))
      if (layer.filter2) {
        f2.append(B.slider('filter2.cutoffHz', 'frequency', { min: 20, max: 12000, log: true, def: 1000, round: 0, fmt: hzFmt }))
        f2.append(B.slider('filter2.resonance', 'resonance', { min: 0, max: 0.95, def: 0, fmt: pct }))
        f2.append(row('stays put', B.toggle('filter2.static', 'second filter static'), 'ignores the sweep and all modulation, like a fixed cabinet filter'))
        f2.append(row('with the first', B.choice('filterRouting', [['serial', 'after it'], ['parallel', 'beside it']], 'serial', 'filter routing')))
      }
      tone.append(f2)
    }
    tone.append(el('div', { class: 'sub', text: 'Filter sweep' }))
    if (layer.filterEnv) {
      tone.append(envEditor(ctx, api, li, layer, 'filterEnv', { depth: { key: 'depth', label: 'sweep amount', min: -1, max: 1, def: 0.5, fmt: (x) => `${x > 0 ? '+' : ''}${Math.round(x * 100)}%` } }))
      tone.append(el('button', { class: 'ghost danger', text: 'Remove the sweep', onclick: () => B.put('filterEnv', undefined, 'remove filter sweep') }))
    } else {
      tone.append(el('button', { text: '＋ Add a filter sweep', onclick: () => B.put('filterEnv', { attack: 0, decay: 150, sustain: 0, depth: 0.5 }, 'add filter sweep') }))
    }
  }
  tone.append(el('div', { class: 'sliders', style: 'margin-top:8px' }, B.slider('postDrive', 'distortion after', { min: 1, max: 8, def: 1, fmt: (x) => x <= 1 ? 'none' : `×${x.toFixed(1)}` })))
  card.append(sec(ctx, 'tone', 'Tone', layer.filter ? FILTER_WORDS[mode] ?? mode : 'no filter', tone, true))

  // ---- pitch and timbre envelopes ----
  const envToggle = (key, title, fresh, depth) => {
    const b = el('div', {})
    if (layer[key]) {
      b.append(envEditor(ctx, api, li, layer, key, { depth }))
      b.append(el('button', { class: 'ghost danger', text: `Remove the ${title.toLowerCase()}`, onclick: () => B.put(key, undefined, `remove ${title}`) }))
    } else {
      b.append(el('button', { text: `＋ Add a ${title.toLowerCase()}`, onclick: () => B.put(key, fresh, `add ${title}`) }))
    }
    return b
  }
  if (osc !== 'noise') {
    card.append(sec(ctx, 'pitchenv', 'Pitch sweep', layer.pitchEnv ? `${signed(layer.pitchEnv.depthSemis ?? 0, 1)} st` : 'none',
      envToggle('pitchEnv', 'Pitch sweep', { attack: 0, decay: 80, sustain: 0, depthSemis: 12 },
        { key: 'depthSemis', label: 'starts off by', min: -48, max: 48, step: 0.5, def: 0, round: 1, fmt: (x) => `${x > 0 ? '+' : ''}${x} st` }), !!layer.pitchEnv))
  }
  if (osc === 'wavetable' && layer.wavetable) {
    card.append(sec(ctx, 'wtenv', 'Timbre sweep', layer.wtPosEnv ? pct(layer.wtPosEnv.depth ?? 1) : 'none',
      envToggle('wtPosEnv', 'Timbre sweep', { attack: 0, decay: 200, sustain: 0, depth: 1 },
        { key: 'depth', label: 'how far', min: 0, max: 1, def: 1, fmt: pct }), !!layer.wtPosEnv))
  }

  // ---- stack and ring ----
  const st = el('div', { class: 'sliders' })
  const un = layer.unison
  st.append(row('stack of voices', toggle(!!un, (on) => B.put('unison', on ? { voices: 3, detuneCents: 12, spread: 0.5 } : undefined, 'unison')), 'several detuned copies, for thickness'))
  if (un) {
    st.append(B.slider('unison.voices', 'voices', { min: 1, max: 7, step: 1, def: 1, round: 0, fmt: (x) => String(x) }))
    st.append(B.slider('unison.detuneCents', 'spread in pitch', { min: 0, max: 100, step: 0.5, def: 7, round: 1, fmt: (x) => `${x}¢` }))
    st.append(B.slider('unison.spread', 'spread in space', { min: 0, max: 1, def: 0.5, fmt: pct }))
    st.append(B.slider('unison.phaseSpread', 'start apart', { min: 0, max: 1, def: 0, fmt: pct, hint: 'begin the copies at different points, so they beat from the start' }))
  }
  const rg = layer.ring
  const rmode = rg ? (rg.mode ?? 'ring') : 'off'
  st.append(row('ring / FM', choice([['off', 'Off'], ['ring', 'Ring'], ['fm', 'FM']], rmode, (x) => B.put('ring', x === 'off' ? undefined
    : { ratio: rg?.ratio ?? 2, ...(x === 'fm' ? { mode: 'fm', index: rg?.index ?? 2 } : { amount: rg?.amount ?? 0.5 }) }, 'ring mode')),
  'multiplies (ring) or bends (FM) the tone by a second, hidden sine'))
  if (rg) {
    st.append(B.slider('ring.ratio', 'second sine at', { min: 0.25, max: 16, log: true, def: 1, fmt: (x) => `×${x.toFixed(2)}` }))
    if (rmode === 'fm') st.append(B.slider('ring.index', 'depth', { min: 0, max: 12, def: 2, fmt: (x) => x.toFixed(1) }))
    else st.append(B.slider('ring.amount', 'depth', { min: 0, max: 1, def: 1, fmt: pct }))
  }
  card.append(sec(ctx, 'stack', 'Stack and ring', [un ? `${un.voices ?? 1} voices` : '', rg ? rmode : ''].filter(Boolean).join(' · ') || 'off', st, false))

  // ---- strength, turns, sends ----
  const vr = el('div', { class: 'sliders' })
  vr.append(B.slider('velLow', 'plays from strength', { min: 0, max: 127, step: 1, def: 0, round: 0, fmt: (x) => String(x), hint: 'the layer only sounds for note strengths in this window (Timing sets the strength)' }))
  vr.append(B.slider('velHigh', 'up to strength', { min: 0, max: 127, step: 1, def: 127, round: 0, fmt: (x) => String(x) }))
  vr.append(row('strength → level', B.choice('velCurve', [['squared', 'expressive'], ['soft', 'soft'], ['linear', 'flat']], 'squared', 'velocity curve')))
  vr.append(B.slider('releaseVel', 'release follows lift', { min: 0, max: 1, def: 0, fmt: pct }))
  vr.append(B.slider('rrGroup', 'take turns (group)', { min: 0, max: 15, step: 1, def: 0, round: 0, fmt: (x) => x === 0 ? 'off' : `group ${x}`, hint: 'layers in the same group alternate, one per play' }))
  for (const [k, label] of [['sendReverb', 'room send'], ['sendChorus', 'chorus send'], ['sendDelay', 'echo send']]) {
    const has = typeof layer[k] === 'number'
    vr.append(row(`own ${label}`, toggle(has, (on) => B.put(k, on ? 0.3 : undefined, label)), has ? '' : 'uses its group’s send'))
    if (has) vr.append(B.slider(k, label, { min: 0, max: 1, def: 0, fmt: pct }))
  }
  card.append(sec(ctx, 'vel', 'Strength, turns and sends', '', vr, false))

  // ---- LFOs ----
  const lfos = layer.lfos ?? []
  const lb = el('div', {})
  lfos.forEach((lf, k) => {
    const g = el('div', { class: 'sliders', style: 'margin-bottom:8px' })
    g.append(el('div', { class: 'group-label', text: `LFO ${k + 1}` }))
    g.append(row('shape', B.choice(`lfos.${k}.shape`, [['sine', 'sine'], ['triangle', 'tri'], ['saw', 'saw'], ['square', 'square'], ['sampleHold', 'random']], 'sine', 'lfo shape')))
    g.append(B.slider(`lfos.${k}.rateHz`, 'speed', { min: 0.05, max: 40, log: true, def: 5, fmt: (x) => `${x.toFixed(2)} Hz` }))
    g.append(B.slider(`lfos.${k}.delayMs`, 'starts after', { min: 1, max: 5000, log: true, zeroBelow: 1.05, def: 0, fmt: msFmt }))
    g.append(row('restarts each play', B.toggle(`lfos.${k}.retrigger`, 'lfo retrigger', true)))
    g.append(row('tempo sync', B.select(`lfos.${k}.sync`, SYNCS.map((x) => [x, x || 'off']), '', 'lfo sync')))
    g.append(el('div', {}, el('button', { class: 'ghost danger', text: 'Remove LFO', onclick: () => api.commit('remove lfo', (ss, vv) => {
      L(vv).lfos.splice(k, 1); if (!L(vv).lfos.length) delete L(vv).lfos
    }, { structural: true }) })))
    lb.append(g)
  })
  lb.append(el('p', { class: 'note', text: 'An LFO does nothing on its own: route it in Modulation below.' }))
  if (lfos.length < MAX_LFOS) lb.append(el('button', { text: '＋ Add an LFO', onclick: () => api.commit('add lfo', (ss, vv) => { L(vv).lfos = [...(L(vv).lfos ?? []), { shape: 'sine', rateHz: 5 }] }, { structural: true }) }))
  card.append(sec(ctx, 'lfo', 'LFOs', lfos.length ? `${lfos.length}` : 'none', lb, false))

  // ---- modulation ----
  const mm = layer.modMatrix ?? []
  const mb = el('div', {})
  mm.forEach((m, k) => {
    const srcSel = el('select', {}, MOD_SRC.map((x) => el('option', { value: x, text: x, selected: x === m.src })))
    const dstSel = el('select', {}, MOD_DST.map((x) => el('option', { value: x, text: x, selected: x === m.dst })))
    srcSel.onchange = () => api.commit('mod source', (ss, vv) => { L(vv).modMatrix[k].src = srcSel.value })
    dstSel.onchange = () => api.commit('mod target', (ss, vv) => { L(vv).modMatrix[k].dst = dstSel.value })
    mb.append(el('div', { class: 'step' }, srcSel, el('span', { class: 'hint', text: '→' }), dstSel,
      el('div', { style: 'flex:1; min-width:220px' }, B.slider(`modMatrix.${k}.amount`, 'amount', { min: -1, max: 1, step: 0.001, def: 0, round: 3, fmt: (x) => signed(x, 3) })),
      el('button', { class: 'ghost danger', text: '×', onclick: () => api.commit('remove routing', (ss, vv) => {
        L(vv).modMatrix.splice(k, 1); if (!L(vv).modMatrix.length) delete L(vv).modMatrix
      }, { structural: true }) })))
  })
  if (mm.length < MAX_ROUTINGS) mb.append(el('div', { class: 'gen-row' }, el('button', { text: '＋ Add a routing', onclick: () => api.commit('add routing', (ss, vv) => {
    L(vv).modMatrix = [...(L(vv).modMatrix ?? []), { src: (L(vv).lfos?.length ? 'LFO1' : 'AmpEnv'), dst: 'Pitch', amount: 0.05 }]
  }, { structural: true }) })))
  card.append(sec(ctx, 'mod', 'Modulation', mm.length ? mm.map((m) => `${m.src}→${m.dst}`).join(', ') : 'none', mb, false))

  return card
}

// ---------------------------------------------------------------------------
// The card.
// ---------------------------------------------------------------------------
export function renderLayers (ctx, s, vi, api) {
  const v = s.variants[vi]
  const box = el('div', {})
  const content = () => {
    const out = el('div', {})
    out.append(el('p', { class: 'note', html: 'All layers <b>start together</b> when the sound plays (unless one “starts after” a delay) and are <b>added together</b>. Each has its own source, tone and level. The timeline under the waveform shows when each one sounds; <b>S</b> and <b>M</b> there let you hear one alone or without it.' }))
    out.append(renderFlow(s, v, (i) => document.getElementById(`layer-${i}`)?.scrollIntoView({ behavior: 'smooth', block: 'start' })))

    const whole = el('div', { class: 'sliders', style: 'margin-bottom:10px' })
    whole.append(row('Ends by itself', toggle(!!v.patch.oneShot, (on) => api.commit('ends by itself', (ss, vv) => { if (on) vv.patch.oneShot = true; else delete vv.patch.oneShot }, { structural: true })),
      'On for almost every effect. Off only for a sound the game holds and releases, like an engine.'))
    whole.append(row('Choke group', numberBox(v.patch.exclusiveGroup ?? -1, { min: -1, max: 15, onChange: (x) => api.commit('choke group', (ss, vv) => { if (x === undefined || x < 0) delete vv.patch.exclusiveGroup; else vv.patch.exclusiveGroup = Math.round(x) }) }),
      'A new sound in the same group cuts the others short; −1 is none.'))
    const nm = el('input', { type: 'text', value: v.patch.name ?? '', placeholder: 'unnamed' })
    nm.onchange = () => api.commit('patch name', (ss, vv) => { if (nm.value.trim()) vv.patch.name = nm.value.trim(); else delete vv.patch.name })
    whole.append(row('Patch name', nm, 'a label for the recipe itself; the game asks for the sound’s name, not this'))
    out.append(whole)

    for (let li = 0; li < (v.patch.layers?.length ?? 0); li++) out.append(layerCard(ctx, s, v, li, api))
    if ((v.patch.layers?.length ?? 0) < MAX_LAYERS) {
      out.append(el('button', { text: '＋ Add a layer', onclick: () => api.commit('add layer', (ss, vv) => {
        vv.patch.layers.push({ osc: 'noise', mixGain: 0.6, ampEnv: { attack: 0, decay: 120, sustain: 0, release: 10 }, filter: { mode: 'bandpass', cutoffHz: 1500, resonance: 0.3 } })
      }, { structural: true }) }))
    }

    out.append(el('div', { class: 'sub', style: 'margin-top:18px', text: 'Rings (a struck body)' }))
    out.append(ringsEditor(s, api))

    out.append(el('div', { class: 'sub', style: 'margin-top:18px', text: 'Everything, as JSON' }))
    const ta = el('textarea', { class: 'json', spellcheck: 'false' })
    ta.value = JSON.stringify({ patch: v.patch, ...(v.gesture ? { gesture: v.gesture } : {}), ...(s.resonators ? { resonators: s.resonators } : {}) }, null, 2)
    const msg = el('span', { class: 'hint' })
    out.append(ta, el('div', { class: 'gen-row' }, el('button', { text: 'Apply JSON', onclick: () => {
      let j
      try { j = JSON.parse(ta.value) } catch (e) { msg.textContent = `not valid JSON: ${e.message}`; return }
      if (!j.patch || !Array.isArray(j.patch.layers)) { msg.textContent = 'needs a "patch" with "layers"'; return }
      api.commit('edit JSON', (ss, vv) => {
        vv.patch = j.patch
        if (j.gesture) vv.gesture = j.gesture; else delete vv.gesture
        if (j.resonators) ss.resonators = j.resonators; else delete ss.resonators
      }, { structural: true })
    } }), msg))
    return out
  }

  if (v._recipe && !ctx.editRecipeLayers?.has(`${ctx.sel.sound}:${vi}`)) {
    box.append(renderFlow(s, v))
    const open = el('button', { text: 'Edit the layers anyway', onclick: () => {
      ctx.editRecipeLayers = ctx.editRecipeLayers ?? new Set()
      ctx.editRecipeLayers.add(`${ctx.sel.sound}:${vi}`)
      box.replaceChildren(content())
    } })
    box.append(el('p', { class: 'note', html: 'This take is built from its <b>recipe</b>, so the knobs in Shape drive it. Editing layers by hand turns the recipe off for this take — Shape keeps only its four general knobs. Undo brings the recipe back.' }), open)
  } else {
    box.append(content())
  }
  return box
}

function ringsEditor (s, api) {
  const rings = s.resonators ?? []
  const box = el('div', {})
  if (!rings.length) box.append(el('p', { class: 'note', text: 'None. Rings make a sound ring like a struck object — metal, glass, a bell. The layers above become the strike.' }))
  rings.forEach((m, k) => {
    const set1 = (key, x) => (ss) => { ss.resonators[k][key] = x }
    const line = el('div', { class: 'sliders', style: 'margin-bottom:6px' })
    const mk = (key, label, o) => slider({ label: `${k + 1} · ${label}`, value: m[key] ?? o.def, ...o,
      onInput: (x) => api.preview(set1(key, r(x, 1))), onChange: (x) => api.commit(`ring ${label}`, set1(key, r(x, 1))) })
    line.append(mk('hz', 'pitch', { min: 20, max: 12000, log: true, def: 440, fmt: hzFmt }))
    line.append(mk('decayMs', 'ring time', { min: 1, max: 4000, log: true, def: 300, fmt: msFmt }))
    line.append(mk('gain', 'level', { min: 0, max: 4, def: 1, fmt: (x) => x.toFixed(2) }))
    line.append(el('div', {}, el('button', { class: 'ghost danger', text: 'Remove ring', onclick: () => api.commit('remove ring', (ss) => {
      ss.resonators.splice(k, 1); if (!ss.resonators.length) { delete ss.resonators; delete ss.resonatorDry }
    }, { structural: true }) })))
    box.append(line)
  })
  const add = el('div', { class: 'gen-row' })
  if (rings.length < MAX_RINGS) {
    add.append(el('button', { text: '＋ Add a ring', onclick: () => api.commit('add ring', (ss) => {
      const base = ss.resonators?.at(-1)?.hz ?? 520
      ss.resonators = [...(ss.resonators ?? []), { hz: Math.round(Math.min(12000, base * 1.59)), decayMs: 400, gain: 1 }]
    }, { structural: true }) }))
  }
  if (rings.length) {
    add.append(slider({ label: 'dry strike', value: s.resonatorDry ?? 1, min: 0, max: 2, def: 1, fmt: (x) => x.toFixed(2),
      onInput: (x) => api.preview((ss) => { ss.resonatorDry = r(x) }),
      onChange: (x) => api.commit('dry strike', (ss) => { if (r(x) === 1) delete ss.resonatorDry; else ss.resonatorDry = r(x) }) }))
  }
  box.append(add)
  return box
}

// ---------------------------------------------------------------------------
// Timing & notes: the take's gesture.
// ---------------------------------------------------------------------------
export function renderTiming (ctx, s, vi, api) {
  const v = s.variants[vi]
  const steps = v.gesture ?? []
  const box = el('div', {})
  box.append(el('p', { class: 'note', text: steps.length
    ? 'The take plays these notes in order. Layers with a fixed pitch ignore the note; the others follow it. Strength picks layers by their strength window and sets their level.'
    : 'Right now the take plays once, at its own pitch. Add steps to play it several times — at different pitches, like a jingle, or held for a while.' }))
  const list = el('div', { class: 'steps' })
  steps.forEach((g, k) => {
    const upd = (key, x) => api.commit(`step ${key}`, (ss, vv) => { if (x === undefined) delete vv.gesture[k][key]; else vv.gesture[k][key] = x })
    const noteLbl = el('span', { class: 'hint', text: noteName(g.note ?? 60) })
    list.append(el('div', { class: 'step' },
      el('span', { class: 'faint', text: `${k + 1}.` }),
      el('span', { class: 'hint', text: 'at' }), numberBox(g.atMs ?? 0, { min: 0, max: 32000, unit: 'ms', onChange: (x) => upd('atMs', x) }),
      el('span', { class: 'hint', text: 'note' }), numberBox(g.note ?? 60, { min: 0, max: 127, onChange: (x) => { noteLbl.textContent = noteName(x ?? 60); upd('note', x) } }), noteLbl,
      el('span', { class: 'hint', text: 'strength' }), numberBox(g.vel ?? 100, { min: 1, max: 127, onChange: (x) => upd('vel', x) }),
      s.loop ? null : el('span', { class: 'hint', text: 'held' }),
      s.loop ? null : numberBox(g.durMs ?? 0, { min: 0, max: 32000, unit: 'ms', onChange: (x) => upd('durMs', x || undefined) }),
      el('button', { class: 'ghost danger', text: '×', title: 'remove this step', onclick: () => api.commit('remove step', (ss, vv) => {
        vv.gesture.splice(k, 1); if (!vv.gesture.length) delete vv.gesture
      }, { structural: true }) })))
  })
  box.append(list)
  if (steps.length < 8) {
    box.append(el('div', { class: 'gen-row' }, el('button', { text: '＋ Add a step', onclick: () => api.commit('add step', (ss, vv) => {
      const last = vv.gesture?.at(-1)
      vv.gesture = [...(vv.gesture ?? []), { atMs: last ? (last.atMs ?? 0) + 100 : 0, note: last?.note ?? 60, vel: 100 }]
    }, { structural: true }) })))
  }
  return box
}
