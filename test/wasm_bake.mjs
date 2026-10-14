// The browser must produce the same .sfxb the CLI does -- byte for byte.
//
// A standalone product whose users cannot make the shipping artifact without a
// native toolchain has a hole where its output should be. So WebSfx has
// bakeToBuffer, and this is what stops it being a second, slightly different
// baker that nobody compares.
//
// Byte equality is a fair demand here rather than an ambitious one:
// bake_bank writes no timestamp, iterates no hash map, and emits patch JSON as
// an ordered vector. There is nothing in it that is allowed to vary. If this
// ever fails, do NOT relax it to "they render the same" -- a byte divergence
// in the baker is a real finding about the baker.
//
//   node test/wasm_bake.mjs [bank.sfxproj ...]
//
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { existsSync, mkdtempSync, readFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const banks = process.argv.length > 2
  ? process.argv.slice(2)
  : ['test/golden/identity.sfxproj', 'content/banks/starter.sfxproj',
     'content/game/doom2.sfxproj'].filter((b) => existsSync(b) || (console.log(`skip: ${b} missing (a private bank)`), false))

let failures = 0
let checks = 0
const check = (cond, what) => {
  checks++
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

const BAKE = './build/dsound_bake'
if (!existsSync(BAKE)) { console.log('skip: ./build/dsound_bake missing'); process.exit(0) }
if (!existsSync('tool/dist/dsound.mjs')) {
  console.log('skip: tool/dist/dsound.mjs missing -- run scripts/build_wasm.sh')
  process.exit(0)
}

const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()
const tmp = mkdtempSync(join(tmpdir(), 'dsnd-bake-'))
const sha = (b) => createHash('sha256').update(b).digest('hex')

for (const bank of banks) {
  if (!existsSync(bank)) { console.log(`skip: ${bank} missing`); continue }

  const native = join(tmp, 'native.sfxb')
  execFileSync(BAKE, [bank, native, '--quiet'])
  const wantBytes = readFileSync(native)

  const sfx = new mod.WebSfx()
  const rep = JSON.parse(sfx.bakeToBuffer(readFileSync(bank, 'utf8')))
  if (!rep.ok) {
    check(false, `${bank}: bakes in wasm`)
    console.error(rep.errors.map((e) => e.text ?? e).join('; '))
    continue
  }
  const ptr = sfx.bakedPtr()
  const size = sfx.bakedSize()
  const got = Buffer.from(mod.HEAPU8.buffer, ptr, size)

  check(size === wantBytes.length,
        `${bank}: same size (${size} B)`)
  check(sha(got) === sha(wantBytes),
        `${bank}: browser bake is byte-identical to dsound_bake (${sha(wantBytes).slice(0, 16)})`)

  // And the bytes are loadable -- a baker that produces a stable but wrong
  // file would pass everything above.
  const sfx2 = new mod.WebSfx()
  const p = sfx2.bankBufferPrepare(size)
  mod.HEAPU8.set(got, p)
  const lr = JSON.parse(sfx2.loadBankFromBuffer())
  check(lr.ok, `${bank}: and the result loads`)
  if (lr.ok) {
    const src = JSON.parse(readFileSync(bank, 'utf8'))
    check(sfx2.soundCount() === src.sounds.length,
          `${bank}: with all ${src.sounds.length} sounds`)
  }
}

// A document that cannot bake must report, not half-write.
{
  const sfx = new mod.WebSfx()
  const rep = JSON.parse(sfx.bakeToBuffer('{"version":1,"name":"x","sounds":[]}'))
  check(!rep.ok, 'a bank with no sounds is refused')
  check(sfx.bakedSize() === 0, 'and leaves no bytes behind')
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
