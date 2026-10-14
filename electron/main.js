//
// The desktop shell.
//
// It serves the staged payload over a custom `dsnd://` protocol rather than
// file://, for one reason that matters: ES modules and AudioWorklet need a
// real origin, and file:// is not one. `dsnd://app/...` is a secure context,
// so the page runs exactly as it does on a web host -- which is the point, and
// the reason scripts/package_electron.sh stages the SAME payload that
// package_web.sh produces.
//
const { app, BrowserWindow, Menu, dialog, ipcMain, protocol, shell } = require('electron')
const fs = require('node:fs/promises')
const path = require('node:path')

const PAYLOAD = path.join(__dirname, 'payload')
let win = null
let launchFile = null

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.css': 'text/css; charset=utf-8',
  '.sfxproj': 'application/json; charset=utf-8',
  '.wav': 'audio/wav',
}

protocol.registerSchemesAsPrivileged([{
  scheme: 'dsnd',
  privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true },
}])

function createWindow () {
  win = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 380,
    backgroundColor: '#14161a',
    title: 'DigidreamSound',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  })
  win.loadURL('dsnd://app/tool/sfx.html')
  win.on('closed', () => { win = null })
}

app.whenReady().then(() => {
  protocol.handle('dsnd', async (req) => {
    const url = new URL(req.url)
    // Resolve inside the payload and refuse anything that climbs out of it.
    const rel = decodeURIComponent(url.pathname).replace(/^\/+/, '')
    const file = path.normalize(path.join(PAYLOAD, rel))
    if (!file.startsWith(PAYLOAD)) return new Response('forbidden', { status: 403 })
    try {
      const body = await fs.readFile(file)
      return new Response(body, {
        headers: { 'content-type': MIME[path.extname(file)] ?? 'application/octet-stream' },
      })
    } catch {
      return new Response('not found', { status: 404 })
    }
  })

  buildMenu()
  createWindow()
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow() })
})

app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit() })
app.on('open-file', (e, p) => {
  e.preventDefault()
  if (win) win.webContents.send('openPath', p)
  else launchFile = p
})

// ---- the bridge -----------------------------------------------------------

ipcMain.handle('open', async (_e, opts = {}) => {
  const r = await dialog.showOpenDialog(win, {
    properties: opts.multiple ? ['openFile', 'multiSelections'] : ['openFile'],
    filters: [{ name: 'DigidreamSound', extensions: opts.extensions ?? ['sfxproj', 'sfxb', 'json'] }],
  })
  if (r.canceled) return []
  return Promise.all(r.filePaths.map(async (p) => ({
    name: path.basename(p), path: p, data: new Uint8Array(await fs.readFile(p)),
  })))
})

ipcMain.handle('save', async (_e, name, data, opts = {}) => {
  // `toPath` is an overwrite of a file the page already has a path for -- the
  // difference between Save and Save As, and the reason Save does not nag.
  let target = opts.toPath
  if (!target) {
    const r = await dialog.showSaveDialog(win, { defaultPath: name })
    if (r.canceled) return null
    target = r.filePath
  }
  await fs.writeFile(target, Buffer.from(data))
  return { path: target, name: path.basename(target) }
})

ipcMain.handle('saveMany', async (_e, files) => {
  const r = await dialog.showOpenDialog(win, { properties: ['openDirectory', 'createDirectory'] })
  if (r.canceled) return null
  const dir = r.filePaths[0]
  for (const f of files) await fs.writeFile(path.join(dir, f.name), Buffer.from(f.data))
  return { dir, count: files.length }
})

ipcMain.handle('confirm', async (_e, message, detail, ok) => {
  const r = await dialog.showMessageBox(win, {
    type: 'question', buttons: [ok ?? 'OK', 'Cancel'], defaultId: 0, cancelId: 1, message, detail,
  })
  return r.response === 0
})

ipcMain.handle('ready', async () => {
  if (!launchFile) return null
  const p = launchFile
  launchFile = null
  return { name: path.basename(p), path: p, data: new Uint8Array(await fs.readFile(p)) }
})

ipcMain.on('setDocument', (_e, doc) => {
  if (!win) return
  win.setTitle(doc?.name ? `${doc.name}${doc.dirty ? ' — edited' : ''}` : 'DigidreamSound')
  // The proxy icon and the macOS edited dot, which is the only place a desktop
  // app can say "unsaved" without inventing its own badge.
  if (doc?.path) win.setRepresentedFilename(doc.path)
  win.setDocumentEdited(!!doc?.dirty)
})

ipcMain.on('reveal', (_e, p) => { if (p) shell.showItemInFolder(p) })

// ---- the menu -------------------------------------------------------------
// Every item sends the same action string the in-page menu uses, so there is
// one action table (tool/sfx/app.js) rather than two that drift.

function buildMenu () {
  const send = (action) => () => win?.webContents.send('menu', action)
  const isMac = process.platform === 'darwin'
  const template = [
    ...(isMac ? [{ role: 'appMenu' }] : []),
    {
      label: 'File',
      submenu: [
        { label: 'New bank', accelerator: 'CmdOrCtrl+N', click: send('new') },
        { label: 'Open…', accelerator: 'CmdOrCtrl+O', click: send('open') },
        { type: 'separator' },
        { label: 'Save', accelerator: 'CmdOrCtrl+S', click: send('save') },
        { label: 'Save As…', accelerator: 'CmdOrCtrl+Shift+S', click: send('save-as') },
        { type: 'separator' },
        { label: 'Bake .sfxb…', accelerator: 'CmdOrCtrl+B', click: send('bake') },
        { label: 'Render selected to .wav…', click: send('wav') },
        { type: 'separator' },
        isMac ? { role: 'close' } : { role: 'quit' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        { label: 'Undo', accelerator: 'CmdOrCtrl+Z', click: send('undo') },
        { label: 'Redo', accelerator: 'CmdOrCtrl+Shift+Z', click: send('redo') },
        { type: 'separator' },
        { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
      ],
    },
    { label: 'View', submenu: [{ role: 'reload' }, { role: 'toggleDevTools' },
                               { type: 'separator' }, { role: 'togglefullscreen' }] },
    { role: 'windowMenu' },
  ]
  Menu.setApplicationMenu(Menu.buildFromTemplate(template))
}
