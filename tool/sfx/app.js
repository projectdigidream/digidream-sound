//
// The shell: boot, the two views, the document, files, keys and window.__sfx.
//
// The page is built around one question at a time. SOUNDS is "what does this
// sound sound like, and how do I change it" -- a library, the selected sound,
// and how the game plays it. MIX & TEST is "how does the bank behave as a
// whole" -- groups, effects and the live engine. Everything that used to be a
// tab (Bank, Design, Fit, Play) lives in one of those two places now.
//
// Rendering happens in a Worker (tool-worker.js), never on the audio thread:
// loadBankJson costs 2-25 ms per edit and unloads the bank when a patch fails
// to validate, which is why the live engine (the AudioWorklet) is only used by
// "Test in the engine".
//
import { bootStatus, makeLogger } from '../lib/ui.js'
import {
  desktop, isDesktop, onMenu, onOpenPath, pickFiles, saveBytes, setDocument,
} from '../lib/desktop.js'
import { ProjStore, newProj, readProj, writeProj } from './proj.js'
import { ToolClient } from './tool-client.js'
import { Audition } from './audition.js'
import { initLibrary } from './library.js'
import { initStage } from './stage.js'
import { initInGame } from './ingame.js'
import { initMatch } from './match.js'
import { initMix } from './mix.js'
import { KINDS, pick } from './recipes.js'
import { el } from './controls.js'

const $ = (id) => document.getElementById(id)
const logLine = makeLogger($('log'))

const ctx = {
  store: new ProjStore(newProj('untitled')),
  client: new ToolClient(),
  sel: { sound: -1, variant: 0 },
  view: 'sounds',
  tab: 'sounds',
  listeners: new Set(),
  archs: [],
  autoPlay: true,
  workerReady: false,
}

/** Every message lands in the log; the latest one is the status line. */
ctx.log = (msg, cls = '') => {
  logLine(msg, cls)
  const s = $('status-msg')
  s.textContent = msg
  s.className = cls
}

ctx.on = (fn) => { ctx.listeners.add(fn); return () => ctx.listeners.delete(fn) }
ctx.emit = (what) => { for (const fn of ctx.listeners) fn(what) }
ctx.select = (sound, variant = 0) => {
  const s = ctx.store.doc.sounds[sound]
  ctx.sel = { sound, variant: s ? Math.min(variant, s.variants.length - 1) : 0 }
  ctx.ui?.save({ sound })
  ctx.emit('select')
}

// ---------------------------------------------------------------------------
// UI state -- and only UI state. The .sfxproj is the document; a browser copy
// of it would be a second source of truth that disagrees with disk at the
// worst moment. What is kept is where you were.
// ---------------------------------------------------------------------------
const UI_KEY = 'dsfx.ui2'
ctx.ui = {
  load () { try { return JSON.parse(localStorage.getItem(UI_KEY) ?? '{}') } catch { return {} } },
  save (patch) { try { localStorage.setItem(UI_KEY, JSON.stringify({ ...ctx.ui.load(), ...patch })) } catch { /* no storage */ } },
}

// ---------------------------------------------------------------------------
// Hearing the selected sound.
// ---------------------------------------------------------------------------
ctx.audition = new Audition(ctx)
ctx.play = () => {
  const ok = ctx.audition.play()
  ctx.playStartedAt = ctx.audition.audio?.currentTime ?? 0
  return ok
}
ctx.togglePlay = () => (ctx.audition.playing ? (ctx.audition.stop(), false) : ctx.play())

// ---------------------------------------------------------------------------
// Views and panes.
// ---------------------------------------------------------------------------
let mix = null
function showView (v) {
  if (ctx.view === v) return
  if (ctx.view === 'mix') mix?.deactivate()
  ctx.view = v
  // play-tab.js follows the selection only while ctx.tab is 'play'.
  ctx.tab = v === 'mix' ? 'play' : 'sounds'
  $('app').dataset.view = v
  for (const b of $('view-seg').children) b.classList.toggle('on', b.dataset.view === v)
  if (v === 'mix') { ctx.audition.stop(); mix?.activate() } else ctx.emit('structural')
  ctx.ui.save({ view: v })
}
ctx.showView = showView
$('view-seg').onclick = (e) => { const b = e.target.closest('button[data-view]'); if (b) showView(b.dataset.view) }

/** On a phone: the list, or the sound. Harmless on a desktop. */
ctx.showPane = (p) => { $('app').dataset.pane = p; if (p === 'stage') $('stage').scrollTop = 0 }

ctx.openInGame = (open) => {
  $('ingame').classList.toggle('open', open)
  $('scrim').classList.toggle('open', open)
  ctx.sheetOpen = open ? () => ctx.openInGame(false) : null
}
$('scrim').addEventListener('click', () => {
  if ($('ingame').classList.contains('open')) ctx.openInGame(false)
  if ($('help').classList.contains('open')) closeHelp()
})

// ---------------------------------------------------------------------------
// The document.
// ---------------------------------------------------------------------------
function docChanged (e) {
  const d = ctx.store.doc
  $('btn-undo').disabled = !ctx.store.canUndo
  $('btn-redo').disabled = !ctx.store.canRedo
  $('btn-undo').title = ctx.store.undoLabel ? `Undo ${ctx.store.undoLabel} (⌘Z)` : 'Undo (⌘Z)'
  $('btn-redo').title = ctx.store.redoLabel ? `Redo ${ctx.store.redoLabel} (⌘⇧Z)` : 'Redo (⌘⇧Z)'
  $('doc-name').textContent = d.name || 'untitled'
  $('dirty').classList.toggle('on', ctx.store.dirty)
  setDocument({ path: ctx.store.path, name: d.name, dirty: ctx.store.dirty })
  // An undo can remove the sound or take that was selected.
  if (ctx.sel.sound >= d.sounds.length) ctx.sel = { sound: d.sounds.length - 1, variant: 0 }
  const s = d.sounds[ctx.sel.sound]
  if (s && ctx.sel.variant >= s.variants.length) ctx.sel.variant = 0
  ctx.emit(e?.detail?.structural ? 'structural' : 'change')
}
ctx.store.addEventListener('change', docChanged)

async function openText (text, path, { dirty = false } = {}) {
  let parsed
  try { parsed = readProj(text) } catch (err) { ctx.log(`cannot open: ${err.message}`, 'err'); return false }
  for (const r of parsed.repairs) ctx.log(`repaired: ${r}`, 'warn')
  ctx.store.replace(parsed.doc, path, { dirty })
  const keep = ctx.ui.load().sound
  ctx.sel = { sound: parsed.doc.sounds.length ? (keep >= 0 && keep < parsed.doc.sounds.length ? keep : 0) : -1, variant: 0 }
  // The worker keeps a whole-bank copy for baking; opening is the moment to
  // hand it over, off the main thread.
  $('worker-chip').textContent = 'reading…'
  const rep = await ctx.client.open(writeProj(parsed.doc))
  $('worker-chip').textContent = ''
  for (const w of rep.warnings ?? []) ctx.log(`${w.path ? w.path + ': ' : ''}${w.text}`, 'warn')
  for (const e of rep.errors ?? []) ctx.log(`${e.path ? e.path + ': ' : ''}${e.text}`, 'err')
  ctx.emit('opened')
  return true
}
ctx.openText = openText

// ---------------------------------------------------------------------------
// Sounds: new, rename, duplicate, delete.
// ---------------------------------------------------------------------------
const uniqueName = (base, doc = ctx.store.doc) => {
  if (!doc.sounds.some((s) => s.name === base)) return base
  let n = 2
  while (doc.sounds.some((s) => s.name === `${base}${n}`)) n++
  return `${base}${n}`
}

ctx.newSound = async (kindId) => {
  if (ctx.view !== 'sounds') showView('sounds')
  const kind = KINDS.find((k) => k.id === kindId) ?? KINDS.find((k) => k.id === ctx.ui.load().lastKind) ?? pick(KINDS)
  const doc = ctx.store.doc
  const name = uniqueName(`new.${kind.id}`)
  // A spatial group if there is one: most effects happen somewhere.
  const bus = doc.buses.find((b) => b.spatial !== false) ?? doc.buses[0]
  ctx.store.mutate('new sound', (d) => {
    d.sounds.push({ name, bus: bus.name, variants: [{ patch: { name: 'new', oneShot: true, layers: [{ osc: 'noise', mixGain: 0.8, ampEnv: { attack: 0, decay: 120, sustain: 0, release: 10 } }] } }] })
  }, { structural: true })
  ctx.select(ctx.store.doc.sounds.length - 1, 0)
  ctx.showPane('stage')
  await ctx.stage.fromKind(kind)
  document.getElementById('snd-name')?.select()
}

ctx.renameSound = (i, name) => {
  const doc = ctx.store.doc
  if (!name) return false
  if (doc.sounds.some((s, k) => k !== i && s.name === name)) { ctx.log(`there is already a sound called ${name}`, 'warn'); return false }
  ctx.store.mutate('rename', (d) => { d.sounds[i].name = name }, { structural: true })
  return true
}

ctx.duplicateSound = (i) => {
  const s = structuredClone(ctx.store.doc.sounds[i])
  s.name = uniqueName(s.name + '.copy')
  ctx.store.mutate('duplicate sound', (d) => { d.sounds.splice(i + 1, 0, s) }, { structural: true })
  ctx.select(i + 1, 0)
}

ctx.deleteSound = (i) => {
  const s = ctx.store.doc.sounds[i]
  if (!s) return
  ctx.store.mutate(`delete ${s.name}`, (d) => { d.sounds.splice(i, 1) }, { structural: true })
  ctx.select(Math.min(i, ctx.store.doc.sounds.length - 1), 0)
  ctx.log(`deleted ${s.name} — undo brings it back`, 'ok')
}

// ---------------------------------------------------------------------------
// Autosave.
//
// Two things, 1 s after edits stop (and at most 10 s apart while they don't):
//
//   * The WORKING COPY goes into this app's own storage, every platform. A
//     reload, a crash or a closed tab reopens the bank as it was, unsaved
//     changes included -- so nothing asks "leave the page?" any more.
//   * A bank that came from a FILE (the desktop and Android apps know its
//     path) is written back to that file, unless ⋯ -> Save automatically is
//     off. A browser has no path, only downloads, so there the working copy
//     is the autosave and Save is still how a file is made.
//
// Inside the studio neither happens: the studio keeps drafts and saves into
// the module on Back (studio/plugins/sfx/suite-shim.js), and a save from here
// would mark the bank saved under it.
// ---------------------------------------------------------------------------
const AUTO_KEY = 'dsfx.autosave'
const inSuite = new URLSearchParams(location.search).get('host') === 'suite'
const autoToFile = () => ctx.ui.load().autoFile !== false
let autoTimer = null
let autoSince = 0
let autoWriting = false

function keepWorkingCopy (text = writeProj(ctx.store.doc)) {
  // ponytail: one slot per browser profile; two tabs of the tool keep whichever saved last.
  try {
    localStorage.setItem(AUTO_KEY, JSON.stringify({ text, path: ctx.store.path, dirty: ctx.store.dirty }))
  } catch { /* storage full or blocked: the file save below still runs */ }
}

function scheduleAutosave () {
  if (inSuite) return
  const now = Date.now()
  if (!autoTimer) autoSince = now
  clearTimeout(autoTimer)
  autoTimer = setTimeout(autosave, Math.max(0, Math.min(1000, autoSince + 10000 - now)))
}

async function autosave () {
  autoTimer = null
  const text = writeProj(ctx.store.doc)
  if (ctx.store.dirty && ctx.store.path && autoToFile() && !autoWriting) {
    autoWriting = true
    try {
      const r = await saveBytes(`${ctx.store.doc.name || 'bank'}.sfxproj`, text, 'application/json', { toPath: ctx.store.path })
      // Only what was written counts as saved: an edit made while the file
      // was being written is still unsaved, and the next autosave takes it.
      if (r && writeProj(ctx.store.doc) === text) ctx.store.markSaved(r.path ?? ctx.store.path)
      if (!r) throw new Error('not written')
    } catch (e) {
      // A file that moved or lost its permission: stop trying until the
      // next Save says where it lives now.
      ctx.store.path = null
      ctx.log(`could not save to the file automatically (${e.message}); it is kept here, use Save`, 'warn')
    } finally { autoWriting = false }
  }
  keepWorkingCopy()
}

ctx.store.addEventListener('change', scheduleAutosave)
// Leaving: the working copy as it is this instant, without waiting for the timer.
globalThis.addEventListener('pagehide', () => { if (!inSuite) keepWorkingCopy() })

$('auto-file').onclick = () => {
  ctx.ui.save({ autoFile: !autoToFile() })
  paintAutoFile()
  if (autoToFile()) scheduleAutosave()
}
function paintAutoFile () { $('auto-file').querySelector('.k').textContent = autoToFile() ? 'on' : 'off' }
paintAutoFile()
if (inSuite) $('auto-file').style.display = 'none'

// ---------------------------------------------------------------------------
// File actions.
// ---------------------------------------------------------------------------
async function confirmDiscard () {
  if (!ctx.store.dirty) return true
  return globalThis.confirm('This bank has unsaved changes. Discard them?')
}

const actions = {
  async new () {
    if (!(await confirmDiscard())) return
    ctx.store.replace(newProj('untitled'), null)
    ctx.sel = { sound: -1, variant: 0 }
    await ctx.client.open(writeProj(ctx.store.doc))
    ctx.emit('opened')
    ctx.showPane('list')
  },
  async open () {
    if (!(await confirmDiscard())) return
    const files = await pickFiles('.sfxproj,.json', false)
    if (!files.length) return
    const f = files[0]
    if (await openText(await f.text(), f.path ?? null)) ctx.log(`opened ${f.name}`, 'ok')
  },
  async 'load-starter' () {
    if (!(await confirmDiscard())) return
    try {
      const r = await fetch('../content/banks/starter.sfxproj')
      if (!r.ok) throw new Error(`${r.status}`)
      await openText(await r.text(), null)
      ctx.log('opened the example bank', 'ok')
    } catch (e) {
      ctx.log(`cannot reach the example bank (${e.message}). Use Open.`, 'err')
    }
  },
  async save () {
    const text = writeProj(ctx.store.doc)
    const name = `${ctx.store.doc.name || 'bank'}.sfxproj`
    const r = await saveBytes(name, text, 'application/json', { toPath: ctx.store.path })
    if (r) { ctx.store.markSaved(r.path ?? ctx.store.path); ctx.log(`saved ${r.name ?? name}`, 'ok') }
  },
  async 'save-as' () {
    const text = writeProj(ctx.store.doc)
    const r = await saveBytes(`${ctx.store.doc.name || 'bank'}.sfxproj`, text, 'application/json')
    if (r) { ctx.store.markSaved(r.path ?? null); ctx.log(`saved ${r.name}`, 'ok') }
  },
  // What the game loads. The same bake_bank the CLI uses; test/wasm_bake.mjs
  // pins that the bytes are identical.
  async bake () {
    const rep = await ctx.client.bake(writeProj(ctx.store.doc))
    if (!rep.ok) { for (const e of rep.errors ?? []) ctx.log(`export: ${e.text ?? e}`, 'err'); return }
    await saveBytes(`${ctx.store.doc.name || 'bank'}.sfxb`, rep.bytes, 'application/octet-stream')
    ctx.log(`exported ${ctx.store.doc.name}.sfxb (${Math.round(rep.bytes.length / 1024)} KB)`, 'ok')
  },
  async wav () {
    const s = ctx.store.doc.sounds[ctx.sel.sound]
    if (!s) { ctx.log('select a sound first', 'warn'); return }
    const r = await ctx.client.wav(s, ctx.sel.variant)
    if (!r.ok) { ctx.log(`render: ${r.errors?.[0]?.text ?? 'failed'}`, 'err'); return }
    const suffix = s.variants.length > 1 ? `.${String.fromCharCode(65 + ctx.sel.variant)}` : ''
    await saveBytes(`${s.name}${suffix}.wav`, r.bytes, 'audio/wav')
    ctx.log(`exported ${s.name}${suffix}.wav`, 'ok')
  },
  undo () { ctx.store.undo() },
  redo () { ctx.store.redo() },
  help () { openHelp() },
}

async function runAction (act) {
  const fn = actions[act]
  if (!fn) return
  try { await fn() } catch (e) { ctx.log(`${act}: ${e.message}`, 'err') }
}
ctx.runAction = runAction

$('play').onclick = () => ctx.togglePlay()
$('btn-undo').onclick = () => runAction('undo')
$('btn-redo').onclick = () => runAction('redo')
$('doc-name').onclick = () => {
  const n = globalThis.prompt('Name of this bank', ctx.store.doc.name)
  if (n && n.trim()) ctx.store.mutate('rename bank', (d) => { d.name = n.trim() })
}
$('status-msg').onclick = () => $('log-wrap').classList.toggle('open')

// Menus: one open at a time, closed by any click outside.
function closeMenus () { for (const m of document.querySelectorAll('.menu.open')) m.classList.remove('open') }
document.addEventListener('click', (e) => {
  const act = e.target.closest('[data-act]')
  if (act && actions[act.dataset.act]) { closeMenus(); runAction(act.dataset.act); return }
  const m = e.target.closest('.menu')
  const toggler = e.target.closest('.menu > button')
  if (toggler) { const was = m.classList.contains('open'); closeMenus(); if (!was) m.classList.add('open'); return }
  if (!e.target.closest('.drop')) closeMenus()
  else if (e.target.closest('.drop button')) closeMenus()
})

// ---------------------------------------------------------------------------
// Keys.
// ---------------------------------------------------------------------------
document.addEventListener('keydown', (e) => {
  const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) && e.target.type !== 'range' && e.target.type !== 'checkbox'
  const mod = e.metaKey || e.ctrlKey
  if (mod && e.key.toLowerCase() === 's') { e.preventDefault(); runAction(e.shiftKey ? 'save-as' : 'save'); return }
  if (mod && e.key.toLowerCase() === 'o') { e.preventDefault(); runAction('open'); return }
  if (mod && e.key.toLowerCase() === 'z' && !typing) { e.preventDefault(); runAction(e.shiftKey ? 'redo' : 'undo'); return }
  if (e.key === 'Escape') {
    closeMenus()
    if (ctx.sheetOpen) ctx.sheetOpen()
    $('log-wrap').classList.remove('open')
    return
  }
  if (typing || mod) return
  if (e.key === ' ') { e.preventDefault(); ctx.togglePlay() }
  if (e.key.toLowerCase() === 'l') {
    ctx.audition.setLoop(!ctx.audition.loop)
    ctx.ui.save({ loop: ctx.audition.loop })
    ctx.emit('structural')
  }
  if (e.key === '?') openHelp()
})

// Only the studio still needs the "leave the page?" question: everywhere else
// the working copy is kept on the way out (Autosave, above).
globalThis.addEventListener('beforeunload', (e) => {
  if (inSuite && ctx.store.dirty) { e.preventDefault(); e.returnValue = '' }
})

// A .sfxproj dropped anywhere opens it. (A WAV goes to the match sheet, which
// handles its own drop.)
document.addEventListener('dragover', (e) => e.preventDefault())
document.addEventListener('drop', async (e) => {
  e.preventDefault()
  const f = e.dataTransfer?.files?.[0]
  if (!f) return
  if (/\.(sfxproj|json)$/i.test(f.name)) {
    if (!(await confirmDiscard())) return
    if (await openText(await f.text(), null)) ctx.log(`opened ${f.name}`, 'ok')
  } else if (/\.(wav|mp3|ogg|flac)$/i.test(f.name)) {
    ctx.log('to match a recording, open “Match a recording…” on a sound and drop it there', 'warn')
  }
})

// ---------------------------------------------------------------------------
// Help: the five ideas, and the keys.
// ---------------------------------------------------------------------------
function openHelp () {
  const h = $('help')
  h.replaceChildren(
    el('div', { style: 'display:flex; align-items:center' }, el('h2', { text: 'How this works', style: 'flex:1' }),
      el('button', { class: 'ghost', text: '✕', onclick: closeHelp })),
    el('div', { class: 'sheet-body' },
      el('p', { class: 'hint', text: 'A bank is every sound effect a game uses. It is saved as recipes, not recordings, so it is tiny and every sound can still be changed.' }),
      el('dl', {},
        el('dt', { text: 'Sound' }), el('dd', { text: 'One thing the game asks for by name, like weapon.pistol.fire. The list on the left.' }),
        el('dt', { text: 'Start from' }), el('dd', { text: 'Pick a kind of sound and a new one is made. Press again, or Randomize, until one is close.' }),
        el('dt', { text: 'Shape' }), el('dd', { text: 'Sliders that change it: four for every sound, plus the recipe’s own. It plays each time you let go.' }),
        el('dt', { text: 'Takes' }), el('dd', { text: 'Different versions of one sound. The game picks one at random each time, so repeats do not sound identical.' }),
        el('dt', { text: 'Match' }), el('dd', { text: 'Give it a recording and it finds a recipe that sounds like it. Only the numbers are kept.' }),
        el('dt', { text: 'In the game' }), el('dd', { text: 'How the game plays the sound: its group, how many at once, how much it varies, how far it carries.' }),
        el('dt', { text: 'Mix & test' }), el('dd', { text: 'Groups, effects, and the real engine playing the whole bank.' }),
        el('dt', { text: 'Export' }), el('dd', { text: '⋯ → Export for the game writes the .sfxb the game loads. Save keeps the editable .sfxproj.' })),
      el('p', { html: '<kbd>Space</kbd> play · <kbd>R</kbd> randomize · <kbd>V</kbd> vary · <kbd>L</kbd> repeat · <kbd>↑</kbd><kbd>↓</kbd> next sound · <kbd>⌘Z</kbd> undo · <kbd>⌘S</kbd> save · double-click a slider’s name to reset it' })))
  h.classList.add('open')
  $('scrim').classList.add('open')
  ctx.sheetOpen = closeHelp
}
function closeHelp () {
  $('help').classList.remove('open')
  $('scrim').classList.remove('open')
  ctx.sheetOpen = null
}

// ---------------------------------------------------------------------------
// Boot.
// ---------------------------------------------------------------------------
async function boot () {
  const stage = bootStatus($('overlay'))
  try {
    stage?.('starting the sound engine')
    await ctx.client.open(writeProj(ctx.store.doc))
    ctx.workerReady = true
    ctx.archs = (await ctx.client.archs()).archs ?? []

    stage?.('building the interface')
    const ui = ctx.ui.load()
    ctx.autoPlay = ui.autoPlay ?? true
    if (ui.loop) ctx.audition.setLoop(true)
    ctx.library = initLibrary(ctx)
    ctx.stage = initStage(ctx)
    ctx.ingame = initInGame(ctx)
    ctx.match = initMatch(ctx)
    ctx.openMatch = () => ctx.match.open()
    mix = initMix(ctx)
    ctx.mix = mix

    $('lib-new').onclick = () => ctx.newSound()
    $('lib-sort').onclick = () => {
      const name = ctx.store.doc.sounds[ctx.sel.sound]?.name
      ctx.store.mutate('sort sounds', (d) => { d.sounds.sort((a, b) => a.name.localeCompare(b.name)) }, { structural: true })
      if (name) ctx.select(ctx.store.doc.sounds.findIndex((s) => s.name === name), 0)
    }
    $('lib-level').onclick = levelAll

    $('overlay').remove()
    // Where you were: the working copy autosave kept, else the example bank.
    let kept = null
    try { kept = inSuite ? null : JSON.parse(localStorage.getItem(AUTO_KEY) ?? 'null') } catch { /* unreadable: start fresh */ }
    if (!kept || !(await openText(kept.text, kept.path ?? null, { dirty: !!kept.dirty }))) await runAction('load-starter')
    if (ui.view === 'mix') showView('mix')

    if (isDesktop) {
      onMenu((a) => runAction(a))
      onOpenPath(async () => {
        const launched = await desktop.ready()
        if (launched) await openText(new TextDecoder().decode(launched.data), launched.path)
      })
    }

    // The testability global: the live subsystems, not a bespoke test API, so
    // a smoke test drives the same objects the page does.
    globalThis.__sfx = ctx
    ctx.log('Ready. Pick a sound, or ＋ New.', 'ok')
  } catch (e) {
    stage?.(`failed: ${e.message}`, true)
    ctx.log(`boot failed: ${e.message}`, 'err')
    throw e
  }
}

/**
 * Even out the levels: measure every sound (the library already has), pick a
 * target, and move each sound's volume by the difference. Volume only --
 * never a recipe -- so it is one undo step and changes nobody's tuning.
 */
async function levelAll () {
  await ctx.library.idle()
  const doc = ctx.store.doc
  const known = doc.sounds.map((s, i) => ({ i, t: ctx.library.measured(s) })).filter((x) => x.t?.info?.frames)
  if (!known.length) { ctx.log('nothing measured yet', 'warn'); return }
  const target = Number(globalThis.prompt(`Set the loudest moment of all ${known.length} sounds to (dB, 0 is the ceiling):`, '-6'))
  if (!Number.isFinite(target)) return
  ctx.store.mutate(`level ${known.length} sounds`, (d) => {
    for (const { i, t } of known) {
      // The measured peak is dry (volume is applied at playback), so the
      // volume that lands it on the target is simply target minus that.
      const next = Math.max(-40, Math.min(12, target - t.info.peakDbMilli / 1000))
      if (Math.abs(next) < 0.05) delete d.sounds[i].gainDb
      else d.sounds[i].gainDb = Math.round(next * 10) / 10
    }
  }, { structural: true })
  ctx.log(`levelled ${known.length} sounds to ${target} dB`, 'ok')
}

$('start-btn').onclick = () => { $('start-btn').disabled = true; boot() }
// A desktop or phone shell has no autoplay policy to satisfy.
if (isDesktop) $('start-btn').click()
