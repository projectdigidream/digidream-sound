// WebSfx::renderVariant must be the cache's own renderer, not something like it.
//
// The editor's Bank tab draws every sound's waveform from these bytes, measures
// its peak from them, and offers a batch normalise based on that peak. So if
// they diverge from what the engine actually caches, the tool is showing the
// author a sound the game does not play -- and it would show it confidently,
// because a waveform always looks like a waveform.
//
// The native side (`dsound_render <bank> --variant-hashes`) calls
// render_variant directly. The wasm side calls renderVariant and reads the
// same Q8.24 i32 out of HEAP32. Same function, two compilers, two targets:
// the hashes must match exactly, and so must the frame counts and the `leaks`
// flag, which is the per-variant warning nothing else can compute.
//
//   node test/wasm_variant.mjs [bank.sfxproj ...]
//
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { existsSync } from 'node:fs'

const banks = process.argv.length > 2
  ? process.argv.slice(2)
  : ['test/golden/identity.sfxproj', 'content/banks/starter.sfxproj']

let failures = 0
let checks = 0
const check = (cond, what) => {
  checks++
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

const RENDER = './build/dsound_render'
if (!existsSync(RENDER)) {
  console.log('skip: ./build/dsound_render missing -- run cmake && ninja')
  process.exit(0)
}
if (!existsSync('tool/dist/dsound.mjs')) {
  console.log('skip: tool/dist/dsound.mjs missing -- run scripts/build_wasm.sh')
  process.exit(0)
}

const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()

for (const bank of banks) {
  if (!existsSync(bank)) { console.log(`skip: ${bank} missing`); continue }

  // Native: index variant sha256 frames ok leaks name
  const native = execFileSync(RENDER, [bank, '--variant-hashes'], { encoding: 'utf8' })
    .trim().split('\n').filter(Boolean)
    .map((l) => {
      const [i, v, hash, frames, ok, leaks, ...name] = l.split(' ')
      return { i: +i, v: +v, hash, frames: +frames, ok: ok === '1',
               leaks: leaks === '1', name: name.join(' ') }
    })
  check(native.length > 0, `${bank}: native rendered ${native.length} variants`)

  const sfx = new mod.WebSfx()
  const rep = JSON.parse(sfx.loadBankJson(
    (await import('node:fs')).readFileSync(bank, 'utf8')))
  if (!rep.ok) {
    check(false, `${bank}: loads in wasm`)
    console.error(rep.errors.map((e) => e.text ?? e).join('; '))
    continue
  }

  let mismatches = 0
  for (const n of native) {
    const ptr = sfx.renderVariant(n.i, n.v, false)
    const info = JSON.parse(sfx.renderVariantInfoJson())

    // The bytes: frames * 2 channels * i32.
    const words = info.frames * 2
    const view = mod.HEAP32.subarray(ptr >> 2, (ptr >> 2) + words)
    const hash = createHash('sha256')
      .update(Buffer.from(view.buffer, view.byteOffset, view.byteLength))
      .digest('hex')

    if (hash !== n.hash || info.frames !== n.frames
        || info.ok !== n.ok || info.leaks !== n.leaks) {
      if (mismatches < 5) {
        console.error(`  ${n.name} v${n.v}:`)
        console.error(`    native ${n.hash.slice(0, 16)} frames ${n.frames}`
                    + ` ok ${n.ok} leaks ${n.leaks}`)
        console.error(`    wasm   ${hash.slice(0, 16)} frames ${info.frames}`
                    + ` ok ${info.ok} leaks ${info.leaks}`)
      }
      mismatches++
    }
  }
  check(mismatches === 0,
        `${bank}: all ${native.length} variants render identically native and wasm`)

  // The peak is computed in C++ from unclipped Q8.24, which is the whole
  // reason this binding exists: render() returns i16 and has already clamped,
  // so a peak read from it could never exceed 0 dBFS however hot the sound is.
  sfx.renderVariant(0, 0, false)
  const info = JSON.parse(sfx.renderVariantInfoJson())
  check(typeof info.peakDbMilli === 'number' && info.peakDbMilli > -96000,
        `${bank}: peak is reported (${(info.peakDbMilli / 1000).toFixed(1)} dBFS)`)
  check(info.bytes === info.frames * 2 * 4, `${bank}: byte count matches the frame count`)

  // Out-of-range asks must be refused, not read off the end of the heap.
  check(sfx.renderVariant(9999, 0, false) === 0, `${bank}: an unknown sound returns null`)
  check(sfx.renderVariant(0, 9999, false) === 0, `${bank}: an unknown variant returns null`)
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
