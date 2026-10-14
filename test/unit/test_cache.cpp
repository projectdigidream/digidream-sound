//
// The PCM cache, and the end-to-end identity: a cached variant played at
// unity rate is the same bytes as rendering it live.
//
// test_sampler.cpp proves the interpolator is exact. This proves the whole
// chain is -- render, store, read back -- which is the claim the engine
// actually relies on when it caches some sounds and not others.
//
#include <string>
#include <vector>

#include "sfx/pcm_cache.h"
#include "sfx/sfx_json.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

const char* kDecayPatch =
    "{\"layers\":[{\"osc\":\"noise\","
    "\"ampEnv\":{\"attack\":1,\"decay\":60,\"sustain\":0,\"release\":10},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":4000,\"resonance\":0.3}}]}";

// Sustains forever with no note-off and no loop: the leak the baker must
// refuse rather than ship.
const char* kEndlessPatch =
    "{\"layers\":[{\"osc\":\"wavetable\","
    "\"wave\":{\"gen\":\"additiveHarmonicMorph\",\"frames\":[{\"harmonics\":[1]}]},"
    "\"ampEnv\":{\"attack\":1,\"decay\":0,\"sustain\":1,\"release\":50}}]}";

std::string proj(const std::string& sounds, const std::string& buses =
                     "[{\"name\":\"world\",\"maxInstances\":8},"
                     "{\"name\":\"ui\",\"spatial\":false}]") {
    return "{\"version\":1,\"name\":\"t\",\"buses\":" + buses + ",\"sounds\":[" + sounds + "]}";
}

std::string sound(const std::string& name, const char* patch, const std::string& extra = "") {
    return "{\"name\":\"" + name + "\"" + extra + ",\"variants\":[{\"patch\":" + patch + "}]}";
}

bool load(const std::string& j, WaveTableCache& cache, SoundBank& b) {
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
    return r.ok;
}

}  // namespace

TEST(cache_renders_a_one_shot_and_stops) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("hit", kDecayPatch)), wc, b));

    PcmCache cache;
    CacheReport rep;
    LoadResult r = cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024, &rep);
    for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK_EQ(rep.cached_variants, 1u);
    CHECK_EQ(rep.live_variants, 0u);
    CHECK_EQ(cache.size(), 1u);

    const CacheEntry& e = cache.entry(0);
    CHECK(!e.stereo);  // a spatial bus caches mono
    CHECK(e.frames > 0);
    // A 60 ms decay is about 2900 samples; well under the 4 s ceiling.
    CHECK(e.frames < 48000u);
    CHECK_EQ(b.variants[0].cache_index, 0);
}

TEST(cache_uses_stereo_for_a_non_spatial_bus) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("click", kDecayPatch, ",\"bus\":\"ui\"")), wc, b));
    PcmCache cache;
    CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    CHECK(cache.entry(0).stereo);
}

// THE PROPERTY, end to end. Playing the cached entry back at unity rate must
// reproduce exactly what the live render produced.
TEST(cached_playback_equals_the_live_render) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("hit", kDecayPatch)), wc, b));

    // The live reference, mono-summed the way a spatial bus sums it.
    std::vector<i32> live;
    u32 frames = 0;
    CHECK(render_variant(b, 0, 0, /*mono=*/true, 48000 / CONTROL_BLOCK * 4, live, &frames));
    CHECK(frames > 100);

    PcmCache cache;
    CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    const CacheEntry& e = cache.entry(0);
    CHECK_EQ(e.frames, frames);

    // Play it back exactly as the engine would.
    SampleVoice sv;
    sv.pcm = e.data();
    sv.frames = e.frames;
    sv.stereo = e.stereo;
    sv.rate_q32 = rate_from_cents(0);

    std::vector<i64> played;
    while (!sv.finished()) {
        i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
        const bool alive = sv.render_block(l, r);
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            // The spatialiser's mono sum, applied to both paths alike.
            played.push_back(msynth::asr(l[i] + r[i], 1));
        }
        if (!alive) break;
    }

    CHECK(played.size() >= frames);
    int mismatches = 0;
    for (u32 i = 0; i < frames; ++i)
        if (played[i] != live[i]) ++mismatches;
    CHECK_EQ(mismatches, 0);
}

// Control: the comparison above is only meaningful if this signal is not
// simply silence or a constant.
TEST(control_the_cached_signal_is_real_audio) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("hit", kDecayPatch)), wc, b));
    PcmCache cache;
    CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    const CacheEntry& e = cache.entry(0);

    i64 peak = 0;
    int distinct = 0;
    i32 last = e.data()[0];
    for (u32 i = 0; i < e.frames; ++i) {
        const i32 v = e.data()[i];
        const i64 a = v < 0 ? -static_cast<i64>(v) : v;
        if (a > peak) peak = a;
        if (v != last) ++distinct;
        last = v;
    }
    CHECK(peak > (1 << SAMPLE_SHIFT) / 100);  // audible
    CHECK(distinct > 100);                    // not a constant
}

// A sustaining envelope with no note-off and no loop is a voice that can
// never finish and an instance that is never reclaimed. It is invisible in a
// UI and fatal in a game, so the loader refuses it by name -- and refuses it
// at LOAD, from the patch itself, rather than by watching a render, because a
// render can only see that something is still quietly sounding and a long
// decay looks exactly like that.
TEST(a_variant_that_can_never_end_is_refused_at_load) {
    WaveTableCache wc;
    SoundBank b;
    const std::string j = proj(sound("drone", kEndlessPatch));
    LoadResult r = load_bank_json(j.c_str(), j.size(), wc, b);
    CHECK(!r.ok);
    CHECK(!r.errors.empty());
    bool named = false;
    for (const auto& e : r.errors)
        if (e.find("never released") != std::string::npos && e.find("drone") != std::string::npos)
            named = true;
    CHECK(named);
}

// Marking it `live` does NOT make it safe -- a live sustaining voice with no
// note-off leaks just the same. Only a loop, or a note-off, or a one-shot
// envelope actually ends it.
TEST(live_alone_does_not_excuse_an_endless_patch) {
    WaveTableCache wc;
    SoundBank b;
    const std::string j = proj(sound("drone", kEndlessPatch, ",\"live\":true"));
    CHECK(!load_bank_json(j.c_str(), j.size(), wc, b).ok);
}

// A loop is meant to sustain until something stops it, so the check does not
// apply, and the sound is live by construction.
TEST(a_looping_sound_may_sustain) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("wind", kEndlessPatch, ",\"loop\":true")), wc, b));
    PcmCache cache;
    CacheReport rep;
    LoadResult r = cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024, &rep);
    CHECK(r.ok);
    CHECK_EQ(rep.cached_variants, 0u);
    CHECK_EQ(rep.live_variants, 1u);
}

// And so does a gesture note that is explicitly released.
TEST(a_released_gesture_note_may_sustain) {
    WaveTableCache wc;
    SoundBank b;
    const std::string j = proj(
        "{\"name\":\"held\",\"variants\":[{\"patch\":" + std::string(kEndlessPatch) +
        ",\"gesture\":[{\"atMs\":0,\"note\":60,\"durMs\":120}]}]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), wc, b);
    for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
    CHECK(r.ok);
}

// Budget exhaustion falls back to live, deterministically, and keeps going so
// that a smaller high-priority sound further down still gets cached.
TEST(cache_budget_falls_back_to_live_in_priority_order) {
    WaveTableCache wc;
    SoundBank b;
    const std::string j = proj(sound("low", kDecayPatch, ",\"priority\":10") + "," +
                               sound("high", kDecayPatch, ",\"priority\":250"));
    CHECK(load(j, wc, b));

    // Enough for one of them, not both.
    PcmCache probe;
    CHECK(probe.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    const u32 one = probe.entry(0).bytes() + 64;

    WaveTableCache wc2;
    SoundBank b2;
    CHECK(load(j, wc2, b2));
    PcmCache cache;
    CacheReport rep;
    LoadResult r = cache.build(b2, one, &rep);
    CHECK(r.ok);
    CHECK_EQ(rep.cached_variants, 1u);
    CHECK_EQ(rep.live_variants, 1u);

    // The high-priority one is the one that got the space.
    const int hi = b2.sound_index("high");
    const int lo = b2.sound_index("low");
    CHECK(!b2.sounds[hi].live);
    CHECK(b2.sounds[lo].live);
}

// Same inputs, same outcome -- on every machine and every run. Without this
// the golden renders would be reproducible only by luck.
TEST(cache_build_is_reproducible) {
    const std::string j = proj(sound("a", kDecayPatch, ",\"priority\":10") + "," +
                               sound("b", kDecayPatch, ",\"priority\":200"));
    std::vector<i32> first;
    u32 first_bytes = 0;
    for (int pass = 0; pass < 2; ++pass) {
        WaveTableCache wc;
        SoundBank b;
        CHECK(load(j, wc, b));
        PcmCache cache;
        CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
        if (pass == 0) {
            first_bytes = cache.bytes();
            const CacheEntry& e = cache.entry(0);
            first.assign(e.data(), e.data() + e.frames);
        } else {
            CHECK_EQ(cache.bytes(), first_bytes);
            const CacheEntry& e = cache.entry(0);
            CHECK_EQ(e.frames, first.size());
            int mismatches = 0;
            for (u32 i = 0; i < e.frames; ++i)
                if (e.data()[i] != first[i]) ++mismatches;
            CHECK_EQ(mismatches, 0);
        }
    }
}

// Guard frames are physically present, so the interpolator's inner loop needs
// no bounds check. If this ever stops holding, the sampler reads out of
// bounds and the failure is a crash a long way from the cause.
TEST(cache_entries_carry_their_guard_frames) {
    WaveTableCache wc;
    SoundBank b;
    CHECK(load(proj(sound("hit", kDecayPatch)), wc, b));
    PcmCache cache;
    CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    const CacheEntry& e = cache.entry(0);
    CHECK_EQ(e.store.size(), e.frames + 3u);  // mono: 1 before, 2 after
    CHECK_EQ(e.data(), e.store.data() + 1);
    CHECK_EQ(e.data()[-1], 0);
    CHECK_EQ(e.data()[e.frames], 0);
    CHECK_EQ(e.data()[e.frames + 1], 0);
}
