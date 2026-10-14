//
// The promise face of sfx-worklet.js.
//
// Request/response over the worklet port with an echoed id, plus a `diag`
// event the worklet pushes unprompted a few times a second. Same shape as
// midisynth/tool/lib/synth-client.js.
//
const WORKLET_URL = new URL('../sfx-worklet.js', import.meta.url)

export class SfxClient extends EventTarget {
  constructor () {
    super()
    this.ctx = null
    this.node = null
    this.analyser = null
    this._next = 1
    this._pending = new Map()
    this.loaded = false
    this.diag = null
  }

  /**
   * Build the graph. MUST be called from a user gesture: a browser will not
   * start an AudioContext otherwise, and on iOS it will silently stay
   * suspended rather than throw.
   */
  static async create ({ analyser = true } = {}) {
    const c = new SfxClient()
    c.ctx = new AudioContext({ sampleRate: 48000 })
    await c.ctx.audioWorklet.addModule(WORKLET_URL)
    c.node = new AudioWorkletNode(c.ctx, 'dsound', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
    })

    const ready = new Promise((resolve, reject) => {
      const onReady = (e) => {
        if (e.data.type === 'ready') { c.node.port.removeEventListener('message', onReady); resolve() }
        if (e.data.type === 'fatal') { reject(new Error(e.data.error)) }
      }
      c.node.port.addEventListener('message', onReady)
    })

    c.node.port.onmessage = (e) => c._message(e.data)
    c.node.port.start()

    if (analyser) {
      c.analyser = c.ctx.createAnalyser()
      c.analyser.fftSize = 2048
      c.node.connect(c.analyser)
      c.analyser.connect(c.ctx.destination)
    } else {
      c.node.connect(c.ctx.destination)
    }

    await ready
    return c
  }

  _message (m) {
    if (m.type === 'diag' && m.requestId === undefined) {
      // Unprompted: the worklet's periodic push.
      this.diag = m.diag
      this.dispatchEvent(new CustomEvent('diag', { detail: m.diag }))
      return
    }
    if (m.type === 'fatal') {
      this.dispatchEvent(new CustomEvent('fatal', { detail: m.error }))
      return
    }
    const p = this._pending.get(m.requestId)
    if (!p) return
    this._pending.delete(m.requestId)
    p.resolve(m)
  }

  send (type, args = {}) {
    const requestId = this._next++
    return new Promise((resolve) => {
      this._pending.set(requestId, { resolve })
      this.node.port.postMessage({ requestId, type, ...args })
    })
  }

  /** Fire and forget: the hot path, where a round trip would be waste. */
  post (type, args = {}) { this.node.port.postMessage({ type, ...args }) }

  async resume () { if (this.ctx.state === 'suspended') await this.ctx.resume() }

  /**
   * Wait until the audio thread is actually rendering.
   *
   * `ctx.state === 'running'` is not that: it means the graph has been told to
   * start, and `currentTime` can sit still for a moment afterwards while the
   * device opens. Anything triggered in that window is queued against a clock
   * that has not moved, and the first block applies all of it at once -- so a
   * trigger fired immediately after "start" looks like it did nothing.
   *
   * Resolves false on timeout rather than throwing: a tool that refuses to
   * open because an audio device was slow is worse than one that says so.
   */
  async waitForClock (timeoutMs = 2000) {
    const t0 = this.ctx.currentTime
    const deadline = Date.now() + timeoutMs
    while (Date.now() < deadline) {
      if (this.ctx.currentTime > t0) return true
      await new Promise((r) => setTimeout(r, 16))
    }
    return false
  }

  async loadBank (json, budgetKb = 0) {
    const r = await this.send('loadBank', { json, budgetKb })
    this.loaded = r.report?.ok ?? false
    return r
  }

  unload () { return this.send('unload') }
  play (sound, opts = {}) { return this.send('play', { sound, ...opts }) }
  playNow (sound, opts = {}) { this.post('play', { sound, ...opts }) }
  stopAll (fadeBlocks = 0) { this.post('stopAll', { fadeBlocks }) }
  stopBus (bus, fadeBlocks = 0) { this.post('stopBus', { bus, fadeBlocks }) }
  listener (x, y, z, yaw) { this.post('listener', { x, y, z, yaw }) }
  busGain (bus, gain, rampBlocks = 0) { this.post('busGain', { bus, gain, rampBlocks }) }
  masterVolume (milli) { this.post('masterVolume', { milli }) }
  async getDiag () { return (await this.send('diag')).diag }
  async info () { return (await this.send('info')).info }

  close () { try { this.ctx.close() } catch { /* already closed */ } }
}

// Q16.16, the engine's world-unit fixed point. One unit is 48 Doom units,
// about 1.46 m.
export const q16 = (v) => Math.round(v * 65536)

// Binary radians about +Z, 0 facing +X. NOT the renderer's yaw: a Three.js
// camera is Y-up with Z = -y, and its yaw pans everything backwards.
export const brad = (radians) =>
  (Math.round((radians / (Math.PI * 2)) * 4294967296) >>> 0)
