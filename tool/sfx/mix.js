//
// Mix & test: everything that belongs to the BANK rather than to one sound.
//
//   Groups    the buses: a shared volume, a limit on how many play at once,
//             whether they are placed in the world, and how much of each
//             effect they send.
//   Effects   the room (reverb), chorus and echo that groups send into.
//   Test      the real engine, in the AudioWorklet, with the whole bank:
//             fire a sound, flood it, walk the listener around, and watch
//             what the engine refuses and why. (play-tab.js, unchanged.)
//
// The main view auditions one sound through the worker; only this view runs
// the live engine, because voice limits, stealing and bus budgets only exist
// there.
//
import { el, slider, toggle, numberBox, row, card } from './controls.js'
import { BUS_FIELDS, REVERB_FIELDS, CHORUS_FIELDS, DELAY_FIELDS, LIMITS, getField, setField } from './proj.js'
import { initPlayTab } from './play-tab.js'

const F = (list, path) => list.find((f) => f.path === path)
const pct = (v) => `${Math.round(v * 100)}%`

export function initMix (ctx) {
  const host = document.getElementById('mix-in')
  const busesBox = el('div', {})
  const fxBox = el('div', {})
  const testBox = el('div', { id: 'play-host' })

  host.append(
    el('div', { class: 'note', html: 'Settings for the <b>whole bank</b>. Each sound chooses its group in “In the game”.' }),
    card('Groups', busesBox, { key: 'mix-groups', ui: ctx.ui, subtitle: 'shared volume and limits' }),
    card('Effects', fxBox, { key: 'mix-fx', ui: ctx.ui, subtitle: 'room, chorus and echo that groups send into' }),
    card('Test in the engine', testBox, { key: 'mix-test', ui: ctx.ui, subtitle: 'the real engine, with every sound, as the game runs it' }))

  const play = initPlayTab(ctx)

  function commit (label, fn, structural = false) {
    ctx.store.mutate(label, fn, { coalesce: !structural, structural })
  }

  function buildBuses () {
    const doc = ctx.store.doc
    busesBox.replaceChildren()
    const grid = el('div', { class: 'buses' })
    doc.buses.forEach((b, bi) => {
      const used = doc.sounds.filter((s) => (s.bus ?? doc.buses[0].name) === b.name).length
      const name = el('input', { type: 'text', class: 'bus-name', value: b.name })
      name.onchange = () => {
        const nn = name.value.trim()
        if (!nn || doc.buses.some((x, k) => k !== bi && x.name === nn)) { name.value = b.name; ctx.log('a group needs a unique name', 'warn'); return }
        commit('rename group', (d) => {
          for (const s of d.sounds) if (s.bus === b.name) s.bus = nn
          d.buses[bi].name = nn
        }, true)
      }
      const f = (p) => F(BUS_FIELDS, p)
      const set = (p, label) => (v) => commit(label, (d) => setField(d.buses[bi], f(p), v))
      const card = el('div', { class: 'bus' },
        el('div', { class: 'field-top' }, name, el('span', { class: 'hint', text: `${used} sound${used === 1 ? '' : 's'}` })),
        slider({ label: 'Volume', min: -40, max: 12, step: 0.5, def: 0, value: getField(b, f('gainDb')),
          fmt: (v) => `${v > 0 ? '+' : ''}${v} dB`, onChange: set('gainDb', 'group volume') }),
        row('At most, at once', numberBox(getField(b, f('maxInstances')), { min: 1, max: 48, onChange: set('maxInstances', 'group limit') })),
        row('Placed in the world', toggle(getField(b, f('spatial')), set('spatial', 'placed in world'))),
        slider({ label: 'Room', hint: 'reverb send', min: 0, max: 1, def: 0, value: getField(b, f('sendReverb')), fmt: pct, onChange: set('sendReverb', 'room send') }),
        slider({ label: 'Chorus', min: 0, max: 1, def: 0, value: getField(b, f('sendChorus')), fmt: pct, onChange: set('sendChorus', 'chorus send') }),
        slider({ label: 'Echo', min: 0, max: 1, def: 0, value: getField(b, f('sendDelay')), fmt: pct, onChange: set('sendDelay', 'echo send') }),
        doc.buses.length > 1
          ? el('button', { class: 'ghost danger', text: 'Remove group', disabled: used > 0,
            title: used ? 'move its sounds to another group first' : '',
            onclick: () => commit('remove group', (d) => { d.buses.splice(bi, 1) }, true) })
          : null)
      grid.append(card)
    })
    busesBox.append(grid)
    if (doc.buses.length < LIMITS.maxBuses) {
      busesBox.append(el('div', { class: 'gen-row' }, el('button', { text: '＋ Add a group', onclick: () => commit('add group', (d) => {
        let n = 1
        while (d.buses.some((x) => x.name === `group${n}`)) n++
        d.buses.push({ name: `group${n}`, spatial: false })
      }, true) })))
    }
  }

  function buildFx () {
    const doc = ctx.store.doc
    fxBox.replaceChildren()
    const section = (title, hint, fields, labels) => {
      const box = el('div', { class: 'sliders' })
      for (const fd of fields) {
        const label = labels[fd.path] ?? fd.label
        if (fd.kind === 'bool') {
          box.append(row(label, toggle(getField(doc, fd), (v) => commit(label, (d) => setField(d, fd, v)))))
          continue
        }
        const isHz = fd.unit === 'Hz'
        const isMs = fd.unit === 'ms'
        box.append(slider({
          label, min: fd.lo, max: fd.hi, step: (isHz || isMs) ? 1 : 0.01, def: fd.def, value: getField(doc, fd),
          fmt: (v) => isHz ? (v === 0 ? 'off' : `${Math.round(v)} Hz`) : isMs ? `${Math.round(v)} ms` : fd.hi <= 2 ? pct(v) : String(v),
          onChange: (v) => commit(label, (d) => setField(d, fd, v)),
        }))
      }
      return el('div', { style: 'margin-bottom:14px' }, el('div', { class: 'sub', text: title }), el('p', { class: 'note', text: hint }), box)
    }
    fxBox.append(
      section('Room (reverb)', 'Nothing is heard until “level” is above zero AND a group sends into it.', REVERB_FIELDS,
        { 'fx.reverb.roomSize': 'size', 'fx.reverb.damping': 'darkness', 'fx.reverb.width': 'width', 'fx.reverb.wetGain': 'level', 'fx.reverb.preDelayMs': 'gap before', 'fx.reverb.highPassHz': 'cut lows' }),
      section('Chorus', 'Thickens and blurs; good on engines and magic.', CHORUS_FIELDS,
        { 'fx.chorus.rateHz': 'speed', 'fx.chorus.depthMs': 'depth', 'fx.chorus.wetGain': 'level' }),
      section('Echo (delay)', 'Repeats the sound; ping-pong bounces it left and right.', DELAY_FIELDS,
        { 'fx.delay.timeMs': 'time', 'fx.delay.feedback': 'repeats', 'fx.delay.wetGain': 'level', 'fx.delay.pingPong': 'ping-pong' }))
    // Echo time as a beat division instead of milliseconds. The SFX engine
    // has no song, so it follows the default tempo; it is here because the
    // format accepts it, and a field the file can hold must be editable.
    const syncs = ['', '1/1', '1/2', '1/4', '1/8', '1/16', '1/32', '1/4T', '1/8T', '1/16T', '1/4.', '1/8.', '1/16.']
    const cur = doc.fx?.delay?.sync ?? ''
    const sel = el('select', {}, syncs.map((x) => el('option', { value: x, text: x || 'off (use time)', selected: x === cur })))
    sel.onchange = () => commit('echo sync', (d) => {
      d.fx = d.fx ?? {}; d.fx.delay = d.fx.delay ?? {}
      if (sel.value) d.fx.delay.sync = sel.value; else delete d.fx.delay.sync
      if (!Object.keys(d.fx.delay).length) delete d.fx.delay
      if (!Object.keys(d.fx).length) delete d.fx
    })
    fxBox.append(row('Echo time as a beat', sel, 'wins over the time when set; the SFX engine has no song, so it counts at 120 BPM'))
  }

  // ---- the master chain: EQ, compressor, saturation, width, gain, limiter ----
  // midisynth's own mastering section, handed to its own loader, so these
  // mean exactly what they mean in an fx.json. Off, the output is bit-exact
  // with an unmastered engine.
  const masterBox = el('div', {})
  host.insertBefore(card('Master', masterBox, { key: 'mix-master', ui: ctx.ui, subtitle: 'EQ, compressor and limiter on everything' }), host.children[3])
  function buildMaster () {
    const doc = ctx.store.doc
    masterBox.replaceChildren()
    const g = (p, def) => { let v = doc.master; for (const k of p.split('.')) { if (v == null) return def; v = v[k] } return v ?? def }
    const put = (p, value, def, label, structural = false) => commit(label, (d) => {
      d.master = d.master ?? {}
      const parts = p.split('.'); const last = parts.pop(); let cur = d.master
      for (const k of parts) { cur[k] = cur[k] ?? {}; cur = cur[k] }
      if (value === def) delete cur[last]; else cur[last] = value
      // Prune empty groups so an untouched chain writes nothing at all.
      const prune = (o) => { for (const k of Object.keys(o)) if (o[k] && typeof o[k] === 'object') { prune(o[k]); if (!Object.keys(o[k]).length) delete o[k] } }
      prune(d.master)
      if (!Object.keys(d.master).length) delete d.master
    }, structural)
    const on = !!g('enabled', false)
    masterBox.append(row('Mastering on', toggle(on, (v) => put('enabled', v, false, 'mastering', true)), 'off: the output is exactly the unmastered engine'))
    if (!on) return
    const sl = (p, label, o) => slider({ label, ...o, value: g(p, o.def), onChange: (v) => put(p, v, o.def, label) })
    const db = (v) => `${v > 0 ? '+' : ''}${v} dB`
    const hz = (v) => v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`
    const ms = (v) => `${v} ms`
    const sec2 = (title, items) => el('div', { style: 'margin-bottom:12px' }, el('div', { class: 'sub', text: title }), el('div', { class: 'sliders' }, ...items))
    masterBox.append(
      sec2('EQ', [
        sl('eq.low.hz', 'low shelf at', { min: 20, max: 20000, log: true, step: 1, def: 120, fmt: hz }),
        sl('eq.low.db', 'low', { min: -24, max: 24, step: 0.5, def: 0, fmt: db }),
        sl('eq.mid.hz', 'mid at', { min: 20, max: 20000, log: true, step: 1, def: 1000, fmt: hz }),
        sl('eq.mid.db', 'mid', { min: -24, max: 24, step: 0.5, def: 0, fmt: db }),
        sl('eq.mid.q', 'mid width (Q)', { min: 0.5, max: 10, step: 0.1, def: 1, fmt: (v) => v.toFixed(1) }),
        sl('eq.high.hz', 'high shelf at', { min: 20, max: 20000, log: true, step: 1, def: 8000, fmt: hz }),
        sl('eq.high.db', 'high', { min: -24, max: 24, step: 0.5, def: 0, fmt: db }),
      ]),
      sec2('Compressor', [
        sl('comp.thresholdDb', 'threshold', { min: -60, max: 0, step: 0.5, def: -12, fmt: db }),
        sl('comp.ratio', 'ratio', { min: 1, max: 20, step: 0.1, def: 2, fmt: (v) => `${v.toFixed(1)}:1` }),
        sl('comp.attackMs', 'attack', { min: 0.1, max: 200, log: true, step: 0.1, def: 10, fmt: ms }),
        sl('comp.releaseMs', 'release', { min: 5, max: 2000, log: true, step: 1, def: 150, fmt: ms }),
        sl('comp.makeupDb', 'makeup', { min: 0, max: 24, step: 0.5, def: 0, fmt: db }),
        sl('comp.kneeDb', 'knee', { min: 0, max: 24, step: 0.5, def: 6, fmt: db }),
      ]),
      sec2('Colour and width', [
        sl('satDrive', 'saturation', { min: 1, max: 8, step: 0.1, def: 1, fmt: (v) => v <= 1 ? 'none' : `×${v.toFixed(1)}` }),
        sl('width', 'stereo width', { min: 0, max: 2, step: 0.01, def: 1, fmt: (v) => `${Math.round(v * 100)}%` }),
        sl('gainDb', 'output gain', { min: -24, max: 24, step: 0.5, def: 0, fmt: db }),
      ]),
      sec2('Limiter', [
        sl('limiter.ceilingDb', 'ceiling', { min: -12, max: 0, step: 0.1, def: -1, fmt: db }),
        sl('limiter.lookaheadBlocks', 'look-ahead', { min: 1, max: 8, step: 1, def: 2, fmt: (v) => `${v} × 1.3 ms` }),
        sl('limiter.releaseMs', 'release', { min: 1, max: 1000, log: true, step: 1, def: 80, fmt: ms }),
      ]))
  }

  function build () { buildBuses(); buildFx(); buildMaster() }

  ctx.on((what) => {
    if (ctx.view !== 'mix') return
    if (what === 'opened' || what === 'structural') build()
  })

  return {
    activate () { build(); play.activate() },
    deactivate () { play.deactivate() },
    play,
  }
}
