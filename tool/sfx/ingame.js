//
// "In the game": how the selected sound behaves when the game plays it.
//
// The same fields the old policy rail had (proj.js SOUND_FIELDS and friends,
// which carry the engine's real clamps and defaults), asked as the questions
// a designer actually has: where does it play, what happens when it is
// triggered a lot, does it vary, how far does it carry. Defaults are never
// written into the document -- setField removes a key set back to its
// default -- so a round trip still changes only what a person changed.
//
import { el, slider, choice, toggle, numberBox, row } from './controls.js'
import { SOUND_FIELDS, ATTEN_FIELDS, DUCK_FIELDS, getField, setField, resolveBus, liveReason } from './proj.js'

const F = (list, path) => list.find((f) => f.path === path)
const msFmt = (v) => v <= 0 ? 'none' : v >= 1000 ? `${(v / 1000).toFixed(2)} s` : `${Math.round(v)} ms`

export function initInGame (ctx) {
  const host = document.getElementById('ingame')
  const sound = () => ctx.store.doc.sounds[ctx.sel.sound]

  function commit (label, fn, structural = false) {
    const i = ctx.sel.sound
    ctx.store.mutate(label, (d) => fn(d.sounds[i]), { coalesce: !structural, structural })
  }
  const setter = (field, label) => (v) => commit(label ?? field.label, (s) => setField(s, field, v))

  function build () {
    host.replaceChildren()
    const s = sound()
    const head = el('div', { style: 'display:flex; align-items:center; gap:8px' },
      el('h3', { class: 'ig-title', text: 'In the game', style: 'flex:1' }),
      el('button', { id: 'ig-close', class: 'ghost', text: '✕', onclick: () => ctx.openInGame(false) }))
    const box = el('div', { class: 'ig' }, head)
    host.append(box)
    if (!s) { box.append(el('p', { class: 'hint', text: 'Select a sound to see how the game plays it.' })); return }

    const doc = ctx.store.doc
    const bus = doc.buses[resolveBus(doc, s)]
    const spatial = bus ? bus.spatial !== false : true

    // ---- where ----
    const where = el('div', { class: 'ig-sec' }, el('h3', { text: 'Where it plays' }))
    const busSel = el('select', {}, ...doc.buses.map((b) => el('option', { value: b.name, text: b.name, selected: b === bus })))
    busSel.onchange = () => commit('bus', (x) => { x.bus = busSel.value }, true)
    where.append(row('Group', busSel, `${spatial ? 'Placed in the world: it gets quieter with distance.' : 'Not placed in the world: it plays at the same level wherever you are.'} Groups are set up in Mix & test.`))
    where.append(slider({
      label: 'Importance', hint: 'when a group is full, a more important sound wins',
      min: 0, max: 255, step: 1, def: 128, value: getField(s, F(SOUND_FIELDS, 'priority')),
      fmt: (v) => v < 64 ? `low · ${v}` : v < 170 ? `normal · ${v}` : `high · ${v}`,
      onChange: setter(F(SOUND_FIELDS, 'priority'), 'importance'),
    }))
    box.append(where)

    // ---- repeats ----
    const rep = el('div', { class: 'ig-sec' }, el('h3', { text: 'When it is triggered a lot' }))
    const maxF = F(SOUND_FIELDS, 'maxInstances')
    rep.append(row('At most, at once', numberBox(getField(s, maxF), { min: maxF.lo, max: maxF.hi, onChange: setter(maxF, 'at most') })))
    rep.append(row('Then', choice([['reject', 'Skip the new one'], ['stealOldest', 'Cut the oldest']],
      getField(s, F(SOUND_FIELDS, 'onLimit')), setter(F(SOUND_FIELDS, 'onLimit'), 'on limit'))))
    rep.append(slider({
      label: 'Minimum gap', hint: 'triggers closer together than this are skipped',
      min: 1, max: 5000, log: true, def: 1, value: Math.max(1, getField(s, F(SOUND_FIELDS, 'cooldownMs'))),
      fmt: (v) => msFmt(v <= 1.05 ? 0 : v),
      onChange: (v) => setter(F(SOUND_FIELDS, 'cooldownMs'), 'minimum gap')(v <= 1.05 ? 0 : Math.round(v)),
    }))
    box.append(rep)

    // ---- variation ----
    const vary = el('div', { class: 'ig-sec' }, el('h3', { text: 'Different every time' }))
    vary.append(slider({
      label: 'Pitch', hint: 'a random pitch change each time it plays',
      min: 0, max: 1200, step: 5, def: 0, value: getField(s, F(SOUND_FIELDS, 'pitchJitterCents')),
      fmt: (v) => v === 0 ? 'off' : `± ${v >= 100 ? (v / 100).toFixed(1) + ' st' : v + '¢'}`,
      onChange: setter(F(SOUND_FIELDS, 'pitchJitterCents'), 'pitch variation'),
    }))
    vary.append(slider({
      label: 'Volume', hint: 'a random volume change each time it plays',
      min: 0, max: 12, step: 0.5, def: 0, value: getField(s, F(SOUND_FIELDS, 'gainJitterDb')),
      fmt: (v) => v === 0 ? 'off' : `± ${v} dB`,
      onChange: setter(F(SOUND_FIELDS, 'gainJitterDb'), 'volume variation'),
    }))
    vary.append(el('div', { class: 'hint', text: s.variants.length > 1
      ? `And it picks one of its ${s.variants.length} takes at random.`
      : 'Add takes (under the waveform) for more variety than a pitch change can give.' }))
    box.append(vary)

    // ---- distance ----
    const dist = el('div', { class: 'ig-sec' }, el('h3', { text: 'Distance' }))
    if (!spatial) {
      dist.append(el('div', { class: 'off-note', text: `The “${bus?.name}” group is not placed in the world, so distance does not apply.` }))
    } else {
      const f = (p) => F(ATTEN_FIELDS, p)
      dist.append(slider({ label: 'Full volume to', hint: 'closer than this, it is at full volume', min: 0, max: 200, step: 0.5,
        def: f('attenuation.minDist').def, value: getField(s, f('attenuation.minDist')), fmt: (v) => `${v} u`,
        onChange: setter(f('attenuation.minDist'), 'full volume distance') }))
      dist.append(slider({ label: 'Silent from', hint: 'further than this, it is not heard', min: 1, max: 1000, log: true,
        def: f('attenuation.maxDist').def, value: getField(s, f('attenuation.maxDist')), fmt: (v) => `${Math.round(v)} u`,
        onChange: (v) => setter(f('attenuation.maxDist'), 'silent distance')(Math.round(v * 10) / 10) }))
      dist.append(row('Fade', choice([['linear', 'Even'], ['exp', 'Natural'], ['expAttack', 'Late'], ['sCurve', 'Smooth']],
        getField(s, f('attenuation.curve')), setter(f('attenuation.curve'), 'distance fade'))))
      dist.append(slider({ label: 'Stereo width', hint: 'near sounds are wide, far ones narrow to a point; this is how wide the near ones get', min: 0, max: 1, step: 0.01,
        def: f('attenuation.spread').def, value: getField(s, f('attenuation.spread')), fmt: (v) => `${Math.round(v * 100)}%`,
        onChange: setter(f('attenuation.spread'), 'spread') }))
      dist.append(slider({ label: 'Muffled far away', hint: 'distant sounds lose their highs, like through air', min: 0, max: 20000, step: 100,
        def: 0, value: getField(s, f('attenuation.airLpfHz')), fmt: (v) => v === 0 ? 'off' : `to ${(v / 1000).toFixed(1)} kHz`,
        onChange: setter(f('attenuation.airLpfHz'), 'air') }))
      dist.append(slider({ label: 'Quieter behind', hint: 'how much quieter when it is behind the listener', min: -24, max: 0, step: 0.5,
        def: 0, value: getField(s, f('attenuation.rearDb')), fmt: (v) => v === 0 ? 'no' : `${v} dB`,
        onChange: setter(f('attenuation.rearDb'), 'rear') }))
    }
    box.append(dist)

    // ---- music ----
    const mus = el('div', { class: 'ig-sec' }, el('h3', { text: 'Music' }))
    const dbF = F(DUCK_FIELDS, 'duckMusic.db')
    const msF = F(DUCK_FIELDS, 'duckMusic.ms')
    mus.append(slider({ label: 'Lower music', hint: 'turn the music down while this plays — for big moments', min: -24, max: 0, step: 0.5,
      def: 0, value: getField(s, dbF), fmt: (v) => v === 0 ? 'no' : `${v} dB`,
      onChange: (v) => commit('lower music', (x) => {
        setField(x, dbF, v)
        if (v < 0 && !(x.duckMusic?.ms > 0)) setField(x, msF, 600)
        if (v === 0) delete x.duckMusic
      }, true) }))
    if ((s.duckMusic?.db ?? 0) < 0) {
      mus.append(slider({ label: 'For', min: 50, max: 8000, log: true, def: 600, value: getField(s, msF) || 600, fmt: msFmt,
        onChange: (v) => setter(msF, 'lower music for')(Math.round(v)) }))
    }
    box.append(mus)

    // ---- playback ----
    const pb = el('div', { class: 'ig-sec' }, el('h3', { text: 'Playback' }))
    pb.append(row('Repeats until stopped', toggle(!!s.loop, (on) => commit('loop', (x) => { if (on) x.loop = true; else delete x.loop }, true)),
      'For engines, alarms and ambience. The game has to stop it.'))
    const why = s.live ? null : liveReason(s)
    pb.append(row('Render live', toggle(!!s.live, (on) => commit('live', (x) => { if (on) x.live = true; else delete x.live }, true)),
      why ? `It will be live anyway: ${why}.` : 'Off: rendered once and cached, which is cheaper. On: rendered every time it plays.'))
    box.append(pb)
  }

  ctx.on((what) => {
    if (what === 'select' || what === 'opened' || what === 'structural') build()
  })
  return { build }
}
