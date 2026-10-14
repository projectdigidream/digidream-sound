// No shipped sound may clip.
//
// Every variant in every committed bank is rendered through renderVariant --
// the cache's own renderer, unclipped Q8.24 -- and its peak measured. Above
// 0 dBFS fails the build.
//
// This exists because the gate once passed a bank in which seven sounds peaked
// at exactly +42.1 dBFS: 20*log10(128), the point where Q8.24 overflows the
// i32 a render is stored in. They were modal bodies from an un-normalised
// resonator, the fitter's largely level-insensitive loss had not objected,
// and the only thing that noticed was a browser test printing a summary it
// did not assert on. A player would have heard them as square waves.
//
//   node test/content_levels.mjs [bank.sfxproj ...]
//
import { existsSync, readFileSync } from 'node:fs'

const banks = process.argv.length > 2
  ? process.argv.slice(2)
  : ['content/banks/starter.sfxproj', 'content/game/doom2.sfxproj'].filter((b) => existsSync(b) || (console.log(`skip: ${b} missing (a private bank)`), false))

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

for (const path of banks) {
  if (!existsSync(path)) { console.log(`skip: ${path} missing`); continue }
  const sfx = new mod.WebSfx()
  const rep = JSON.parse(sfx.loadBankJson(readFileSync(path, 'utf8')))
  check(rep.ok, `${path}: loads`)
  if (!rep.ok) continue
  const doc = JSON.parse(readFileSync(path, 'utf8'))

  const hot = []
  let loudest = -96
  for (let i = 0; i < sfx.soundCount(); i++) {
    const nv = doc.sounds[i]?.variants?.length ?? 1
    for (let v = 0; v < nv; v++) {
      sfx.renderVariant(i, v, false)
      const info = JSON.parse(sfx.renderVariantInfoJson())
      const db = info.peakDbMilli / 1000
      loudest = Math.max(loudest, db)
      if (db > 0) hot.push(`${sfx.soundName(i)} v${v} ${db > 0 ? '+' : ''}${db.toFixed(1)} dBFS`)
    }
  }
  check(hot.length === 0,
        hot.length === 0
          ? `${path}: nothing clips (loudest ${loudest.toFixed(1)} dBFS)`
          : `${path}: ${hot.length} clip -- ${hot.slice(0, 4).join(', ')}`)
}

console.log(`\n${checks} checks, ${failures} failures`)
process.exit(failures ? 1 : 0)
