// The module must load where it is going to run: inside an AudioWorklet.
//
// This exists because I got the reasoning wrong once and want the conclusion
// checked rather than remembered. `scripts/build_wasm.sh` passes
// -sENVIRONMENT=shell, and the obvious-looking fix is to add `web,worker` so
// browsers work. That is backwards:
//
//   - ENVIRONMENT selects JS glue, not codegen, and with -sSINGLE_FILE=1 the
//     wasm is decoded from an inline string, so readAsync/readBinary/fetch are
//     never reached and the web/worker preamble would be dead code.
//   - AudioWorkletGlobalScope is neither a Window nor a WorkerGlobalScope, so
//     ENVIRONMENT_IS_WEB and ENVIRONMENT_IS_WORKER are both FALSE there. The
//     added branches would not even run in the host they were added for.
//
// So rather than change a working flag, prove the property directly: strip the
// globals an AudioWorkletGlobalScope does not have, make the ones it must
// never touch throw, and require the module to load and render anyway.
//
// This is the offline 80% of the browser claim -- no Chrome, no network, runs
// in ci.sh. test/browser_determinism.mjs (opt-in, DSND_BROWSER=1) is the
// other 20%.
//
//   node test/worklet_env.mjs
//
import { existsSync, readFileSync } from 'node:fs'

let failures = 0
let checks = 0
const check = (cond, what) => {
  checks++
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

const modules = ['dsound', 'dsound-tool'].filter((m) => existsSync(`tool/dist/${m}.mjs`))
if (modules.length === 0) {
  console.log('skip: no tool/dist modules -- run scripts/build_wasm.sh')
  process.exit(0)
}

// --- stand in for AudioWorkletGlobalScope ---------------------------------
// Present: what a worklet has. Absent or throwing: what it does not, or what
// reaching for it would mean (a network fetch inside an audio callback).
const removed = []
for (const k of ['window', 'document', 'location', 'XMLHttpRequest',
                 'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval']) {
  if (k in globalThis) { removed.push([k, globalThis[k]]); delete globalThis[k] }
}
globalThis.AudioWorkletGlobalScope = function AudioWorkletGlobalScope () {}
globalThis.sampleRate = 48000
globalThis.currentTime = 0
globalThis.registerProcessor = () => {}

const tripped = []
globalThis.fetch = () => { tripped.push('fetch'); throw new Error('fetch in a worklet') }

for (const name of modules) {
  let mod
  try {
    const { default: createSfx } = await import(`../tool/dist/${name}.mjs`)
    mod = await createSfx()
  } catch (e) {
    check(false, `${name}: loads with worklet globals (${e.message})`)
    continue
  }
  check(true, `${name}: loads with worklet globals`)
  check(tripped.length === 0, `${name}: without reaching for fetch`)

  // Loading is not enough -- it has to work.
  const sfx = new mod.WebSfx()
  const bank = readFileSync('test/golden/identity.sfxproj', 'utf8')
  const rep = JSON.parse(sfx.loadBankJson(bank))
  check(rep.ok, `${name}: and loads a bank`)

  const ptr = sfx.render(128)
  check(ptr !== 0, `${name}: and renders a block`)
  const view = mod.HEAP16.subarray(ptr >> 1, (ptr >> 1) + 256)
  check(view.length === 256, `${name}: and the output is readable through HEAP16`)

  // The one shell-path statement that actually matters in a worklet: the glue
  // defines clearTimeout when it is missing. If that ever stopped happening,
  // anything in the runtime that calls it would throw mid-callback.
  check(typeof globalThis.clearTimeout === 'function',
        `${name}: the glue supplies clearTimeout, which a worklet lacks`)
}

for (const [k, v] of removed) globalThis[k] = v

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
