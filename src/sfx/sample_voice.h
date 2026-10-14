#pragma once
//
// Playing back a cached sound.
//
// A cached instance costs about ninety bytes and one multiply-add per sample,
// against ~17 KB and a full synthesis chain for a live one. That difference is
// the entire argument for the hybrid model, and it is only safe because of one
// property:
//
//   AT RATE EXACTLY 1.0, A CACHED READ RETURNS THE STORED SAMPLES EXACTLY.
//
// Which means a cached sound and a live one are not merely similar, they are
// bit-identical -- so the engine may cache some sounds and not others, may
// finish building its cache while already playing, and may fall back to live
// synthesis when a budget runs out, all without anyone hearing a seam.
//
// The property rests on four things, and every one of them is a constraint on
// code elsewhere, so they are written down here where the reader is standing:
//
//   1. hermite4 at frac16 == 0 returns exactly y1. Every correction term is
//      multiplied by zero, and asr(0, 16) is 0 for either sign. Verified in
//      midisynth/src/synth/oscillator.h:17.
//   2. rate_from_cents(0) is exactly 1<<32, so the fraction stays 0 forever.
//      exp2_q16(0) is exactly GAIN_ONE; the shift is exact.
//   3. sgain(s, GAIN_ONE) == s for every s, because the product is an exact
//      multiple of 2^16 and the shift is therefore lossless.
//   4. The cache stores Q8.24, the engine's own sample format. An i16 cache
//      would be deterministic but DIFFERENT, and the property would be gone.
//
// A fifth belongs to the spatialiser rather than here, and it is the one most
// likely to be broken by someone tidying up: a voice already ramps its own
// output gain from zero across its first 64 samples (voice.cpp:334), and that
// ramp is baked into the cache. If the spatialiser also ramps from zero on an
// instance's first block, the cached path gets two ramps and the live path
// one. It must start at its target.
//
#include <cstddef>

#include "config.h"
#include "synth/tables.h"

namespace dsnd {

// 4-point Catmull-Rom over a linear buffer, Q8.24 in and out.
//
// The arithmetic is midisynth's hermite4 (oscillator.h) with one change: that
// one wraps its index around a power-of-two wavetable, and PCM does not wrap.
// `p` points AT the sample being interpolated from, and the caller guarantees
// p[-1] and p[2] are readable -- the cache pads every entry, so the inner
// loop carries no bounds branch. test_sampler.cpp pins the two against each
// other for every index and fraction, which is what keeps the copy honest.
// `stride` is 1 for a mono entry and 2 for an interleaved stereo one: the
// four points of a stereo channel are four FRAMES apart, not four samples.
// Interpolating at stride 1 over interleaved audio reads the other channel as
// if it were the next sample, which sounds like ring modulation at half the
// sample rate and is very easy to write by accident.
inline i32 hermite4_pcm(const i32* p, u32 frac16, int stride = 1) noexcept {
    const i64 y0 = p[-stride];
    const i64 y1 = p[0];
    const i64 y2 = p[stride];
    const i64 y3 = p[2 * stride];
    const i64 c1 = msynth::asr(y2 - y0, 1);
    const i64 c2 = msynth::asr(2 * y0 - 5 * y1 + 4 * y2 - y3, 1);
    const i64 c3 = msynth::asr(3 * (y1 - y2) + y3 - y0, 1);
    i64 acc = c3;
    acc = c2 + msynth::asr(acc * frac16, 16);
    acc = c1 + msynth::asr(acc * frac16, 16);
    return static_cast<i32>(y1 + msynth::asr(acc * frac16, 16));
}

// Cents -> playback rate, Q32.32. Zero cents is exactly 1<<32.
inline u64 rate_from_cents(i32 cents_q16) noexcept {
    const gain_t r = msynth::exp2_q16(cents_q16 / 1200);
    u64 rate = static_cast<u64>(static_cast<u32>(r)) << 16;
    if (rate < RATE_MIN) rate = RATE_MIN;
    if (rate > RATE_MAX) rate = RATE_MAX;
    return rate;
}

// One cached entry being played.
//
// `pcm` points at the entry's first real sample; the cache guarantees one
// guard sample before it and two after, so `idx - 1` and `idx + 2` are always
// in bounds.
struct SampleVoice {
    const i32* pcm = nullptr;
    u32 frames = 0;      // real samples, excluding the guards
    bool stereo = false;
    u64 pos_q32 = 0;
    u64 rate_q32 = RATE_ONE;
    bool looping = false;

    bool finished() const noexcept {
        return !pcm || (!looping && (pos_q32 >> 32) >= frames);
    }

    // Render one control block, ADDING into the lane. Returns false once the
    // entry has run out.
    //
    // Adding rather than writing: an instance's lane is shared by its layers
    // in the live path, and the two paths must leave the lane in the same
    // state or the spatialiser downstream would need to know which it was.
    bool render_block(i64* lane_l, i64* lane_r) noexcept {
        if (!pcm) return false;
        const u32 stride = stereo ? 2u : 1u;
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            u64 idx = pos_q32 >> 32;
            if (idx >= frames) {
                if (!looping) return false;
                pos_q32 -= static_cast<u64>(frames) << 32;
                idx = pos_q32 >> 32;
            }
            const u32 frac = static_cast<u32>((pos_q32 >> 16) & 0xFFFFu);
            const i32* at = pcm + static_cast<size_t>(idx) * stride;
            const int st = static_cast<int>(stride);
            const i32 l = hermite4_pcm(at, frac, st);
            lane_l[i] += l;
            lane_r[i] += stereo ? hermite4_pcm(at + 1, frac, st) : l;
            pos_q32 += rate_q32;
        }
        return true;
    }
};

}  // namespace dsnd
