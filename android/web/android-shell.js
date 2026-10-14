//
// The Android shell's half of window.digidream.
//
// MainActivity injects this as the first classic script in sfx.html's <head>,
// so it runs before the page's module graph -- and tool/lib/desktop.js reads
// globalThis.digidream exactly once, at import. It defines the same object
// electron/preload.js defines, with the same methods and the same answers, so
// the vendored desktop.js (byte-identical to midisynth's) takes its native path
// and nothing in tool/sfx/ knows which shell it is in.
//
// The Java side is `dsndNative`: every call carries an id, and the answer
// comes back as window.__dsndReply(id, value). Bytes cross as base64, because
// a JavascriptInterface passes strings and nothing else.
//
// It also puts every uncaught error ON THE SCREEN. The phone this runs on has
// no adb and no logcat; a banner with the message and the build stamp means a
// screenshot is a bug report.
//
(() => {
  const N = window.dsndNative
  if (!N) return

  const pending = new Map()
  let next = 1
  window.__dsndReply = (id, value) => {
    const p = pending.get(id)
    if (!p) return
    pending.delete(id)
    if (value && typeof value === 'object' && !Array.isArray(value) && value.error) {
      p.reject(new Error(value.error))
    } else {
      p.resolve(value)
    }
  }
  const call = (fn, ...args) => new Promise((resolve, reject) => {
    const id = String(next++)
    pending.set(id, { resolve, reject })
    try { N[fn](id, ...args) } catch (e) { pending.delete(id); reject(e) }
  })

  const toBytes = (data) => {
    if (typeof data === 'string') return new TextEncoder().encode(data)
    if (data instanceof Uint8Array) return data
    if (data instanceof ArrayBuffer) return new Uint8Array(data)
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
    throw new TypeError('save() takes a string or bytes')
  }
  const b64 = (data) => {
    const b = toBytes(data)
    let s = ''
    // Chunked: spreading a multi-megabyte WAV into one call overflows the stack.
    for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000))
    return btoa(s)
  }
  const unb64 = (s) => {
    const bin = atob(s)
    const out = new Uint8Array(bin.length)
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
    return out
  }
  // application/octet-stream for everything but audio, and NOT application/json
  // for a .sfxproj: given a MIME type it knows, the system picker "fixes" the
  // name to match it and saves bank.sfxproj.json, which File > Open then has to
  // be told about. Octet-stream is the one type it leaves the name alone for.
  const mimeOf = (name) => /\.wav$/i.test(name) ? 'audio/wav' : 'application/octet-stream'

  window.digidream = {
    isDesktop: true,
    isAndroid: true,
    platform: 'android',
    versions: { webview: (navigator.userAgent.match(/Chrome\/([\d.]+)/) || [])[1] || '' },

    open: async (opts = {}) => {
      const files = await call('open', JSON.stringify(opts.extensions || []), !!opts.multiple)
      return (files || []).map((f) => ({ name: f.name, path: f.path, data: unb64(f.data) }))
    },
    save: (name, data, opts = {}) => call('save', name, mimeOf(name), b64(data), opts.toPath || ''),
    saveMany: (files) => call('saveMany',
      JSON.stringify(files.map((f) => ({ name: f.name, data: b64(f.data) })))),
    confirm: (message, detail, ok) => call('confirm', String(message), detail || '', ok || 'OK'),
    // No launch file: nothing opens this app with a document (yet).
    ready: async () => null,

    setDocument: (doc) => { try { N.setDocument(JSON.stringify(doc || {})) } catch {} },
    reveal: () => {},
    // There is no native menu; File lives in the page's own header.
    onMenu: () => {},
    onOpenPath: () => {},
  }

  // ---- errors, visibly ------------------------------------------------------
  let box = null
  function banner (text) {
    if (!document.body) { addEventListener('DOMContentLoaded', () => banner(text)); return }
    if (!box) {
      box = document.createElement('div')
      box.id = 'android-errors'
      box.style.cssText = 'position:fixed;left:8px;right:8px;bottom:8px;z-index:99999;' +
        'max-height:45vh;overflow:auto;background:#3a1212;color:#ffd7d2;border:1px solid #ff8a80;' +
        'border-radius:8px;padding:10px 12px;font:12px/1.4 ui-monospace,monospace;white-space:pre-wrap'
      const head = document.createElement('div')
      head.textContent = `DigidreamSound error (tap to dismiss)\n${N.build ? N.build() : ''}`
      head.style.cssText = 'font-weight:bold;margin-bottom:6px'
      box.appendChild(head)
      box.onclick = () => { box.remove(); box = null }
      document.body.appendChild(box)
    }
    const line = document.createElement('div')
    line.textContent = text
    line.style.marginTop = '6px'
    box.appendChild(line)
  }
  addEventListener('error', (e) => {
    // A resource that failed to load (a <script src>, say) fires here with no
    // message; name the file instead.
    const t = e.target && e.target !== window && (e.target.src || e.target.href)
    banner(t ? `failed to load ${t}` : `${e.message}\n  at ${e.filename}:${e.lineno}`)
  }, true)
  addEventListener('unhandledrejection', (e) => {
    const r = e.reason
    banner(r && r.stack ? r.stack : String(r))
  })
})()
