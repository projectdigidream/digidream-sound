//
// The invariant the whole project rests on: what a sound renders depends on
// the seed the caller gave it and on nothing else.
//
// midisynth derives a voice's noise seed from its POOL SLOT (voice.cpp:41).
// Inside midisynth that is harmless -- a render is a closed system and the
// same song always allocates the same way. Here it is fatal: the same door,
// triggered from the same game event, would sound different depending on what
// else happened to be playing. reseed_voice() replaces those seeds; these
// tests are what say it worked.
//
// The controls matter as much as the assertions. A test that renders the same
// voice twice and finds it identical proves nothing unless an unseeded voice
// in the same harness is shown to DIFFER -- otherwise the patch might simply
// contain no noise. Both directions are checked below.
//
#include <string>
#include <vector>

#include "sfx/sfx_json.h"
#include "sfx/voice_alloc.h"
#include "synth/json_min.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

// A noise layer through a filter: the loudest possible dependence on the
// PRNG stream. If seeding is wrong anywhere, this patch shows it.
const char* kNoisePatch =
    "{\"layers\":[{\"osc\":\"noise\","
    "\"ampEnv\":{\"attack\":1,\"decay\":120,\"sustain\":0,\"release\":10},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":3000,\"resonance\":0.4}}]}";

// A sample-and-hold LFO on the cutoff. This is the one that catches the
// `held` mistake: if reseeding pokes rng without redrawing held, the stream
// is one step out of phase with a fresh voice and only this patch notices.
const char* kSampleHoldPatch =
    "{\"layers\":[{\"osc\":\"wavetable\","
    "\"wave\":{\"gen\":\"additiveHarmonicMorph\",\"frames\":[{\"harmonics\":[1,0.5,0.25]}]},"
    "\"ampEnv\":{\"attack\":1,\"decay\":300,\"sustain\":0,\"release\":10},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":2000,\"resonance\":0.5},"
    "\"lfos\":[{\"shape\":\"sampleHold\",\"rateHz\":40}],"
    "\"modMatrix\":[{\"src\":\"LFO1\",\"dst\":\"FilterCutoff\",\"amount\":0.8}]}]}";

msynth::Patch build(const char* json) {
    std::string err;
    msynth::JsonPtr doc = msynth::json_parse(json, std::string(json).size(), &err);
    msynth::Patch p;
    if (!doc) {
        std::printf("  patch parse failed: %s\n", err.c_str());
        return p;
    }
    static WaveTableCache cache;  // shared: dedupes tables across these tests
    msynth::LoadResult r = msynth::load_patch(*doc, cache, p);
    for (const auto& e : r.errors) std::printf("  patch error: %s\n", e.c_str());
    return p;
}

// Start one voice on a chosen pool slot and render it, so the slot -- the
// thing that must NOT matter -- can be varied deliberately.
std::vector<i64> render_on_slot(const msynth::Patch& p, u8 slot, u32 seed, bool reseed,
                                int blocks = 40) {
    Voice v;
    v.slot = slot;
    const ChannelState ch = neutral_channel();
    const msynth::LayerDef& l = p.layers[0];

    v.start(slot, 0, 60, 100, 1, 0, &p, &l, ch);
    if (reseed) reseed_voice(v, l, mix_seed(seed, 0, 0, 0));

    std::vector<i64> out;
    out.reserve(static_cast<size_t>(blocks) * CONTROL_BLOCK * 2);
    for (int b = 0; b < blocks; ++b) {
        i64 dry_l[CONTROL_BLOCK] = {}, dry_r[CONTROL_BLOCK] = {};
        i64 rev_l[CONTROL_BLOCK] = {}, rev_r[CONTROL_BLOCK] = {};
        i64 cho_l[CONTROL_BLOCK] = {}, cho_r[CONTROL_BLOCK] = {};
        i64 del_l[CONTROL_BLOCK] = {}, del_r[CONTROL_BLOCK] = {};
        VoiceBuses buses{dry_l, dry_r, rev_l, rev_r, cho_l, cho_r, del_l, del_r,
                         120 << GAIN_SHIFT, nullptr};
        v.render_block(ch, buses);
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            out.push_back(dry_l[i]);
            out.push_back(dry_r[i]);
        }
    }
    return out;
}

bool all_zero(const std::vector<i64>& v) {
    for (i64 s : v)
        if (s != 0) return false;
    return true;
}

}  // namespace

// The control. Without reseeding, the pool slot leaks into the audio -- this
// is the bug, demonstrated, so the test below cannot pass vacuously.
TEST(reseed_control_pool_slot_leaks_without_the_fix) {
    const msynth::Patch p = build(kNoisePatch);
    CHECK_EQ(p.layer_count, 1);
    const auto a = render_on_slot(p, 0, 12345, /*reseed=*/false);
    const auto b = render_on_slot(p, 7, 12345, /*reseed=*/false);
    CHECK(!all_zero(a));
    CHECK_EQ(a.size(), b.size());
    CHECK(a != b);  // the defect this project has to correct
}

TEST(reseed_makes_the_pool_slot_irrelevant) {
    const msynth::Patch p = build(kNoisePatch);
    const auto a = render_on_slot(p, 0, 12345, /*reseed=*/true);
    const auto b = render_on_slot(p, 7, 12345, /*reseed=*/true);
    const auto c = render_on_slot(p, 31, 12345, /*reseed=*/true);
    CHECK(!all_zero(a));
    CHECK(a == b);
    CHECK(a == c);
}

// Different callers' seeds must still give different noise, or "seeded" would
// just mean "constant" and every gunshot in a burst would phase against its
// neighbours.
TEST(reseed_different_seeds_still_differ) {
    const msynth::Patch p = build(kNoisePatch);
    const auto a = render_on_slot(p, 3, 1, /*reseed=*/true);
    const auto b = render_on_slot(p, 3, 2, /*reseed=*/true);
    CHECK(a != b);
}

// A sample-and-hold LFO reads its first value at note-on. If reseeding
// replaced `rng` without redrawing `held`, the two streams would be one step
// apart and only this patch would show it.
TEST(reseed_keeps_sample_hold_in_phase) {
    const msynth::Patch p = build(kSampleHoldPatch);
    CHECK_EQ(p.layer_count, 1);
    CHECK_EQ(p.layers[0].lfo_count, 1);

    const auto a = render_on_slot(p, 0, 999, /*reseed=*/true);
    const auto b = render_on_slot(p, 19, 999, /*reseed=*/true);
    CHECK(!all_zero(a));
    CHECK(a == b);

    // Control: this patch really is sensitive to the LFO stream.
    const auto c = render_on_slot(p, 0, 1000, /*reseed=*/true);
    CHECK(a != c);
}

// mix_seed must separate layers and unison members, or a two-layer noise
// patch renders the identical stream twice and sounds like one layer 6 dB up.
TEST(mix_seed_separates_layers_and_unison) {
    const u32 base = 0xABCDEF01u;
    CHECK(mix_seed(base, 0, 0, 0) != mix_seed(base, 0, 1, 0));
    CHECK(mix_seed(base, 0, 0, 0) != mix_seed(base, 1, 0, 0));
    CHECK(mix_seed(base, 0, 0, 0) != mix_seed(base, 0, 0, 1));
    CHECK(mix_seed(1, 0, 0, 0) != mix_seed(2, 0, 0, 0));
    // Never zero: XorShift32 is degenerate at a zero state, and it silently
    // substitutes a constant rather than telling anyone.
    CHECK(mix_seed(0, 0, 0, 0) != 0u);
}

// ------------------------------------------------------------------ the pool

TEST(pool_allocates_all_or_nothing) {
    SfxVoicePool pool;
    Voice* got[MAX_SFX_VOICES];
    CHECK_EQ(pool.free_count(), MAX_SFX_VOICES);

    CHECK(pool.alloc_n(4, 1, got));
    CHECK_EQ(pool.active_count(), 4);

    // More than remains: nothing is taken, and the pool is untouched.
    const int before = pool.free_count();
    CHECK(!pool.alloc_n(MAX_SFX_VOICES, 2, got));
    CHECK_EQ(pool.free_count(), before);
    CHECK_EQ(pool.active_count(), 4);
}

TEST(pool_releases_by_instance) {
    SfxVoicePool pool;
    Voice* a[4];
    Voice* b[4];
    CHECK(pool.alloc_n(4, 10, a));
    CHECK(pool.alloc_n(4, 20, b));
    CHECK_EQ(pool.active_count(), 8);

    // Nothing was started, so these are reservations with no tail: releasing
    // one must hand back its four voices and leave the other instance's alone.
    pool.release_instance(10);
    pool.reap();
    CHECK_EQ(pool.active_count(), 4);
    for (int i = 0; i < MAX_SFX_VOICES; ++i)
        if (pool.owner(i) != NO_OWNER) CHECK_EQ(pool.owner(i), 20);
}

TEST(pool_keeps_a_sounding_voice_until_its_fade_ends) {
    const msynth::Patch p = build(kNoisePatch);
    SfxVoicePool pool;
    Voice* got[1];
    CHECK(pool.alloc_n(1, 5, got));

    const ChannelState ch = neutral_channel();
    got[0]->start(got[0]->slot, 0, 60, 100, 1, 0, &p, &p.layers[0], ch);
    CHECK(got[0]->active);

    pool.release_instance(5);
    pool.reap();
    // Still owned: fast_release is a short ramp, not a hard stop, and handing
    // the slot out mid-ramp is how a click gets into a mix.
    CHECK_EQ(pool.active_count(), 1);

    i64 l[CONTROL_BLOCK], r[CONTROL_BLOCK], z[CONTROL_BLOCK];
    for (int b = 0; b < 16; ++b) {
        for (int i = 0; i < CONTROL_BLOCK; ++i) l[i] = r[i] = z[i] = 0;
        VoiceBuses buses{l, r, z, z, z, z, z, z, 120 << GAIN_SHIFT, nullptr};
        got[0]->render_block(ch, buses);
    }
    pool.reap();
    CHECK_EQ(pool.active_count(), 0);
    CHECK_EQ(pool.free_count(), MAX_SFX_VOICES);
}
