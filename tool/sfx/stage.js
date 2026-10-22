//
// The stage: the one sound you have selected, top to bottom in the order you
// work on it --
//
//   its name and tags
//   a big play button and its waveform            (hear it)
//   anything wrong with it, with a button to fix   (trust it)
//   its takes                                     (the game picks one at random)
//   START FROM: a kind of sound, Randomize, Vary  (make it)
//   SHAPE: volume, pitch, length, brightness,     (tune it)
//          and the recipe's own knobs
//   LAYERS and TIMING, folded away                (take it apart)
//
// Every slider re-renders while you drag and plays when you let go, the way
// sfxr does: the fastest loop between a change and hearing it is what makes
// a sound editor feel obvious. The render is the real engine's, through the
// worker, so what you hear is what the game will play.
//
import { el, slider, card, numberBox } from './controls.js'
import { drawEnvelope } from './waveform.js'
import {
  KINDS, SHAPE, archsOfKind, kindOfArch, describeParam, denorm, formatValue,
  randomVector, varyVector, pick, hoist, writeTake, sourceOf, ensureBase, detach,
} from './recipes.js'
import { toList, inspect } from './validate.js'
import { renderLayers, renderTiming, LAYER_COLORS, layerSummary } from './layers.js'
import { icon } from './icons.js'

const TARGET_PEAK = -6        // dBFS a generated sound is levelled to

/**
 * How loud a sound actually plays, in dBFS.
 *
 * The worker's peak is the DRY render: a sound's volume (gainDb) is applied
 * at playback, not baked into its samples, so the render is the same whatever
 * the volume says. The level a person hears -- and the one that clips -- is
 * the dry peak plus the volume.
 */
export const heardPeak = (s, r) => r?.info ? r.info.peakDbMilli / 1000 + (s?.gainDb ?? 0) : null

export function initStage (ctx) {
  const host = document.getElementById('stage-in')
  let seq = 0                 // newest audition wins
  let lastReport = null
  let lastSound = null
  let dom = {}                // the live elements of the current build

  // What you are LISTENING to, which is not the document: one layer soloed,
  // or some muted. Never saved, reset when the selection moves.
  const listen = { solo: -1, mute: new Set() }
  const listening = () => listen.solo >= 0 || listen.mute.size > 0

  /** The sound as you have asked to hear it: soloed or muted layers applied. */
  function heardOf (s) {
    if (!listening()) return s
    const c = structuredClone(s)
    const v = c.variants[take()]
    const keep = (v.patch.layers ?? []).map((_, i) => listen.solo >= 0 ? i === listen.solo : !listen.mute.has(i))
    // A muted layer is removed rather than set to zero: a patch with no layers
    // does not load, so "everything muted" becomes one silent layer.
    const layers = v.patch.layers.filter((_, i) => keep[i])
    v.patch.layers = layers.length ? layers : [{ osc: 'noise', mixGain: 0, ampEnv: { attack: 0, decay: 1, sustain: 0, release: 0 } }]
    return c
  }

  const idx = () => ctx.sel.sound
  const sound = () => ctx.store.doc.sounds[idx()]
  const take = () => ctx.sel.variant

  // ---------------------------------------------------------------------------
  // Hearing it.
  // ---------------------------------------------------------------------------

  /**
   * Render `s` (a document sound, or a candidate that is not in the document
   * yet) and show it. `play` plays it once rendered.
   */
  async function hear (s, { play = false } = {}) {
    const my = ++seq
    let r
    try {
      r = await ctx.client.audition(heardOf(s), take())
    } catch (e) {
      ctx.log(`render: ${e.message}`, 'err')
      return null
    }
    if (my !== seq) return null
    lastReport = r
    lastSound = s
    ctx.audition.setGainDb?.(s.gainDb ?? 0)
    paintWave(r)
    paintProblems(s, r)
    if (r.ok && r.left) {
      ctx.audition.adopt(r, idx(), take())
      if (play && ctx.autoPlay) ctx.play()
    }
    if (take() === 0 && s === sound() && !listening()) ctx.library?.adopt(s, r)
    if (s === sound()) paintLanes(s, r)
    return r
  }

  // ---------------------------------------------------------------------------
  // The timeline: each layer on its own, on one time axis.
  //
  // Rendered, not estimated: every lane is that layer played ALONE through
  // the real engine (rings included, since the layer is what strikes them), so
  // its length and loudness are exactly its share of the sound. The same
  // scale is used for every lane, so a quiet layer looks quiet.
  // ---------------------------------------------------------------------------
  let laneSeq = 0
  async function paintLanes (s, whole) {
    if (!dom.lanes) return
    const my = ++laneSeq
    const v = s.variants[take()]
    const layers = v?.patch?.layers ?? []
    if (layers.length < 2 && !s.resonators?.length) { dom.lanes.replaceChildren(); return }
    const renders = []
    for (let li = 0; li < layers.length; li++) {
      const c = structuredClone(s)
      c.variants[take()].patch.layers = [structuredClone(layers[li])]
      try { renders.push(await ctx.client.audition(c, take(), { wantAudio: false, buckets: 240 })) } catch { renders.push(null) }
      if (my !== laneSeq) return
    }
    const T = Math.max(whole?.info?.frames ?? 1, ...renders.map((r) => r?.info?.frames ?? 0), 1)
    let peak = 1e-6
    for (const r of renders) if (r?.max) for (let i = 0; i < r.max.length; i++) peak = Math.max(peak, r.max[i], -r.min[i])

    const rows = layers.map((l, li) => {
      const cv = el('canvas')
      const off = listen.solo >= 0 ? listen.solo !== li : listen.mute.has(li)
      const solo = el('button', { text: 'S', class: listen.solo === li ? 'on-s' : '', title: 'hear this layer alone',
        onclick: () => { listen.solo = listen.solo === li ? -1 : li; listen.mute.clear(); hear(sound(), { play: true }) } })
      const mute = el('button', { text: 'M', class: listen.mute.has(li) ? 'on-m' : '', title: 'hear the sound without this layer',
        onclick: () => { listen.solo = -1; if (listen.mute.has(li)) listen.mute.delete(li); else listen.mute.add(li); hear(sound(), { play: true }) } })
      const row = el('div', { class: 'lane' + (off ? ' off' : '') },
        el('button', { class: 'lane-name', title: 'show this layer', onclick: () => openLayer(li) },
          el('span', { class: 'flow-dot', style: `--c:${LAYER_COLORS[li % 4]}` }), `${li + 1} · ${layerSummary(l)}`),
        cv, el('div', { class: 'sm' }, solo, mute))
      requestAnimationFrame(() => drawLane(cv, renders[li], T, peak, LAYER_COLORS[li % 4]))
      return row
    })
    const ms = Math.round(T / 48)
    const axis = el('div', { class: 'lane' }, el('span'), el('div', { class: 'lane-axis' },
      el('span', { text: '0' }), el('span', { text: `${Math.round(ms / 2)} ms` }), el('span', { text: `${ms} ms` })), el('span'))
    const note = listening()
      ? el('div', { class: 'listening' }, listen.solo >= 0 ? `Hearing layer ${listen.solo + 1} alone.` : `Hearing without layer${listen.mute.size > 1 ? 's' : ''} ${[...listen.mute].map((x) => x + 1).join(', ')}.`,
        el('button', { text: 'Hear everything', onclick: () => { listen.solo = -1; listen.mute.clear(); hear(sound(), { play: true }) } }))
      : null
    dom.lanes.replaceChildren(...rows, axis, ...(note ? [note] : []))
  }

  function drawLane (cv, r, T, peak, colour) {
    const dpr = globalThis.devicePixelRatio || 1
    const w = cv.clientWidth, h = cv.clientHeight
    if (!w) return
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr)
    const g = cv.getContext('2d')
    g.setTransform(dpr, 0, 0, dpr, 0, 0)
    g.clearRect(0, 0, w, h)
    if (!r?.max) return
    const n = r.max.length
    const span = (r.info.frames / T) * w
    const mid = h / 2
    g.fillStyle = colour
    for (let i = 0; i < n; i++) {
      const a = Math.max(r.max[i], -r.min[i]) / peak
      // Nothing where the layer is silent, so a delayed layer visibly STARTS
      // late instead of looking like a flat line from zero.
      if (a < 0.004) continue
      const x = (i / n) * span
      const hh = Math.max(0.75, a * (h / 2 - 1))
      g.fillRect(x, mid - hh, Math.max(1, span / n), hh * 2)
    }
  }

  function openLayer (li) {
    const c = [...host.querySelectorAll('.card')].find((x) => x.querySelector('.card-title')?.textContent === 'Layers')
    if (c && !c.classList.contains('open')) c.querySelector('.card-head').click()
    requestAnimationFrame(() => (document.getElementById(`layer-${li}`) ?? c)?.scrollIntoView({ behavior: 'smooth', block: 'start' }))
  }
  ctx.hearSelected = (opts) => sound() && hear(sound(), opts)

  let previewTimer = null
  /** Re-render a candidate while a slider moves; coalesced to one in flight. */
  function preview (cand) {
    clearTimeout(previewTimer)
    previewTimer = setTimeout(() => hear(cand), 30)
  }

  function paintWave (r) {
    if (!dom.wave) return
    if (!r || !r.ok) {
      drawEnvelope(dom.wave, null, { emptyText: 'this sound does not load — see the problem above' })
      dom.len.textContent = '—'
      dom.peak.textContent = '—'
      dom.peak.className = 'badge'
      dom.fixLevel.hidden = true
      return
    }
    // Drawn at the level it plays at, so turning the volume up visibly
    // brings it toward the ceiling -- and past it, in the warning colour.
    const lin = Math.pow(10, (lastSound?.gainDb ?? 0) / 20)
    drawEnvelope(dom.wave, lin === 1 ? r : { min: r.min.map((x) => x * lin), max: r.max.map((x) => x * lin) })
    const ms = Math.round(r.info.frames / 48)
    dom.len.textContent = ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${ms} ms`
    const db = heardPeak(lastSound, r)
    const over = db > 0
    dom.peak.textContent = over ? `too loud: +${db.toFixed(1)} dB` : `peak ${db.toFixed(1)} dB`
    dom.peak.className = 'badge' + (over ? ' over' : '')
    dom.peak.title = over
      ? 'Above full scale: the game would clip this. Press “Fix level”.'
      : 'How loud the loudest moment is. 0 dB is the ceiling.'
    dom.fixLevel.hidden = !(over || db < -24)
    dom.fixLevel.textContent = over ? 'Fix level' : 'Make louder'
  }

  function paintProblems (s, r) {
    if (!dom.problems) return
    const list = [...toList(r), ...inspect(s, take())]
    dom.problems.replaceChildren(...list.map((p) => {
      const fixes = p.fix?.fixes ?? []
      return el('div', { class: `problem ${p.severity}` },
        el('span', { class: `problem-ico ${p.severity}` }, icon(p.severity === 'error' ? 'error' : 'warning')),
        el('div', {},
          el('div', { text: plain(p.text) }),
          fixes.length ? el('div', { class: 'fixes' }, fixes.map((f) =>
            el('button', { text: f.label, onclick: () => applyFix(f) }))) : null))
    }))
  }

  // The engine speaks in paths; people speak in sounds and takes.
  const plain = (t) => t
    .replace(/^sounds\[\d+\]\.?/, '')
    .replace(/variants\[(\d+)\]/g, (_, n) => `take ${String.fromCharCode(65 + Number(n))}`)
    .replace(/\bvariant (\d+)\b/g, (_, n) => `take ${String.fromCharCode(65 + Number(n))}`)

  function applyFix (f) {
    const i = idx()
    const vi = take()
    ctx.store.mutate(f.label, (d) => {
      const s = d.sounds[i]
      const v = s.variants[vi]
      if (f.path === 'patch.oneShot') v.patch.oneShot = true
      else if (f.path === 'ampEnv.sustain') for (const l of v.patch.layers ?? []) { l.ampEnv = { ...(l.ampEnv ?? {}), sustain: 0 } }
      else if (f.path === 'gesture.durMs') { v.gesture = v.gesture?.length ? v.gesture : [{ atMs: 0 }]; v.gesture[0].durMs = f.value }
      else if (f.path === 'loop') s.loop = true
      detach(s, vi)
    }, { structural: true })
  }

  /** Move gainDb so the loudest moment sits at `target` dBFS. */
  async function level (target = TARGET_PEAK) {
    const s = sound()
    const r = lastReport
    if (!s || !r?.ok) return
    // The volume that puts the loudest moment at the target: the dry peak
    // does not move with the volume, so this is target minus dry peak.
    const next = Math.max(-40, Math.min(12, Math.round((target - r.info.peakDbMilli / 1000) * 10) / 10))
    ctx.store.mutate('level', (d) => {
      if (Math.abs(next) < 0.05) delete d.sounds[idx()].gainDb
      else d.sounds[idx()].gainDb = next
    })
    dom.volume?.set(next)
    await hear(sound(), { play: true })
  }

  // ---------------------------------------------------------------------------
  // Making it: recipes.
  // ---------------------------------------------------------------------------

  const archByName = (n) => ctx.archs.find((a) => a.name === n)

  /** A copy of the selected sound with take `vi` recomposed from its source. */
  async function compose (s, vi = take()) {
    const snd = structuredClone(s)
    const v = snd.variants[vi]
    let source
    if (v._recipe) {
      const r = await ctx.client.recipe(v._recipe.arch, v._recipe.u)
      if (!r.ok) throw new Error(r.error)
      source = hoist(r.patch)
    } else {
      source = sourceOf(snd, vi)
    }
    writeTake(snd, vi, source, v._shape)
    return snd
  }

  /** Replace the take with a recipe, render it, level it, play it. */
  async function generate (arch, u, label, { keepLevel = false, decorate = null } = {}) {
    const s = sound()
    if (!s) return
    const cand = structuredClone(s)
    const v = cand.variants[take()]
    v._recipe = { arch: arch.name, u: u.map((x) => Math.round(x * 10000) / 10000) }
    delete v._base
    delete v._shape
    let snd
    try { snd = await compose(cand) } catch (e) { ctx.log(`${label}: ${e.message}`, 'err'); return }
    // Level it before anyone hears it: the architectures span a 40 dB range
    // between their corners, and a generator that sometimes blasts you at
    // +20 dB is one nobody presses twice.
    if (!keepLevel) {
      const probe = await ctx.client.audition(snd, take(), { wantAudio: false, buckets: 8 })
      if (probe.ok) {
        const g = Math.max(-40, Math.min(12, Math.round((TARGET_PEAK - probe.info.peakDbMilli / 1000) * 10) / 10))
        if (Math.abs(g) < 0.05) delete snd.gainDb; else snd.gainDb = g
      }
    }
    decorate?.(snd)
    const i = idx()
    ctx.store.mutate(label, (d) => { d.sounds[i] = snd }, { structural: true })
    await hear(sound(), { play: true })
  }

  async function fromKind (kind) {
    const choices = archsOfKind(kind, ctx.archs)
    if (!choices.length) return
    const a = pick(choices)
    ctx.ui.save({ lastKind: kind.id })
    await generate(a, randomVector(a), `new ${kind.label.toLowerCase()}`)
  }

  async function randomize () {
    const v = sound()?.variants[take()]
    const cur = v?._recipe ? archByName(v._recipe.arch) : null
    const kind = cur ? kindOfArch(cur.name) : KINDS.find((k) => k.id === ctx.ui.load().lastKind) ?? KINDS[0]
    // Stay in the same architecture when there is one: "another one like
    // this" is what Randomize means once you have picked a kind.
    const a = cur ?? pick(archsOfKind(kind, ctx.archs))
    await generate(a, randomVector(a), 'randomize')
  }

  async function vary () {
    const v = sound()?.variants[take()]
    if (!v?._recipe) return
    const a = archByName(v._recipe.arch)
    await generate(a, varyVector(v._recipe.u), 'vary', { keepLevel: true })
    if (lastReport?.ok && heardPeak(sound(), lastReport) > 0) await level()
  }

  // ---------------------------------------------------------------------------
  // Building the page.
  // ---------------------------------------------------------------------------

  // Which sound the page was last built for. Rebuilding the SAME sound -- a
  // structural edit such as a new layer source, a filter mode, an added LFO
  // -- keeps the scroll position; only moving to another sound starts at the
  // top. Without this, every such click threw the page back to its start.
  let builtFor = null
  function build () {
    const s = sound()
    const scroller = document.getElementById('stage')
    const key = `${idx()}:${take()}`
    const keep = builtFor === key ? scroller.scrollTop : 0
    builtFor = key
    // Hold the page's height while it is rebuilt, so the browser does not
    // clamp the scroll position to a momentarily empty page.
    host.style.minHeight = `${host.offsetHeight}px`
    dom = {}
    host.replaceChildren()
    requestAnimationFrame(() => { host.style.minHeight = ''; })
    if (!s) {
      paintPlay()
      host.append(el('div', { class: 'empty' },
        el('h2', { text: ctx.store.doc.sounds.length ? 'Pick a sound' : 'An empty bank' }),
        el('p', { text: ctx.store.doc.sounds.length
          ? 'Choose one on the left, or make a new one.'
          : 'Make your first sound: pick what kind it is and press play.' }),
        el('button', { class: 'primary', text: '＋ New sound', onclick: () => ctx.newSound() })))
      return
    }
    const vi = Math.min(take(), s.variants.length - 1)
    const v = s.variants[vi]

    // ---- name, tags, actions ----
    const name = el('input', { type: 'text', id: 'snd-name', value: s.name, spellcheck: 'false', title: 'the name the game asks for — click to rename' })
    name.onchange = () => ctx.renameSound(idx(), name.value.trim()) || (name.value = sound().name)
    const tags = el('div', { class: 'tags' })
    for (const t of s.tags ?? []) {
      tags.append(el('span', { class: 'tag' }, t, el('button', {
        text: '×', title: `remove ${t}`,
        onclick: () => ctx.store.mutate('remove tag', (d) => {
          const ss = d.sounds[idx()]; ss.tags = ss.tags.filter((x) => x !== t); if (!ss.tags.length) delete ss.tags
        }, { structural: true }),
      })))
    }
    const addTag = el('input', { type: 'text', class: 'tag-add', placeholder: '+ tag' })
    addTag.onkeydown = (e) => {
      if (e.key !== 'Enter' || !addTag.value.trim()) return
      const t = addTag.value.trim()
      ctx.store.mutate('add tag', (d) => {
        const ss = d.sounds[idx()]; ss.tags = [...new Set([...(ss.tags ?? []), t])]
      }, { structural: true })
    }
    tags.append(addTag)

    const menu = el('div', { class: 'menu' },
      el('button', { class: 'ghost', text: '⋯', title: 'more' }),
      el('div', { class: 'drop' },
        el('button', { text: 'Duplicate this sound', onclick: () => ctx.duplicateSound(idx()) }),
        el('button', { text: 'Export as WAV', 'data-act': 'wav' }),
        el('hr'),
        el('button', { class: 'danger', text: 'Delete this sound', onclick: () => ctx.deleteSound(idx()) })))

    host.append(el('div', { id: 'title-row' },
      el('button', { id: 'back', class: 'ghost', text: '‹ Sounds', onclick: () => ctx.showPane('list') }),
      name,
      el('button', { id: 'ig-open', text: 'In the game ›', onclick: () => ctx.openInGame(true) }),
      menu))
    host.append(tags)

    // ---- the player ----
    dom.wave = el('canvas', { id: 'wave' })
    new ResizeObserver(() => { if (dom.wave?.clientWidth && lastReport) paintWave(lastReport) }).observe(dom.wave)
    dom.playhead = el('div', { id: 'playhead' })
    dom.len = el('span', { class: 'badge', text: '…' })
    dom.peak = el('span', { class: 'badge', text: '…' })
    dom.fixLevel = el('button', { text: 'Fix level', hidden: true, onclick: () => level() })
    const loopBox = el('input', { type: 'checkbox' })
    loopBox.checked = !!ctx.audition.loop
    loopBox.onchange = () => { ctx.audition.setLoop(loopBox.checked); ctx.ui.save({ loop: loopBox.checked }) }
    const autoBox = el('input', { type: 'checkbox' })
    autoBox.checked = ctx.autoPlay
    autoBox.onchange = () => { ctx.autoPlay = autoBox.checked; ctx.ui.save({ autoPlay: autoBox.checked }) }
    dom.lanes = el('div', { class: 'lanes' })
    host.append(el('div', { class: 'player' },
      el('div', { class: 'wave-wrap' }, dom.wave, dom.playhead),
      dom.lanes,
      el('div', { class: 'player-info' }, dom.len, dom.peak, dom.fixLevel, el('span', { class: 'sp' }),
        el('label', { class: 'check', title: 'keep playing it, to judge a sound by repetition (L)' }, loopBox, 'Repeat'),
        el('label', { class: 'check', title: 'play after every change' }, autoBox, 'Play on change'))))
    dom.problems = el('div', { class: 'problems' })
    host.append(dom.problems)
    paintPlay()

    // ---- takes ----
    const takes = el('div', { class: 'takes' }, el('span', { class: 'hint', text: 'Takes' }))
    s.variants.forEach((_, k) => {
      takes.append(el('button', {
        class: 'take' + (k === vi ? ' on' : ''), text: String.fromCharCode(65 + k),
        title: `take ${String.fromCharCode(65 + k)}`, onclick: () => ctx.select(idx(), k),
      }))
    })
    takes.append(el('button', { text: '＋', title: 'add a take: a slightly different version the game can pick instead', onclick: addTake }))
    if (s.variants.length > 1) {
      const total = s.variants.reduce((a, x) => a + (x.weight ?? 1), 0)
      takes.append(el('span', { class: 'hint', text: `picked ${Math.round(((v.weight ?? 1) / total) * 100)}% of the time · weight` }))
      takes.append(numberBox(v.weight ?? 1, {
        min: 1, max: 1000, onChange: (w) => ctx.store.mutate('take weight', (d) => {
          const t = d.sounds[idx()].variants[vi]; if (w === 1) delete t.weight; else t.weight = w
        }, { structural: true }),
      }))
      takes.append(el('button', { class: 'ghost danger', text: 'Remove take', onclick: removeTake }))
    } else {
      takes.append(el('span', { class: 'hint', text: 'Add takes and the game picks one at random each time — footsteps and gunshots stop sounding repeated.' }))
    }
    host.append(takes)

    // ---- start from ----
    const curArch = v._recipe ? archByName(v._recipe.arch) : null
    const curKind = curArch ? kindOfArch(curArch.name) : (s._fitted?.arch ? kindOfArch(s._fitted.arch) : null)
    const kinds = el('div', { class: 'kinds' }, KINDS.map((k) => el('button', {
      class: 'kind' + (curKind?.id === k.id ? ' on' : ''), title: k.hint, onclick: () => fromKind(k),
    }, el('span', { class: 'ico' }, icon(k.icon)), el('span', { class: 'kl', text: k.label }))))
    const gen = el('div', { class: 'gen-row' },
      el('button', { class: 'primary', title: 'another sound of the same kind (R)', onclick: randomize }, icon('randomize'), 'Randomize'),
      el('button', { title: 'the same sound, a little different (V)', disabled: !v._recipe, onclick: vary }, icon('vary'), 'Vary'),
      el('span', { class: 'sp' }),
      el('button', { title: 'make a recipe that sounds like a WAV you have', onclick: () => ctx.openMatch() }, icon('match'), 'Match a recording…'))
    const origin = curArch
      ? `made from the ${curKind?.label ?? curArch.name} recipe`
      : s._fitted ? `matched to ${s._fitted.ref}` : 'hand-made'
    host.append(card('Start from', el('div', {},
      el('p', { class: 'note', html: 'Pick what kind of sound this is. Every press makes a new one — keep pressing until one is close, then shape it below. <b>Undo</b> brings back the last one.' }),
      kinds, gen), { key: 'start', ui: ctx.ui, subtitle: origin }))

    // ---- shape ----
    host.append(card('Shape', shapeBody(s, vi), { key: 'shape', ui: ctx.ui,
      subtitle: curArch ? `${curArch.params.length} recipe knobs` : 'volume, pitch, length, brightness' }))

    // ---- layers + timing ----
    host.append(card('Layers', renderLayers(ctx, s, vi, editApi()), { key: 'layers', ui: ctx.ui, open: false,
      subtitle: `${v.patch.layers?.length ?? 0} layer${(v.patch.layers?.length ?? 0) === 1 ? '' : 's'}${s.resonators?.length ? ` · ${s.resonators.length} rings` : ''} — full control` }))
    host.append(card('Timing & notes', renderTiming(ctx, s, vi, editApi()), { key: 'timing', ui: ctx.ui, open: false,
      subtitle: v.gesture?.length ? `${v.gesture.length} step${v.gesture.length === 1 ? '' : 's'}` : 'plays once, at its own pitch' }))

    scroller.scrollTop = keep
    hear(s)
  }

  // ---- the shape card ----
  function shapeBody (s, vi) {
    const v = s.variants[vi]
    const body = el('div', {})

    const universal = el('div', { class: 'sliders' })
    dom.volume = slider({
      label: 'Volume', hint: 'how loud, in decibels', min: -40, max: 12, step: 0.5, def: 0, value: s.gainDb ?? 0,
      fmt: (x) => `${x > 0 ? '+' : ''}${x.toFixed(1)} dB`,
      onInput: (x) => { const c = structuredClone(sound()); c.gainDb = x; preview(c) },
      onChange: (x) => {
        ctx.store.mutate('volume', (d) => { if (x === 0) delete d.sounds[idx()].gainDb; else d.sounds[idx()].gainDb = x }, { coalesce: true })
        hear(sound(), { play: true })
      },
    })
    universal.append(dom.volume)
    for (const k of SHAPE) {
      universal.append(slider({
        label: k.label, hint: k.hint, min: k.lo, max: k.hi, step: k.step, def: k.def,
        value: v._shape?.[k.key] ?? 0, fmt: k.fmt,
        onInput: async (x) => { const c = await shaped(k.key, x); if (c) preview(c) },
        onChange: async (x) => {
          const c = await shaped(k.key, x)
          if (!c) return
          ctx.store.mutate(k.label.toLowerCase(), (d) => { d.sounds[idx()] = c }, { coalesce: true })
          hear(sound(), { play: true })
        },
      }))
    }
    body.append(universal)

    const arch = v._recipe ? archByName(v._recipe.arch) : null
    if (arch) {
      body.append(el('div', { class: 'sub', style: 'margin-top:16px', text: `${kindOfArch(arch.name)?.label ?? arch.name} recipe` }))
      const grid = el('div', { class: 'sliders' })
      const groups = new Map()
      arch.params.forEach((p, pi) => {
        const d = describeParam(p.name)
        if (!groups.has(d.group)) groups.set(d.group, [])
        groups.get(d.group).push({ p, pi, d })
      })
      for (const [g, items] of groups) {
        grid.append(el('div', { class: 'group-label', text: g }))
        for (const { p, pi, d } of items) {
          grid.append(slider({
            label: d.label, hint: `${g} · ${d.label} (${p.name})`, min: 0, max: 1, step: 0.001, def: 0.5,
            value: v._recipe.u[pi] ?? 0.5, fmt: (u) => formatValue(denorm(p, u), d.unit),
            parse: (t) => undenorm(p, t, d.unit),
            onInput: async (u) => { const c = await withU(pi, u); if (c) preview(c) },
            onChange: async (u) => {
              const c = await withU(pi, u)
              if (!c) return
              ctx.store.mutate(`${g.toLowerCase()} ${d.label}`, (dd) => { dd.sounds[idx()] = c }, { coalesce: true })
              hear(sound(), { play: true })
            },
          }))
        }
      }
      body.append(grid)
    } else {
      body.append(el('p', { class: 'note', style: 'margin-top:12px', text: s._fitted
        ? 'This sound was matched before recipes were kept, so only the four knobs above apply. Match it again, or pick a kind in “Start from”, to get its recipe knobs.'
        : 'A hand-made sound: the four knobs above work on it, and Layers below has every detail. Pick a kind in “Start from” for a sound with its own knobs.' }))
    }
    return body
  }

  /** Typed text -> normalised u, for a recipe slider's click-to-type. */
  function undenorm (p, text, unit) {
    let x = parseFloat(String(text).replace(',', '.'))
    if (!Number.isFinite(x)) return NaN
    if (unit === 'Hz' && /k/i.test(text)) x *= 1000
    if (unit === 'ms' && /\bs\b/.test(text) && !/ms/.test(text)) x *= 1000
    x = Math.min(p.hi, Math.max(p.lo, x))
    return p.log ? Math.log(x / p.lo) / Math.log(p.hi / p.lo) : (x - p.lo) / (p.hi - p.lo)
  }

  async function shaped (key, x) {
    const s = structuredClone(sound())
    if (!s) return null
    ensureBase(s, take())
    const v = s.variants[take()]
    v._shape = { ...(v._shape ?? {}), [key]: x }
    for (const k of Object.keys(v._shape)) if (!v._shape[k]) delete v._shape[k]
    try { return await compose(s) } catch (e) { ctx.log(e.message, 'err'); return null }
  }

  async function withU (pi, u) {
    const s = structuredClone(sound())
    if (!s) return null
    const v = s.variants[take()]
    if (!v._recipe) return null
    v._recipe.u = [...v._recipe.u]
    v._recipe.u[pi] = Math.round(u * 10000) / 10000
    try { return await compose(s) } catch (e) { ctx.log(e.message, 'err'); return null }
  }

  // ---- takes ----
  async function addTake () {
    const s = sound()
    const v = s.variants[take()]
    const copy = structuredClone(v)
    delete copy.weight
    const i = idx()
    ctx.store.mutate('add take', (d) => { d.sounds[i].variants.push(copy) }, { structural: true })
    const k = sound().variants.length - 1
    ctx.select(i, k)
    // A recipe take gets a nearby variation straight away -- a copy that
    // sounds identical is not what anyone adds a take for.
    if (copy._recipe) await vary()
  }

  function removeTake () {
    const i = idx()
    const vi = take()
    ctx.store.mutate('remove take', (d) => { d.sounds[i].variants.splice(vi, 1) }, { structural: true })
    ctx.select(i, Math.max(0, vi - 1))
  }

  // ---- the editing API the layer and timing editors use ----
  function editApi () {
    return {
      /** Live, while dragging: render a candidate, touch nothing. */
      preview: (fn) => {
        const c = structuredClone(sound())
        fn(c, c.variants[take()])
        detach(c, take())
        preview(c)
      },
      /** Commit one undoable step. A hand edit detaches the take from its recipe. */
      commit: (label, fn, { structural = false } = {}) => {
        const i = idx()
        const vi = take()
        ctx.store.mutate(label, (d) => {
          fn(d.sounds[i], d.sounds[i].variants[vi])
          detach(d.sounds[i], vi)
        }, { coalesce: !structural, structural })
        if (!structural) hear(sound(), { play: true })
      },
    }
  }

  // ---- the play button and playhead ----
  function paintPlay () {
    // The button is the top bar's, not the stage's: it outlives every rebuild.
    const b = document.getElementById('play')
    const a = ctx.audition
    b.textContent = a.playing ? '■' : '▶'
    b.classList.toggle('playing', a.playing)
    b.disabled = !a.ready
    const s = sound()
    const name = document.getElementById('play-name')
    name.textContent = s ? `${s.name}${s.variants.length > 1 ? ` · take ${String.fromCharCode(65 + take())}` : ''}` : ''
    name.title = name.textContent
  }
  ctx.audition.on(() => { paintPlay(); if (ctx.audition.playing) runPlayhead() })

  let raf = 0
  function runPlayhead () {
    cancelAnimationFrame(raf)
    const step = () => {
      const a = ctx.audition
      if (!dom.playhead || !a.playing || !a.buffer || !a.audio) { if (dom.playhead) dom.playhead.style.display = 'none'; return }
      const dur = a.buffer.duration
      let t = a.audio.currentTime - (ctx.playStartedAt ?? 0)
      if (a.loop) t %= dur
      dom.playhead.style.display = 'block'
      dom.playhead.style.left = `${Math.min(100, (t / dur) * 100)}%`
      raf = requestAnimationFrame(step)
    }
    raf = requestAnimationFrame(step)
  }

  // ---- when to rebuild ----
  ctx.on((what) => {
    if (ctx.view !== 'sounds' && what !== 'opened') return
    if (what === 'select' || what === 'opened') { listen.solo = -1; listen.mute.clear() }
    if (what === 'select' || what === 'opened' || what === 'structural') build()
    // A policy edit (volume, loop, live) changes the render but not the page.
    else if (what === 'change' && sound()) hear(sound())
  })

  // Keyboard: R randomizes, V varies -- the two presses you make most.
  document.addEventListener('keydown', (e) => {
    if (ctx.view !== 'sounds' || e.metaKey || e.ctrlKey || e.altKey) return
    if (/^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) && e.target.type !== 'range' && e.target.type !== 'checkbox') return
    if (!sound()) return
    if (e.key === 'r') { e.preventDefault(); randomize() }
    if (e.key === 'v') { e.preventDefault(); vary() }
  })

  return { build, hear, level, generate, randomize, vary, fromKind, get report () { return lastReport } }
}
