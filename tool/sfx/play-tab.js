//
// The Play tab: the real engine, under load, with the refusals visible.
//
// Everything else in this tool shows you what a sound IS. This is the only
// place that shows you what a BANK DOES -- because cooldowns, instance caps,
// bus budgets, stealing and priority do not exist until sounds compete for a
// finite pool in real time. A designer can read `maxInstances: 3` in the rail
// and have no idea it is the reason half their shotgun blasts are missing.
//
// So the tab is built around refusals rather than around playback. "rejected
// 40" is useless; "rejected 40: 38 cooldown, 2 bus budget" tells you which
// number to change. Every reason the engine counts is shown separately, which
// is why WebSfx::diagJson had to grow rejectedNoVoices -- README.md advertised
// six reasons and the JS face reported five.
//
import { SfxClient, brad, q16 } from './sfx-client.js'
import { writeProj } from './proj.js'

const HTML = `
<section>
  <h2>Engine</h2>
  <div class="row">
    <button id="pl-start" class="primary">Start the engine</button>
    <button id="pl-reload" disabled>Reload bank</button>
    <button id="pl-panic" disabled>Stop everything</button>
    <span class="sp" style="flex:1"></span>
    <span class="chip" id="pl-state">not started</span>
  </div>
  <div class="hint" id="pl-cache"></div>
</section>

<section>
  <h2>Trigger</h2>
  <div class="row">
    <select id="pl-sound"></select>
    <button id="pl-once" disabled>Play once</button>
    <button id="pl-spam" disabled>Hold to flood</button>
    <label>rate <input type="number" id="pl-rate" class="narrow" value="20" min="1" max="200"></label>
    <span class="hint">per second</span>
  </div>
  <div class="row">
    <label><input type="checkbox" id="pl-random"> random sound each trigger</label>
    <label><input type="checkbox" id="pl-moving" checked> from random positions</label>
  </div>
  <div class="hint">A flood is the point: the engine is designed to refuse
    politely rather than stutter, and this is where you can see it do it.</div>
</section>

<section>
  <h2>World</h2>
  <canvas id="pl-field" style="height:260px; cursor:crosshair; touch-action:none"></canvas>
  <div class="hint">Drag to move the listener. Each dot is a trigger, placed
    where it fired and fading as it ages. The wedge is the way you are facing —
    sim yaw about +Z, not a camera's.</div>
</section>

<section>
  <h2>Load</h2>
  <div id="pl-counters"></div>
</section>

<section>
  <h2>Refusals</h2>
  <div id="pl-rejects"></div>
  <div class="hint" id="pl-verdict"></div>
</section>`

export function initPlayTab (ctx) {
  const host = document.getElementById('play-host')
  host.innerHTML = HTML
  const $ = (id) => document.getElementById(id)

  let client = null
  let spamming = false
  let spamTimer = null
  let seed = 1
  const events = []          // recent triggers, for the field
  let listener = { x: 0, y: 0, yaw: 0 }
  let raf = null

  // ---- the field -----------------------------------------------------------
  const RANGE = 30           // world units across the view

  function drawField () {
    const c = $('pl-field')
    const dpr = globalThis.devicePixelRatio || 1
    const w = c.clientWidth
    const h = c.clientHeight
    if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr) }
    const g = c.getContext('2d')
    g.setTransform(dpr, 0, 0, dpr, 0, 0)
    const css = (n, f) => getComputedStyle(document.documentElement).getPropertyValue(n).trim() || f

    g.fillStyle = '#101318'
    g.fillRect(0, 0, w, h)

    const scale = Math.min(w, h) / RANGE
    const cx = w / 2
    const cy = h / 2
    const px = (x) => cx + (x - listener.x) * scale
    const py = (y) => cy - (y - listener.y) * scale

    // Distance rings at the attenuation defaults, so the numbers in the rail
    // have somewhere to be seen.
    g.strokeStyle = css('--edge', '#32363f')
    for (const r of [3.333, 12.5, 25]) {
      g.beginPath(); g.arc(cx, cy, r * scale, 0, Math.PI * 2); g.stroke()
    }
    g.fillStyle = css('--dim', '#8a8f9c')
    g.font = '10px ui-monospace, monospace'
    g.fillText('25 u (silent)', cx + 25 * scale * 0.7, cy - 25 * scale * 0.7)

    const now = performance.now()
    for (let i = events.length - 1; i >= 0; i--) {
      const e = events[i]
      const age = (now - e.at) / 1200
      if (age > 1) { events.splice(i, 1); continue }
      g.globalAlpha = 1 - age
      g.fillStyle = e.rejected ? css('--bad', '#e07070') : css('--wave', '#6fd66f')
      g.beginPath()
      g.arc(px(e.x), py(e.y), 3 + age * 9, 0, Math.PI * 2)
      g.fill()
    }
    g.globalAlpha = 1

    // The listener, with a facing wedge.
    g.fillStyle = css('--accent', '#64b5f6')
    g.beginPath(); g.arc(cx, cy, 6, 0, Math.PI * 2); g.fill()
    g.beginPath()
    g.moveTo(cx, cy)
    g.arc(cx, cy, 26, -listener.yaw - 0.45, -listener.yaw + 0.45)
    g.closePath()
    g.globalAlpha = 0.25
    g.fill()
    g.globalAlpha = 1
  }

  function tick () {
    drawField()
    raf = requestAnimationFrame(tick)
  }

  // ---- diagnostics ---------------------------------------------------------
  const REJECTS = [
    ['rejectedCooldown', 'cooldown', 'lower cooldownMs on the sound'],
    ['rejectedMaxInstances', 'instance cap', 'raise maxInstances, or set onLimit to stealOldest'],
    ['rejectedBusBudget', 'bus budget', 'raise the bus budget in the policy rail'],
    ['rejectedPriority', 'outranked', 'a higher-priority instance was playing; raise this sound\'s priority'],
    ['rejectedNoVoices', 'no voices', 'the synth pool is exhausted — cache more sounds, or use fewer layers'],
    ['rejectedUnknown', 'unknown sound', 'the id did not resolve'],
  ]

  function paintDiag (d) {
    if (!d) return
    const bar = (label, v, of, colour) => {
      const pct = of > 0 ? Math.min(100, (v / of) * 100) : 0
      return `<div class="row" style="gap:6px">
        <span style="width:116px; color:var(--dim); font-size:11px">${label}</span>
        <span style="flex:1; height:8px; background:var(--panel2); border-radius:4px; overflow:hidden">
          <span style="display:block; height:100%; width:${pct}%; background:${colour}"></span>
        </span>
        <span class="val">${v}${of ? ' / ' + of : ''}</span></div>`
    }
    $('pl-counters').innerHTML =
      bar('instances', d.instances, 48, 'var(--accent)')
      + bar('retiring', d.retiring, 48, 'var(--dim)')
      + bar('synth voices', d.synthVoices, 48, 'var(--warn)')
      + bar('peak instances', d.peakInstances, 48, 'var(--accent2)')
      + `<div class="row"><span class="hint">played ${d.played} · stolen ${d.stolen}
         · live ${d.liveInstances} / cached ${d.cachedInstances}</span></div>`
      + `<div class="row"><span class="hint">queue high water ${d.queueHighWater}
         ${d.queueDropped ? ' · <b style="color:var(--bad)">dropped ' + d.queueDropped + '</b>' : ''}</span></div>`

    const total = REJECTS.reduce((a, [k]) => a + (d[k] ?? 0), 0)
    $('pl-rejects').innerHTML = total === 0
      ? '<div class="hint">Nothing has been refused.</div>'
      : REJECTS.filter(([k]) => d[k] > 0)
          .map(([k, label, fix]) =>
            `<div class="prob"><span class="dot warning"></span><div>
               <div><b>${d[k]}</b> ${label}</div>
               <div class="where">${fix}</div></div></div>`).join('')

    // The honest summary. A refusal is not a fault -- it is the design working
    // -- and the number that would be a fault is a dropped command, which means
    // the control thread outran the audio thread.
    $('pl-verdict').textContent = total === 0
      ? ''
      : `${total} refusals out of ${d.played + total} triggers. `
        + 'Refusing is the design: the alternative is stealing something the '
        + 'player was listening to, or running out of voices mid-explosion.'
  }

  // ---- triggering ----------------------------------------------------------
  function fire () {
    if (!client?.loaded) return
    const doc = ctx.store.doc
    if (!doc.sounds.length) return
    const idx = $('pl-random').checked
      ? Math.floor(Math.random() * doc.sounds.length)
      : Number($('pl-sound').value)
    if (!(idx >= 0)) return

    let x = listener.x
    let y = listener.y
    if ($('pl-moving').checked) {
      const a = Math.random() * Math.PI * 2
      const r = 2 + Math.random() * 14
      x = listener.x + Math.cos(a) * r
      y = listener.y + Math.sin(a) * r
    }
    // A caller-supplied seed, always. It drives variant choice, pitch and gain
    // jitter and the synthesis noise in a fixed order, so two machines that
    // agree on the simulation agree on the sound.
    client.playNow(idx, { seed: (seed = (seed * 1664525 + 1013904223) >>> 0),
                          pos: [q16(x), q16(y), 0] })
    events.push({ x, y, at: performance.now(), rejected: false })
    if (events.length > 200) events.shift()
  }

  function setSpam (on) {
    spamming = on
    $('pl-spam').classList.toggle('active', on)
    clearInterval(spamTimer)
    if (on) {
      const rate = Math.max(1, Math.min(200, Number($('pl-rate').value) || 20))
      spamTimer = setInterval(fire, 1000 / rate)
    }
  }

  // ---- engine lifecycle ----------------------------------------------------
  async function start () {
    $('pl-start').disabled = true
    $('pl-state').textContent = 'starting…'
    try {
      client = await SfxClient.create()
      ctx.engine = client
      client.addEventListener('diag', (e) => paintDiag(e.detail))
      client.addEventListener('fatal', (e) => {
        ctx.log(`engine: ${e.detail}`, 'err')
        $('pl-state').textContent = 'engine stopped'
      })
      await reload()
      // Do not arm the triggers until the clock is moving, or the first thing
      // someone does produces silence and looks broken.
      const live = await client.waitForClock()
      if (!live) {
        ctx.log('the audio device did not start; triggers may be queued until it does', 'warn')
      }
      for (const id of ['pl-once', 'pl-spam', 'pl-reload', 'pl-panic']) $(id).disabled = false
      $('pl-start').textContent = 'Engine running'
    } catch (e) {
      ctx.log(`engine: ${e.message}`, 'err')
      $('pl-state').textContent = 'failed'
      $('pl-start').disabled = false
    }
  }

  async function reload () {
    if (!client) return
    $('pl-state').textContent = 'loading bank…'
    $('pl-state').classList.add('busy')
    await client.resume()
    const r = await client.loadBank(writeProj(ctx.store.doc))
    $('pl-state').classList.remove('busy')
    if (!r.report?.ok) {
      $('pl-state').textContent = 'bank has errors'
      for (const e of r.report?.errors ?? []) ctx.log(`engine: ${e.text ?? e}`, 'err')
      return
    }
    $('pl-state').textContent = `${r.sounds} sounds live`
    ctx.engineGen = ctx.docGen
    const info = await client.info()
    const d = await client.getDiag()
    paintDiag(d)
    $('pl-cache').textContent =
      `${d.cachedVariants} variants cached, ${d.liveVariants} live · `
      + `${(d.cacheBytes / 1048576).toFixed(2)} MB of `
      + `${(d.cacheBudgetBytes / 1048576).toFixed(0)} MB budget`
    fillSounds()
    return info
  }

  function fillSounds () {
    const sel = $('pl-sound')
    const keep = sel.value
    sel.replaceChildren(...ctx.store.doc.sounds.map((s, i) => {
      const o = document.createElement('option')
      o.value = String(i)
      o.textContent = s.name
      return o
    }))
    sel.value = keep && Number(keep) < ctx.store.doc.sounds.length
      ? keep
      : String(Math.max(0, ctx.sel.sound))
  }

  // ---- wiring --------------------------------------------------------------
  $('pl-start').onclick = start
  $('pl-reload').onclick = reload
  $('pl-panic').onclick = () => { setSpam(false); client?.stopAll(0) }
  $('pl-once').onclick = fire
  $('pl-rate').onchange = () => { if (spamming) setSpam(true) }

  const spam = $('pl-spam')
  spam.onpointerdown = (e) => { spam.setPointerCapture(e.pointerId); setSpam(true) }
  spam.onpointerup = () => setSpam(false)
  spam.onpointercancel = () => setSpam(false)

  const field = $('pl-field')
  let dragging = false
  const moveTo = (e) => {
    const r = field.getBoundingClientRect()
    const scale = Math.min(r.width, r.height) / RANGE
    listener.x += (e.clientX - r.left - r.width / 2) / scale * 0.08
    listener.y -= (e.clientY - r.top - r.height / 2) / scale * 0.08
    client?.listener(q16(listener.x), q16(listener.y), q16(0), brad(listener.yaw))
  }
  field.onpointerdown = (e) => { dragging = true; field.setPointerCapture(e.pointerId); moveTo(e) }
  field.onpointermove = (e) => { if (dragging) moveTo(e) }
  field.onpointerup = () => { dragging = false }
  field.onpointercancel = () => { dragging = false }

  ctx.on((what) => {
    if (what === 'opened' || what === 'structural') fillSounds()
    if (what === 'select' && ctx.tab === 'play') {
      const sel = $('pl-sound')
      if (ctx.sel.sound >= 0) sel.value = String(ctx.sel.sound)
    }
  })

  return {
    activate () {
      fillSounds()
      if (!raf) tick()
      // The worklet's bank goes stale as you edit. Reload on entry rather than
      // silently playing the version you had ten edits ago.
      if (client?.loaded && ctx.store.dirty) reload()
    },
    deactivate () {
      setSpam(false)
      client?.stopAll(2)
      if (raf) { cancelAnimationFrame(raf); raf = null }
    },
    get client () { return client },
    reload,
    fire,
  }
}
