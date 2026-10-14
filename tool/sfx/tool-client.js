//
// The promise face of tool-worker.js, plus the one cache the tool needs.
//
// Every call is request/response with an echoed id, the same shape
// midisynth/tool/lib/synth-client.js uses for the worklet port. Nothing here
// is clever; the point is that callers never see postMessage.
//
const WORKER_URL = new URL('../tool-worker.js', import.meta.url)

export class ToolClient {
  constructor () {
    this.worker = new Worker(WORKER_URL, { type: 'module' })
    this.worker.onmessage = (e) => this._reply(e.data)
    this.worker.onerror = (e) => this._fail(e.message || 'worker error')
    this._next = 1
    this._pending = new Map()

    // Thumbnails, keyed by CONTENT rather than by index.
    //
    // Content-addressing is what makes this cache impossible to get wrong:
    // there is no invalidation, because a key that still matches is a key
    // whose answer is still right. Keying on (soundIndex, variant) would need
    // eviction on every edit, every reorder and every undo, and one missed
    // path shows the author a picture of a sound they have already changed.
    this._thumbs = new Map()
    this._thumbLimit = 400
  }

  _reply (msg) {
    const p = this._pending.get(msg.id)
    if (!p) return
    this._pending.delete(msg.id)
    if (msg.ok) p.resolve(msg.result)
    else p.reject(new Error(msg.error))
  }

  _fail (message) {
    for (const [, p] of this._pending) p.reject(new Error(message))
    this._pending.clear()
  }

  send (type, args = {}, transfer = []) {
    const id = this._next++
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject })
      this.worker.postMessage({ id, type, ...args }, transfer)
    })
  }

  // ---- content ------------------------------------------------------------

  /** Load the whole project. ~1.2 s for a 101-sound bank, off the audio thread. */
  open (text) { return this.send('open', { text }) }

  bake (text) { return this.send('bake', { text }) }

  // ---- analysis -----------------------------------------------------------

  /**
   * A waveform envelope and a peak for one sound, from the project bank.
   *
   * `key` is a hash of the variant's patch supplied by the caller (it has the
   * document; we do not). Passing a falsy key bypasses the cache.
   */
  async thumb (sound, variant, key, buckets = 512) {
    if (key && this._thumbs.has(key)) return this._thumbs.get(key)
    const r = await this.send('thumb', { sound, variant, buckets })
    // NEVER memoise a failure. A zero-frame answer almost always means the
    // question was asked of a worker that did not have this bank yet -- and
    // caching it turns a momentary race into a sound that is permanently
    // reported as silent, with a content key that will never be invalidated
    // because the content never changed. Cost me a real bug.
    if (key && r && r.frames > 0) {
      if (this._thumbs.size >= this._thumbLimit) {
        // Insertion order is close enough to LRU for a bank-sized map, and a
        // real LRU here would be more machinery than the problem deserves.
        this._thumbs.delete(this._thumbs.keys().next().value)
      }
      this._thumbs.set(key, r)
    }
    return r
  }

  /** Validate and render one edited sound. 2-25 ms depending on the patch. */
  audition (sound, variant = 0, opts = {}) {
    return this.send('audition', { sound, variant, ...opts })
  }

  /** Validation only -- no render, so a whole-bank sweep is cheap. */
  validate (sound) { return this.send('validate', { sound }) }

  wav (sound, variant = 0) { return this.send('wav', { sound, variant }) }

  // ---- the fitter ---------------------------------------------------------

  archs () { return this.send('archs') }
  fitTarget (samples, bandHz = 24000) {
    return this.send('fitTarget', { samples, bandHz }, [samples.buffer])
  }
  fitConfigure (opts) { return this.send('fitConfigure', opts) }
  fitOnce (opts) { return this.send('fitOnce', opts) }
  recipe (arch, u) { return this.send('recipe', { arch, u }) }
  fitAnalyze (patch) { return this.send('fitAnalyze', { patch }) }

  dispose () { this.worker.terminate() }
}

/**
 * A stable content key for a variant.
 *
 * Deliberately NOT a cryptographic hash: this keys a picture cache, a
 * collision costs one wrong thumbnail until the next edit, and doing it
 * synchronously on the main thread for 101 sounds rules out SubtleCrypto,
 * which is async. FNV-1a over the serialised patch is enough and is ~0.1 ms
 * for a large patch.
 */
export function variantKey (sound, variantIndex) {
  const v = sound.variants?.[variantIndex]
  if (!v) return ''
  // gainDb, loop and live change the render; the rest of the policy does not.
  const text = JSON.stringify([v.patch, v.gesture, sound.gainDb, sound.loop, sound.live])
  let h = 0x811c9dc5
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i)
    h = Math.imul(h, 0x01000193)
  }
  return `${(h >>> 0).toString(36)}:${text.length}`
}
