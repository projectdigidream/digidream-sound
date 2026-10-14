// The editor's own checks must agree with the engine's.
//
// validate.js re-implements one thing the loader does: the §3.1 error, where a
// layer sustains above zero on a non-looping sound that is never released, so
// its voice can never finish. The editor needs its own copy because the engine
// can only speak about a document it LOADED, and this particular condition
// makes a document refuse to load -- at which point the message arrives with no
// bank behind it, the binding cannot resolve a sound index, and the UI has
// nothing to point at.
//
// A second copy of a rule is a second thing to be wrong, so it gets checked
// against the first. If these ever disagree the editor is either blocking
// documents the engine would accept, or -- worse, because it is silent --
// letting through ones it would refuse.
//
//   node test/validate_agrees.mjs
//
import { existsSync } from 'node:fs'
import { neverEnds } from '../tool/sfx/validate.js'

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
const { default: createSfx } = await import('../tool/dist/dsound.mjs')
const mod = await createSfx()
const sfx = new mod.WebSfx()

// A sustaining layer with no release: the shape the rule is about.
const sustaining = (over = {}) => ({
  name: 'probe',
  variants: [{
    patch: {
      name: 'probe',
      layers: [{
        osc: 'wavetable',
        wavetable: { generator: 'additiveHarmonicMorph', frameCount: 2, params: { falloff: 'saw' } },
        ampEnv: { attack: 1, decay: 20, sustain: 0.8, release: 40 },
      }],
      ...(over.patch ?? {}),
    },
    ...(over.variant ?? {}),
  }],
  ...(over.sound ?? {}),
})

const loads = (sound) => {
  const doc = {
    version: 1,
    name: 'probe',
    buses: [{ name: 'b', spatial: false }],
    sounds: [sound],
  }
  return JSON.parse(sfx.loadBankJson(JSON.stringify(doc))).ok
}

// name -> [sound, expected-to-load]
const cases = [
  ['a sustaining layer with no escape', sustaining(), false],
  ['...fixed by oneShot', sustaining({ patch: { oneShot: true } }), true],
  ['...fixed by sustain 0', (() => {
    const s = sustaining()
    s.variants[0].patch.layers[0].ampEnv.sustain = 0
    return s
  })(), true],
  ['...fixed by a gesture hold', sustaining({ variant: { gesture: [{ atMs: 0, durMs: 200 }] } }), true],
  ['...fixed by loop', sustaining({ sound: { loop: true } }), true],
  // A gesture step with durMs 0 is explicitly "no note-off", so it is NOT a fix.
  ['a gesture with durMs 0 is not a release', sustaining({ variant: { gesture: [{ atMs: 0, durMs: 0 }] } }), false],
]

for (const [label, sound, expectLoad] of cases) {
  const engineOk = loads(sound)
  const editorComplains = neverEnds(sound, 0) !== null

  check(engineOk === expectLoad,
        `engine: ${label} -> ${expectLoad ? 'loads' : 'refused'}`)
  check(editorComplains === !engineOk,
        `editor agrees: ${label} -> ${editorComplains ? 'flagged' : 'clean'}`)
}

// And the fixes it offers are the four the format documents.
{
  const fix = neverEnds(sustaining(), 0)
  check(fix !== null && fix.fixes.length === 4, 'four fixes are offered')
  const paths = fix.fixes.map((f) => f.path).join(' ')
  check(paths.includes('patch.oneShot') && paths.includes('ampEnv.sustain')
     && paths.includes('gesture.durMs') && paths.includes('loop'),
        'and they are the four docs/SFX_FORMAT.md 3.1 names')
}

// The shipped banks must be clean by the editor's reckoning too -- if the
// editor flagged a sound in a bank the engine loads happily, it would be
// showing a permanent false alarm on content nobody can fix.
for (const path of ['content/banks/starter.sfxproj', 'content/game/doom2.sfxproj']) {
  if (!existsSync(path)) continue
  const { readFileSync } = await import('node:fs')
  const doc = JSON.parse(readFileSync(path, 'utf8'))
  const flagged = doc.sounds.filter((s) => neverEnds(s, 0) !== null).map((s) => s.name)
  check(flagged.length === 0,
        flagged.length === 0
          ? `${path}: the editor flags nothing (${doc.sounds.length} sounds)`
          : `${path}: the editor falsely flags ${flagged.slice(0, 3).join(', ')}`)
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
