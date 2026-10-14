//
// The handful of controls every panel is built from.
//
// One slider, one number row, one choice, one switch -- so a person learns
// the interaction once. Every slider:
//   * shows its value in words and units, and the readout is click-to-type;
//   * resets to its default on double-click (the Ableton/Vital convention);
//   * reports `input` while dragging and `change` once on release, so the
//     caller can re-render live and still record ONE undo step per gesture.
//
import { parseNum } from './proj.js'

export function el (tag, attrs = {}, ...kids) {
  const e = document.createElement(tag)
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue
    if (k === 'class') e.className = v
    else if (k === 'text') e.textContent = v
    else if (k === 'html') e.innerHTML = v
    else if (k.startsWith('on')) e[k] = v
    else if (k === 'style') e.style.cssText = v
    else e.setAttribute(k, v === true ? '' : v)
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) e.append(c)
  return e
}

/**
 * A slider.
 *
 *   value, min, max, step, def   in the slider's own units
 *   fmt(v)                       the readout
 *   parse(text)                  typed text -> slider units (default: a number)
 *   log                          map the track logarithmically (min > 0)
 *   onInput(v), onChange(v)
 */
export function slider (o) {
  const {
    label, hint = '', min = 0, max = 1, step = 0.01, def = min, fmt = (v) => String(v),
    parse = (t) => parseNum(t), log = false, onInput = () => {}, onChange = () => {},
  } = o
  let value = o.value ?? def
  // The track runs 0..1000 either way; the mapping is ours, so a log range
  // (20 Hz - 12 kHz, 1 ms - 30 s) spends its travel where the ear does.
  const toT = (v) => log
    ? 1000 * Math.log(v / min) / Math.log(max / min)
    : 1000 * (v - min) / (max - min)
  const fromT = (t) => {
    let v = log ? min * Math.pow(max / min, t / 1000) : min + (max - min) * t / 1000
    if (step) v = Math.round(v / step) * step
    return Math.min(max, Math.max(min, Number(v.toFixed(6))))
  }

  const range = el('input', { type: 'range', min: 0, max: 1000, step: 1, 'aria-label': label })
  const out = el('button', { class: 'readout', type: 'button', title: 'click to type a value' })
  const name = el('span', { class: 'lbl', text: label, title: hint ? `${hint} — double-click to reset` : 'double-click to reset' })
  const row = el('div', { class: 'slider' }, name, range, out)

  const paint = () => {
    range.value = String(Math.round(toT(value)))
    out.textContent = fmt(value)
    row.classList.toggle('moved', Math.abs(value - def) > (step || 1e-9) / 2)
  }
  paint()

  range.oninput = () => { value = fromT(Number(range.value)); out.textContent = fmt(value); row.classList.toggle('moved', value !== def); onInput(value) }
  range.onchange = () => { value = fromT(Number(range.value)); paint(); onChange(value) }
  name.ondblclick = () => { value = def; paint(); onInput(value); onChange(value) }

  out.onclick = () => {
    const box = el('input', { type: 'text', class: 'typed', value: String(value) })
    out.replaceWith(box)
    box.focus(); box.select()
    // Once only: Enter commits, the commit can rebuild the whole panel, and
    // the box then loses focus -- a second commit from blur would run against
    // an element that is no longer in the page.
    let finished = false
    const done = (commit) => {
      if (finished) return
      finished = true
      if (box.isConnected) box.replaceWith(out)
      paint()
      if (commit) {
        const v = parse(box.value)
        if (Number.isFinite(v)) { value = Math.min(max, Math.max(min, v)); paint(); onInput(value); onChange(value) }
      }
    }
    box.onkeydown = (e) => { if (e.key === 'Enter') done(true); if (e.key === 'Escape') done(false) }
    box.onblur = () => done(true)
  }

  row.set = (v) => { value = v; paint() }
  row.get = () => value
  return row
}

/** A labelled row holding any control, with an optional one-line explanation. */
export function row (label, control, hint = '') {
  return el('div', { class: 'field' },
    el('div', { class: 'field-top' }, el('span', { class: 'lbl', text: label }), control),
    hint ? el('div', { class: 'hint', text: hint }) : null)
}

/** A segmented choice: [a | b | c]. `options` is [[value, label, title?]]. */
export function choice (options, value, onChange) {
  const box = el('div', { class: 'seg', role: 'radiogroup' })
  const paint = (v) => {
    for (const b of box.children) b.classList.toggle('on', b.dataset.v === String(v))
  }
  for (const [v, label, title] of options) {
    box.append(el('button', {
      type: 'button', 'data-v': String(v), text: label, title,
      onclick: () => { paint(v); onChange(v) },
    }))
  }
  paint(value)
  box.set = paint
  return box
}

/** An on/off switch. */
export function toggle (value, onChange, label = '') {
  const b = el('button', { type: 'button', class: 'switch', role: 'switch', 'aria-label': label })
  const paint = (v) => { b.classList.toggle('on', !!v); b.setAttribute('aria-checked', v ? 'true' : 'false') }
  let v = !!value
  paint(v)
  b.onclick = () => { v = !v; paint(v); onChange(v) }
  b.set = (x) => { v = !!x; paint(v) }
  return b
}

/** A compact number box with a unit, committing on change. */
export function numberBox (value, { min, max, unit = '', onChange, placeholder = '' }) {
  const inp = el('input', { type: 'text', inputmode: 'decimal', class: 'num', value: value === undefined || value === '' ? '' : String(value), placeholder })
  inp.onchange = () => {
    // Empty means "not set" -- for a field whose absence means something.
    if (inp.value.trim() === '') { value = undefined; onChange(undefined); return }
    let v = parseNum(inp.value)
    if (!Number.isFinite(v)) { inp.value = String(value); return }
    if (min !== undefined) v = Math.max(min, v)
    if (max !== undefined) v = Math.min(max, v)
    value = v
    inp.value = String(v)
    onChange(v)
  }
  const box = el('span', { class: 'numbox' }, inp, unit ? el('span', { class: 'unit', text: unit }) : null)
  box.set = (v) => { value = v; inp.value = String(v) }
  return box
}

/** A collapsible card. Remembers open/closed per key in UI state. */
export function card (title, body, { open = true, key = null, subtitle = '', actions = null, ui = null } = {}) {
  const saved = key && ui ? ui.load().cards?.[key] : undefined
  const isOpen = saved ?? open
  const head = el('button', { type: 'button', class: 'card-head', 'aria-expanded': String(isOpen) },
    el('span', { class: 'chev', text: '▸' }),
    el('span', { class: 'card-title', text: title }),
    subtitle ? el('span', { class: 'card-sub', text: subtitle }) : null)
  const c = el('section', { class: 'card' + (isOpen ? ' open' : '') },
    el('div', { class: 'card-bar' }, head, actions), el('div', { class: 'card-body' }, body))
  head.onclick = () => {
    const o = !c.classList.contains('open')
    c.classList.toggle('open', o)
    head.setAttribute('aria-expanded', String(o))
    if (key && ui) ui.save({ cards: { ...(ui.load().cards ?? {}), [key]: o } })
  }
  return c
}
