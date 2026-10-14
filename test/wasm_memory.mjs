// Pin the heap high-water marks, the way goldens.txt pins hashes.
//
// Both modules are built with -sALLOW_MEMORY_GROWTH=0, which is deliberate:
// growth changes the emitted wasm and, worse, detaches the cached HEAP16 view
// the worklet's render loop holds. The cost of that choice is that
// INITIAL_MEMORY is a decision rather than a default, and the failure mode for
// getting it wrong is bad in a specific way -- emmalloc calls abort(), the
// worklet processor throws, Chrome disables the node, and the user gets
// SILENCE plus one console line. Not a crash anyone can act on.
//
// So the sizes are measured, and a regression fails a build instead of
// reaching a user. If a number here moves, that is the interesting part.
//
// NOTE what these numbers are: sbrk(0), the arena's HIGH-WATER MARK under
// emmalloc. It never falls, so `after unloadBank` cannot be read as "the
// memory came back" and this test cannot detect a leak. What it detects is a
// peak growing, which is the thing that aborts a worklet. Budgets are set just
// above the measured peak so a regression is visible rather than absorbed --
// re-run with --update after a deliberate change.
//
//   node test/wasm_memory.mjs [--update]
//
import { existsSync, readFileSync } from 'node:fs'

const update = process.argv.includes('--update')
let failures = 0
let checks = 0
const check = (cond, what) => {
  checks++
  if (cond) console.log('ok', what)
  else { console.error('FAIL', what); failures++ }
}

if (!existsSync('tool/dist/dsound.mjs')) {
  console.log('skip: tool/dist/dsound.mjs missing -- run scripts/build_wasm.sh')
  process.exit(0)
}

// Headroom over the measured peak, not a guess at the peak. Each budget is
// what the engine actually reached when this was last checked, rounded up.
const BUDGET_MB = {
  'createSfx': 7,
  'starter loaded': 10,
  'starter cached': 13,
  'doom2 loaded': 24,
  'doom2 cached': 43,
  'doom2 all variants rendered': 43,
  'after unloadBank': 43,
}

const ENGINE_MB = 64   // must match ENGINE_MEMORY in scripts/build_wasm.sh
const mb = (b) => b / (1024 * 1024)

const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()
const marks = {}
const mark = (name, sfx) => {
  marks[name] = mb(sfx ? sfx.heapUsedBytes() : 0)
  return marks[name]
}

{
  const probe = new mod.WebSfx()
  mark('createSfx', probe)
}

for (const [label, path] of [['starter', 'content/banks/starter.sfxproj'],
                             ['doom2', 'content/game/doom2.sfxproj']]) {
  if (!existsSync(path)) { console.log(`skip: ${path} missing`); continue }
  const sfx = new mod.WebSfx()
  const rep = JSON.parse(sfx.loadBankJson(readFileSync(path, 'utf8')))
  if (!rep.ok) { check(false, `${label}: loads`); continue }
  mark(`${label} loaded`, sfx)
  JSON.parse(sfx.buildCache(0))
  mark(`${label} cached`, sfx)

  if (label === 'doom2') {
    // The Bank tab's opening move: render every variant for its waveform and
    // its peak. This is the editor's real peak, not the engine's.
    const n = sfx.soundCount()
    for (let i = 0; i < n; i++) sfx.renderVariant(i, 0, false)
    mark('doom2 all variants rendered', sfx)
    sfx.unloadBank()
    mark('after unloadBank', sfx)
  }
}

console.log()
for (const [name, used] of Object.entries(marks)) {
  const budget = BUDGET_MB[name]
  const line = `${name.padEnd(30)} ${used.toFixed(1).padStart(6)} MB`
  if (budget === undefined) { console.log(`${line}   (no budget)`); continue }
  check(used <= budget, `${line}  <= ${budget} MB`)
}

// The whole point: the engine module's fixed heap must hold all of it, with
// room left. A peak that reaches the ceiling is an abort, not a slowdown.
const peak = Math.max(...Object.values(marks))
check(peak < ENGINE_MB * 0.75,
      `peak ${peak.toFixed(1)} MB leaves headroom in the ${ENGINE_MB} MB engine heap`)

if (update) {
  console.log('\nmeasured (paste into BUDGET_MB, rounded up):')
  for (const [k, v] of Object.entries(marks)) console.log(`  '${k}': ${Math.ceil(v) + 4},`)
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
