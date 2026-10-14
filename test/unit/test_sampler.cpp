//
// The sampler, and the identity property the hybrid model rests on.
//
// If a cached sound at unity rate is bit-identical to its live render, then
// the engine is free to cache some sounds and not others, to keep building
// its cache while already playing, and to fall back to live synthesis when a
// budget runs out -- none of which a listener can detect. If it is not, every
// one of those becomes an audible seam. So this file is less a test of the
// sampler than the licence for the design.
//
#include <vector>

#include "sfx/sample_voice.h"
#include "synth/oscillator.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

// A buffer with the guard samples the cache promises: one before, two after.
struct Guarded {
    std::vector<i32> store;
    const i32* data() const { return store.data() + 1; }
    u32 frames = 0;

    explicit Guarded(const std::vector<i32>& real) : frames(static_cast<u32>(real.size())) {
        store.push_back(0);
        for (i32 v : real) store.push_back(v);
        store.push_back(0);
        store.push_back(0);
    }
};

std::vector<i32> ramp_and_wiggle(int n) {
    std::vector<i32> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        // Deliberately signed and not smooth: asr rounds toward -inf, so a
        // rounding mistake shows up on negatives and nowhere else.
        const i32 s = static_cast<i32>((i * 7919) % 2003) - 1000;
        v.push_back(s * (1 << 12) * ((i % 3 == 0) ? -1 : 1));
    }
    return v;
}

}  // namespace

// The port has to be exact, not merely equivalent. On a power-of-two buffer
// the wavetable version's wrap never engages, so the two must agree for every
// index and every fraction.
TEST(hermite4_pcm_matches_the_wavetable_hermite) {
    constexpr int N = 64;
    std::vector<i32> tab = ramp_and_wiggle(N);
    const u32 mask = N - 1;
    int compared = 0;
    for (u32 idx = 1; idx + 2 < N; ++idx) {
        for (u32 frac : {0u, 1u, 7u, 1000u, 32768u, 65535u}) {
            const i32 a = msynth::hermite4(tab.data(), idx, mask, frac);
            const i32 b = hermite4_pcm(tab.data() + idx, frac);
            CHECK_EQ(a, b);
            ++compared;
        }
    }
    CHECK(compared > 300);
}

TEST(hermite4_pcm_at_zero_fraction_is_the_sample_itself) {
    std::vector<i32> tab = ramp_and_wiggle(64);
    for (u32 idx = 1; idx + 2 < 64; ++idx) CHECK_EQ(hermite4_pcm(tab.data() + idx, 0), tab[idx]);
}

// Link 2 of the identity chain: unity rate must be EXACTLY 1<<32, or the
// fraction drifts away from zero and everything above stops applying.
TEST(rate_from_zero_cents_is_exactly_unity) {
    CHECK_EQ(rate_from_cents(0), static_cast<i64>(RATE_ONE));
    // And a real interval is what it should be, within the table's precision.
    const u64 octave = rate_from_cents(1200 * GAIN_ONE);
    CHECK_NEAR(static_cast<i64>(octave >> 16), static_cast<i64>(2 * GAIN_ONE), 4);
    const u64 down = rate_from_cents(-1200 * GAIN_ONE);
    CHECK_NEAR(static_cast<i64>(down >> 16), static_cast<i64>(GAIN_ONE / 2), 4);
}

TEST(rate_is_clamped_at_the_aliasing_bound) {
    // Beyond an octave up, a 4-point Hermite read of PCM aliases audibly.
    CHECK_EQ(rate_from_cents(9600 * GAIN_ONE), static_cast<i64>(RATE_MAX));
    CHECK_EQ(rate_from_cents(-9600 * GAIN_ONE), static_cast<i64>(RATE_MIN));
}

// THE PROPERTY. A cached entry played at unity rate reproduces itself sample
// for sample -- so a cached sound and its live render are the same bytes.
TEST(unity_rate_playback_reproduces_the_buffer_exactly) {
    const std::vector<i32> real = ramp_and_wiggle(CONTROL_BLOCK * 3);
    Guarded g(real);

    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;
    v.rate_q32 = rate_from_cents(0);

    std::vector<i64> out;
    for (int b = 0; b < 3; ++b) {
        i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
        CHECK(v.render_block(l, r));
        for (int i = 0; i < CONTROL_BLOCK; ++i) out.push_back(l[i]);
    }
    CHECK_EQ(out.size(), real.size());
    for (size_t i = 0; i < real.size(); ++i) CHECK_EQ(out[i], real[i]);
}

// Control: the property is about unity rate specifically. At any other rate
// the read is an interpolation and must NOT come back identical, or the test
// above would be proving nothing about rate at all.
TEST(control_non_unity_rate_does_not_reproduce_the_buffer) {
    const std::vector<i32> real = ramp_and_wiggle(CONTROL_BLOCK * 2);
    Guarded g(real);

    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;
    v.rate_q32 = rate_from_cents(35 * GAIN_ONE);  // a typical pitch jitter

    i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
    CHECK(v.render_block(l, r));
    bool any_differ = false;
    for (int i = 0; i < CONTROL_BLOCK; ++i)
        if (l[i] != real[i]) any_differ = true;
    CHECK(any_differ);
}

TEST(mono_entries_feed_both_lanes_equally) {
    const std::vector<i32> real = ramp_and_wiggle(CONTROL_BLOCK);
    Guarded g(real);
    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;

    i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
    v.render_block(l, r);
    for (int i = 0; i < CONTROL_BLOCK; ++i) CHECK_EQ(l[i], r[i]);
}

// A stereo entry is interleaved, so a channel's four interpolation points are
// four FRAMES apart. Reading them one sample apart picks up the other channel
// and sounds like ring modulation; this is the test that catches it.
TEST(stereo_entries_keep_their_channels_apart) {
    constexpr int frames = 32;
    std::vector<i32> real;
    for (int i = 0; i < frames; ++i) {
        real.push_back(1000 * (1 << 12));   // L: constant
        real.push_back(-1000 * (1 << 12));  // R: constant, opposite
    }
    // Guards are whole frames for a stereo entry.
    std::vector<i32> store;
    store.push_back(0);
    store.push_back(0);
    for (i32 s : real) store.push_back(s);
    for (int i = 0; i < 4; ++i) store.push_back(0);

    SampleVoice v;
    v.pcm = store.data() + 2;
    v.frames = frames;
    v.stereo = true;

    i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
    v.render_block(l, r);
    // Both channels are constant, so at unity rate every sample must be that
    // constant -- not a blend of the two.
    for (int i = 0; i < frames; ++i) {
        CHECK_EQ(l[i], 1000 * (1 << 12));
        CHECK_EQ(r[i], -1000 * (1 << 12));
    }
}

TEST(a_finished_entry_reports_itself_finished) {
    const std::vector<i32> real = ramp_and_wiggle(CONTROL_BLOCK / 2);
    Guarded g(real);
    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;

    i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
    CHECK(!v.render_block(l, r));  // runs out mid-block
    CHECK(v.finished());
}

TEST(a_looping_entry_wraps_without_a_gap) {
    const std::vector<i32> real = ramp_and_wiggle(16);
    Guarded g(real);
    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;
    v.looping = true;

    i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
    CHECK(v.render_block(l, r));
    CHECK(!v.finished());
    // At unity rate the loop repeats the buffer verbatim.
    for (int i = 0; i < CONTROL_BLOCK; ++i) CHECK_EQ(l[i], real[i % real.size()]);
}

// The lane is ADDED into, not written: an instance's layers share one lane in
// the live path, and the two paths must leave it in the same state.
TEST(render_adds_into_the_lane) {
    const std::vector<i32> real = ramp_and_wiggle(CONTROL_BLOCK);
    Guarded g(real);
    SampleVoice v;
    v.pcm = g.data();
    v.frames = g.frames;

    i64 l[CONTROL_BLOCK], r[CONTROL_BLOCK];
    for (int i = 0; i < CONTROL_BLOCK; ++i) l[i] = r[i] = 7;
    v.render_block(l, r);
    for (int i = 0; i < CONTROL_BLOCK; ++i) CHECK_EQ(l[i], real[i] + 7);
}
