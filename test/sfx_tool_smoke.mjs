// The editor, in a real browser.
//
// Everything else in test/ runs under Node, which is deliberate -- the gate
// stays offline and needs no Chrome. But the tool is a page, and a page that
// has only ever been reasoned about is a page that does not work. This drives
// it through window.__sfx, which is the live subsystems rather than a bespoke
// test API, so what it exercises is what a finger would.
//
// Needs a dev server and Chrome, and skips cleanly without either:
//
//   node tool/serve.js &
//   node test/sfx_tool_smoke.mjs
//
// puppeteer-core is not a dependency of this repo. Install it in a scratch
// directory and run from there, or set PUPPETEER_DIR.
//
import { createRequire } from 'node:module'
import { existsSync } from 'node:fs'

const BASE = process.env.DSND_TOOL_URL ?? 'http://127.0.0.1:8940/tool/sfx.html'
const CHROME = process.env.CHROME
  ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'

let failures = 0
let checks = 0
const check = (cond, what) => {
  checks++
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

let puppeteer
for (const dir of [process.env.PUPPETEER_DIR, process.cwd(), process.env.HOME].filter(Boolean)) {
  try {
    puppeteer = createRequire(`${dir}/package.json`)('puppeteer-core')
    break
  } catch { /* try the next one */ }
}
if (!puppeteer) {
  try { puppeteer = (await import('puppeteer-core')).default } catch { /* none */ }
}
if (!puppeteer) {
  console.log('skip: puppeteer-core not installed (set PUPPETEER_DIR to a dir that has it)')
  process.exit(0)
}
if (!existsSync(CHROME)) { console.log(`skip: no Chrome at ${CHROME}`); process.exit(0) }

try {
  const r = await fetch(BASE)
  if (!r.ok) throw new Error(String(r.status))
} catch {
  console.log(`skip: no dev server at ${BASE} -- run: node tool/serve.js &`)
  process.exit(0)
}

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'],
})
const page = await browser.newPage()

const pageErrors = []
const consoleErrors = []
const badResponses = []
page.on('pageerror', (e) => pageErrors.push(e.message))
page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()) })
page.on('response', (r) => { if (r.status() >= 400) badResponses.push(`${r.status()} ${r.url()}`) })

try {
  await page.goto(BASE, { waitUntil: 'domcontentloaded', timeout: 20000 })
  await page.click('#start-btn')

  // The global appearing IS the "booted" signal, the same way midisynth's
  // daw_smoke.mjs waits on window.__daw.
  await page.waitForFunction(
    () => globalThis.__sfx && !document.getElementById('overlay'),
    { timeout: 30000 })
  check(true, 'boots and removes the overlay')

  const ev = (fn, ...a) => page.evaluate(fn, ...a)
  const settle = (ms = 400) => new Promise((r) => setTimeout(r, ms))
  const clickText = (sel, text) => ev((sel, text) => {
    const b = [...document.querySelectorAll(sel)].find((x) => x.textContent.includes(text))
    if (!b) throw new Error(`no ${sel} "${text}"`)
    b.click()
  }, sel, text)
  // The level the page says it plays at: dry peak + the sound's volume.
  const heard = () => ev(() => {
    const t = globalThis.__sfx
    const r = t.stage.report
    const s = t.store.doc.sounds[t.sel.sound]
    return r?.info ? r.info.peakDbMilli / 1000 + (s.gainDb ?? 0) : null
  })

  // ---- the library ----
  await ev(() => globalThis.__sfx.library.idle())
  const lib = await ev(() => ({
    sounds: globalThis.__sfx.store.doc.sounds.length,
    rows: document.querySelectorAll('#lib-list .item').length,
    groups: document.querySelectorAll('#lib-list .group').length,
    painted: [...document.querySelectorAll('#lib-list .item canvas')].filter((c) => {
      const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data
      let n = 0
      for (let i = 3; i < d.length; i += 4) if (d[i] > 0) n++
      return n > 20
    }).length,
  }))
  check(lib.sounds === 7 && lib.rows === 7, `the library lists every sound (${lib.rows})`)
  check(lib.groups >= 5, `grouped by the first part of the name (${lib.groups} groups)`)
  check(lib.painted === 7, `and every row has a picture of its sound (${lib.painted}/7)`)
  await ev(() => { document.getElementById('lib-search').value = 'weapon'; document.getElementById('lib-search').dispatchEvent(new Event('input')) })
  check(await ev(() => document.querySelectorAll('#lib-list .item').length) === 2, 'search filters by name and tag')
  await ev(() => { document.getElementById('lib-search').value = ''; document.getElementById('lib-search').dispatchEvent(new Event('input')) })

  // ---- the stage and the transport ----
  await ev(() => document.querySelectorAll('#lib-list .item')[1].click())
  await page.waitForFunction(() => globalThis.__sfx.audition.ready && globalThis.__sfx.stage.report, { timeout: 15000 })
  const stage = await ev(() => ({
    name: document.getElementById('snd-name').value,
    want: globalThis.__sfx.store.doc.sounds[1].name,
    enabled: !document.getElementById('play').disabled,
  }))
  check(stage.name === stage.want, `selecting a row shows that sound (${stage.name})`)
  check(stage.enabled, 'and arms the play button')
  const played = await ev(() => { const ok = globalThis.__sfx.play(); return { ok, playing: globalThis.__sfx.audition.playing, glyph: document.getElementById('play').textContent } })
  check(played.ok && played.playing && played.glyph === '■', 'it plays, and the button becomes stop')
  await ev(() => globalThis.__sfx.togglePlay())
  check(!(await ev(() => globalThis.__sfx.audition.playing)), 'a second press stops it')

  // ---- in the game ----
  await clickText('#ingame .seg button', 'Cut the oldest')
  check(await ev(() => globalThis.__sfx.store.doc.sounds[1].onLimit) === 'stealOldest', '“Cut the oldest” writes onLimit')
  await ev(() => globalThis.__sfx.store.undo())
  check(await ev(() => globalThis.__sfx.store.doc.sounds[1].onLimit) === undefined, 'and undo takes it back out (no default written)')

  // ---- start from ----
  const archs = await ev(() => globalThis.__sfx.archs.length)
  check(archs === 32, `all ${archs} recipes are available`)
  await clickText('.kind', 'Explosion')
  await page.waitForFunction(() => globalThis.__sfx.store.doc.sounds[1].variants[0]._recipe, { timeout: 15000 })
  await settle(600)
  const made = await ev(() => {
    const t = globalThis.__sfx
    const v = t.store.doc.sounds[1].variants[0]
    const a = t.archs.find((x) => x.name === v._recipe.arch)
    return { arch: v._recipe.arch, n: v._recipe.u.length, params: a.params.length,
      sliders: document.querySelectorAll('.card .sliders .slider').length }
  })
  check(/^(explosion|burst)/.test(made.arch), `“Explosion” makes an explosion recipe (${made.arch})`)
  check(made.n === made.params, 'and records every knob of it')
  check(made.sliders >= made.params + 4, `every knob is a slider, plus the four general ones (${made.sliders})`)
  const lvl = await heard()
  check(Math.abs(lvl + 6) < 0.3, `a generated sound is levelled to -6 dB (${lvl?.toFixed(1)})`)

  const u0 = await ev(() => globalThis.__sfx.store.doc.sounds[1].variants[0]._recipe.u.slice())
  await clickText('.gen-row button', '✨ Vary')
  await settle(800)
  const varied = await ev(() => globalThis.__sfx.store.doc.sounds[1].variants[0]._recipe)
  check(varied.arch === made.arch && varied.u.some((x, i) => x !== u0[i]), 'Vary keeps the recipe and moves its knobs')

  // A recipe knob, moved the way a finger would: the track, then release.
  const before = await ev(() => JSON.stringify(globalThis.__sfx.store.doc.sounds[1].variants[0].patch))
  await ev(() => {
    const r = [...document.querySelectorAll('.card .sliders .slider')].at(-1).querySelector('input[type=range]')
    r.value = String(Number(r.value) > 500 ? 100 : 900)
    r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change'))
  })
  await settle(800)
  check(await ev(() => JSON.stringify(globalThis.__sfx.store.doc.sounds[1].variants[0].patch)) !== before,
    'moving a recipe knob rebuilds the patch')

  // ---- the four knobs, on a hand-made sound ----
  await ev(() => globalThis.__sfx.select(0, 0))
  await settle(600)
  const handPatch = await ev(() => JSON.stringify(globalThis.__sfx.store.doc.sounds[0].variants[0].patch))
  const pitchSlider = () => ev((to) => {
    const row = [...document.querySelectorAll('.card .slider')].find((x) => x.querySelector('.lbl')?.textContent === 'Pitch')
    const r = row.querySelector('input[type=range]')
    r.value = String(to); r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change'))
  }, globalThis.__to)
  await ev(() => { globalThis.__to = 750 })
  await ev(() => {
    const row = [...document.querySelectorAll('.card .slider')].find((x) => x.querySelector('.lbl')?.textContent === 'Pitch')
    const r = row.querySelector('input[type=range]'); r.value = '750'; r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change'))
  })
  await settle(800)
  const shaped = await ev(() => { const v = globalThis.__sfx.store.doc.sounds[0].variants[0]; return { shape: v._shape, base: !!v._base } })
  check(shaped.shape?.pitch > 0 && shaped.base, `Pitch works on a hand-made sound, and keeps its original (${shaped.shape?.pitch} st)`)
  await ev(() => {
    const row = [...document.querySelectorAll('.card .slider')].find((x) => x.querySelector('.lbl')?.textContent === 'Pitch')
    row.querySelector('.lbl').dispatchEvent(new MouseEvent('dblclick'))
  })
  await settle(800)
  check(await ev(() => JSON.stringify(globalThis.__sfx.store.doc.sounds[0].variants[0].patch)) === handPatch,
    'double-clicking it back to zero restores the patch exactly — the knobs never compound')
  void pitchSlider

  // ---- volume, clipping, and fixing it ----
  await ev(() => globalThis.__sfx.store.mutate('t', (d) => { d.sounds[0].gainDb = 12 }))
  await settle(700)
  const hot = await ev(() => document.querySelector('.player-info .badge.over')?.textContent ?? '')
  check(/too loud/.test(hot), `a sound turned up past the ceiling says so (${hot})`)
  await clickText('.player-info button', 'Fix level')
  await settle(800)
  const fixed = await heard()
  check(Math.abs(fixed + 6) < 0.3, `and “Fix level” brings it back to -6 dB (${fixed?.toFixed(1)})`)

  // ---- takes ----
  await ev(() => globalThis.__sfx.select(1, 0))
  await settle(500)
  await ev(() => [...document.querySelectorAll('.takes button')].find((b) => b.textContent === '＋').click())
  await settle(1200)
  const takes = await ev(() => { const s = globalThis.__sfx.store.doc.sounds[1]; return { n: s.variants.length, sel: globalThis.__sfx.sel.variant, differ: JSON.stringify(s.variants[0]._recipe.u) !== JSON.stringify(s.variants[1]._recipe?.u) } })
  check(takes.n === 2 && takes.sel === 1, 'adding a take selects it')
  check(takes.differ, 'and a recipe take starts as a variation, not a copy')

  // ---- layers: the flow, the timeline, solo ----
  await ev(() => globalThis.__sfx.select(0, 0))
  await page.waitForFunction(() => document.querySelectorAll('.lanes .lane canvas').length >= 2, { timeout: 15000 })
  const lanes = await ev(() => ({ lanes: document.querySelectorAll('.lanes .lane canvas').length, layers: globalThis.__sfx.store.doc.sounds[0].variants[0].patch.layers.length }))
  check(lanes.lanes === lanes.layers, `the timeline has one lane per layer (${lanes.lanes})`)
  const whole = await ev(() => globalThis.__sfx.stage.report.info.frames)
  await ev(() => document.querySelector('.lanes .sm button').click())
  await settle(900)
  const solo = await ev(() => ({ frames: globalThis.__sfx.stage.report.info.frames, note: document.querySelector('.listening')?.textContent ?? '' }))
  check(/alone/.test(solo.note) && solo.frames !== whole, `S plays one layer alone (${solo.frames} vs ${whole} frames)`)
  check(await ev(() => !globalThis.__sfx.store.dirty || !globalThis.__sfx.store.doc.sounds[0].variants[0]._listen), 'and never touches the document')
  await clickText('.listening button', 'Hear everything')
  await settle(700)
  await clickText('.card-head', 'Layers')
  const flow = await ev(() => document.querySelectorAll('.flow .flow-lane').length)
  check(flow === lanes.layers, `the Layers card draws how the layers combine (${flow} lanes into +)`)

  // Changing a layer's tone rebuilds the page; it must not move under you.
  await ev(() => document.getElementById('layer-1').scrollIntoView({ block: 'start' }))
  await settle(300)
  const topBefore = await ev(() => document.getElementById('layer-1').getBoundingClientRect().top)
  await ev(() => { const sel = document.querySelector('#layer-1 select'); sel.value = 'harmonics'; sel.dispatchEvent(new Event('change')) })
  await settle(900)
  const topAfter = await ev(() => document.getElementById('layer-1').getBoundingClientRect().top)
  check(Math.abs(topAfter - topBefore) < 2, `changing a layer's tone does not scroll the page (${Math.round(topBefore)} → ${Math.round(topAfter)} px)`)

  // Drawn harmonics are drawn: drag down-right across the bars.
  const bb = await ev(() => { const c = document.querySelector('#layer-1 canvas.bars'); const r = c.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height } })
  await page.mouse.move(bb.x + 4, bb.y + bb.h * 0.15); await page.mouse.down()
  for (let i = 0; i <= 16; i++) await page.mouse.move(bb.x + 4 + (bb.w - 8) * i / 16, bb.y + bb.h * (0.15 + 0.65 * i / 16))
  await page.mouse.up()
  await settle(800)
  const amps = await ev(() => globalThis.__sfx.store.doc.sounds[0].variants[0].patch.layers[1].wavetable.params.amps)
  check(amps.length === 8 && amps[0] > amps[7] + 0.3, `drawn harmonics take the shape you draw (${amps.map((x) => x.toFixed(2)).join(' ')})`)
  const hidden = await ev(() => {
    // Every folded section in a layer must exist -- folded, not missing.
    const kinds = [...document.querySelectorAll('#layer-1 details.lsec > summary .lsec-t')].map((x) => x.textContent)
    return kinds
  })
  for (const want of ['Source', 'Level and pitch', 'Loudness over time', 'Tone', 'Pitch sweep', 'Timbre sweep', 'Stack and ring', 'Strength, turns and sends', 'LFOs', 'Modulation']) {
    check(hidden.includes(want), `a tone layer offers “${want}”`)
  }
  await ev(() => globalThis.__sfx.store.undo())
  await ev(() => globalThis.__sfx.store.undo())
  await settle(500)

  // A hand edit on a recipe take detaches it -- and the card says so first.
  await ev(() => globalThis.__sfx.select(1, 0))
  await settle(600)
  const notice = await ev(() => [...document.querySelectorAll('.card')].find((c) => c.querySelector('.card-title')?.textContent === 'Layers')?.textContent ?? '')
  check(/recipe/.test(notice), 'a recipe take warns before its layers are edited by hand')

  // ---- timing (on a hand-made sound: a recipe may already use all 8 steps) ----
  await ev(() => globalThis.__sfx.select(3, 0))
  await settle(600)
  const steps0 = await ev(() => globalThis.__sfx.store.doc.sounds[3].variants[0].gesture?.length ?? 0)
  await ev(() => { const c = [...document.querySelectorAll('.card')].find((x) => x.querySelector('.card-title')?.textContent === 'Timing & notes'); if (!c.classList.contains('open')) c.querySelector('.card-head').click() })
  await clickText('.gen-row button', 'Add a step')
  await settle(500)
  check(await ev(() => globalThis.__sfx.store.doc.sounds[3].variants[0].gesture?.length ?? 0) === steps0 + 1, 'a timing step can be added')

  // ---- new, delete, undo ----
  const n0 = await ev(() => globalThis.__sfx.store.doc.sounds.length)
  await ev(() => document.getElementById('lib-new').click())
  await page.waitForFunction((n) => globalThis.__sfx.store.doc.sounds.length === n + 1
    && globalThis.__sfx.store.doc.sounds.at(-1).variants[0]._recipe, { timeout: 15000 }, n0)
  check(true, '＋ New makes a sound from a recipe')
  await ev(() => globalThis.__sfx.deleteSound(globalThis.__sfx.sel.sound))
  check(await ev(() => globalThis.__sfx.store.doc.sounds.length) === n0, 'delete removes it')
  await ev(() => globalThis.__sfx.store.undo())
  check(await ev(() => globalThis.__sfx.store.doc.sounds.length) === n0 + 1, 'and undo brings it back')

  // ---- export ----
  const baked = await ev(async () => { const t = globalThis.__sfx; const { writeProj } = await import('./sfx/proj.js'); const r = await t.client.bake(writeProj(t.store.doc)); return { ok: r.ok, n: r.bytes?.length ?? 0 } })
  check(baked.ok && baked.n > 1000, `the bank exports for the game (${baked.n} bytes)`)
  const wav = await ev(async () => { const t = globalThis.__sfx; const r = await t.client.wav(t.store.doc.sounds[0], 0); return r.ok && r.bytes.length > 44 })
  check(wav, 'a sound exports as WAV')

  // ---- match a recording ----
  // A WAV built here, so the test carries no audio: decaying noise, 11 kHz,
  // which also exercises the band limit read from the header.
  const wavBytes = (() => {
    const rate = 11025, n = 5000
    const buf = Buffer.alloc(44 + n * 2)
    buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVE', 8); buf.write('fmt ', 12)
    buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22); buf.writeUInt32LE(rate, 24)
    buf.writeUInt32LE(rate * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36); buf.writeUInt32LE(n * 2, 40)
    let x = 12345
    for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; buf.writeInt16LE(Math.round(((x / 4294967296) * 2 - 1) * 20000 * Math.exp(-i / 900)), 44 + i * 2) }
    return buf
  })()
  const { writeFileSync, mkdtempSync } = await import('node:fs')
  const { tmpdir } = await import('node:os')
  const wavPath = `${mkdtempSync(`${tmpdir()}/dsnd-`)}/hit.wav`
  writeFileSync(wavPath, wavBytes)
  await ev(() => globalThis.__sfx.select(0, 0))
  await ev(() => globalThis.__sfx.openMatch())
  const inp = await page.$('#match input[type=file]')
  await inp.uploadFile(wavPath)
  await page.waitForFunction(() => /Play it/.test(document.getElementById('match').textContent), { timeout: 15000 })
  await clickText('#match button', 'Next')
  await clickText('#match .kind', 'Hit')
  await clickText('#match .seg button', 'Quick')
  await clickText('#match button', 'Find a match')
  await page.waitForFunction(() => /Done|Nothing/.test(document.getElementById('m-status')?.textContent ?? ''), { timeout: 240000, polling: 1000 })
  const status = await ev(() => document.getElementById('m-status').textContent)
  check(/Done/.test(status), `a match is found (${status})`)
  await ev(() => document.getElementById('m-use').click())
  await page.waitForFunction(() => globalThis.__sfx.store.doc.sounds[0]._fitted, { timeout: 15000 })
  const fitted = await ev(() => { const s = globalThis.__sfx.store.doc.sounds[0]; return { f: s._fitted, r: s.variants[0]._recipe?.arch } })
  check(fitted.f.ref === 'hit.wav' && /^impact/.test(fitted.r), `and used as a recipe, with provenance (${fitted.r}, from ${fitted.f.ref})`)

  // ---- mix & test ----
  await ev(() => globalThis.__sfx.showView('mix'))
  await settle(400)
  check(await ev(() => document.querySelectorAll('#mix .bus').length) === 3, 'Mix & test shows every group')
  await ev(() => [...document.querySelectorAll('#mix .card-head')].find((h) => h.textContent.includes('Master'))?.closest('.card').classList.contains('open') || [...document.querySelectorAll('#mix .card-head')].find((h) => h.textContent.includes('Master')).click())
  await ev(() => [...document.querySelectorAll('#mix .card')].find((c) => c.textContent.includes('Mastering on')).querySelector('.switch').click())
  await settle(500)
  const master = await ev(() => ({ on: globalThis.__sfx.store.doc.master?.enabled, sliders: [...document.querySelectorAll('#mix .card')].find((c) => c.textContent.includes('Mastering on')).querySelectorAll('.slider').length }))
  check(master.on === true && master.sliders === 19, `the master chain can be switched on, with every control (${master.sliders} sliders)`)
  await ev(() => globalThis.__sfx.store.undo())
  await page.click('#pl-start')
  await page.waitForFunction(() => /sounds live|failed|errors/.test(document.getElementById('pl-state').textContent), { timeout: 40000 })
  check(/sounds live/.test(await ev(() => document.getElementById('pl-state').textContent)), 'the real engine starts with the whole bank')
  check(await ev(async () => await globalThis.__sfx.engine.waitForClock(3000)), 'and its audio clock runs')
  const flood = await ev(async () => {
    const t = globalThis.__sfx.mix.play
    for (let i = 0; i < 200; i++) t.fire()
    await new Promise((r) => setTimeout(r, 900))
    return await globalThis.__sfx.engine.getDiag()
  })
  const refused = flood.rejectedCooldown + flood.rejectedMaxInstances + flood.rejectedBusBudget
                + flood.rejectedPriority + flood.rejectedNoVoices + flood.rejectedUnknown
  check(flood.queueDropped === 0 && flood.played > 0 && refused > 0,
    `a flood is refused politely, not dropped (${flood.played} played, ${refused} refused)`)
  await ev(() => { globalThis.__sfx.engine.stopAll(0); globalThis.__sfx.showView('sounds') })

  // ---- locale ----
  const typed = await ev(async () => {
    const row = [...document.querySelectorAll('.card .slider')].find((x) => x.querySelector('.lbl')?.textContent === 'Volume')
    row.querySelector('.readout').click()
    const box = row.querySelector('input.typed')
    box.value = '-4,5'
    box.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' }))
    await new Promise((r) => setTimeout(r, 300))
    return globalThis.__sfx.store.doc.sounds[globalThis.__sfx.sel.sound].gainDb
  })
  check(typed === -4.5, `a comma decimal typed into a slider is read as a number (${typed})`)

  // ---- a phone ----
  // Width only: isMobile makes Chrome reload the page, which the unsaved-changes
  // guard then (correctly) blocks.
  await page.setViewport({ width: 390, height: 844 })
  await ev(() => globalThis.__sfx.showPane('list'))
  await settle(300)
  const phoneList = await ev(() => ({ lib: getComputedStyle(document.getElementById('library')).display, stage: getComputedStyle(document.getElementById('stage')).display }))
  check(phoneList.lib !== 'none' && phoneList.stage === 'none', 'on a phone the library is its own screen')
  await ev(() => document.querySelectorAll('#lib-list .item')[2].click())
  await settle(500)
  const phoneStage = await ev(() => ({ lib: getComputedStyle(document.getElementById('library')).display, stage: getComputedStyle(document.getElementById('stage')).display }))
  check(phoneStage.lib === 'none' && phoneStage.stage !== 'none', 'and tapping a sound opens it')
  await ev(() => document.getElementById('ig-open').click())
  await settle(400)
  const onTop = await ev(() => {
    const r = document.getElementById('ingame').getBoundingClientRect()
    const hit = document.elementFromPoint(r.left + r.width / 2, r.top + 60)
    return document.getElementById('ingame').contains(hit)
  })
  check(onTop, '“In the game” opens as a sheet ABOVE its dimmer, so it can be touched')
  await ev(() => globalThis.__sfx.openInGame(false))

  check(pageErrors.length === 0, `no page errors${pageErrors.length ? ': ' + pageErrors[0] : ''}`)
  // A console error and a failed response are usually the same event seen
  // twice. The message text carries no URL, so filtering it by pattern would
  // mean filtering every 404 -- correlate against the responses instead.
  const onlyFavicon = badResponses.length > 0
    && badResponses.every((t) => /favicon/.test(t))
  const realConsole = consoleErrors.filter(
    (t) => !/favicon/.test(t) && !(onlyFavicon && /Failed to load resource/.test(t)))
  check(realConsole.length === 0,
        `no console errors${realConsole.length ? ': ' + realConsole[0] : ''}`)
  const realBad = badResponses.filter((t) => !/favicon/.test(t))
  check(realBad.length === 0, `no failed requests${realBad.length ? ': ' + realBad[0] : ''}`)
} finally {
  await browser.close()
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
