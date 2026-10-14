//
// The only bridge between the editor page and the desktop shell.
//
// Everything is a narrow, typed call: no Node, no ipcRenderer, no file paths
// the page did not already get from a dialog. Anything crossing is normalised
// to bytes here, so the main process only ever sees bytes.
//
// The global is `digidream`, NOT a DigidreamSound-specific name, because that
// is what tool/lib/desktop.js probes -- and that module is vendored from
// midisynth byte-identically (scripts/check_lib_sync.sh). It is an internal
// contract between a page and its shell, not a product name.
//
const { contextBridge, ipcRenderer } = require('electron')

function toBytes (data) {
  if (typeof data === 'string') return new TextEncoder().encode(data)
  if (data instanceof Uint8Array) return data
  if (data instanceof ArrayBuffer) return new Uint8Array(data)
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
  throw new TypeError('save() takes a string or bytes')
}

contextBridge.exposeInMainWorld('digidream', {
  isDesktop: true,
  platform: process.platform,
  versions: { electron: process.versions.electron, chrome: process.versions.chrome },

  open: (opts = {}) => ipcRenderer.invoke('open', opts),
  save: (name, data, opts = {}) => ipcRenderer.invoke('save', name, toBytes(data), opts),
  saveMany: (files) => ipcRenderer.invoke('saveMany',
    files.map((f) => ({ name: f.name, data: toBytes(f.data) }))),
  confirm: (message, detail, ok) => ipcRenderer.invoke('confirm', message, detail, ok),
  ready: () => ipcRenderer.invoke('ready'),

  setDocument: (doc) => ipcRenderer.send('setDocument', doc),
  reveal: (path) => ipcRenderer.send('reveal', path),

  onMenu: (fn) => ipcRenderer.on('menu', (_e, action) => fn(action)),
  onOpenPath: (fn) => ipcRenderer.on('openPath', (_e, path) => fn(path)),
})
