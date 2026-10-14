//
// The library: every sound in the bank, as a list you can scan.
//
// Grouped by the first part of the name -- `weapon.pistol.fire` sits under
// WEAPON -- because that is how every bank here is already named, and a
// hundred sounds are a list of ten groups, not a grid of a hundred cards.
// Each row shows the three things you scan for: the name, a picture of the
// sound, and whether it is too loud or broken.
//
// Thumbnails are rendered through the worker's scratch path and cached by
// CONTENT (tool-client.js variantKey), so an edit can never leave a stale
// picture and there is no invalidation to get wrong. The same render carries
// the sound's validation report, which is where the warning dot comes from.
//
import { el } from './controls.js'
import { drawEnvelope, peakLabel } from './waveform.js'
import { variantKey } from './tool-client.js'
import { inspect, toList, worst } from './validate.js'
import { heardPeak } from './stage.js'

export function initLibrary (ctx) {
  const $ = (id) => document.getElementById(id)
  const list = $('lib-list')
  const search = $('lib-search')
  const cache = new Map()       // variantKey -> {min, max, info, ok, problems}
  const closed = new Set(ctx.ui.load().closedGroups ?? [])
  let sweepGen = 0
  let idle = Promise.resolve()

  search.value = ctx.ui.load().search ?? ''

  const groupOf = (name) => {
    const i = name.indexOf('.')
    return i > 0 ? name.slice(0, i) : 'other'
  }

  function matches (s, q) {
    if (!q) return true
    const hay = `${s.name} ${(s.tags ?? []).join(' ')}`.toLowerCase()
    return q.toLowerCase().split(/\s+/).every((w) => hay.includes(w))
  }

  function render () {
    const doc = ctx.store.doc
    const q = search.value.trim()
    const groups = new Map()
    doc.sounds.forEach((s, i) => {
      if (!matches(s, q)) return
      const g = groupOf(s.name)
      if (!groups.has(g)) groups.set(g, [])
      groups.get(g).push(i)
    })

    list.replaceChildren()
    if (!doc.sounds.length) {
      list.append(el('div', { class: 'empty' },
        el('p', { text: 'No sounds yet.' }),
        el('button', { class: 'primary', text: '＋ Make the first one', onclick: () => ctx.newSound() })))
    } else if (!groups.size) {
      list.append(el('div', { class: 'empty hint', text: `Nothing matches “${q}”.` }))
    }

    for (const [g, idx] of groups) {
      const isClosed = closed.has(g) && !q
      const items = el('div', { class: 'items' })
      const box = el('div', { class: 'group' + (isClosed ? ' closed' : '') },
        el('button', {
          class: 'group-head', type: 'button',
          onclick: () => {
            if (closed.has(g)) closed.delete(g); else closed.add(g)
            ctx.ui.save({ closedGroups: [...closed] })
            box.classList.toggle('closed')
          },
        }, el('span', { class: 'chev', text: '▾' }), `${g}`, el('span', { class: 'faint', text: ` ${idx.length}` })),
        items)
      for (const i of idx) items.append(itemRow(i, g))
      list.append(box)
    }
    $('lib-count').textContent = `${doc.sounds.length} sound${doc.sounds.length === 1 ? '' : 's'}`
    paintAll()
  }

  function itemRow (i, g) {
    const s = ctx.store.doc.sounds[i]
    const rest = s.name.startsWith(g + '.') ? s.name.slice(g.length + 1) : s.name
    const c = el('canvas')
    const meta = el('div', { class: 'meta' })
    const row = el('div', {
      class: 'item' + (i === ctx.sel.sound ? ' sel' : ''), 'data-i': String(i), role: 'button', tabindex: '0',
      onclick: () => { ctx.select(i, 0); ctx.showPane('stage') },
      onkeydown: (e) => { if (e.key === 'Enter') { ctx.select(i, 0); ctx.showPane('stage') } },
    }, el('div', { class: 'nm', title: s.name }, rest), c, meta)
    return row
  }

  /** Draw whatever the cache already knows, without asking the worker. */
  function paintAll () {
    for (const row of list.querySelectorAll('.item')) paintRow(row)
  }

  function paintRow (row) {
    const i = Number(row.dataset.i)
    const s = ctx.store.doc.sounds[i]
    if (!s) return
    const t = cache.get(variantKey(s, 0))
    const c = row.querySelector('canvas')
    drawEnvelope(c, t?.min ? t : null, { background: 'none', clip: false, emptyText: t ? '!' : '' })
    const meta = row.querySelector('.meta')
    meta.replaceChildren()
    const takes = s.variants?.length ?? 0
    if (t?.problems?.length) meta.append(el('span', { class: `dot ${worst(t.problems)}`, title: t.problems.map((p) => p.text).join('\n') }))
    if (t?.info) {
      const over = heardPeak(s, t) > 0
      meta.append(el('span', { class: 'lvl' + (over ? ' over' : ''), text: over ? 'too loud' : `${Math.round(t.info.frames / 48)} ms` }))
    }
    if (takes > 1) meta.append(el('span', { text: `${takes} takes` }))
    if (s.loop) meta.append(el('span', { text: 'loop' }))
  }

  /**
   * Render every sound the cache does not know yet. One at a time, newest
   * sweep wins, and the selected sound first so the screen you are looking
   * at fills in before the rest of the bank.
   */
  async function sweep () {
    const gen = ++sweepGen
    let done
    idle = new Promise((r) => { done = r })
    const doc = ctx.store.doc
    const order = [...doc.sounds.keys()]
    if (ctx.sel.sound >= 0) order.sort((a, b) => (a === ctx.sel.sound ? -1 : b === ctx.sel.sound ? 1 : 0))
    for (const i of order) {
      if (gen !== sweepGen) break
      const s = ctx.store.doc.sounds[i]
      if (!s) continue
      const key = variantKey(s, 0)
      if (cache.has(key)) continue
      try {
        const r = await ctx.client.audition(s, 0, { buckets: 96, wantAudio: false })
        remember(s, r)
      } catch { /* a failed thumbnail is a blank row, not an error */ }
      const row = list.querySelector(`.item[data-i="${i}"]`)
      if (row) paintRow(row)
    }
    done()
  }

  function remember (s, r) {
    const problems = [...toList(r), ...inspect(s, 0)]
    cache.set(variantKey(s, 0), { min: r.min ?? null, max: r.max ?? null, info: r.info ?? null, ok: r.ok, problems })
    if (cache.size > 800) cache.delete(cache.keys().next().value)
  }

  search.oninput = () => { render(); ctx.ui.save({ search: search.value }) }

  // Up/down through the list from anywhere that is not a text field.
  document.addEventListener('keydown', (e) => {
    if (ctx.view !== 'sounds' || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return
    const rows = [...list.querySelectorAll('.item')].map((r) => Number(r.dataset.i))
    if (!rows.length) return
    e.preventDefault()
    const at = rows.indexOf(ctx.sel.sound)
    const next = rows[Math.max(0, Math.min(rows.length - 1, at + (e.key === 'ArrowDown' ? 1 : -1)))]
    ctx.select(next, 0)
    list.querySelector(`.item[data-i="${next}"]`)?.scrollIntoView({ block: 'nearest' })
  })

  ctx.on((what) => {
    if (what === 'select') {
      for (const r of list.querySelectorAll('.item')) r.classList.toggle('sel', Number(r.dataset.i) === ctx.sel.sound)
      return
    }
    if (what === 'opened' || what === 'structural' || what === 'change') {
      render()
      sweep()
    }
  })

  return {
    render,
    sweep,
    /** The stage's own render of take 0, so the selected row never waits. */
    adopt (s, r) { if (r) { remember(s, r); const row = list.querySelector(`.item[data-i="${ctx.sel.sound}"]`); if (row) paintRow(row) } },
    measured: (s) => cache.get(variantKey(s, 0)),
    idle: () => idle,
  }
}

/** The peak, as the library and the stage both label it. */
export const levelText = (info) => info ? `${peakLabel(info.peakDbMilli)} dB` : '—'
