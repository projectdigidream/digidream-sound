//
// Drawing a rendered sound.
//
// The worker hands back a min/max envelope rather than samples, so this only
// ever draws a few hundred pairs however long the sound is. That is the whole
// reason the envelope is computed over there.
//
const DPR = () => (globalThis.devicePixelRatio || 1)

/** Size a canvas for the display, once, and return its 2D context. */
export function fit (canvas) {
  const r = DPR()
  const w = canvas.clientWidth || canvas.width
  const h = canvas.clientHeight || canvas.height
  if (canvas.width !== Math.round(w * r) || canvas.height !== Math.round(h * r)) {
    canvas.width = Math.round(w * r)
    canvas.height = Math.round(h * r)
  }
  const ctx = canvas.getContext('2d')
  ctx.setTransform(r, 0, 0, r, 0, 0)
  return { ctx, w, h }
}

const css = (name, fallback) => {
  try {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim()
    return v || fallback
  } catch { return fallback }
}

/**
 * Draw an envelope.
 *
 * `opts.clip` draws the region above full scale in the warning colour. That
 * matters here in a way it would not in a normal waveform view: these samples
 * are unclipped Q8.24 straight from renderVariant, so a sound CAN exceed 1.0,
 * and it is the one thing a peak column exists to show. Drawing it the same
 * colour as everything else would hide exactly the problem.
 */
export function drawEnvelope (canvas, env, opts = {}) {
  const { ctx, w, h } = fit(canvas)
  const bg = opts.background ?? css('--panel2', '#23262e')
  ctx.clearRect(0, 0, w, h)
  if (bg !== 'none') { ctx.fillStyle = bg; ctx.fillRect(0, 0, w, h) }

  const mid = h / 2
  ctx.strokeStyle = css('--edge', '#32363f')
  ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke()

  if (!env || !env.min || !env.max || env.min.length === 0) {
    ctx.fillStyle = css('--dim', '#8a8f9c')
    ctx.font = '11px ui-monospace, monospace'
    ctx.fillText(opts.emptyText ?? 'silent', 6, mid - 4)
    return
  }

  // Scale so a sound that clips is visibly taller than the lane, not flattened
  // against it. `1` means full scale sits at 80% of the height.
  const headroom = opts.headroom ?? 1.25
  const scale = (h / 2) / headroom

  const n = env.min.length
  const colour = opts.color ?? css('--wave', '#6fd66f')
  const hot = opts.clipColor ?? css('--bad', '#e07070')

  for (let i = 0; i < n; i++) {
    const x = (i / n) * w
    const lo = env.min[i]
    const hi = env.max[i]
    const over = lo < -1 || hi > 1
    ctx.fillStyle = over ? hot : colour
    const y0 = mid - hi * scale
    const y1 = mid - lo * scale
    ctx.fillRect(x, y0, Math.max(1, w / n), Math.max(1, y1 - y0))
  }

  if (opts.clip !== false) {
    // Where full scale sits, so "how close am I" is readable at a glance.
    ctx.strokeStyle = css('--warn', '#e0b060')
    ctx.globalAlpha = 0.35
    ctx.beginPath()
    ctx.moveTo(0, mid - scale); ctx.lineTo(w, mid - scale)
    ctx.moveTo(0, mid + scale); ctx.lineTo(w, mid + scale)
    ctx.stroke()
    ctx.globalAlpha = 1
  }
}

/** A peak in dBFS from the engine's own millibel figure. */
export function peakLabel (peakDbMilli) {
  if (peakDbMilli === undefined || peakDbMilli <= -96000) return '—'
  const db = peakDbMilli / 1000
  return (db > 0 ? '+' : '') + db.toFixed(1)
}

/** Milliseconds from a frame count at the engine's fixed rate. */
export const msOf = (frames) => Math.round((frames / 48000) * 1000)
