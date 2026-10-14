// The document model must not change a document it did not mean to change.
//
// `content/banks/starter.sfxproj` and `content/game/doom2.sfxproj` are pinned
// goldens: run_goldens.sh renders `starter` to a fixed hash, and ci.sh bakes
// both. So a lossy round trip through the editor's model is not a cosmetic
// bug, it is a silent content change that the gate would then re-pin.
//
// Four properties, in increasing strength:
//
//   1. parse -> write -> parse is deep-equal        (the model loses nothing)
//   2. write is idempotent                          (no drift on repeated saves)
//   3. unknown keys survive                         (_generated_by, _fitted, ...)
//   4. the rewritten file BAKES BYTE-IDENTICALLY    (the engine cannot tell)
//
// (4) is the one that matters, and it needs ./build/dsound_bake; it skips
// cleanly when the native build is absent, like every other optional check.
//
//   node test/proj_roundtrip.mjs
//
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import {
  ATTEN_FIELDS, SOUND_FIELDS, clampField, getField, hasField, liveReason,
  readProj, setField, writeProj,
} from '../tool/sfx/proj.js';

let failures = 0;
let checks = 0;
const check = (cond, what) => {
  checks++;
  if (cond) { console.log('ok', what); } else { console.error('FAIL', what); failures++; }
};

const BANKS = ['content/banks/starter.sfxproj', 'content/game/doom2.sfxproj'];
const sha = (b) => createHash('sha256').update(b).digest('hex');

// --- 1-3: the model round-trips -------------------------------------------

for (const path of BANKS) {
  if (!existsSync(path)) { console.log(`skip: ${path} missing`); continue; }
  const original = readFileSync(path, 'utf8');
  const { doc, repairs } = readProj(original);

  check(repairs.length === 0, `${path}: loads with no repairs needed`);

  const once = writeProj(doc);
  const twice = writeProj(readProj(once).doc);
  check(once === twice, `${path}: writing is idempotent`);

  const reparsed = readProj(once).doc;
  check(JSON.stringify(reparsed) === JSON.stringify(doc),
        `${path}: parse -> write -> parse is deep-equal`);

  // Unknown keys the loader ignores by design.
  const src = JSON.parse(original);
  const underscored = Object.keys(src).filter((k) => k.startsWith('_'));
  for (const k of underscored) {
    check(JSON.stringify(reparsed[k]) === JSON.stringify(src[k]),
          `${path}: preserved ${k}`);
  }

  // Key order is the authored order, which keeps diffs legible.
  check(Object.keys(reparsed).join() === Object.keys(src).join(),
        `${path}: top-level key order is unchanged`);

  console.log(`   (${path}: ${doc.sounds.length} sounds, ${original.length} B in, `
            + `${once.length} B out, ${once.split('\n').length} lines)`);
}

// --- getField / setField ---------------------------------------------------

{
  const s = { name: 'x', variants: [] };
  const prio = SOUND_FIELDS.find((f) => f.path === 'priority');
  const minD = ATTEN_FIELDS.find((f) => f.path === 'attenuation.minDist');

  check(getField(s, prio) === 128, 'an absent field reads as its default');
  check(hasField(s, prio) === false, 'and hasField knows it was absent');

  setField(s, prio, 200);
  check(s.priority === 200 && hasField(s, prio), 'setField writes');

  // The rule that keeps documents from inflating: writing the default removes
  // the key rather than recording it.
  setField(s, prio, 128);
  check(!('priority' in s), 'writing the default removes the key');

  setField(s, minD, 5);
  check(s.attenuation.minDist === 5, 'setField creates intermediate objects');
  setField(s, minD, minD.def);
  check(!('attenuation' in s), 'and removes a container it emptied');

  check(clampField(prio, 999) === 255, 'clampField clamps high');
  check(clampField(prio, -5) === 0, 'clampField clamps low');
  check(clampField(prio, 'nonsense') === 128, 'clampField rejects non-numbers');
}

// --- liveReason mirrors the loader ----------------------------------------

{
  check(liveReason({}) === null, 'a plain sound caches');
  check(liveReason({ live: true }) !== null, 'authored live is live');
  check(liveReason({ loop: true }) !== null, 'loop implies live');
  check(liveReason({ pitchJitterCents: 601 }) !== null, 'jitter above 600 implies live');
  check(liveReason({ pitchJitterCents: 600 }) === null, 'jitter at exactly 600 still caches');
}

// --- 4: the engine cannot tell the difference ------------------------------

const BAKE = './build/dsound_bake';
if (!existsSync(BAKE)) {
  console.log('skip: ./build/dsound_bake missing -- run cmake && ninja for the bake leg');
} else {
  const tmp = mkdtempSync(join(tmpdir(), 'dsnd-proj-'));
  for (const path of BANKS) {
    if (!existsSync(path)) continue;
    const { doc } = readProj(readFileSync(path, 'utf8'));
    const rewritten = join(tmp, 'rewritten.sfxproj');
    writeFileSync(rewritten, writeProj(doc));

    const a = join(tmp, 'a.sfxb');
    const b = join(tmp, 'b.sfxb');
    execFileSync(BAKE, [path, a, '--quiet']);
    execFileSync(BAKE, [rewritten, b, '--quiet']);
    check(sha(readFileSync(a)) === sha(readFileSync(b)),
          `${path}: the rewritten document bakes byte-identically`);
  }
}

console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures ? 1 : 0);
