// The WASM module's binary path: load a baked .sfxb through the heap, the way
// a game does, and check it agrees with the JSON path.
//
// determinism_wasm.mjs covers loadBankJson because that is what the goldens
// render from. A shipped game loads a .sfxb instead, through
// bankBufferPrepare() and the heap, and that transfer is the one place a
// browser build can silently differ from a native one.
//
//   node test/wasm_bank.mjs [bank.sfxproj] [bank.sfxb]
//
// Skips with exit 0 if the module or the bank is missing, like every other
// optional check here.
import { createHash } from 'node:crypto'
import { existsSync, readFileSync } from 'node:fs'

const proj = process.argv[2] ?? 'test/golden/identity.sfxproj'
const sfxb = process.argv[3] ?? 'build/identity.sfxb'
const script = 'test/golden/identity.sfxscript'

let failures = 0
const check = (cond, what) => {
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

if (!existsSync('tool/dist/dsound.mjs')) {
  console.error('skip: tool/dist/dsound.mjs missing -- run scripts/build_wasm.sh')
  process.exit(0)
}
if (!existsSync(sfxb)) {
  console.error(`skip: ${sfxb} missing -- run dsound_bake ${proj} ${sfxb}`)
  process.exit(0)
}

const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()

const scriptText = readFileSync(script, 'utf8')

function render (load) {
  const sfx = new mod.WebSfx()
  const rep = JSON.parse(load(sfx))
  check(rep.ok, 'bank loads')
  if (!rep.ok) { console.error(rep.errors.join('; ')); return null }
  check(JSON.parse(sfx.buildCache(0)).ok, 'cache builds')

  const seconds = sfx.scriptSeconds(scriptText)
  check(JSON.parse(sfx.queueScript(scriptText)).ok, 'script queues')

  const total = Math.floor((seconds * 48000) / 64) * 64
  const hash = createHash('sha256')
  for (let at = 0; at < total; at += 512) {
    const n = Math.min(512, total - at)
    const ptr = sfx.render(n)
    const view = mod.HEAP16.subarray(ptr >> 1, (ptr >> 1) + n * 2)
    hash.update(Buffer.from(view.buffer, view.byteOffset, view.byteLength))
  }
  return { hash: hash.digest('hex'), info: JSON.parse(sfx.bankInfoJson()) }
}

const fromJson = render((sfx) => sfx.loadBankJson(readFileSync(proj, 'utf8')))

const bytes = readFileSync(sfxb)
const fromBin = render((sfx) => {
  // The heap handoff: ask for a buffer, write into it, then load from it.
  // Copying through a JS array per call would be the most expensive thing in
  // the whole pipeline, which is why the pointer is exposed at all.
  const ptr = sfx.bankBufferPrepare(bytes.length)
  mod.HEAPU8.set(bytes, ptr)
  return sfx.loadBankFromBuffer()
})

if (fromJson && fromBin) {
  check(fromJson.hash === fromBin.hash,
        `baked and JSON render identically (${fromBin.hash.slice(0, 16)})`)
  check(fromJson.info.sounds.length === fromBin.info.sounds.length,
        `same sound count (${fromBin.info.sounds.length})`)
  for (let i = 0; i < fromBin.info.sounds.length; i++) {
    if (fromJson.info.sounds[i].name !== fromBin.info.sounds[i].name) {
      check(false, `sound ${i} name matches`)
      break
    }
  }
}

// A corrupt bank must be refused, not half-loaded.
{
  const bad = Buffer.from(bytes)
  bad[bad.length - 6] ^= 0x04
  const sfx = new mod.WebSfx()
  const ptr = sfx.bankBufferPrepare(bad.length)
  mod.HEAPU8.set(bad, ptr)
  const rep = JSON.parse(sfx.loadBankFromBuffer())
  check(!rep.ok, 'a corrupt bank is refused')
  check(sfx.soundCount() === 0, 'and leaves nothing loaded')
}

process.exit(failures ? 1 : 0)
