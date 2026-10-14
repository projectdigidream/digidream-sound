//
// The engine, in the audio thread.
//
// This is the ONLY place in the tool that makes sound through the real engine.
// Everything else -- the Bank grid's waveforms, the Design tab's audition, the
// fitter -- goes through tool-worker.js and plays a rendered buffer, because
// loadBankJson costs 2-25 ms and running it here would drop audio quanta.
//
// The division is by what each can show you. A rendered buffer tells you what a
// sound IS. It cannot tell you anything about cooldowns, instance caps, bus
// budgets, stealing or priority, because none of that exists until sounds
// compete for a pool in real time. That is what the Play tab is for, and it is
// why the engine has to be here rather than mocked.
//
// It loads dsound.mjs -- the ENGINE module, not the tool one. What a game
// ships is what you hear.
//
import createSfx from './dist/dsound.mjs'

const BLOCK = 128        // an AudioWorklet render quantum
const CONTROL = 64       // the engine's control block; frames must be a multiple

class SfxProcessor extends AudioWorkletProcessor {
  constructor () {
    super()
    this.ready = false
    this.pending = []
    this.sfx = null
    this.mod = null
    this.sinceStatus = 0
    this.loaded = false

    // A master ramp used around bank loads. A 1.2 s load on the audio thread
    // is going to glitch whatever we do; a deliberate silence reads as
    // "loading" and a buzz reads as "broken".
    this.muted = false

    this.port.onmessage = (e) => {
      if (!this.ready) this.pending.push(e.data)
      else this.handle(e.data)
    }

    createSfx().then((mod) => {
      this.mod = mod
      this.sfx = new mod.WebSfx()
      this.ready = true
      for (const m of this.pending) this.handle(m)
      this.pending = []
      this.port.postMessage({ type: 'ready' })
    }).catch((err) => {
      this.port.postMessage({ type: 'fatal', error: String(err) })
    })
  }

  reply (type, extra = {}) {
    this.port.postMessage({ type, requestId: this.rid, ...extra })
  }

  handle (m) {
    const s = this.sfx
    this.rid = m.requestId
    switch (m.type) {
      case 'loadBank': {
        // Mute first. The load blocks this thread for as long as it takes, and
        // whatever was ringing would be frozen mid-sample otherwise.
        this.muted = true
        s.stopAll(0)
        const rep = JSON.parse(s.loadBankJson(m.json))
        this.loaded = rep.ok
        if (rep.ok) JSON.parse(s.buildCache(m.budgetKb | 0))
        this.muted = false
        this.reply('bank', { report: rep, sounds: this.loaded ? s.soundCount() : 0 })
        break
      }
      case 'unload':
        s.unloadBank()
        this.loaded = false
        this.reply('bank', { report: { ok: true, warnings: [], errors: [] }, sounds: 0 })
        break

      case 'play':
        // A handle comes back so the caller can stop or move this instance --
        // but it is not a promise of audibility. A cooldown, an instance cap, a
        // bus budget or a higher-priority neighbour can all still refuse it,
        // which is exactly what the Play tab exists to make visible.
        this.reply('handle', {
          handle: m.pos
            ? s.play(m.sound | 0, m.variant ?? -1, m.seed >>> 0,
                     m.pos[0] | 0, m.pos[1] | 0, m.pos[2] | 0)
            : s.play2d(m.sound | 0, m.variant ?? -1, m.seed >>> 0, m.pan | 0),
        })
        break
      case 'stop': s.stop(m.handle >>> 0, m.fadeBlocks | 0); break
      case 'stopBus': s.stopBus(m.bus | 0, m.fadeBlocks | 0); break
      case 'stopAll': s.stopAll(m.fadeBlocks | 0); break
      case 'listener':
        s.setListener(m.x | 0, m.y | 0, m.z | 0, m.yaw >>> 0)
        break
      case 'setPosition': s.setPosition(m.handle >>> 0, m.x | 0, m.y | 0, m.z | 0); break
      case 'busGain': s.busGain(m.bus | 0, m.gain | 0, m.rampBlocks | 0); break
      case 'masterVolume': s.masterVolumeMilli(m.milli | 0); break
      case 'diag':
        this.reply('diag', { diag: JSON.parse(s.diagJson()) })
        break
      case 'info':
        this.reply('info', { info: this.loaded ? JSON.parse(s.bankInfoJson()) : null })
        break
      default:
        this.reply('error', { error: `unknown message '${m.type}'` })
    }
  }

  process (_inputs, outputs) {
    const out = outputs[0]
    if (!out || out.length === 0) return true
    const left = out[0]
    const right = out.length > 1 ? out[1] : out[0]
    const n = left.length

    if (!this.ready || !this.loaded || this.muted) {
      left.fill(0)
      if (right !== left) right.fill(0)
      return true
    }

    // AudioWorklet gives 128 frames; the engine wants a multiple of 64. Those
    // agree, so no partial block ever reaches it -- a partial one would be
    // filled with silence rather than rendered and the engine would fall half
    // a block out of step with its own clock.
    const frames = Math.floor(n / CONTROL) * CONTROL
    if (frames > 0) {
      const ptr = this.sfx.render(frames)
      if (ptr) {
        // HEAP16 is re-read every block rather than cached. The heap cannot
        // grow (ALLOW_MEMORY_GROWTH=0) so the view would in fact stay valid,
        // but caching it is the kind of thing that survives until someone
        // enables growth and then fails as silence.
        const pcm = this.mod.HEAP16
        const base = ptr >> 1
        for (let i = 0; i < frames; i++) {
          left[i] = pcm[base + i * 2] / 32768
          right[i] = pcm[base + i * 2 + 1] / 32768
        }
      }
    }
    for (let i = frames; i < n; i++) { left[i] = 0; right[i] = 0 }

    // Diagnostics, a few times a second. The Play tab is a live picture of
    // rejection reasons, so the rate has to be high enough to see a burst.
    this.sinceStatus += n
    if (this.sinceStatus >= 4096) {
      this.sinceStatus = 0
      this.port.postMessage({ type: 'diag', diag: JSON.parse(this.sfx.diagJson()) })
    }
    return true
  }
}

registerProcessor('dsound', SfxProcessor)
