//
// Hearing the selected sound, from anywhere in the tool.
//
// This is shared rather than living in the Design tab because "play the thing
// I have selected" is not a Design-tab question -- you want it while scanning
// the Bank grid, while reading the policy rail, and while fitting.
//
// It plays a RENDERED BUFFER, not the live engine. The worker hands back the
// variant's samples, they go into an AudioBuffer, and an AudioBufferSourceNode
// plays it. That keeps a 2-25 ms reload off the audio thread, makes a rejected
// patch cost nothing, and means the waveform on screen and the sound in your
// ears come from one render rather than two.
//
// The Play tab is the other half of this and is deliberately different: it
// drives the real engine through the AudioWorklet, because voice limiting,
// stealing and bus budgets only exist there. This module cannot show you any
// of that, and is not trying to.
//
export class Audition {
  constructor (ctx) {
    this.ctx = ctx
    this.audio = null
    this.buffer = null
    this.info = null
    this.source = null
    this.key = null        // which (sound, variant) the buffer holds
    this.pending = 0
    this.loop = false
    this.listeners = new Set()
  }

  on (fn) { this.listeners.add(fn); return () => this.listeners.delete(fn) }
  _emit () { for (const fn of this.listeners) fn(this) }

  /** Lazily built, and only ever from a user gesture upstream. */
  context () {
    if (!this.audio) this.audio = new AudioContext({ sampleRate: 48000 })
    if (this.audio.state === 'suspended') this.audio.resume()
    return this.audio
  }

  get ready () { return !!this.buffer }
  get playing () { return !!this.source }

  /**
   * Render (sound, variant) and hold the result.
   *
   * Stale renders are dropped rather than cancelled: the worker has no cancel,
   * and a sequence number is cheaper than one.
   */
  async load (soundIndex, variant = 0) {
    const doc = this.ctx.store.doc
    const s = doc.sounds[soundIndex]
    const seq = ++this.pending
    if (!s) { this.buffer = null; this.info = null; this.key = null; this._emit(); return null }

    let r
    try {
      r = await this.ctx.client.audition(s, variant)
    } catch (e) {
      this.ctx.log(`audition: ${e.message}`, 'err')
      return null
    }
    if (seq !== this.pending) return null    // a newer selection won

    if (!r.ok || !r.left) {
      this.buffer = null
      this.info = null
      this.key = `${soundIndex}:${variant}`
      this._emit()
      return r
    }

    const c = this.context()
    const b = c.createBuffer(2, r.left.length, 48000)
    b.copyToChannel(r.left, 0)
    b.copyToChannel(r.right ?? r.left, 1)
    this.buffer = b
    this.info = r.info
    this.key = `${soundIndex}:${variant}`
    this._emit()
    return r
  }

  /**
   * Take a render someone else already asked for.
   *
   * The Design tab re-renders on every apply to draw its waveform and read its
   * peak. Without this the transport would ask the worker for the same samples
   * again, so an edit would cost two renders and the button could briefly hold
   * the previous version of a patch you had just changed.
   */
  adopt (r, soundIndex, variant = 0) {
    if (!r || !r.left || !r.left.length) return false
    this.pending++            // any in-flight load() for this slot is now stale
    const c = this.context()
    const b = c.createBuffer(2, r.left.length, 48000)
    b.copyToChannel(r.left, 0)
    b.copyToChannel(r.right ?? r.left, 1)
    this.buffer = b
    this.info = r.info
    this.key = `${soundIndex}:${variant}`
    this._emit()
    return true
  }

  /** Retrigger: a second press restarts rather than layering. */
  play () {
    if (!this.buffer) return false
    this.stop()
    const c = this.context()
    const src = c.createBufferSource()
    src.buffer = this.buffer
    // Looping here is a LISTENING aid, not the sound's `loop` policy flag.
    // A one-shot repeated on a timer is how you judge a transient, and it says
    // nothing about whether the engine would treat the sound as looping -- that
    // is sound.loop in the policy rail, which also forces the sound live.
    //
    // It also loops the WHOLE render including its silent tail, so the gap you
    // hear is the sound's own decay rather than a seam.
    src.loop = this.loop
    // The render is DRY: a sound's volume is applied at playback in the
    // engine, so it has to be applied here too, or every sound is auditioned
    // at 0 dB whatever its volume slider says.
    const g = c.createGain()
    g.gain.value = Math.pow(10, (this.gainDb ?? 0) / 20)
    src.connect(g)
    g.connect(c.destination)
    src.onended = () => {
      if (this.source === src) { this.source = null; this._emit() }
    }
    src.start()
    this.source = src
    this._emit()
    return true
  }

  /** The sound's volume, applied at playback exactly as the engine does. */
  setGainDb (db) { this.gainDb = db }

  /** Toggle looping; takes effect on the playing source immediately. */
  setLoop (on) {
    this.loop = !!on
    if (this.source) this.source.loop = this.loop
    this._emit()
  }

  /** Play, or stop if already playing -- what a single big button should do. */
  toggle () {
    if (this.playing) { this.stop(); return false }
    return this.play()
  }

  stop () {
    if (!this.source) return
    try { this.source.stop() } catch { /* already finished */ }
    this.source = null
    this._emit()
  }
}
