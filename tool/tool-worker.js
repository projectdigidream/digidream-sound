//
// Everything offline: thumbnails, auditions, validation, baking, WAV export,
// and the fitter.
//
// WHY A WORKER AND NOT THE WORKLET. The obvious design is one engine that the
// whole tool drives, with the Design tab auditioning through it. Measured, that
// is wrong: loadBankJson costs 10-27 ms even for a single-sound bank and 667 ms
// for the 101-sound DOOM bank, and in the worklet that runs on the audio render
// thread -- four to ten dropped quanta on every debounced slider release.
// Worse, WebSfx::loadBankJson calls release() BEFORE it parses, so a patch that
// fails validation does not merely get refused, it unloads the bank and
// silences the engine mid-edit.
//
// So the split is by purpose, not by tab: the worklet owns playback and never
// reloads because a tab changed, and this worker owns everything that produces
// DATA. Auditioning is then a rendered buffer handed to an AudioBufferSourceNode
// -- and the waveform, the thumbnail and the peak all fall out of that same
// render instead of being computed twice.
//
// It loads dsound-tool.mjs, which is the engine plus the fitter. One module
// instance, one heap, three objects inside it:
//
//   bank_   the whole project, indexed by sound/variant, for thumbnails
//   scratch a one-sound bank, reloaded per edit, for audition and validation
//   fitter_ the SfxFitter
//
// Two WebSfx objects share the module's heap, so the second one costs what its
// data costs and not another WebAssembly.Memory.
//
import createSfx from './dist/dsound-tool.mjs'

let mod = null
let bank = null      // WebSfx holding the whole project
let scratch = null   // WebSfx holding one sound
let fitter = null

const SAMPLE_SHIFT = 24          // Q8.24
const Q = 1 / (1 << SAMPLE_SHIFT)

async function ready () {
  if (mod) return
  mod = await createSfx()
  bank = new mod.WebSfx()
  scratch = new mod.WebSfx()
}

// ---------------------------------------------------------------------------
// The scratch bank.
//
// A neutral document holding exactly one sound, so an edit re-renders in ~10 ms
// regardless of how large the project is.
//
// What it KEEPS and why:
//   name, gainDb   the sound's own level, so a peak means something
//   loop           NOT optional. sfx_json.cpp gates its "voice can never end"
//                  error on !loop, so dropping it turns a perfectly valid
//                  looping sound into a hard load failure. The fitter's own
//                  scratch bank gets away without it only because all nine
//                  architectures hard-code oneShot; an authored bank has no
//                  such guarantee, and doom2 has two loops.
//   live           so the render path matches what the engine will do
//
// What it DROPS: bus, priority, maxInstances, onLimit, cooldownMs, attenuation,
// duckMusic, tags -- placement and policy, none of which affects the waveform --
// and both jitters, which is what makes a thumbnail stable across renders. That
// last one is deliberate and someone will try to "fix" it.
//
// fx and master are omitted entirely; an absent master section means bypass.
// ---------------------------------------------------------------------------
function scratchDoc (sound) {
  return JSON.stringify({
    version: 1,
    name: 'scratch',
    buses: [{ name: 'b', spatial: false }],
    sounds: [{
      name: sound.name || 'scratch',
      ...(sound.gainDb !== undefined ? { gainDb: sound.gainDb } : {}),
      ...(sound.loop ? { loop: true } : {}),
      ...(sound.live ? { live: true } : {}),
      variants: sound.variants,
    }],
  })
}

/** Q8.24 interleaved stereo in the heap -> two planar Float32Arrays. */
function readStereo (ptr, frames) {
  const words = frames * 2
  const src = mod.HEAP32.subarray(ptr >> 2, (ptr >> 2) + words)
  const l = new Float32Array(frames)
  const r = new Float32Array(frames)
  for (let i = 0; i < frames; i++) {
    l[i] = src[i * 2] * Q
    r[i] = src[i * 2 + 1] * Q
  }
  return { l, r }
}

/**
 * A min/max envelope at `buckets` resolution.
 *
 * Computed here rather than on the main thread because it turns a 4-second
 * stereo render (768k samples) into a few hundred pairs before it crosses the
 * postMessage boundary, and because drawing wants exactly this and nothing else.
 */
function envelope (ptr, frames, buckets = 512) {
  const words = frames * 2
  const src = mod.HEAP32.subarray(ptr >> 2, (ptr >> 2) + words)
  const n = Math.min(buckets, Math.max(1, frames))
  const min = new Float32Array(n)
  const max = new Float32Array(n)
  const per = frames / n
  for (let b = 0; b < n; b++) {
    const from = Math.floor(b * per)
    const to = Math.min(frames, Math.floor((b + 1) * per)) || from + 1
    let lo = 0
    let hi = 0
    for (let i = from; i < to; i++) {
      // Mono sum for the picture; the peak column uses the engine's own
      // unclipped figure, not this.
      const v = (src[i * 2] + src[i * 2 + 1]) * 0.5 * Q
      if (v < lo) lo = v
      if (v > hi) hi = v
    }
    min[b] = lo
    max[b] = hi
  }
  return { min, max }
}

function reportOf (json) {
  try { return JSON.parse(json) } catch { return { ok: false, warnings: [], errors: [] } }
}

// ---------------------------------------------------------------------------

const handlers = {
  async open ({ text }) {
    const rep = reportOf(bank.loadBankJson(text))
    return {
      ...rep,
      soundCount: bank.soundCount(),
      info: rep.ok ? JSON.parse(bank.bankInfoJson()) : null,
    }
  },

  // A thumbnail from the PROJECT bank, by index. These bytes are the cache's
  // own -- test/wasm_variant.mjs pins that they equal the native render -- so
  // the picture is of the sound the game plays.
  async thumb ({ sound, variant = 0, buckets = 512 }) {
    const ptr = bank.renderVariant(sound, variant, false)
    const info = JSON.parse(bank.renderVariantInfoJson())
    if (!ptr || !info.frames) return { ...info, min: null, max: null }
    const env = envelope(ptr, info.frames, buckets)
    return { ...info, ...env }
  },

  // Validate one edited sound and render it, in one round trip: the Design tab
  // wants both after every apply and they share the load.
  async audition ({ sound, variant = 0, buckets = 512, wantAudio = true }) {
    const rep = reportOf(scratch.loadBankJson(scratchDoc(sound)))
    if (!rep.ok) return { ...rep, info: null }

    const ptr = scratch.renderVariant(0, variant, false)
    const info = JSON.parse(scratch.renderVariantInfoJson())
    if (!ptr || !info.frames) return { ...rep, info, min: null, max: null }

    const env = envelope(ptr, info.frames, buckets)
    const out = { ...rep, info, ...env }
    if (wantAudio) {
      const { l, r } = readStereo(ptr, info.frames)
      out.left = l
      out.right = r
    }
    return out
  },

  // Validation alone, for the Bank tab's problems column: no render, so a
  // whole-bank sweep is cheap.
  async validate ({ sound }) {
    return reportOf(scratch.loadBankJson(scratchDoc(sound)))
  },

  async bake ({ text }) {
    const rep = reportOf(bank.bakeToBuffer(text))
    if (!rep.ok) return rep
    const size = bank.bakedSize()
    const bytes = new Uint8Array(size)
    bytes.set(mod.HEAPU8.subarray(bank.bakedPtr(), bank.bakedPtr() + size))
    return { ...rep, bytes }
  },

  // 48 kHz stereo 16-bit, the same format dsound_render writes.
  async wav ({ sound, variant = 0 }) {
    const ptr = scratch.loadBankJson(scratchDoc(sound))
    const rep = reportOf(ptr)
    if (!rep.ok) return rep
    const p = scratch.renderVariant(0, variant, false)
    const info = JSON.parse(scratch.renderVariantInfoJson())
    if (!p || !info.frames) return { ok: false, errors: [{ text: 'nothing rendered' }] }

    const frames = info.frames
    const src = mod.HEAP32.subarray(p >> 2, (p >> 2) + frames * 2)
    const buf = new ArrayBuffer(44 + frames * 4)
    const dv = new DataView(buf)
    const ascii = (at, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(at + i, s.charCodeAt(i)) }
    ascii(0, 'RIFF'); dv.setUint32(4, 36 + frames * 4, true); ascii(8, 'WAVE')
    ascii(12, 'fmt '); dv.setUint32(16, 16, true); dv.setUint16(20, 1, true)
    dv.setUint16(22, 2, true); dv.setUint32(24, 48000, true)
    dv.setUint32(28, 48000 * 4, true); dv.setUint16(32, 4, true); dv.setUint16(34, 16, true)
    ascii(36, 'data'); dv.setUint32(40, frames * 4, true)
    for (let i = 0; i < frames * 2; i++) {
      // Q8.24 -> i16 with the same clamp the engine's output stage uses.
      let v = src[i] >> 9
      if (v > 32767) v = 32767
      if (v < -32768) v = -32768
      dv.setInt16(44 + i * 2, v, true)
    }
    return { ok: true, warnings: [], errors: [], bytes: new Uint8Array(buf), frames }
  },

  // ---- the fitter ----------------------------------------------------------

  async archs () {
    if (!fitter) fitter = new mod.SfxFitter()
    return { archs: JSON.parse(fitter.archListJson()) }
  },

  // Mono float at 48 kHz, already resampled by the caller.
  // `bandHz` is the reference's bandwidth: half its ORIGINAL sample rate,
  // before decodeAudioData resampled it. Nothing above it is scored.
  async fitTarget ({ samples, bandHz = 24000 }) {
    if (!fitter) fitter = new mod.SfxFitter()
    const bytes = samples.length * 4
    const ptr = mod._malloc(bytes)
    mod.HEAPF32.set(samples, ptr >> 2)
    fitter.setTarget(ptr, samples.length, bandHz)
    mod._free(ptr)
    return { ok: true, frames: samples.length }
  },

  async fitConfigure ({ arch, stft = 1.0, env = 0.05, bands = 0.04, windowMs = 0 }) {
    if (!fitter) return { ok: false, error: 'no target' }
    const ok = fitter.configure(arch, stft, env, bands, windowMs)
    return { ok, frames: fitter.targetFrames() }
  },

  // ONE RESTART. The panel loops and posts progress between calls, so a long
  // fit stays cancellable and can show `restart k/N` -- a call that ran to
  // completion would freeze the worker for minutes with no way out.
  async fitOnce ({ iters = 60, seed = 0 }) {
    if (!fitter) return { error: 'no fitter' }
    const r = JSON.parse(fitter.fitOnce(iters, seed))
    r.heapMb = fitter.heapUsedBytes() / (1024 * 1024)
    return r
  },

  // A recipe -> a patch: the "Start from" generators and the recipe sliders.
  // No target and no fit, so it needs no configure() first.
  async recipe ({ arch, u }) {
    if (!fitter) fitter = new mod.SfxFitter()
    const s = fitter.buildPatch(arch, JSON.stringify(u ?? []))
    return s ? { ok: true, patch: JSON.parse(s) } : { ok: false, error: `no architecture '${arch}'` }
  },

  async fitAnalyze ({ patch }) {
    if (!fitter) return { error: 'no fitter' }
    return JSON.parse(fitter.analyze(typeof patch === 'string' ? patch : JSON.stringify(patch)))
  },
}

// Transferables: buffers move rather than copy, which matters for a 101-sound
// thumbnail sweep and for audition buffers on every keystroke.
function transfers (r) {
  const t = []
  for (const k of ['min', 'max', 'left', 'right', 'bytes']) {
    if (r && r[k] && r[k].buffer) t.push(r[k].buffer)
  }
  return t
}

self.onmessage = async (e) => {
  const { id, type, ...args } = e.data
  try {
    await ready()
    const fn = handlers[type]
    if (!fn) throw new Error(`unknown message '${type}'`)
    const result = await fn(args)
    self.postMessage({ id, ok: true, result }, transfers(result))
  } catch (err) {
    self.postMessage({ id, ok: false, error: err && err.message ? err.message : String(err) })
  }
}
