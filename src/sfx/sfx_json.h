#pragma once
//
// .sfxproj (JSON) -> SoundBank. Control thread only.
//
// Same error policy as midisynth's patch loader, for the same reason: a bank
// that half-loads and plays is worth more than one that refuses, because the
// person who has to fix it is usually mid-session with headphones on. Bad
// values are clamped with a warning; a broken sound is dropped with an error
// but the rest of the bank still loads.
//
// One class of problem is a hard error rather than a warning, because it
// cannot be heard until it has already gone wrong: a variant whose envelope
// sustains, whose gesture note has no duration, and which is not a loop,
// produces a voice that never ends and an instance that leaks forever. The
// cache build detects it for free (it renders until silence) and the baker
// refuses. See check_endings() in sfx_json.cpp.
//
#include <string>

#include "sound_def.h"
#include "synth/patch_loader.h"
#include "synth/wavetable_cache.h"

namespace dsnd {

using msynth::LoadResult;
using msynth::WaveTableCache;

// Parse a whole .sfxproj document.
LoadResult load_bank_json(const char* json, size_t len, WaveTableCache& cache, SoundBank& out);

// Parse one sound object, for the tool's live edit path. `patches` and
// `variants` are appended to; `out` is filled but not added to the bank.
LoadResult load_sound(const msynth::JsonValue& doc, WaveTableCache& cache, SoundBank& bank,
                      SoundDef& out);

// dB (Q16.16) -> linear gain (Q16.16), through midisynth's baked table so no
// libm touches a bank. Clamped to [-96, +24] dB.
i32 db_q16_to_gain(i32 db_q16);

}  // namespace dsnd
