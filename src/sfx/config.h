#pragma once
//
// DigidreamSound — fixed constants.
//
// The sample rate, control block and every fixed-point format are NOT ours to
// choose: they are midisynth's, because we render through midisynth's `Voice`.
// This file re-exports them so `dsnd` code never has to reach into `msynth`
// for a number, and adds the handful that are genuinely this project's.
//
#include "synth/config.h"
#include "synth/fixmath.h"

namespace dsnd {

// ---- borrowed, non-negotiable ---------------------------------------------
using msynth::CONTROL_BLOCK;  // 64 samples, the granularity of every decision
using msynth::CONTROL_RATE;   // 750 Hz
using msynth::MAX_LAYERS;     // 4 layers per patch
using msynth::SAMPLE_RATE;    // 48000, compile-time

using msynth::gain_t;
using msynth::i16;
using msynth::i32;
using msynth::i64;
using msynth::i8;
using msynth::sample_t;
using msynth::u16;
using msynth::u32;
using msynth::u64;
using msynth::u8;

using msynth::coef_t;
using msynth::COEF_ONE;
using msynth::COEF_SHIFT;
using msynth::GAIN_ONE;
using msynth::GAIN_SHIFT;
using msynth::SAMPLE_SHIFT;

// ---- ours ------------------------------------------------------------------

// Synth voices available to `live` sounds. A midisynth Voice is ~17 KB (the
// 16 KB comb delay line dominates, and almost no SFX patch uses it), so this
// is ~820 KB resident. It is a ceiling only `live` sounds can reach: a cached
// instance plays through SampleVoice (~96 bytes) and never touches this pool,
// which is why the number can stay small.
inline constexpr int MAX_SFX_VOICES = 48;

// Concurrent instances across all buses. Each owns a stereo i64 lane of one
// control block: 48 * 64 * 2 * 8 = 48 KB.
inline constexpr int MAX_SFX_INSTANCES = 48;

inline constexpr int MAX_BUSES = 8;

// An instance younger than this is never a steal candidate. Matches
// midisynth's FAST_RELEASE_BLOCKS / STEAL_SHIELD_BLOCKS (voice.cpp) so a
// stolen instance's fade and its shield agree.
inline constexpr u32 STEAL_SHIELD_BLOCKS = 3;

// Default fade applied when an instance is stolen rather than asked to stop.
inline constexpr u32 STEAL_FADE_BLOCKS = 3;

// Blocks of exact silence before an instance is reclaimed. See
// Instance::silent_blocks for why the rule is silence rather than "the
// sources say they are done".
inline constexpr u8 END_SILENT_BLOCKS = 2;

// PCM cache default budget. Q8.24 mono at 48 kHz is 192 KB per second, so
// this is roughly 43 seconds of cached audio — a 60-sound bank of half-second
// one-shots fits with room to spare.
inline constexpr u32 DEFAULT_CACHE_BUDGET_KB = 8192;

// A variant whose render has not fallen silent by here is assumed not to end,
// and falls back to `live` with a warning. Also the hard cap on cache entry
// length.
inline constexpr u32 MAX_CACHE_MS = 4000;

// Playback-rate limits for the sampler, Q32.32. Beyond +1 octave a 4-point
// Hermite read of PCM aliases audibly; below -2 octaves it is pure
// interpolation and merely dull, but the bound keeps the entry length finite.
inline constexpr u64 RATE_ONE = u64(1) << 32;
inline constexpr u64 RATE_MAX = RATE_ONE * 2;
inline constexpr u64 RATE_MIN = RATE_ONE / 4;

// Above this, jitter would push a cached read past RATE_MAX often enough that
// the sound is better synthesized per instance. 600 cents is a tritone.
inline constexpr i32 MAX_CACHED_JITTER_CENTS = 600;

// Concurrent music-duck requests honoured. The engine takes the minimum of
// the active ones; past this, the oldest is dropped. Ducking is a scarce
// dramatic effect and a bank where everything ducks is a bank where nothing
// stands out.
inline constexpr int MAX_DUCK_REQUESTS = 4;

}  // namespace dsnd
