// The JS face and the C face must describe the same engine.
//
// `stopBus` and `setPitch` existed in c_api.h and in SfxCommand for months and
// simply were not bound to JS. From outside that is indistinguishable from a
// missing feature -- you reach for it, it is not there, and there is nothing
// to tell you it is a binding gap rather than a design decision. Two whole
// capabilities were invisible.
//
// So: extract every `dsnd_*` from the C header, map it to the name the embind
// class uses, and require that the class has it. A deliberate omission is
// listed in NOT_BOUND with a reason, which means the list of things JS cannot
// do is written down instead of being discovered.
//
//   node test/embind_parity.mjs
//
import { existsSync, readFileSync } from 'node:fs'

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

// dsnd_foo_bar -> fooBar
const camel = (s) => s.replace(/^dsnd_/, '').replace(/_([a-z])/g, (_, c) => c.toUpperCase())

// Deliberately absent from JS, each with the reason it is absent.
const NOT_BOUND = {
  dsnd_create: 'the embind constructor is `new mod.WebSfx()`',
  dsnd_destroy: 'embind objects are deleted with .delete()',
  dsnd_load_bank: 'bankBufferPrepare + loadBankFromBuffer: the heap handoff, not a pointer arg',
  dsnd_load_bank_json: 'named loadBankJson',
  dsnd_build_cache: 'named buildCache',
  dsnd_unload_bank: 'named unloadBank',
  dsnd_diag_get: 'diagJson returns the same counters as JSON, since JS has no struct',
  dsnd_now: 'the worklet tracks its own sample position; nothing in the tool needs it',
  dsnd_master_volume_milli: 'named masterVolumeMilli',
  dsnd_music_duck_milli: 'named musicDuckMilli',
  dsnd_sound_count: 'named soundCount',
  dsnd_sound_name: 'named soundName',
  dsnd_bus_count: 'bankInfoJson carries the bus list, which is what a tool wants',
  dsnd_sound_id: 'named soundId',
  dsnd_bus_id: 'named busId',
  dsnd_set_listener: 'named setListener',
  dsnd_play_2d: 'named play2d',
  dsnd_stop_bus: 'named stopBus',
  dsnd_stop_all: 'named stopAll',
  dsnd_set_position: 'named setPosition',
  dsnd_set_gain: 'named setGain',
  dsnd_set_pitch: 'named setPitch',
  dsnd_bus_gain: 'named busGain',
  dsnd_master_param: 'named masterParam',
  dsnd_set_dither: 'named setDither',
}

const header = readFileSync('src/bindings/c_api.h', 'utf8')
// Declarations only: `<type> dsnd_name(` at the start of a line-ish position.
const declared = [...header.matchAll(/\b(dsnd_[a-z0-9_]+)\s*\(/g)]
  .map((m) => m[1])
  .filter((n) => !['dsnd_ctx', 'dsnd_diag'].includes(n))
const unique = [...new Set(declared)]
check(unique.length > 20, `c_api.h declares ${unique.length} entry points`)

const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()
const bound = new Set(Object.getOwnPropertyNames(mod.WebSfx.prototype))

const missing = []
for (const fn of unique) {
  const js = camel(fn)
  if (bound.has(js)) continue
  // An explicitly-renamed one counts as bound if its documented JS name exists.
  const note = NOT_BOUND[fn]
  if (note) {
    const named = note.match(/named (\w+)/)
    if (named && !bound.has(named[1])) { missing.push(`${fn} (claims to be ${named[1]})`); continue }
    continue
  }
  missing.push(fn)
}

check(missing.length === 0,
      missing.length === 0
        ? 'every dsnd_* entry point is reachable from JS or listed as deliberately absent'
        : `unbound and unexplained: ${missing.join(', ')}`)

// The reverse direction: a JS method that the C API has no equivalent for is
// fine (the tool needs analysis the game does not), but it should be one we
// meant to add rather than one that drifted in.
const TOOL_ONLY = new Set([
  'constructor', 'bankBufferPrepare', 'loadBankFromBuffer', 'loadBankJson', 'buildCache',
  'unloadBank', 'soundId', 'busId', 'soundCount', 'soundName', 'bankInfoJson', 'setListener',
  'play', 'play2d', 'stop', 'stopBus', 'stopAll', 'setPosition', 'setGain', 'setPitch',
  'busGain', 'masterVolumeMilli', 'masterParam', 'setDither', 'alive', 'musicDuckMilli',
  'render', 'diagJson', 'queueScript', 'scriptSeconds',
  'renderVariant', 'renderVariantInfoJson', 'bakeToBuffer', 'bakedPtr', 'bakedSize',
  'heapUsedBytes',
])
const unexpected = [...bound].filter((m) => !TOOL_ONLY.has(m))
check(unexpected.length === 0,
      unexpected.length === 0
        ? `WebSfx exposes ${bound.size - 1} methods, all accounted for`
        : `WebSfx grew methods this test does not know about: ${unexpected.join(', ')}`)

// The six refusal reasons README.md advertises must all be in diagJson --
// rejectedNoVoices was missing, which made one of them unobservable.
{
  const sfx = new mod.WebSfx()
  const d = JSON.parse(sfx.diagJson())
  for (const k of ['rejectedCooldown', 'rejectedMaxInstances', 'rejectedPriority',
                   'rejectedBusBudget', 'rejectedNoVoices', 'rejectedUnknown']) {
    check(k in d, `diagJson reports ${k}`)
  }
  for (const k of ['queueHighWater', 'cacheBudgetBytes']) {
    check(k in d, `diagJson reports ${k}`)
  }
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
