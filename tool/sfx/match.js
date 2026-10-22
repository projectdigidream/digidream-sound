//
// Match a recording: the fitter, as three steps instead of a control panel.
//
//   1. THE RECORDING   drop a WAV, and optionally drag across the part you mean
//   2. WHAT IT IS      the same kinds as "Start from", or "not sure"
//   3. THE MATCH       hear the recording and the match side by side; use it
//
// What the old Fit tab exposed -- architecture names, restarts, iterations,
// loss weights -- is still here, folded under "Advanced", with the documented
// defaults. The search itself is unchanged: one restart per worker call, so it
// stays cancellable and shows progress, plus the extra restart seeded from the
// recording's own spectral peaks (seed -1), exactly as dsound_fit runs it.
//
// "Not sure" scans every architecture briefly and refines the best one. That
// is the honest version of automatic: it costs a minute, and says so.
//
// The result is written as a RECIPE (`_recipe`), so a matched sound keeps its
// knobs, and with `_fitted` provenance naming the recording. The recording
// itself is analysed and dropped; nothing but numbers reaches the bank.
//
import { el, choice } from './controls.js'
import { drawEnvelope } from './waveform.js'
import { KINDS, archsOfKind, kindOfArch } from './recipes.js'
import { parseNum } from './proj.js'
import { icon } from './icons.js'

const QUALITY = {
  quick: { label: 'Quick', restarts: 1, iters: 25 },
  good: { label: 'Good', restarts: 2, iters: 40 },
  best: { label: 'Best', restarts: 4, iters: 60 },
}

export function initMatch (ctx) {
  const sheet = document.getElementById('match')
  const scrim = document.getElementById('scrim')

  let target = null       // Float32Array at 48 kHz, the whole recording
  let range = null        // {from, to} in samples
  let refName = ''
  let refBuffer = null
  let bandHz = 24000
  let kindId = null       // null = not sure
  let quality = 'good'
  let running = false
  let cancelled = false
  let best = null         // {loss, arch, u, patch, render}
  let adv = { stft: 1, env: 0.05, bands: 0.04 }
  let step = 1
  let doneText = null
  let src = null

  function open () {
    const v = ctx.store.doc.sounds[ctx.sel.sound]?.variants[ctx.sel.variant]
    if (!ctx.store.doc.sounds[ctx.sel.sound]) { ctx.log('select or make a sound first: the match goes into it', 'warn'); return }
    const k = v?._recipe ? kindOfArch(v._recipe.arch) : null
    if (k) kindId = k.id
    step = target ? 2 : 1
    sheet.classList.add('open')
    scrim.classList.add('open')
    ctx.sheetOpen = close
    paint()
  }
  function close () {
    cancelled = true
    stopRef()
    sheet.classList.remove('open')
    scrim.classList.remove('open')
    ctx.sheetOpen = null
  }

  // ---- the recording ------------------------------------------------------
  function bandOf (bytes) {
    // The recording's own bandwidth, from its header: an 11 kHz WAV has
    // nothing above 5.5 kHz, and scoring those bins would score the
    // resampler. Anything that is not a WAV is assumed full-band.
    const v = new DataView(bytes)
    const tag = (at) => String.fromCharCode(v.getUint8(at), v.getUint8(at + 1), v.getUint8(at + 2), v.getUint8(at + 3))
    if (bytes.byteLength < 44 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return 24000
    for (let at = 12; at + 8 <= bytes.byteLength;) {
      const size = v.getUint32(at + 4, true)
      if (tag(at) === 'fmt ') return Math.min(24000, v.getUint32(at + 12, true) / 2)
      at += 8 + size + (size & 1)
    }
    return 24000
  }

  async function load (file) {
    const bytes = await file.arrayBuffer()
    bandHz = bandOf(bytes)
    let buf
    try {
      buf = await ctx.audition.context().decodeAudioData(bytes.slice(0))
    } catch {
      ctx.log(`${file.name}: not audio this browser can read`, 'err')
      return
    }
    const n = buf.length
    const mono = new Float32Array(n)
    for (let c = 0; c < buf.numberOfChannels; c++) {
      const ch = buf.getChannelData(c)
      for (let i = 0; i < n; i++) mono[i] += ch[i] / buf.numberOfChannels
    }
    target = mono
    refBuffer = buf
    refName = file.name
    range = null
    best = null
    paint()
  }

  const envOf = (s, buckets = 400) => {
    const n = Math.min(buckets, s.length)
    const min = new Float32Array(n)
    const max = new Float32Array(n)
    const per = s.length / n
    for (let b = 0; b < n; b++) {
      let lo = 0, hi = 0
      for (let i = Math.floor(b * per); i < Math.min(s.length, Math.floor((b + 1) * per)); i++) {
        if (s[i] < lo) lo = s[i]
        if (s[i] > hi) hi = s[i]
      }
      min[b] = lo; max[b] = hi
    }
    return { min, max }
  }

  function drawRef (c) {
    drawEnvelope(c, envOf(target), { clip: false })
    if (!range) return
    const g = c.getContext('2d')
    const w = c.clientWidth, h = c.clientHeight
    const x0 = (range.from / target.length) * w
    const x1 = (range.to / target.length) * w
    g.fillStyle = '#111318c0'
    g.fillRect(0, 0, x0, h); g.fillRect(x1, 0, w - x1, h)
    g.fillStyle = '#5aa9ff'
    g.fillRect(x0, 0, 2, h); g.fillRect(x1 - 2, 0, 2, h)
  }

  function playBuffer (buffer, from = 0, to = null) {
    stopRef()
    const c = ctx.audition.context()
    src = c.createBufferSource()
    src.buffer = buffer
    src.connect(c.destination)
    const t0 = from / 48000
    src.start(0, t0, to ? (to - from) / 48000 : undefined)
  }
  function stopRef () { try { src?.stop() } catch { /* done */ } src = null }

  // ---- the search ---------------------------------------------------------
  function candidates () {
    const kinds = kindId ? [KINDS.find((k) => k.id === kindId)] : KINDS
    return kinds.flatMap((k) => archsOfKind(k, ctx.archs))
  }

  function estimate () {
    const q = QUALITY[quality]
    const list = candidates()
    const frames = range ? range.to - range.from : (target?.length ?? 48000)
    // Measured: a Chrome worker runs about 4x the native per-render figure.
    const per = 0.011 * Math.max(0.25, frames / 43200)
    const avgP = list.reduce((a, x) => a + x.params.length + 1, 0) / Math.max(1, list.length)
    const scan = list.length > 1 ? list.length * 12 * avgP * per : 0
    const refine = (q.restarts + 1) * q.iters * avgP * per
    return Math.round(scan + refine)
  }

  async function run () {
    if (!target || running) return
    running = true
    cancelled = false
    best = null
    step = 3
    paint()
    const q = QUALITY[quality]
    const slice = range ? target.slice(range.from, range.to) : target.slice()
    await ctx.client.fitTarget(slice, bandHz)
    const list = candidates()
    const t0 = performance.now()
    const say = (t, frac) => { const s = sheet.querySelector('#m-status'); if (s) s.textContent = t; const b = sheet.querySelector('#m-bar'); if (b) b.style.width = `${Math.round(frac * 100)}%` }

    // Scan: a short descent from the centre of every candidate, when there
    // is more than one, so the refine spends its time on the right one.
    let pickArch = list[0]
    if (list.length > 1) {
      let bestScan = Infinity
      for (let k = 0; k < list.length && !cancelled; k++) {
        say(`Trying ${kindOfArch(list[k].name)?.label ?? list[k].name} (${k + 1} of ${list.length})…`, (k / list.length) * 0.4)
        const cfg = await ctx.client.fitConfigure({ arch: list[k].name, ...adv })
        if (!cfg.ok) continue
        const r = await ctx.client.fitOnce({ iters: 12, seed: 0 })
        if (r.finite && r.loss < bestScan) {
          bestScan = r.loss
          pickArch = list[k]
          best = { loss: r.loss, arch: r.arch, u: r.u, patch: r.patch }
          await showBest()
        }
      }
    }
    if (!cancelled && pickArch) {
      const cfg = await ctx.client.fitConfigure({ arch: pickArch.name, ...adv })
      const total = q.restarts + 1
      for (let r = 0; cfg.ok && r < total && !cancelled; r++) {
        say(`Refining ${kindOfArch(pickArch.name)?.label ?? pickArch.name}: pass ${r + 1} of ${total}…`,
          (list.length > 1 ? 0.4 : 0) + (r / total) * (list.length > 1 ? 0.6 : 1))
        const res = await ctx.client.fitOnce({ iters: q.iters, seed: r === total - 1 ? -1 : r })
        if (res.error) { ctx.log(`match: ${res.error}`, 'err'); break }
        if (res.finite && (!best || res.loss < best.loss)) {
          best = { loss: res.loss, arch: res.arch, u: res.u, patch: res.patch }
          await showBest()
        }
        if (res.heapMb > 140) { ctx.log('match: stopping before memory runs out', 'warn'); break }
      }
    }
    running = false
    doneText = null
    const secs = ((performance.now() - t0) / 1000).toFixed(0)
    doneText = best ? `${cancelled ? 'Stopped' : 'Done'} in ${secs} s. Closeness: ${closeness(best.loss)}.` : 'Nothing matched.'
    paint()
    say(doneText, 1)
  }

  // The loss means nothing to a person. This maps it onto words, from what
  // the DOOM refit measured: most good matches land between 2 and 5.
  const closeness = (loss) => loss < 2 ? 'very close' : loss < 3.5 ? 'close' : loss < 5 ? 'rough' : 'loose'

  async function showBest () {
    const patch = typeof best.patch === 'string' ? JSON.parse(best.patch) : best.patch
    try {
      const r = await ctx.client.audition({ name: 'match', variants: [{ patch }] }, 0)
      if (r.ok) best.render = r
    } catch { /* the picture is a nicety */ }
    paintResult()
  }

  function paintResult () {
    const c = sheet.querySelector('#m-res')
    if (c && best?.render) drawEnvelope(c, best.render, { clip: false })
    const btn = sheet.querySelector('#m-hear')
    if (btn) btn.disabled = !best?.render
    const use = sheet.querySelector('#m-use')
    if (use) use.disabled = !best || running
  }

  function hearMatch () {
    const r = best?.render
    if (!r?.left) return
    const c = ctx.audition.context()
    const b = c.createBuffer(2, r.left.length, 48000)
    b.copyToChannel(r.left, 0); b.copyToChannel(r.right ?? r.left, 1)
    playBuffer(b)
  }

  async function use () {
    if (!best) return
    const arch = ctx.archs.find((a) => a.name === best.arch)
    if (!arch) return
    const loss = Number(best.loss.toFixed(4))
    close()
    await ctx.stage.generate(arch, best.u, `match ${refName}`, {
      decorate: (snd) => {
        snd._fitted = { arch: best.arch, ref: refName, loss, at: new Date().toISOString().slice(0, 10) }
      },
    })
    ctx.log(`matched ${refName} as ${kindOfArch(best.arch)?.label ?? best.arch} (${closeness(loss)})`, 'ok')
  }

  // ---- the sheet ----------------------------------------------------------
  function paint () {
    const s = ctx.store.doc.sounds[ctx.sel.sound]
    const takeName = String.fromCharCode(65 + ctx.sel.variant)
    sheet.replaceChildren()
    const bar = el('div', { class: 'steps-bar' },
      ['1  The recording', '2  What it is', '3  The match'].map((t, k) =>
        el('span', { class: k + 1 === step ? 'on' : k + 1 < step ? 'done' : '', text: t })))
    sheet.append(el('div', { style: 'display:flex; align-items:center' },
      el('h2', { text: 'Match a recording', style: 'flex:1' }),
      el('button', { class: 'ghost', text: '✕', onclick: close })),
      el('div', { class: 'hint', text: `The match goes into ${s?.name ?? '—'}, take ${takeName}. Your recording is only listened to — nothing but numbers is saved.` }),
      bar)

    if (step === 1) {
      const inp = el('input', { type: 'file', accept: 'audio/*,.wav', style: 'display:none' })
      inp.onchange = () => { if (inp.files[0]) load(inp.files[0]) }
      const zone = el('div', { class: 'drop-zone', onclick: () => inp.click() },
        el('div', { class: 'drop-ico' }, icon('match')),
        el('div', { text: target ? `${refName} — drop another to replace it` : 'Drop a WAV here, or click to choose one' }),
        el('div', { class: 'hint', text: 'Your own recording, or one you are allowed to use.' }))
      zone.ondragover = (e) => { e.preventDefault(); zone.classList.add('hover') }
      zone.ondragleave = () => zone.classList.remove('hover')
      zone.ondrop = (e) => { e.preventDefault(); e.stopPropagation(); zone.classList.remove('hover'); const f = e.dataTransfer?.files?.[0]; if (f) load(f) }
      sheet.append(zone, inp)
      if (target) {
        const c = el('canvas', { style: 'width:100%; height:90px; margin-top:14px; cursor:crosshair; touch-action:none' })
        sheet.append(c)
        requestAnimationFrame(() => drawRef(c))
        let anchor = null
        const at = (e) => { const r = c.getBoundingClientRect(); return Math.round(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * target.length) }
        c.onpointerdown = (e) => { c.setPointerCapture(e.pointerId); anchor = at(e) }
        c.onpointermove = (e) => { if (anchor === null) return; const b = at(e); const f = Math.min(anchor, b), t = Math.max(anchor, b); range = t - f >= 4096 ? { from: f, to: t } : null; drawRef(c) }
        c.onpointerup = () => { anchor = null; paint() }
        const ms = (n) => Math.round(n / 48)
        sheet.append(el('div', { class: 'gen-row' },
          el('button', { text: '▶ Play it', onclick: () => playBuffer(refBuffer, range?.from ?? 0, range?.to ?? null) }),
          el('span', { class: 'hint', text: range
            ? `Matching ${ms(range.from)}–${ms(range.to)} ms of ${ms(target.length)} ms.`
            : `${(target.length / 48000).toFixed(2)} s. Drag across the waveform to match only one part — a real sound is often a crack, then a tail, and one part at a time matches better.` }),
          range ? el('button', { class: 'ghost', text: 'Use all of it', onclick: () => { range = null; paint() } }) : null))
      }
      sheet.append(el('div', { class: 'sheet-foot' }, el('span', { class: 'sp' }),
        el('button', { class: 'primary', text: 'Next ›', disabled: !target, onclick: () => { step = 2; paint() } })))
    }

    if (step === 2) {
      sheet.append(el('p', { class: 'note', text: 'What kind of sound is it? Picking one makes the match faster and better.' }))
      const kinds = el('div', { class: 'kinds' },
        el('button', { class: 'kind' + (kindId === null ? ' on' : ''), onclick: () => { kindId = null; paint() } },
          el('span', { class: 'ico' }, icon('unsure')), el('span', { class: 'kl', text: 'Not sure' })),
        KINDS.map((k) => el('button', { class: 'kind' + (kindId === k.id ? ' on' : ''), title: k.hint, onclick: () => { kindId = k.id; paint() } },
          el('span', { class: 'ico' }, icon(k.icon)), el('span', { class: 'kl', text: k.label }))))
      sheet.append(kinds)
      sheet.append(el('div', { class: 'gen-row' }, el('span', { class: 'hint', text: 'Effort' }),
        choice(Object.entries(QUALITY).map(([k, q]) => [k, q.label]), quality, (x) => { quality = x; paint() }),
        el('span', { class: 'hint', text: `about ${estimate()} s` })))
      const det = el('details', { style: 'margin-top:12px' }, el('summary', { class: 'hint', text: 'Advanced: what “close” means' }))
      const w = (k, label) => {
        const i = el('input', { type: 'text', class: 'num', value: String(adv[k]) })
        i.onchange = () => { const x = parseNum(i.value); if (Number.isFinite(x) && x >= 0) adv[k] = x; i.value = String(adv[k]) }
        return el('label', { class: 'check', style: 'margin-right:14px' }, label, i)
      }
      det.append(el('div', { class: 'gen-row' }, w('stft', 'spectrum'), w('env', 'loudness shape'), w('bands', 'octave balance')),
        el('div', { class: 'hint', text: 'Weights of the three things the match compares. A zero turns one off, which is rarely what you want.' }))
      sheet.append(det)
      sheet.append(el('div', { class: 'sheet-foot' },
        el('button', { class: 'ghost', text: '‹ Back', onclick: () => { step = 1; paint() } }),
        el('span', { class: 'sp' }),
        el('button', { class: 'primary', text: 'Find a match ›', onclick: run })))
    }

    if (step === 3) {
      const ref = el('canvas')
      const res = el('canvas', { id: 'm-res' })
      sheet.append(el('div', { class: 'cmp' },
        el('button', { text: '▶ Recording', onclick: () => playBuffer(refBuffer, range?.from ?? 0, range?.to ?? null) }), ref,
        el('button', { id: 'm-hear', text: '▶ Match', disabled: true, onclick: hearMatch }), res))
      requestAnimationFrame(() => {
        drawEnvelope(ref, envOf(range ? target.slice(range.from, range.to) : target), { clip: false })
        paintResult()
      })
      sheet.append(el('div', { style: 'margin-top:14px' },
        el('div', { class: 'bar' }, el('div', { id: 'm-bar' })),
        el('div', { class: 'hint', id: 'm-status', style: 'margin-top:6px', text: running ? 'Starting…' : doneText ?? (best ? `Closeness: ${closeness(best.loss)}.` : '') })))
      sheet.append(el('div', { class: 'sheet-foot' },
        running
          ? el('button', { text: 'Stop', onclick: () => { cancelled = true } })
          : el('button', { class: 'ghost', text: '‹ Try another kind', onclick: () => { step = 2; paint() } }),
        el('span', { class: 'sp' }),
        el('button', { id: 'm-use', class: 'primary', text: `Use it for take ${takeName}`, disabled: !best || running, onclick: use })))
    }
  }

  scrim.addEventListener('click', () => { if (ctx.sheetOpen === close) close() })
  return { open, close, get busy () { return running } }
}
