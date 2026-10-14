// Render a .sfxscript through the WASM engine under Node and print its hash.
//
// This is the leg of the gate that actually catches per-target divergence.
// The unit tests and the native goldens both run one compiler on one
// architecture; this runs a different compiler targeting a different machine,
// and the bytes must still be identical. If a shift, a rounding rule or an
// integer width ever behaves differently under wasm, this is what says so.
//
// Deliberately plain Node with no browser and no bundler: the module is built
// with -sENVIRONMENT=shell and imports cleanly, so the gate needs no Chrome.
//
// `--module` selects which of the two builds to render through. They contain
// the same engine; dsound-tool.mjs additionally links the fitter. Running both
// and comparing is what proves that linking float code beside the engine moved
// nothing -- a standing per-build A/B rather than a one-off inspection of a
// diff. See scripts/build_wasm.sh for why there are two.
//
//   node test/determinism_wasm.mjs <bank.sfxproj> <script.sfxscript>
//        [--no-cache] [--module dsound|dsound-tool]
//
import { createHash } from 'node:crypto'
import { readFileSync } from 'node:fs'

const [, , bankPath, scriptPath, ...rest] = process.argv
if (!bankPath || !scriptPath) {
  console.error('usage: node test/determinism_wasm.mjs <bank.sfxproj> <script.sfxscript>'
              + ' [--no-cache] [--module dsound|dsound-tool]')
  process.exit(2)
}
const noCache = rest.includes('--no-cache')
const mi = rest.indexOf('--module')
const moduleName = mi >= 0 ? rest[mi + 1] : 'dsound'

let createSfx
try {
  ;({ default: createSfx } = await import(`../tool/dist/${moduleName}.mjs`))
} catch (e) {
  console.error(`skip: tool/dist/${moduleName}.mjs missing -- run scripts/build_wasm.sh`)
  process.exit(0)
}

const mod = await createSfx()
const sfx = new mod.WebSfx()

let bankText = readFileSync(bankPath, 'utf8')
if (noCache) {
  // Force every sound live, the same way dsound_render --no-cache does. The
  // hash must not move: a cached sound and its live render are the same bytes.
  const doc = JSON.parse(bankText)
  for (const s of doc.sounds) s.live = true
  bankText = JSON.stringify(doc)
}

const rep = JSON.parse(sfx.loadBankJson(bankText))
if (!rep.ok) {
  console.error('bank failed:', rep.errors.join('; '))
  process.exit(1)
}
for (const w of rep.warnings) console.error('warning:', w)

const cacheRep = JSON.parse(sfx.buildCache(0))
if (!cacheRep.ok) {
  console.error('cache failed:', cacheRep.errors.join('; '))
  process.exit(1)
}

const scriptText = readFileSync(scriptPath, 'utf8')
const seconds = sfx.scriptSeconds(scriptText)
const qrep = JSON.parse(sfx.queueScript(scriptText))
if (!qrep.ok) {
  console.error('script failed:', qrep.errors.join('; '))
  process.exit(1)
}

// Same chunking as the native renderer, so the command queue drains on the
// same schedule. A different chunk size would still be deterministic, but it
// would not be the SAME render, and the comparison would be meaningless.
const CONTROL_BLOCK = 64
const CHUNK = 512
const total = Math.floor((seconds * 48000) / CONTROL_BLOCK) * CONTROL_BLOCK

const hash = createHash('sha256')
for (let at = 0; at < total; at += CHUNK) {
  const n = Math.min(CHUNK, total - at)
  const ptr = sfx.render(n)
  // HEAP16 is a view over the wasm heap; the bytes here are the bytes the
  // native renderer writes into its WAV.
  const view = mod.HEAP16.subarray(ptr >> 1, (ptr >> 1) + n * 2)
  hash.update(Buffer.from(view.buffer, view.byteOffset, view.byteLength))
}

console.log('sha256', hash.digest('hex'))
console.error(sfx.diagJson())
